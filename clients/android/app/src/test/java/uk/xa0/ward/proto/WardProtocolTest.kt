/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream

class WardProtocolTest {

    private fun hex(text: String): ByteArray =
        text.replace(" ", "").replace("\n", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun `the captured welcome decodes as the protocol document says`() {
        val body = hex(
            "a4 61 76 1b 00 00 00 00 00 00 00 01 66 73 65 72 76 65 72 6a 77 61 72 64 2f 30 2e 30 2e 31 " +
                "64 61 75 74 68 a2 63 75 69 64 1b 00 00 00 00 00 00 03 e8 65 73 63 6f 70 65 65 6c 6f 63 61 6c " +
                "66 6c 69 6d 69 74 73 a2 64 62 6f 64 79 1b 00 00 00 00 00 01 00 00 68 69 6e 66 6c 69 67 68 74 " +
                "1b 00 00 00 00 00 00 00 40",
        )
        val welcome = CborCodec.decode(body).asMap()
        assertEquals(1L, welcome.long("v"))
        assertEquals("ward/0.0.1", welcome.text("server"))
        assertEquals(1000L, welcome.map("auth")?.long("uid"))
        assertEquals("local", welcome.map("auth")?.text("scope"))
        assertEquals(65536L, welcome.map("limits")?.long("body"))
        assertEquals(64L, welcome.map("limits")?.long("inflight"))
    }

    @Test
    fun `the captured session create request re-encodes byte for byte`() {
        val body = CborCodec.encode(
            cborMap(
                "id" to 1L.cbor(),
                "method" to "session.create".cbor(),
                "params" to cborMap("cwd" to "/tmp".cbor()),
            ),
        )
        assertArrayEquals(
            hex("a36269641b0000000000000001666d6574686f646e73657373696f6e2e63726561746566706172616d73a163637764642f746d70"),
            body,
        )
    }

    @Test
    fun `a frame round-trips through the 22 byte header`() {
        val encoded = WardFrames.encode(WardFrameType.EVENT, 7L, 42L, byteArrayOf(1, 2, 3))
        val frame = WardFrames.read(ByteArrayInputStream(encoded))
        assertEquals(WardFrameType.EVENT, frame.type)
        assertEquals(7L, frame.sub)
        assertEquals(42L, frame.seq)
        assertArrayEquals(byteArrayOf(1, 2, 3), frame.body)
    }

    @Test
    fun `an empty or oversized body is a protocol violation`() {
        assertThrows(WardProtocolException::class.java) {
            WardFrames.read(ByteArrayInputStream(ByteArray(22)))
        }
        assertThrows(IllegalArgumentException::class.java) {
            WardFrames.encode(WardFrameType.REQUEST, 0, 0, ByteArray(WARD_MAX_BODY + 1))
        }
    }

    @Test
    fun `indefinite CBOR is refused rather than guessed`() {
        assertThrows(CborException::class.java) {
            CborCodec.decode(byteArrayOf(0x5f.toByte(), 0x61, 0x61, 0xff.toByte()))
        }
    }

    @Test
    fun `the codec round-trips every type in the ward subset`() {
        val value = cborMap(
            "text" to "hello".cbor(),
            "uint" to 70000L.cbor(),
            "negative" to Cbor.NInt(-3),
            "bool" to true.cbor(),
            "null" to Cbor.Null,
            "bytes" to Cbor.Bytes(byteArrayOf(0, 1, 2, 255.toByte())),
            "array" to cborArray(1L.cbor(), 2L.cbor()),
        )
        val decoded = CborCodec.decode(CborCodec.encode(value))
        assertEquals("hello", decoded.text("text"))
        assertEquals(70000L, decoded.long("uint"))
        assertEquals(true, decoded.bool("bool"))
        assertTrue(decoded.get("null") === Cbor.Null)
        assertArrayEquals(byteArrayOf(0, 1, 2, 255.toByte()), decoded.bytes("bytes"))
        assertEquals(2, decoded.array("array")?.items?.size)
    }
}
