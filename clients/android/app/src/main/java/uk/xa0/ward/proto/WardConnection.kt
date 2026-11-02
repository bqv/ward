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
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

enum class WardConnectionState { IDLE, CONNECTING, OPEN, CLOSED }

class WardConnectionException(message: String, cause: Throwable? = null) : IOException(message, cause)

/**
 * One ward protocol connection: the frame reader, the request/response ledger and
 * the session subscriptions, over a loopback TCP socket or a plain host:port.
 *
 * The protocol has one socket and many logical streams. Reads are serialised by
 * the single reader coroutine; writes are serialised by [sendLock]. A request is
 * answered by id, an event by subscription, and neither waits for the other.
 */
class WardConnection(
    private val target: String,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
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

    @Volatile private var socket: Socket? = null
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
            val created = openSocket()
            try {
                handshake(created)
            } catch (error: Throwable) {
                runCatching { created.close() }
                _state.value = WardConnectionState.CLOSED
                throw if (error is WardConnectionException) error else
                    WardConnectionException("ward handshake failed: ${error.message}", error)
            }
            socket = created
            _state.value = WardConnectionState.OPEN
            reader = scope.launch { readLoop(created) }
        }
    }

    fun close() {
        closed = true
        reader?.cancel()
        reader = null
        runCatching { socket?.close() }
        socket = null
        failAll(WardConnectionException("ward connection closed"))
        subscriptions.values.forEach { it.close() }
        subscriptions.clear()
        _state.value = WardConnectionState.CLOSED
    }

    fun restart() {
        val stopping = socket
        reader?.cancel()
        reader = null
        runCatching { stopping?.close() }
        socket = null
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
                    WardConnectionException("$method timed out after ${timeoutMs / 1000}s")
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
        val active = socket ?: throw WardConnectionException("ward is not connected")
        sendLock.withLock {
            try {
                active.getOutputStream().writeFrame(type, sub, seq, body)
            } catch (error: IOException) {
                throw WardConnectionException("ward write failed: ${error.message}", error)
            }
        }
    }

    private fun openSocket(): Socket {
        val parsed = parseTarget(target)
        val socket = Socket()
        socket.tcpNoDelay = true
        socket.connect(InetSocketAddress(parsed.first, parsed.second), 15_000)
        socket.soTimeout = 10_000
        return socket
    }

    private fun handshake(active: Socket) {
        val hello = cborMap(
            "v" to WARD_PROTOCOL_VERSION.cbor(),
            "client" to "ward-android/0.1.0".cbor(),
            "caps" to cborArray("events".cbor()),
        )
        active.getOutputStream().writeFrame(WardFrameType.HELLO, 0, 0, hello)
        val welcome = WardFrames.read(active.getInputStream())
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
        active.soTimeout = 0
    }

    private fun readLoop(active: Socket) {
        try {
            val input = active.getInputStream()
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
                val active = socket ?: return
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

    private fun parseTarget(raw: String): Pair<String, Int> {
        val trimmed = raw.trim()
        val withoutScheme = when {
            trimmed.startsWith("tcp://", ignoreCase = true) -> trimmed.substring(6)
            trimmed.startsWith("http://", ignoreCase = true) -> trimmed.substring(7)
            trimmed.startsWith("https://", ignoreCase = true) ->
                throw WardConnectionException("ward speaks its own TLS/QUIC protocol, not HTTPS; use tcp://host:port over adb reverse")
            trimmed.startsWith("quic://", ignoreCase = true) ->
                throw WardConnectionException("QUIC pairing is not wired into this build; use the loopback TCP door")
            trimmed.startsWith("unix://", ignoreCase = true) ->
                throw WardConnectionException("an Android app cannot open a Unix socket; use the loopback TCP door")
            else -> trimmed
        }.trimEnd('/')
        val colon = withoutScheme.lastIndexOf(':')
        if (colon <= 0 || colon == withoutScheme.length - 1) {
            throw WardConnectionException("ward address must be host:port (or tcp://host:port)")
        }
        val host = withoutScheme.substring(0, colon).trim('[', ']')
        val port = withoutScheme.substring(colon + 1).toIntOrNull()
            ?: throw WardConnectionException("ward address has no usable port")
        if (port !in 1..65535) throw WardConnectionException("ward address port is out of range")
        return host to port
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
