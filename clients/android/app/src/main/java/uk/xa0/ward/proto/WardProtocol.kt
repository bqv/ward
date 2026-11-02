/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream

const val WARD_PROTOCOL_VERSION = 1
const val WARD_FRAME_HEADER = 22
const val WARD_MAX_BODY = 65536

object WardFrameType {
    const val HELLO = 1
    const val WELCOME = 2
    const val REQUEST = 3
    const val RESPONSE = 4
    const val EVENT = 5
    const val CONTROL = 6
}

data class WardFrame(
    val type: Int,
    val sub: Long,
    val seq: Long,
    val body: ByteArray,
)

object WardFrames {
    fun encode(type: Int, sub: Long, seq: Long, body: ByteArray): ByteArray {
        require(body.isNotEmpty() && body.size <= WARD_MAX_BODY) { "frame body is outside the ward limit" }
        val out = ByteArray(WARD_FRAME_HEADER + body.size)
        val len = body.size
        out[0] = len.toByte()
        out[1] = (len ushr 8).toByte()
        out[2] = (len ushr 16).toByte()
        out[3] = (len ushr 24).toByte()
        out[4] = type.toByte()
        out[5] = (type ushr 8).toByte()
        for (i in 0 until 8) {
            out[6 + i] = (sub ushr (8 * i)).toByte()
            out[14 + i] = (seq ushr (8 * i)).toByte()
        }
        body.copyInto(out, WARD_FRAME_HEADER)
        return out
    }

    fun read(input: InputStream): WardFrame {
        val header = readExactly(input, WARD_FRAME_HEADER)
        val len = (header[0].toInt() and 0xff) or
            ((header[1].toInt() and 0xff) shl 8) or
            ((header[2].toInt() and 0xff) shl 16) or
            ((header[3].toInt() and 0xff) shl 24)
        if (len <= 0 || len > WARD_MAX_BODY) throw WardProtocolException("invalid frame body length $len")
        val type = (header[4].toInt() and 0xff) or ((header[5].toInt() and 0xff) shl 8)
        if (type !in 1..6) throw WardProtocolException("reserved frame type $type")
        var sub = 0L
        var seq = 0L
        for (i in 0 until 8) {
            sub = sub or ((header[6 + i].toLong() and 0xff) shl (8 * i))
            seq = seq or ((header[14 + i].toLong() and 0xff) shl (8 * i))
        }
        return WardFrame(type, sub, seq, readExactly(input, len))
    }

    private fun readExactly(input: InputStream, count: Int): ByteArray {
        val out = ByteArray(count)
        var at = 0
        while (at < count) {
            val read = input.read(out, at, count - at)
            if (read < 0) throw EOFException("ward connection closed")
            if (read == 0) continue
            at += read
        }
        return out
    }
}

class WardProtocolException(message: String) : Exception(message)

class WardRpcException(
    val code: Long,
    val name: String,
    override val message: String,
) : Exception(message)

data class WardEvent(val sub: Long, val seq: Long, val kind: String, val payload: Cbor.Map)

class WardSession(val id: String, val cwd: String, val model: String,
                  val created: Long, val lastSeq: Long, val name: String?,
                  val archived: Boolean, val pinned: Boolean, val updatedMs: Long?) {
    fun toCbor(): Cbor.Map = cborMap(
        "id" to id.cbor(),
        "cwd" to cwd.cbor(),
        "model" to model.cbor(),
        "created" to created.cbor(),
        "last_seq" to lastSeq.cbor(),
        *buildList {
            name?.let { add("name" to it.cbor()) }
            add("archived" to archived.cbor())
            add("pinned" to pinned.cbor())
            updatedMs?.let { add("updated_ms" to it.cbor()) }
        }.toTypedArray(),
    )
}

object WardSessionParser {
    fun of(value: Cbor): WardSession {
        val map = value.asMap()
        return WardSession(
            id = map.text("id").orEmpty(),
            cwd = map.text("cwd").orEmpty(),
            model = map.text("model").orEmpty(),
            created = map.long("created") ?: 0L,
            lastSeq = map.long("last_seq") ?: 0L,
            name = map.text("name"),
            archived = map.bool("archived") ?: false,
            pinned = map.bool("pinned") ?: false,
            updatedMs = map.long("updated_ms"),
        )
    }

    fun list(value: Cbor): List<WardSession> =
        value.get("sessions")?.asArray()?.items?.map { of(it) }.orEmpty()
}

fun OutputStream.writeFrame(type: Int, sub: Long, seq: Long, body: Cbor) {
    write(WardFrames.encode(type, sub, seq, CborCodec.encode(body)))
    flush()
}
