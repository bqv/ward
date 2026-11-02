/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.net

import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONArray
import org.json.JSONObject
import uk.xa0.ward.model.AnswerDelivery
import uk.xa0.ward.model.HostVersion
import uk.xa0.ward.model.hostVersionFromReadyFrame
import uk.xa0.ward.proto.Cbor
import uk.xa0.ward.proto.WardApi
import uk.xa0.ward.proto.WardConnection
import uk.xa0.ward.proto.WardConnectionException
import uk.xa0.ward.proto.WardEvent
import uk.xa0.ward.proto.WardRpcException
import uk.xa0.ward.proto.WardSession
import uk.xa0.ward.proto.WardSubscription
import java.util.concurrent.atomic.AtomicLong

typealias AnswerSink = suspend (outcome: JSONObject) -> AnswerDelivery

sealed interface StreamEvent {
    data class Item(val value: JSONObject) : StreamEvent
    data class Failure(val code: String, val message: String) : StreamEvent
    data object End : StreamEvent
}

enum class MuxState { IDLE, CONNECTING, OPEN, CLOSED }

/**
 * Ward's subscription streams behind the four logical endpoint names the copied
 * screens already open.
 *
 * Ward has one socket and one kind of subscription: `session.resume` allocates a
 * `sub` and every durable event for that session arrives on it. The DSH screens
 * speak a multiplexed WebSocket with `$events`, `session/follow`, `session/control`
 * and `workspace/follow`; this class owns that translation and re-subscribes after
 * a dropped socket, so a collector is not silently left on a dead registration.
 */
