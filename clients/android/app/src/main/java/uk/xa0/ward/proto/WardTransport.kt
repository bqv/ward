/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.security.KeyFactory
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.spec.PKCS8EncodedKeySpec
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.time.Duration
import java.util.Base64
import tech.kwik.core.QuicClientConnection
import tech.kwik.core.QuicStream

/** The byte stream one ward connection runs over: TCP for the reverse tunnel, QUIC for a paired device. */
internal interface WardTransport {
    val input: InputStream
    val output: OutputStream
    fun setReadTimeout(ms: Int)
    fun close()
}

internal class TcpWardTransport(host: String, port: Int) : WardTransport {
    private val socket = Socket().apply {
        tcpNoDelay = true
        connect(InetSocketAddress(host, port), 15_000)
        soTimeout = 10_000
    }

    override val input: InputStream get() = socket.getInputStream()
    override val output: OutputStream get() = socket.getOutputStream()

    override fun setReadTimeout(ms: Int) {
        runCatching { socket.soTimeout = ms }
    }

    override fun close() {
        runCatching { socket.close() }
    }
}

/**
 * Kwik is a pure-JVM QUIC stack, chosen because Cronet only exposes HTTP
 * requests; ward needs one raw bidirectional stream with ALPN `ward`.
 *
 * The daemon's certificate is issued by its local CA, so verification is a
 * SHA-256 pin of the leaf certificate that `device.pair` handed over, checked
 * after the TLS handshake and before any application bytes are written. The
 * builder's platform trust check is disabled for exactly that reason: the pin
 * is the trust decision, not the system store.
 */
internal class QuicWardTransport(
    host: String,
    port: Int,
    private val pinSha256: String,
    certificatePem: String?,
    privateKeyPem: String?,
) : WardTransport {
    private val connection: QuicClientConnection
    private val stream: QuicStream

    init {
        val builder = QuicClientConnection.newBuilder()
            .host(host)
            .port(port)
            .applicationProtocol("ward")
            .connectTimeout(Duration.ofSeconds(15))
            .maxIdleTimeout(Duration.ofMinutes(30))
            .noServerCertificateCheck()
        val certificate = certificatePem?.let(::parseCertificate)
        val key = privateKeyPem?.let(::parsePrivateKey)
        if (certificate != null && key != null) {
            builder.clientCertificate(certificate)
            builder.clientCertificateKey(key)
        }
        connection = builder.build()
        try {
            connection.connect()
            connection.keepAlive(15)
            verifyPin(connection.serverCertificateChain, pinSha256)
            stream = connection.createStream(true)
        } catch (error: Throwable) {
            runCatching { connection.close() }
            throw if (error is WardConnectionException) error else
                WardConnectionException("QUIC handshake failed: ${error.message}", error)
        }
    }

    override val input: InputStream get() = stream.inputStream
    override val output: OutputStream get() = stream.outputStream

    override fun setReadTimeout(ms: Int) = Unit

    override fun close() {
        runCatching { stream.resetStream(0) }
        runCatching { connection.close() }
    }

    private fun verifyPin(chain: List<X509Certificate>, expected: String) {
        val leaf = chain.firstOrNull()
            ?: throw WardConnectionException("the ward daemon offered no certificate")
        val digest = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
        val actual = digest.joinToString("") { "%02x".format(it) }
        val wanted = expected.lowercase().replace(":", "").replace(" ", "")
        if (wanted.isEmpty()) {
            throw WardConnectionException("this QUIC door has no pinned fingerprint; pair the device first")
        }
        if (actual != wanted) {
            throw WardConnectionException("the ward daemon's certificate does not match the paired fingerprint")
        }
    }

    private fun parseCertificate(pem: String): X509Certificate {
        val bytes = pemToDer(pem, "CERTIFICATE")
        return CertificateFactory.getInstance("X.509")
            .generateCertificate(ByteArrayInputStream(bytes)) as X509Certificate
    }

    private fun parsePrivateKey(pem: String): PrivateKey {
        val bytes = pemToDer(pem, "PRIVATE KEY")
        return KeyFactory.getInstance("EC").generatePrivate(PKCS8EncodedKeySpec(bytes))
    }

    private fun pemToDer(pem: String, label: String): ByteArray {
        val body = pem
            .replace("-----BEGIN $label-----", "")
            .replace("-----END $label-----", "")
            .filterNot { it.isWhitespace() }
        return Base64.getDecoder().decode(body)
    }
}

internal fun parseWardTarget(raw: String): Triple<String, Int, Boolean> {
    val trimmed = raw.trim()
    val quic = trimmed.startsWith("quic://", ignoreCase = true)
    val withoutScheme = when {
        trimmed.startsWith("quic://", ignoreCase = true) -> trimmed.substring(7)
        trimmed.startsWith("tcp://", ignoreCase = true) -> trimmed.substring(6)
        trimmed.startsWith("http://", ignoreCase = true) -> trimmed.substring(7)
        trimmed.startsWith("https://", ignoreCase = true) ->
            throw WardConnectionException("ward does not speak HTTPS; use tcp:// or quic://")
        trimmed.startsWith("unix://", ignoreCase = true) ->
            throw WardConnectionException("the Android client uses the TCP or QUIC door, not a Unix socket")
        else -> trimmed
    }.trimEnd('/')
    val colon = withoutScheme.lastIndexOf(':')
    if (colon <= 0 || colon == withoutScheme.length - 1) {
        throw WardConnectionException("ward address must be host:port (or tcp://host:port / quic://host:port)")
    }
    val host = withoutScheme.substring(0, colon).trim('[', ']')
    val port = withoutScheme.substring(colon + 1).toIntOrNull()
        ?: throw WardConnectionException("ward address has no usable port")
    if (port !in 1..65535) throw WardConnectionException("ward address port is out of range")
    return Triple(host, port, quic)
}
