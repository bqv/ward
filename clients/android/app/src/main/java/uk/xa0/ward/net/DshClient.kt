/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.net

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import org.json.JSONObject
import uk.xa0.ward.data.DshConfig
import uk.xa0.ward.proto.Cbor
import uk.xa0.ward.proto.WardConnection
import uk.xa0.ward.proto.WardConnectionException
import uk.xa0.ward.proto.WardRpcException
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap

/** Raised when the host rejects a request because the session is gone. */
class DshAuthException(message: String = "Not authenticated") : Exception(message)

/** Raised when the host answered a gateway call with a structured error. */
class DshRpcException(
    val code: String,
    override val message: String,
    val details: JSONObject?,
) : Exception(message)

fun gatewayAnswerText(body: String): String {
    val trimmed = body.trim()
    val markup = trimmed.isEmpty() ||
        trimmed.startsWith("<") ||
        trimmed.contains("<html", ignoreCase = true)
    return if (markup) "the host did not answer; its web server replied instead" else trimmed.take(200)
}

class DshUnreachableException(message: String = "Could not reach the host") : Exception(message)

class DshCertificateException(
    message: String,
    val host: String,
    val chain: List<X509Certificate>,
    val accepted: AcceptedCert?,
) : Exception(message)

sealed interface ProbeResult {
    data class Ok(val serverName: String) : ProbeResult
    data object NeedsLogin : ProbeResult
    data class Failed(val message: String) : ProbeResult
    data class Unreachable(val message: String) : ProbeResult
    data class CertificateUnknown(val host: String, val chain: List<X509Certificate>) : ProbeResult
    data class CertificateChanged(
        val host: String,
        val chain: List<X509Certificate>,
        val accepted: AcceptedCert,
    ) : ProbeResult
}

/**
 * The app's one door to ward.
 *
 * Ward is a binary protocol over a loopback TCP socket, not DSH's HTTP + WebSocket
 * surface. The class keeps the surface the screens already speak so the port is a
 * transport change rather than a rewrite of every screen, and every method below
 * is answered by [uk.xa0.ward.proto.WardApi] or by the compatibility bridge in
 * [WardBridge].
 */
class DshClient(context: Context) {
    private val appContext = context.applicationContext

    val cookieJar = PersistentCookieJar(context)
    val trust = CertTrust()

    @Volatile var config: DshConfig = DshConfig()
        private set
    @Volatile var onTrustChanged: (() -> Unit)? = null
    @Volatile var onRouteChanged: (() -> Unit)? = null
    @Volatile var onSignIn: (() -> Unit)? = null
    @Volatile var onStreamAuthFailure: (() -> Unit)? = null

    @Volatile private var connection: WardConnection? = null
    @Volatile private var bridge: WardBridge? = null
    @Volatile private var muxInstance: RemoteMux? = null
    @Volatile var mux: RemoteMux? = null

    private val http = OkHttpClient.Builder().build()

    fun setAcceptedCerts(entries: List<AcceptedCert>) {
        trust.replace(entries)
        onTrustChanged?.invoke()
    }

    fun applyConfig(next: DshConfig) {
        val previous = config
        config = next
        if (next.baseUrl != previous.baseUrl) {
            muxInstance?.shutdown()
            muxInstance = null
            mux = null
            connection?.close()
            connection = null
            bridge = null
        }
        onRouteChanged?.invoke()
    }

    fun endpointFor(raw: String): HttpUrl? = displayUrlOf(raw)

    fun baseUrl(): HttpUrl? = displayUrlOf(config.baseUrl)

    fun isLanRoute(): Boolean = isPrivateLanHost(hostOf(config.baseUrl))

    fun lanDoorFault(): DoorFault? = null

    fun hasLanCandidate(): Boolean = isPrivateLanHost(hostOf(config.lanUrl))

    fun mux(): RemoteMux {
        val active = muxInstance
        if (active != null) return active
        val created = RemoteMux(::ensureConnection, { config.baseUrl }, this)
        muxInstance = created
        mux = created
        return created
    }

    private suspend fun ensureConnection(): WardConnection {
        val target = config.baseUrl.trim()
        if (target.isEmpty()) throw DshUnreachableException("No ward address is configured")
        val current = connection ?: WardConnection(target).also {
            connection = it
            bridge = WardBridge(it)
        }
        current.connect()
        return current
    }