class RemoteMux(
    private val connect: suspend () -> WardConnection,
    private val label: () -> String,
    private val client: DshClient,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _state = MutableStateFlow(MuxState.IDLE)
    val state: StateFlow<MuxState> = _state.asStateFlow()

    @Volatile private var closed = false
    @Volatile private var connection: WardConnection? = null

    @Volatile var postEventResult: (suspend (JSONObject) -> Unit)? = null
    @Volatile var onWaterfall: (suspend (frame: JSONObject, send: AnswerSink) -> Unit)? = null
    @Volatile var onWaterfallCancel: ((eventId: String) -> Unit)? = null
    @Volatile var onEventsGeneration: ((clientId: String) -> Unit)? = null
    @Volatile var onHostVersion: ((HostVersion) -> Unit)? = null

    @Volatile var hostVersion: HostVersion = HostVersion.Unknown
        private set

    private val lastFrameAt = AtomicLong(0L)
    private val approvalFrames = MutableSharedFlow<JSONObject>(replay = 16, extraBufferCapacity = 64)
    private val forwardedEvents = MutableSharedFlow<JSONObject>(replay = 16, extraBufferCapacity = 64)
    private val projectionFrames = MutableSharedFlow<JSONObject>(replay = 16, extraBufferCapacity = 64)
    private val terminalFrames = MutableSharedFlow<JSONObject>(replay = 32, extraBufferCapacity = 256)
    private val terminalSeq = AtomicLong(1)
    private val nextSynthetic = AtomicLong(1)
    private val knownApprovals = java.util.concurrent.ConcurrentHashMap<String, Boolean>()

    init { supervise() }

    fun lastHostFrameAt(): Long = lastFrameAt.get()

    /** Feed one translated ward approval to the `$events` collectors. */
    internal fun publishApproval(frame: JSONObject) {
        val id = frame.optString("eventId")
        if (id.isNotEmpty()) knownApprovals[id] = true
        approvalFrames.tryEmit(frame)
    }

    /** Tell the attention centre that a ward approval is settled. */
    internal fun cancelApproval(eventId: String) {
        knownApprovals.remove(eventId)
        onWaterfallCancel?.invoke(eventId)
    }

    /** One control-stream projection delta, in the shape `handleControlFrame` reads. */
    internal fun publishProjection(sessionId: String, key: String, value: Any?) {
        projectionFrames.tryEmit(
            JSONObject()
                .put("type", "projection")
                .put("sessionId", sessionId)
                .put("key", key)
                .put("value", value ?: JSONObject.NULL),
        )
    }

    /** One `terminal.output` broadcast, keyed by ward's terminal handle. */
    internal fun publishTerminal(payload: Cbor.Map) {
        val id = payload.text("terminal").orEmpty()
        if (id.isEmpty()) return
        val eof = payload.bool("eof") == true
        terminalFrames.tryEmit(JSONObject()
            .put("type", if (eof) "eof" else "output")
            .put("terminal", id)
            .put("data", payload.text("data").orEmpty())
            .put("sequence", terminalSeq.getAndIncrement()))
    }

    /** Tell the roster that a child session exists before the next pull sees it. */
    internal fun publishSessionAdded(row: JSONObject) {
        forwardedEvents.tryEmit(
            JSONObject()
                .put("type", "emit")
                .put("event", "api-session/added")
                .put("args", JSONArray().put(row)),
        )
    }

    /** A ward turn boundary is the DSH live-status fact for one session. */
    internal fun publishStatus(sessionId: String, running: Boolean) {
        forwardedEvents.tryEmit(
            JSONObject()
                .put("type", "emit")
                .put("event", "api-session/status")
                .put("args", JSONArray().put(sessionId).put(running)),
        )
    }

    suspend fun ensureConnected() {
        if (closed) return
        if (_state.value == MuxState.OPEN) return
        _state.value = MuxState.CONNECTING
        try {
            val active = connect()
            connection = active
            _state.value = MuxState.OPEN
            hostVersion = HostVersion.Reported(active.serverName.ifEmpty { "ward" })
            onHostVersion?.invoke(hostVersion)
        } catch (error: Throwable) {
            _state.value = MuxState.CLOSED
            throw error
        }
    }

    fun openStream(endpoint: String, args: JSONObject): Flow<StreamEvent> = callbackFlow {
        if (closed) {
            close()
            return@callbackFlow
        }
        val producer = this
        val job = scope.launch {
            while (isActive && !closed) {
                try {
                    ensureConnected()
                    when (endpoint) {
                        "\$events" -> collectEvents(producer)
                        "session/follow" -> collectFollow(args, producer)
                        "session/control" -> collectControl(producer)
                        "workspace/follow" -> collectWorkspaces(producer)
                        "job/list" -> collectJobs(args, producer)
                        "job/follow" -> collectJobFollow(args, producer)
                        "terminal/follow" -> collectTerminal(args, producer)
                        else -> {
                            producer.trySend(StreamEvent.Failure("not_supported", "ward has no $endpoint stream"))
                            delay(5_000)
                        }
                    }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Throwable) {
                    Log.w(TAG, "$endpoint stream failed: ${error.message}")
                    producer.trySend(StreamEvent.Failure("stream/error", error.message ?: "stream failed"))
                    delay(3_000)
                }
            }
        }
        awaitClose { job.cancel() }
    }

    fun shutdown() {
        closed = true
        _state.value = MuxState.CLOSED
        connection?.close()
        connection = null
        scope.cancel()
    }

    private fun supervise() {
        scope.launch {
            var backoff = 1_000L
            while (!closed) {
                if (_state.value != MuxState.OPEN) {
                    val ok = runCatching { ensureConnected() }.isSuccess
                    if (ok) {
                        backoff = 1_000L
                    } else {
                        delay(backoff)
                        backoff = (backoff * 2).coerceAtMost(15_000L)
                    }
                } else {
                    delay(5_000)
                }
            }
        }
    }

    private suspend fun collectEvents(out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>) {
        val active = connection ?: connect().also { connection = it }
        val ready = JSONObject()
            .put("type", "ready")
            .put("clientId", "ward-android")
            .put("host", JSONObject().put("version", active.serverName.ifEmpty { "ward" }))
        lastFrameAt.set(System.currentTimeMillis())
        hostVersion = hostVersionFromReadyFrame(ready)
        onHostVersion?.invoke(hostVersion)
        onEventsGeneration?.invoke("ward-android")
        out.trySend(StreamEvent.Item(ready))
        val approvals = scope.launch {
            approvalFrames.collect { frame ->
                lastFrameAt.set(System.currentTimeMillis())
                deliverWaterfall(frame)
            }
        }
        val polls = scope.launch { pollApprovals(active) }
        try {
            forwardedEvents.collect { event -> out.trySend(StreamEvent.Item(event)) }
        } finally {
            approvals.cancel()
            polls.cancel()
        }
    }

    /**
     * Ward answers `approval.list` across every session. That is what makes a
     * background approval visible: the follow stream only ever sees the session
     * it is attached to, and there is no push for a card raised elsewhere.
     *
     * The poll is deliberately slow. It is a reconciliation, not the live path —
     * a followed session publishes its own `approval.requested` immediately — and
     * five seconds is inside the daemon's own approval lifetime by a wide margin.
     */
    private suspend fun pollApprovals(active: WardConnection) {
        val api = WardApi(active)
        while (scope.isActive && !closed) {
            val pending = runCatching { api.listApprovals() }.getOrNull()
            if (pending == null) {
                delay(5_000)
                continue
            }
            if (pending.isNotEmpty()) Log.d(TAG, "approval.list: ${pending.size} pending")
            val seen = HashSet<String>(pending.size)
            for (info in pending) {
                val session = info.text("session").orEmpty()
                val call = info.long("call") ?: 0L
                val epoch = info.long("epoch") ?: 0L
                if (session.isEmpty()) continue
                val eventId = approvalEventId(session, call, epoch)
                seen += eventId
                if (knownApprovals[eventId] == true) continue
                client.rememberApproval(eventId, session, call, epoch)
                val tool = info.text("tool").orEmpty().ifEmpty { "tool call $call" }
                val reason = info.text("reason").orEmpty()
                publishApproval(JSONObject()
                    .put("type", "waterfall")
                    .put("event", "approval/request")
                    .put("eventId", eventId)
                    .put("agentId", session)
                    .put("request", JSONObject()
                        .put("toolName", tool)
                        .put("reason", reason)))
            }
            knownApprovals.keys.toList().filter { it !in seen }.forEach { eventId ->
                client.forgetApproval(eventId)
                cancelApproval(eventId)
            }
            delay(5_000)
        }
    }

    private fun deliverWaterfall(frame: JSONObject) {
        val handler = onWaterfall
        if (handler == null) {
            Log.d(TAG, "waterfall ${frame.optString("event")} has no handler")
            return
        }
        val post = postEventResult ?: return
        scope.launch {
            val send: AnswerSink = { outcome ->
                runCatching {
                    post(JSONObject()
                        .put("eventId", frame.optString("eventId"))
                        .put("outcome", outcome))
                    AnswerDelivery.Accepted
                }.getOrElse { error ->
                    when (error) {
                        is DshRpcException -> AnswerDelivery.Refused(error.message.take(160))
                        else -> AnswerDelivery.Unreachable(error.message?.take(160) ?: "unreachable")
                    }
                }
            }
            runCatching { handler(frame, send) }
        }
    }

    private suspend fun collectFollow(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        val request = args.optJSONObject("request") ?: args
        val address = request.optJSONObject("address") ?: request
        val sessionId = address.optString("sessionId")
            .ifEmpty { address.optString("childSessionId") }
            .ifEmpty { request.optString("sessionId") }
        if (sessionId.isEmpty()) {
            out.trySend(StreamEvent.Failure("not_found", "session/follow has no session id"))
            delay(5_000)
            return
        }
        val active = connection ?: connect().also { connection = it }
        val subscription = active.resume(sessionId, 0)
        val header = subscription.session
        if (header != null) {
            val snapshot = JSONObject()
                .put("type", "snapshot")
                .put("header", JSONObject()
                    .put("id", header.id)
                    .put("cwd", header.cwd)
                    .put("createdAt", header.created))
                .put("records", JSONArray())
                .put("cursor", 0)
                .put("hasMore", false)
            statsProjection(active, sessionId)?.let {
                snapshot.put("projections", JSONObject().put("values", it))
            }
            out.trySend(StreamEvent.Item(snapshot))
        }
        val translator = FollowTranslator(sessionId, client)
        val queuePolls = scope.launch {
            var previous = ""
            while (scope.isActive && !closed) {
                val inbox = client.hostQueueInbox(sessionId)
                if (inbox == null) {
                    delay(2_000)
                    continue
                }
                if (inbox.toString() != previous) {
                    previous = inbox.toString()
                    client.mux?.publishProjection(sessionId, "inbox", inbox)
                }
                delay(2_000)
            }
        }
        var replaying = true
        try {
            subscription.events.collect { event ->
                lastFrameAt.set(System.currentTimeMillis())
                if (event.kind == "replay.end") {
                    replaying = false
                    return@collect
                }
                if (event.kind == "terminal.output") {
                    client.mux?.publishTerminal(event.payload)
                    return@collect
                }
                // A historical approval is not a live card. The daemon's
                // `approval.list` is the authority for what is still parked, and
                // the poll above is what raises one from the past.
                if (replaying && event.kind == "approval.requested") return@collect
                translator.translate(event).forEach { value ->
                    out.trySend(StreamEvent.Item(value))
                }
            }
        } finally {
            queuePolls.cancel()
            subscription.close()
        }
    }

    /**
     * Ward's whole-log counters, in the two projection blocks the copied stats
     * pills already read, plus the policy preset behind the access chip.
     */
    private suspend fun statsProjection(active: WardConnection, sessionId: String): JSONObject? {
        val stats = runCatching { WardApi(active).statsSession(sessionId) }.getOrNull() ?: return null
        val turns = stats.long("turns") ?: 0L
        val prompt = stats.long("prompt_tokens") ?: 0L
        val cache = stats.long("cache_tokens") ?: 0L
        val output = stats.long("predicted_tokens") ?: 0L
        val permission = runCatching { WardApi(active).policyList() }.getOrNull()
            ?.text("current").orEmpty()
            ?.let {
                when {
                    it == "reads-only" || it == "read-only" -> "read-only"
                    it == "workspace" || it == "workspace-write" -> "workspace-write"
                    it.contains("full", ignoreCase = true) -> "danger-full-access"
                    else -> it
                }
            }
        val projection = JSONObject()
            .put("sessionStats", JSONObject()
                .put("turns", turns)
                .put("steps", turns))
            .put("tokenUsage", JSONObject()
                .put("uncachedInputTokens", (prompt - cache).coerceAtLeast(0))
                .put("cacheReadTokens", cache)
                .put("cacheWriteTokens", 0)
                .put("outputTokens", output))
        if (!permission.isNullOrEmpty()) projection.put("permission", permission)
        client.hostQueueInbox(sessionId)?.let { projection.put("inbox", it) }
        return projection
    }

    private suspend fun collectControl(out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>) {
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "baseline")
            .put("value", JSONObject()
                .put("queues", JSONObject())
                .put("projections", JSONObject()))))
        projectionFrames.collect { frame -> out.trySend(StreamEvent.Item(frame)) }
    }

    /**
     * Ward has no Workspace registry, so the drawer's grouping is derived from the
     * session directories themselves. That is a client-owned view, not host state:
     * the id is the cwd, the title is its last segment, and membership follows the
     * roster's own order. A session with no cwd is left out of every group, which
     * is what the drawer's flat "Ungrouped" section already means.
     */
    private suspend fun collectWorkspaces(out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>) {
        var previous = ""
        while (scope.isActive && !closed) {
            val all = runCatching { client.listSessions() }.getOrNull()
            if (all == null) {
                delay(5_000)
                continue
            }
            val archived = JSONArray()
            val pinned = JSONArray()
            val groups = LinkedHashMap<String, MutableList<String>>()
            all.optJSONArray("items")?.let { items ->
                for (i in 0 until items.length()) {
                    val row = items.optJSONObject(i) ?: continue
                    val id = row.optString("sessionId")
                    if (id.isEmpty()) continue
                    if (row.optBoolean("archived")) archived.put(id)
                    if (row.optBoolean("pinned")) pinned.put(id)
                    val cwd = row.optString("cwd").trimEnd('/')
                    if (cwd.isEmpty()) continue
                    groups.getOrPut(cwd) { mutableListOf() } += id
                }
            }
            val rows = JSONArray()
            groups.forEach { (path, ids) ->
                rows.put(JSONObject()
                    .put("workspaceId", path)
                    .put("path", path)
                    .put("title", path.trimEnd('/').substringAfterLast('/').ifEmpty { path })
                    .put("sessionIds", JSONArray(ids)))
            }
            val value = JSONObject()
                .put("items", rows)
                .put("archivedSessionIds", archived)
                .put("pinnedSessionIds", pinned)
            if (value.toString() != previous) {
                previous = value.toString()
                out.trySend(StreamEvent.Item(JSONObject()
                    .put("type", "baseline")
                    .put("value", value)))
            }
            delay(5_000)
        }
    }

    private suspend fun collectJobs(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        var previous = ""
        while (scope.isActive && !closed) {
            val jobs = runCatching { client.rpcRaw("job/list", args) }.getOrNull()
            if (jobs != null) {
                val rows = (jobs as? JSONObject)?.optJSONArray("jobs") ?: JSONArray()
                if (rows.toString() != previous) {
                    previous = rows.toString()
                    out.trySend(StreamEvent.Item(JSONObject()
                        .put("type", "rows")
                        .put("jobs", rows)))
                }
            }
            delay(5_000)
        }
    }

    /**
     * Ward's job ring is a snapshot per call, not a push stream. One `opened`
     * frame anchors the DSH observation, each later call appends whatever the ring
     * has grown by, and a not-found answer is the job being gone: emit the terminal
     * status and let the copied panel fall back to the transcript.
     */
    private suspend fun collectJobFollow(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        val request = args.optJSONObject("request") ?: args
        val jobId = request.optString("jobId").ifEmpty { request.optString("id") }
            .ifEmpty { args.optString("jobId") }
        if (jobId.isEmpty()) {
            out.trySend(StreamEvent.Failure("invalid_params", "job/follow has no job id"))
            return
        }
        val active = connection ?: connect().also { connection = it }
        val api = WardApi(active)
        var cursor = request.optLong("from", 0L).coerceAtLeast(0L)
        var opened = false
        while (scope.isActive && !closed) {
            val page = runCatching { api.jobFollow(jobId, cursor) }.getOrElse { error ->
                if (!opened) {
                    out.trySend(StreamEvent.Failure("not_found", error.message ?: "job is gone"))
                } else {
                    out.trySend(StreamEvent.Item(JSONObject().put("type", "status")))
                    out.trySend(StreamEvent.End)
                }
                return
            }
            val next = page.long("next") ?: cursor
            val chunks = page.array("chunks")?.items.orEmpty().mapNotNull { it as? Cbor.Map }
            val text = chunks.mapNotNull { it.text("text") }.joinToString("")
            val earliest = chunks.firstOrNull()?.long("from") ?: cursor
            if (!opened) {
                opened = true
                out.trySend(StreamEvent.Item(JSONObject()
                    .put("type", "opened")
                    .put("from", cursor)
                    .put("job", JSONObject()
                        .put("id", jobId)
                        .put("output", JSONObject().put("earliest", earliest)))))
            }
            if (text.isNotEmpty()) {
                out.trySend(StreamEvent.Item(JSONObject()
                    .put("type", "output")
                    .put("chunks", JSONArray().put(JSONObject()
                        .put("text", text)
                        .put("gapBefore", cursor < earliest)))
                    .put("next", next)
                    .put("lossy", false)))
            }
            if (next <= cursor) {
                delay(1_000)
            } else {
                cursor = next
            }
        }
    }

    private suspend fun collectTerminal(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        val request = args.optJSONObject("request") ?: args
        val id = request.optString("id").ifEmpty { request.optString("terminalId") }
            .ifEmpty { args.optString("id") }.ifEmpty { args.optString("terminalId") }
        if (id.isEmpty()) {
            out.trySend(StreamEvent.Failure("invalid_params", "terminal/follow has no terminal id"))
            delay(5_000)
            return
        }
        val attachment = request.optString("attachmentId")
        Log.d(TAG, "terminal/follow id=$id attachment=$attachment")
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "snapshot")
            .put("sequence", 0)
            .put("screen", "")
            .put("info", terminalInfo(id, "running", attachment))))
        terminalFrames.collect { frame ->
            if (frame.optString("terminal") != id) return@collect
            lastFrameAt.set(System.currentTimeMillis())
            when (frame.optString("type")) {
                "output" -> out.trySend(StreamEvent.Item(JSONObject()
                    .put("type", "output")
                    .put("sequence", frame.optLong("sequence"))
                    .put("data", frame.optString("data"))))
                "eof" -> {
                    out.trySend(StreamEvent.Item(JSONObject()
                        .put("type", "state")
                        .put("info", terminalInfo(id, "exited", attachment))))
                    return@collect
                }
            }
        }
    }

    private fun terminalInfo(id: String, state: String, controller: String): JSONObject = JSONObject()
        .put("id", id)
        .put("title", "Shell")
        .put("shell", JSONObject().put("path", "sh"))
        .put("cwd", "")
        .put("cols", 80)
        .put("rows", 24)
        .put("controllerId", controller)
        .put("state", state)

    private companion object {
        const val TAG = "WardMux"
    }
}

