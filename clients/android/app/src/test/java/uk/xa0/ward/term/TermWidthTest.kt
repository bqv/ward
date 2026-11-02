package uk.xa0.ward.term

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TermWidthTest {

    @Test
    fun `braille dot mask is the code point offset`() {
        assertEquals(0, TermWidth.brailleMask(0x2800)!!)
        assertEquals(1, TermWidth.brailleMask(0x2801)!!)
        assertEquals(255, TermWidth.brailleMask(0x28FF)!!)
    }

    @Test
    fun `braille mask is null outside the block`() {
        assertNull(TermWidth.brailleMask(0x27FF))
        assertNull(TermWidth.brailleMask(0x2900))
    }

    @Test
    fun `braille occupies one cell`() {
        assertEquals(1, TermWidth.of(0x2800))
    }
}
