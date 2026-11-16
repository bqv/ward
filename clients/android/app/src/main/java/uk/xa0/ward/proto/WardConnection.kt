/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

enum class WardConnectionState { IDLE, CONNECTING, OPEN, CLOSED }

open class WardConnectionException(message: String, cause: Throwable? = null) : IOException(message, cause)

class WardCallTimeoutException(val method: String, val waitedMs: Long) :
    WardConnectionException("$method timed out after ${waitedMs / 1000}s")

/**
 * One ward protocol connection: the frame reader, the request/response ledger and
 * the session subscriptions, over a TCP reverse tunnel or a pinned QUIC stream.
 *
 * The protocol has one byte stream and many logical streams. Reads are serialised
 * by the single reader coroutine; writes are serialised by [sendLock]. A request
 * is answered by id, an event by subscription, and neither waits for the other.
 */
class WardConnection(
    private val target: String,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
    private val quicPin: String? = null,
    private val quicCertificatePem: String? = null,
    private val quicPrivateKeyPem: String? = null,
) {
    private data class Pending(
        val method: String,
        val answer: CompletableDeferred<Cbor>,
        val resume: Channel<WardEvent>? = null,
    )

    private val _state = MutableStateFlow(WardConnectionState.IDLE)
    val state: StateFlow<WardConnectionState> = _state.asStateFlow()

    private val nextId = AtomicLong(1)
    private val pending = ConcurrentHashMap<Long, Pending>()
    private val subscriptions = ConcurrentHashMap<Long, Channel<WardEvent>>()
    private val connectLock = Mutex()
    private val sendLock = Mutex()

    @Volatile private var transport: WardTransport? = null
    @Volatile private var reader: Job? = null
    @Volatile private var closed = false

    @Volatile var serverName: String = ""
        private set
    @Volatile var authScope: String = ""
        private set
    @Volatile var maxBody: Long = WARD_MAX_BODY.toLong()
        private set

    fun isOpen(): Boolean = _state.value == WardConnectionState.OPEN

    suspend fun connect() {
        if (closed) throw WardConnectionException("this connection was shut down")
        if (_state.value == WardConnectionState.OPEN) return
        connectLock.withLock {
            if (_state.value == WardConnectionState.OPEN) return
            _state.value = WardConnectionState.CONNECTING
            val created = withContext(Dispatchers.IO) { openTransport() }
            try {
                withContext(Dispatchers.IO) { handshake(created) }
            } catch (error: Throwable) {
                runCatching { created.close() }
                _state.value = WardConnectionState.CLOSED
                throw if (error is WardConnectionException) error else
                    WardConnectionException("ward handshake failed: ${error.message}", error)
            }
            transport = created
            _state.value = WardConnectionState.OPEN
            reader = scope.launch { readLoop(created) }
        }
    }

    fun close() {
        closed = true
        reader?.cancel()
        reader = null
        runCatching { transport?.close() }
        transport = null
        failAll(WardConnectionException("ward connection closed"))
        subscriptions.values.forEach { it.close() }
        subscriptions.clear()
        _state.value = WardConnectionState.CLOSED
    }

    fun restart() {
        val stopping = transport
        reader?.cancel()
        reader = null
        runCatching { stopping?.close() }
        transport = null
        failAll(WardConnectionException("ward connection restarted"))
        subscriptions.values.forEach { it.close() }
        subscriptions.clear()
        _state.value = WardConnectionState.IDLE
    }

    suspend fun call(
        method: String,
        params: Cbor.Map = Cbor.Map(emptyList()),
        timeoutMs: Long = 60_000,
    ): Cbor {
        require(method.isNotBlank()) { "a ward request needs a method" }
        if (closed) throw WardConnectionException("this connection was shut down")
        if (_state.value != WardConnectionState.OPEN) connect()
        val id = nextId.getAndIncrement()
        val answer = CompletableDeferred<Cbor>()
        pending[id] = Pending(method, answer)
        val request = cborMap(
            "id" to id.cbor(),
            "method" to method.cbor(),
            "params" to params,
        )
        try {
            send(WardFrameType.REQUEST, 0, 0, request)
            return withTimeout(timeoutMs) { answer.await() }
        } catch (error: Throwable) {
            pending.remove(id)
            throw when (error) {
                is WardRpcException, is WardConnectionException -> error
                is kotlinx.coroutines.TimeoutCancellationException ->
                    WardCallTimeoutException(method, timeoutMs)
                else -> WardConnectionException("$method failed: ${error.message}", error)
            }
        } finally {
            pending.remove(id)
        }
    }

    suspend fun resume(session: String, from: Long? = 0): WardSubscription {
        if (_state.value != WardConnectionState.OPEN) connect()
        val id = nextId.getAndIncrement()
        val answer = CompletableDeferred<Cbor>()
        val channel = Channel<WardEvent>(Channel.UNLIMITED)
        pending[id] = Pending("session.resume", answer, channel)
        val params = cborMap(
            "id" to session.cbor(),
            *if (from != null) arrayOf("from" to from.cbor()) else emptyArray(),
        )
        try {
            send(WardFrameType.REQUEST, 0, 0, cborMap(
                "id" to id.cbor(),
                "method" to "session.resume".cbor(),
                "params" to params,
            ))
            val result = withTimeout(30_000) { answer.await() }
            val sub = result.long("sub") ?: throw WardProtocolException("session.resume answered no subscription")
            return WardSubscription(this, sub, result, channel)
        } catch (error: Throwable) {
            pending.remove(id)
            channel.close()
            throw if (error is WardRpcException) error else
                WardConnectionException("session.resume failed: ${error.message}", error)
        }
    }

    suspend fun sendControl(body: Cbor.Map) {
        send(WardFrameType.CONTROL, 0, 0, body)
    }

    private suspend fun send(type: Int, sub: Long, seq: Long, body: Cbor) {
        val active = transport ?: throw WardConnectionException("ward is not connected")
        sendLock.withLock {
            try {
                active.output.writeFrame(type, sub, seq, body)
            } catch (error: IOException) {
                throw WardConnectionException("ward write failed: ${error.message}", error)
            }
        }
    }

    private fun openTransport(): WardTransport {
        val (host, port, quic) = parseWardTarget(target)
        return if (quic) {
            QuicWardTransport(host, port, quicPin.orEmpty(), quicCertificatePem, quicPrivateKeyPem)
        } else {
            TcpWardTransport(host, port)
        }
    }

    private fun handshake(active: WardTransport) {
        val hello = cborMap(
            "v" to WARD_PROTOCOL_VERSION.cbor(),
            "client" to "ward-android/0.1.0".cbor(),
            "caps" to cborArray("events".cbor()),
        )
        active.setReadTimeout(10_000)
        active.output.writeFrame(WardFrameType.HELLO, 0, 0, hello)
        val welcome = WardFrames.read(active.input)
        if (welcome.type != WardFrameType.WELCOME) {
            throw WardConnectionException("the daemon answered ${welcome.type}, not a welcome")
        }
        val body = CborCodec.decode(welcome.body).asMap()
        val version = body.long("v") ?: 0
        if (version != WARD_PROTOCOL_VERSION.toLong()) {
            throw WardConnectionException("ward speaks version $version; this client speaks $WARD_PROTOCOL_VERSION")
        }
        serverName = body.text("server").orEmpty()
        authScope = body.map("auth")?.text("scope").orEmpty()
        maxBody = body.map("limits")?.long("body") ?: WARD_MAX_BODY.toLong()
        active.setReadTimeout(0)
    }

    private fun readLoop(active: WardTransport) {
        try {
            val input = active.input
            while (!closed) {
                val frame = WardFrames.read(input)
                when (frame.type) {
                    WardFrameType.RESPONSE -> handleResponse(frame)
                    WardFrameType.EVENT -> handleEvent(frame)
                    WardFrameType.CONTROL -> handleControl(frame)
                    WardFrameType.HELLO, WardFrameType.WELCOME ->
                        throw WardProtocolException("unexpected frame type ${frame.type}")
                    else -> throw WardProtocolException("reserved frame type ${frame.type}")
                }
            }
        } catch (error: Throwable) {
            if (!closed && error !is java.io.EOFException || !closed && _state.value == WardConnectionState.OPEN) {
                _state.value = WardConnectionState.CLOSED
                failAll(WardConnectionException("ward connection lost: ${error.message}", error))
                subscriptions.values.forEach { it.close() }
            }
        }
    }

    private fun handleResponse(frame: WardFrame) {
        val body = CborCodec.decode(frame.body).asMap()
        val id = body.long("id") ?: return
        val waiting = pending.remove(id) ?: return
        val ok = body.bool("ok") ?: false
        if (!ok) {
            val error = body.map("error")
            waiting.answer.completeExceptionally(
                WardRpcException(
                    code = error?.long("code") ?: 0,
                    name = error?.text("name").orEmpty().ifEmpty { "internal" },
                    message = error?.text("message").orEmpty().ifEmpty { "the daemon refused $id" },
                ),
            )
            waiting.resume?.close()
            return
        }
        val value = body.get("result") ?: Cbor.Null
        if (waiting.method == "session.resume" && waiting.resume != null) {
            val sub = (value as? Cbor.Map)?.long("sub")
            if (sub != null) subscriptions[sub] = waiting.resume
        }
        waiting.answer.complete(value)
    }

    private fun handleEvent(frame: WardFrame) {
        val body = CborCodec.decode(frame.body).asMap()
        val kind = body.text("kind").orEmpty()
        val payload = body.map("payload") ?: Cbor.Map(emptyList())
        val event = WardEvent(frame.sub, frame.seq, kind, payload)
        subscriptions[frame.sub]?.trySend(event)
    }

    private fun handleControl(frame: WardFrame) {
        val body = CborCodec.decode(frame.body).asMap()
        body.long("replay")?.let { sub ->
            body.long("from")?.let { from ->
                val active = transport ?: return
                scope.launch {
                    runCatching {
                        sendControl(cborMap("replay" to sub.cbor(), "from" to from.cbor()))
                    }
                }
            }
        }
    }

    private fun failAll(error: Throwable) {
        pending.values.forEach { it.answer.completeExceptionally(error) }
        pending.clear()
    }

    internal fun closeSubscription(sub: Long) {
        subscriptions.remove(sub)?.close()
    }


}

class WardSubscription internal constructor(
    private val connection: WardConnection,
    val sub: Long,
    val result: Cbor,
    private val channel: Channel<WardEvent>,
) {
    val session: WardSession? get() = result.map("session")?.let(WardSessionParser::of)
    val lastSeq: Long get() = result.long("last_seq") ?: 0L
    val events: Flow<WardEvent> = channel.receiveAsFlow()

    fun close() {
        connection.closeSubscription(sub)
        channel.close()
    }
}