/**
 * Ward durable events to the DSH journal rows the transcript reducer reads.
 *
 * Ward has no `assistant-stream` frame; text deltas are durable events. The
 * reducer wants both a live stream and a committed assistant message, so the
 * translator keeps the turn's text, publishes the live chunks as they arrive and
 * emits one durable message at `turn.end`. `seq` is doubled so the synthetic
 * `turn/end` and assistant message have room without colliding with the log's
 * own numbering.
 */
private class FollowTranslator(
    private val sessionId: String,
    private val client: DshClient,
) {
    private var turn = 0
    private var lastTurn = 0
    private var text = StringBuilder()
    private var toolCall = ""
    private var assistantStarted = false
    private val startedTurns = mutableSetOf<Int>()
    private var latestSeq = 0L

    suspend fun translate(event: WardEvent): List<JSONObject> {
        latestSeq = maxOf(latestSeq, event.seq)
        val payload = event.payload
        val time = System.currentTimeMillis()
        val seq = (event.seq * 2).toInt()
        return when (event.kind) {
            "session.prompted" -> {
                turn = (payload.long("turn") ?: (lastTurn + 1L)).toInt()
                lastTurn = turn
                val prompted = payload.text("text").orEmpty()
                client.notePromptStarted(sessionId, prompted)
                text = StringBuilder()
                assistantStarted = false
                client.mux?.publishStatus(sessionId, true)
                listOf(durable(seq, time, "user/message", JSONObject()
                    .put("id", "ward-$seq")
                    .put("content", JSONArray().put(JSONObject()
                        .put("type", "text")
                        .put("text", payload.text("text").orEmpty())))
                    .put("source", JSONObject().put("kind", "user"))))
            }

            "turn.start" -> {
                turn = (payload.long("turn") ?: (lastTurn + 1L)).toInt()
                lastTurn = turn
                startedTurns += turn
                text = StringBuilder()
                listOf(durable(seq, time, "turn/start", JSONObject().put("turn", turn)))
            }

            "text.delta" -> {
                val delta = payload.text("text").orEmpty()
                text.append(delta)
                val frames = mutableListOf<JSONObject>()
                if (!assistantStarted) {
                    assistantStarted = true
                    frames += assistantStream("start", time)
                }
                frames += assistantStream("chunk", time, JSONObject()
                    .put("type", "text-delta")
                    .put("index", 0)
                    .put("text", delta))
                frames
            }

            "turn.end" -> {
                val ended = (payload.long("turn") ?: turn.toLong()).toInt()
                val frames = mutableListOf<JSONObject>()
                if (assistantStarted) {
                    frames += assistantStream("end", time)
                }
                if (text.isNotEmpty()) {
                    frames += durable(seq, time, "assistant/message", JSONObject()
                        .put("turn", ended)
                        .put("step", 0)
                        .put("message", JSONObject().put("content", JSONArray().put(JSONObject()
                            .put("type", "text")
                            .put("text", text.toString())))))
                }
                val reason = payload.text("reason")
                val aborted = payload.bool("aborted") == true
                val data = JSONObject().put("turn", ended)
                if (aborted || !reason.isNullOrEmpty()) {
                    data.put("reason", JSONObject().put("kind", "error").put("error", JSONObject()
                        .put("message", reason ?: "the turn was aborted")))
                }
                frames += durable(seq + 1, time, "turn/end", data)
                client.mux?.publishStatus(sessionId, false)
                text = StringBuilder()
                assistantStarted = false
                frames
            }

            "tool.call" -> {
                val call = payload.long("call") ?: 0L
                toolCall = "call-$call"
                listOf(durable(seq, time, "tool/call", JSONObject()
                    .put("callId", toolCall)
                    .put("name", payload.text("tool").orEmpty().ifEmpty { "tool" })
                    .put("arguments", "")
                    .put("turn", turn)))
            }

            "tool.result" -> {
                val call = payload.long("call") ?: 0L
                val id = if (call > 0) "call-$call" else toolCall
                if (id.isEmpty()) emptyList() else listOf(
                    durable(seq, time, "tool/result", JSONObject()
                        .put("turn", turn)
                        .put("message", JSONObject().put("source", JSONObject().put("callId", id)))
                        .put("content", JSONArray())),
                )
            }

            "approval.requested" -> {
                publishApproval(payload, event.seq)
                emptyList()
            }

            "approval.resolved" -> {
                val call = payload.long("call") ?: 0L
                val epoch = payload.long("epoch") ?: 0L
                val eventId = approvalEventId(sessionId, call, epoch)
                client.forgetApproval(eventId)
                client.mux?.cancelApproval(eventId)
                emptyList()
            }

            "error" -> listOf(durable(seq, time, "agent/error", JSONObject()
                .put("message", payload.text("message").orEmpty().ifEmpty { "the turn failed" })))

            "subagent.updated" -> {
                val child = payload.text("subagent").orEmpty()
                val parent = payload.text("parent").orEmpty()
                val state = payload.text("state").orEmpty()
                if (child.isNotEmpty() && parent.isNotEmpty()) {
                    client.noteSubagent(parent, child)
                    client.mux?.publishStatus(child, state == "running")
                    client.mux?.publishSessionAdded(JSONObject()
                        .put("sessionId", child)
                        .put("parentSessionId", parent)
                        .put("origin", "subagent")
                        .put("updatedAt", System.currentTimeMillis())
                        .put("running", state == "running")
                        .put("agentAvailable", true)
                        .put("blank", false)
                        .put("title", ""))
                }
                emptyList()
            }

            "goal.set" -> {
                client.mux?.publishProjection(sessionId, "goal", JSONObject()
                    .put("goal", JSONObject()
                        .put("id", "ward-goal")
                        .put("revision", 0)
                        .put("objective", payload.text("text").orEmpty())
                        .put("phase", "active"))
                    .put("roundsStarted", 0))
                emptyList()
            }

            "goal.cleared" -> {
                client.mux?.publishProjection(sessionId, "goal", null)
                emptyList()
            }

            "session.compacted" -> {
                val summary = payload.text("summary").orEmpty()
                listOf(durable(seq, time, "compaction/summary", JSONObject()
                    .put("compactionId", "ward-${event.seq}")
                    .put("summary", JSONArray().put(JSONObject()
                        .put("type", "text")
                        .put("text", summary)))))
            }

            else -> emptyList()
        }
    }

    private fun publishApproval(payload: Cbor.Map, seq: Long) {
        val call = payload.long("call") ?: 0L
        val epoch = payload.long("epoch") ?: 0L
        val reason = payload.text("reason").orEmpty()
        val eventId = approvalEventId(sessionId, call, epoch)
        client.rememberApproval(eventId, sessionId, call, epoch)
        val frame = JSONObject()
            .put("type", "waterfall")
            .put("event", "approval/request")
            .put("eventId", eventId)
            .put("agentId", sessionId)
            .put("request", JSONObject()
                .put("toolName", "tool call $call")
                .put("reason", reason))
        client.mux?.publishApproval(frame)
    }

    private fun assistantStream(type: String, time: Long, chunk: JSONObject? = null): JSONObject {
        val attempt = "ward-%s".format(turn)
        val frame = JSONObject()
            .put("type", type)
            .put("attemptId", attempt)
            .put("turn", turn)
            .put("step", 0)
        if (chunk != null) frame.put("chunk", chunk)
        return JSONObject().put("type", "assistant-stream").put("frame", frame)
    }

    private fun durable(seq: Int, time: Long, type: String, data: JSONObject): JSONObject =
        JSONObject().put("type", "event").put("event", JSONObject()
            .put("seq", seq)
            .put("time", time)
            .put("type", type)
            .put("data", data))

}

internal fun approvalEventId(sessionId: String, call: Long, epoch: Long): String =
    "ward:$sessionId:$call:$epoch"