    suspend fun refreshRoute() {
        runCatching { ensureConnection() }
    }

    suspend fun dialRoute(forgive: Boolean = false): Boolean {
        val wasOpen = connection?.isOpen() == true
        ensureConnection()
        return !wasOpen
    }

    suspend fun recheckRoute(): Boolean = false

    fun requestRouteRefresh() {
        onRouteChanged?.invoke()
    }

    fun resetTransport(): TransportReset {
        val had = muxInstance != null
        val pooled = runCatching { http.connectionPool.connectionCount() }.getOrDefault(0)
        val calls = http.dispatcher.queuedCalls().size + http.dispatcher.runningCalls().size
        muxInstance?.shutdown()
        muxInstance = null
        mux = null
        connection?.restart()
        connection = null
        bridge = null
        runCatching { http.connectionPool.evictAll() }
        runCatching { http.dispatcher.cancelAll() }
        val reset = TransportReset(
            muxClosed = had,
            doorFaultsForgotten = 0,
            cachedWinnerForgotten = had,
            pooledConnections = pooled,
            callsCancelled = calls,
            routeJobsCancelled = 0,
        )
        Log.d("DshRoute", reset.summary())
        return reset
    }

    fun installManualCookie() = Unit

    suspend fun probe(): ProbeResult = withContext(Dispatchers.IO) {
        try {
            val conn = ensureConnection()
            ProbeResult.Ok(conn.serverName.ifEmpty { "ward" })
        } catch (error: Throwable) {
            ProbeResult.Unreachable(error.message ?: "Could not reach the ward daemon")
        }
    }

    suspend fun login(username: String, password: String): Result<Unit> =
        runCatching { ensureConnection() }.map { }

    suspend fun ensureAuthenticated(): Result<Unit> =
        runCatching { ensureConnection() }.map { }

    suspend fun rpc(method: String, args: JSONObject = JSONObject()): JSONObject =
        (rpcRaw(method, args) as? JSONObject) ?: JSONObject()

    suspend fun rpcRaw(method: String, args: JSONObject = JSONObject()): Any? =
        withContext(Dispatchers.IO) {
            try {
                ensureConnection()
                val active = bridge ?: WardBridge(ensureConnection()).also { bridge = it }
                active.call(method, args)
            } catch (error: WardRpcException) {
                throw DshRpcException(error.name, error.message, JSONObject().put("wardCode", error.code))
            } catch (error: WardConnectionException) {
                throw DshUnreachableException(error.message ?: "Ward connection failed")
            }
        }

    suspend fun rpcBytes(method: String, args: JSONObject = JSONObject()): ByteArray =
        withContext(Dispatchers.IO) {
            ensureConnection()
            val active = bridge ?: WardBridge(ensureConnection()).also { bridge = it }
            active.callBytes(method, args)
        }

    fun httpClient(): OkHttpClient = http

    suspend fun listSessions(): JSONObject = rpc("session/list", JSONObject().put("_request", JSONObject()))

    /** The event id a ward approval becomes, so `$events/result` can answer it. */
    fun rememberApproval(eventId: String, session: String, call: Long, epoch: Long) {
        bridge?.rememberApproval(eventId, session, call, epoch)
    }

    fun forgetApproval(eventId: String) {
        bridge?.forgetApproval(eventId)
    }

    /** Remember the parent a ward subagent announced, for the next session list. */
    fun noteSubagent(parent: String, child: String) {
        bridge?.noteSubagent(parent, child)
    }

    fun notePromptStarted(session: String, text: String) {
        bridge?.notePromptStarted(session, text)
    }

    suspend fun hostQueueInbox(session: String): JSONObject? = bridge?.hostQueueInbox(session)

    companion object {
        private const val TAG = "WardRoute"

        private fun displayUrlOf(raw: String): HttpUrl? {
            val trimmed = raw.trim().trimEnd('/')
            if (trimmed.isEmpty()) return null
            val http = when {
                trimmed.startsWith("tcp://", ignoreCase = true) -> "http://" + trimmed.substring(6)
                trimmed.startsWith("http://", ignoreCase = true) -> trimmed
                trimmed.startsWith("https://", ignoreCase = true) -> trimmed
                else -> "http://$trimmed"
            }
            return http.toHttpUrlOrNull()
        }
    }
}
