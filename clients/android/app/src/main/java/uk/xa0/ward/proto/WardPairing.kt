/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.Signature
import java.security.spec.ECGenParameterSpec
import java.util.Base64

/** What a successful phone enrolment leaves for the config store. */
data class WardPairingResult(
    val quicAddress: String,
    val pin: String,
    val certificatePem: String,
    val privateKeyPem: String,
)

/**
 * The desktop's `ward-pair` flow, on the phone.
 *
 * `device.pair` is a local TCP call; it mints a one-use token and reports the
 * daemon's QUIC endpoint and leaf-certificate pin. The phone then opens a QUIC
 * stream with that pin and no client certificate, which is the one method the
 * daemon allows before authentication, and redeems the token with a PKCS#10
 * request. The issued certificate and the generated key are the durable pair.
 */
object WardPairing {
    suspend fun pair(tcpAddress: String, scope: String = "phone"): WardPairingResult =
        withContext(Dispatchers.IO) {
            val local = WardConnection(tcpAddress)
            val paired = try {
                local.connect()
                WardApi(local).pairDevice(scope)
            } finally {
                local.close()
            }
            val token = paired.text("token").orEmpty()
            val pin = paired.text("fingerprint").orEmpty()
            val endpoint = paired.text("endpoint").orEmpty()
            if (token.isEmpty() || pin.isEmpty() || endpoint.isEmpty()) {
                throw WardConnectionException("device.pair did not return a redeemable QUIC endpoint")
            }

            val keyPair = KeyPairGenerator.getInstance("EC").apply {
                initialize(ECGenParameterSpec("secp256r1"))
            }.generateKeyPair()
            val csr = csrPem(keyPair)
            val privateKey = privateKeyPem(keyPair.getPrivate().encoded)

            val quicAddress = if (endpoint.startsWith("quic://")) endpoint else "quic://$endpoint"
            val quic = WardConnection(quicAddress, quicPin = pin)
            val enrolled = try {
                quic.connect()
                WardApi(quic).enrolDevice(token, csr)
            } finally {
                quic.close()
            }
            val certificate = enrolled.text("cert").orEmpty()
            if (certificate.isEmpty()) {
                throw WardConnectionException("device.enrol did not return a certificate")
            }
            WardPairingResult(
                quicAddress = quicAddress,
                pin = pin,
                certificatePem = certificate,
                privateKeyPem = privateKey,
            )
        }

    internal fun csrPem(keyPair: java.security.KeyPair): String {
        val subject = der(0x30,
            der(0x31,
                der(0x30,
                    der(0x06, oid("2.5.4.3")) +
                        der(0x0c, "ward-android".toByteArray(Charsets.UTF_8)))))
        val info = der(0x30,
            der(0x02, byteArrayOf(0)) +
                subject +
                keyPair.getPublic().encoded +
                byteArrayOf(0xa0.toByte(), 0x00))
        val signature = Signature.getInstance("SHA256withECDSA").run {
            initSign(keyPair.getPrivate())
            update(info)
            sign()
        }
        val algorithm = der(0x30, der(0x06, oid("1.2.840.10045.4.3.2")))
        val request = der(0x30, info + algorithm + der(0x03, byteArrayOf(0) + signature))
        return pem("CERTIFICATE REQUEST", request)
    }

    private fun privateKeyPem(encoded: ByteArray): String = pem("PRIVATE KEY", encoded)

    private fun pem(label: String, der: ByteArray): String {
        val body = Base64.getMimeEncoder(64, "\n".toByteArray()).encodeToString(der)
        return "-----BEGIN $label-----\n$body\n-----END $label-----\n"
    }

    private fun oid(dotted: String): ByteArray {
        val parts = dotted.split('.').map { BigInteger(it) }
        val out = ArrayList<Byte>()
        out += (parts[0].toInt() * 40 + parts[1].toInt()).toByte()
        for (part in parts.drop(2)) {
            val value = part.toLong()
            var shift = 63 - java.lang.Long.numberOfLeadingZeros(value)
            shift -= shift % 7
            while (shift > 0) {
                out += (((value ushr shift) and 0x7f) or 0x80).toByte()
                shift -= 7
            }
            out += (value and 0x7f).toByte()
        }
        return out.toByteArray()
    }

    /** One DER TLV with a definite length. */
    private fun der(tag: Int, content: ByteArray): ByteArray {
        val length = when {
            content.size < 0x80 -> byteArrayOf(content.size.toByte())
            content.size < 0x100 -> byteArrayOf(0x81.toByte(), content.size.toByte())
            content.size < 0x10000 -> byteArrayOf(
                0x82.toByte(),
                (content.size ushr 8).toByte(),
                content.size.toByte(),
            )
            else -> byteArrayOf(
                0x83.toByte(),
                (content.size ushr 16).toByte(),
                (content.size ushr 8).toByte(),
                content.size.toByte(),
            )
        }
        return byteArrayOf(tag.toByte()) + length + content
    }
}
