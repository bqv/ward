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
    private val nextSynthetic = AtomicLong(1)

    init { supervise() }

    fun lastHostFrameAt(): Long = lastFrameAt.get()

    /** Feed one translated ward approval to the `$events` collectors. */
    internal fun publishApproval(frame: JSONObject) {
        approvalFrames.tryEmit(frame)
    }

    /** Tell the attention centre that a ward approval is settled. */
    internal fun cancelApproval(eventId: String) {
        onWaterfallCancel?.invoke(eventId)
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
                        "job/list", "job/follow" -> collectJobs(args, producer)
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
        try {
            forwardedEvents.collect { event -> out.trySend(StreamEvent.Item(event)) }
        } finally {
            approvals.cancel()
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
            out.trySend(StreamEvent.Item(JSONObject()
                .put("type", "snapshot")
                .put("header", JSONObject()
                    .put("id", header.id)
                    .put("cwd", header.cwd)
                    .put("createdAt", header.created))
                .put("records", JSONArray())
                .put("cursor", 0)
                .put("hasMore", false)))
        }
        val translator = FollowTranslator(sessionId, client)
        try {
            subscription.events.collect { event ->
                lastFrameAt.set(System.currentTimeMillis())
                if (event.kind == "replay.end") return@collect
                if (event.kind == "terminal.output") return@collect
                translator.translate(event).forEach { value ->
                    out.trySend(StreamEvent.Item(value))
                }
            }
        } finally {
            subscription.close()
        }
    }

    private suspend fun collectControl(out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>) {
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "baseline")
            .put("value", JSONObject()
                .put("queues", JSONObject())
                .put("projections", JSONObject()))))
        delay(Long.MAX_VALUE)
    }

    private suspend fun collectWorkspaces(out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>) {
        val all = runCatching { client.listSessions() }.getOrNull()
        val archived = JSONArray()
        val pinned = JSONArray()
        all?.optJSONArray("items")?.let { items ->
            for (i in 0 until items.length()) {
                val row = items.optJSONObject(i) ?: continue
                val id = row.optString("sessionId")
                if (row.optBoolean("archived")) archived.put(id)
                if (row.optBoolean("pinned")) pinned.put(id)
            }
        }
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "baseline")
            .put("value", JSONObject()
                .put("items", JSONArray())
                .put("archivedSessionIds", archived)
                .put("pinnedSessionIds", pinned))))
        delay(Long.MAX_VALUE)
    }

    private suspend fun collectJobs(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        val jobs = client.rpcRaw("job/list", args)
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "jobs")
            .put("jobs", (jobs as? JSONObject)?.optJSONArray("jobs") ?: JSONArray())))
        delay(Long.MAX_VALUE)
    }

    private suspend fun collectTerminal(
        args: JSONObject,
        out: kotlinx.coroutines.channels.ProducerScope<StreamEvent>,
    ) {
        out.trySend(StreamEvent.Item(JSONObject()
            .put("type", "baseline")
            .put("value", JSONObject())))
        delay(Long.MAX_VALUE)
    }

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
                val eventId = approvalId(sessionId, call, epoch)
                client.forgetApproval(eventId)
                client.mux?.cancelApproval(eventId)
                emptyList()
            }

            "error" -> listOf(durable(seq, time, "agent/error", JSONObject()
                .put("message", payload.text("message").orEmpty().ifEmpty { "the turn failed" })))

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
        val eventId = approvalId(sessionId, call, epoch)
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

    private fun approvalId(sessionId: String, call: Long, epoch: Long): String =
        "ward:$sessionId:$call:$epoch"
}
