/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import java.io.ByteArrayOutputStream

/**
 * The CBOR subset ward speaks: definite-length maps, arrays, text, byte strings,
 * unsigned and negative integers, booleans and null. No tags, no floating point
 * and no indefinite forms, which is exactly what `docs/protocol.md` freezes.
 */
sealed interface Cbor {
    data class UInt(val value: Long) : Cbor
    data class NInt(val value: Long) : Cbor
    data class Text(val value: String) : Cbor
    data class Bytes(val value: ByteArray) : Cbor
    data class Array(val items: List<Cbor>) : Cbor
    data class Map(val entries: List<Pair<String, Cbor>>) : Cbor
    data class Bool(val value: Boolean) : Cbor
    data object Null : Cbor

    fun asMap(): Cbor.Map = this as? Cbor.Map ?: throw CborException("expected a map")
    fun asArray(): Cbor.Array = this as? Cbor.Array ?: throw CborException("expected an array")
    fun asText(): String = (this as? Text)?.value ?: throw CborException("expected text")
    fun asLong(): Long = when (this) {
        is UInt -> value
        is NInt -> value
        else -> throw CborException("expected an integer")
    }
    fun asBoolean(): Boolean = (this as? Bool)?.value ?: throw CborException("expected a bool")
    fun asBytes(): ByteArray = (this as? Bytes)?.value ?: throw CborException("expected bytes")

    fun get(key: String): Cbor? = (this as? Cbor.Map)?.entries?.firstOrNull { it.first == key }?.second
    fun text(key: String): String? = (get(key) as? Text)?.value
    fun long(key: String): Long? = (get(key) as? UInt)?.value ?: (get(key) as? NInt)?.value
    fun bool(key: String): Boolean? = (get(key) as? Bool)?.value
    fun map(key: String): Cbor.Map? = get(key) as? Cbor.Map
    fun array(key: String): Cbor.Array? = get(key) as? Cbor.Array
    fun bytes(key: String): ByteArray? = (get(key) as? Bytes)?.value
}

class CborException(message: String) : Exception(message)

object CborCodec {
    private const val MAX_DEPTH = 64

    fun encode(value: Cbor): ByteArray {
        val out = ByteArrayOutputStream()
        write(value, out, 0)
        return out.toByteArray()
    }

    fun decode(bytes: ByteArray): Cbor = Reader(bytes).read(0)

    fun canonical(value: Cbor): ByteArray = encode(value)

    private fun write(value: Cbor, out: ByteArrayOutputStream, depth: Int) {
        require(depth <= MAX_DEPTH) { "CBOR nesting is too deep" }
        when (value) {
            is Cbor.UInt -> fixed64(out, 0, value.value)
            is Cbor.NInt -> fixed64(out, 1, -1L - value.value)
            is Cbor.Bytes -> {
                head(out, 2, value.value.size.toLong())
                out.write(value.value)
            }
            is Cbor.Text -> {
                val bytes = value.value.toByteArray(Charsets.UTF_8)
                head(out, 3, bytes.size.toLong())
                out.write(bytes)
            }
            is Cbor.Array -> {
                head(out, 4, value.items.size.toLong())
                value.items.forEach { write(it, out, depth + 1) }
            }
            is Cbor.Map -> {
                head(out, 5, value.entries.size.toLong())
                value.entries.forEach { (key, item) ->
                    write(Cbor.Text(key), out, depth + 1)
                    write(item, out, depth + 1)
                }
            }
            is Cbor.Bool -> out.write(if (value.value) 0xf5 else 0xf4)
            Cbor.Null -> out.write(0xf6)
        }
    }

    private fun fixed64(out: ByteArrayOutputStream, major: Int, value: Long) {
        out.write((major shl 5) or 27)
        for (shift in intArrayOf(56, 48, 40, 32, 24, 16, 8, 0)) {
            out.write((value ushr shift).toInt() and 0xff)
        }
    }

    private fun head(out: ByteArrayOutputStream, major: Int, value: Long) {
        require(value >= 0) { "negative CBOR head length" }
        val prefix = major shl 5
        when {
            value < 24 -> out.write(prefix or value.toInt())
            value < 0x100 -> {
                out.write(prefix or 24)
                out.write(value.toInt())
            }
            value < 0x10000 -> {
                out.write(prefix or 25)
                out.write((value ushr 8).toInt() and 0xff)
                out.write(value.toInt() and 0xff)
            }
            value < 0x100000000L -> {
                out.write(prefix or 26)
                for (shift in intArrayOf(24, 16, 8, 0)) out.write((value ushr shift).toInt() and 0xff)
            }
            else -> {
                out.write(prefix or 27)
                for (shift in intArrayOf(56, 48, 40, 32, 24, 16, 8, 0)) {
                    out.write((value ushr shift).toInt() and 0xff)
                }
            }
        }
    }

    private class Reader(val bytes: ByteArray) {
        private var at = 0

        fun read(depth: Int): Cbor {
            if (depth > MAX_DEPTH) throw CborException("CBOR nesting is too deep")
            if (at >= bytes.size) throw CborException("truncated CBOR")
            val initial = bytes[at++].toInt() and 0xff
            val major = initial ushr 5
            val minor = initial and 0x1f
            return when (major) {
                0 -> Cbor.UInt(readLength(minor))
                1 -> Cbor.NInt(readLength(minor))
                2 -> Cbor.Bytes(take(readLength(minor).toInt()))
                3 -> {
                    val raw = take(readLength(minor).toInt())
                    Cbor.Text(String(raw, Charsets.UTF_8))
                }
                4 -> {
                    val count = length(minor)
                    if (count > bytes.size - at) throw CborException("array longer than input")
                    Cbor.Array((0 until count).map { read(depth + 1) })
                }
                5 -> {
                    val count = length(minor)
                    if (count > bytes.size - at) throw CborException("map longer than input")
                    Cbor.Map((0 until count).map {
                        val key = read(depth + 1).asText()
                        key to read(depth + 1)
                    })
                }
                7 -> when (minor) {
                    20 -> Cbor.Bool(false)
                    21 -> Cbor.Bool(true)
                    22 -> Cbor.Null
                    else -> throw CborException("unsupported simple value $minor")
                }
                else -> throw CborException("unsupported major type $major")
            }
        }

        private fun length(minor: Int): Int {
            val value = readLength(minor)
            if (value > Int.MAX_VALUE) throw CborException("CBOR length is too large")
            return value.toInt()
        }

        private fun readLength(minor: Int): Long = when {
            minor < 24 -> minor.toLong()
            minor == 24 -> take(1).let { (it[0].toLong() and 0xff) }
            minor == 25 -> take(2).let { ((it[0].toLong() and 0xff) shl 8) or (it[1].toLong() and 0xff) }
            minor == 26 -> take(4).fold(0L) { acc, b -> (acc shl 8) or (b.toLong() and 0xff) }
            minor == 27 -> take(8).fold(0L) { acc, b -> (acc shl 8) or (b.toLong() and 0xff) }
            else -> throw CborException("indefinite CBOR is not part of the ward wire")
        }

        private fun take(count: Int): ByteArray {
            if (count < 0 || count > bytes.size - at) throw CborException("truncated CBOR")
            val out = bytes.copyOfRange(at, at + count)
            at += count
            return out
        }
    }
}

fun cborMap(vararg entries: Pair<String, Cbor>): Cbor.Map =
    Cbor.Map(entries.toList())

fun cborArray(vararg items: Cbor): Cbor.Array = Cbor.Array(items.toList())

fun String.cbor(): Cbor.Text = Cbor.Text(this)
fun Long.cbor(): Cbor.UInt = Cbor.UInt(this)
fun Int.cbor(): Cbor.UInt = Cbor.UInt(this.toLong())
fun Boolean.cbor(): Cbor.Bool = Cbor.Bool(this)
