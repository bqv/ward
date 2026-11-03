package uk.xa0.ward.term

import org.junit.Assert.assertEquals
import org.junit.Test

class TerminalSelectionTest {

    private fun terminal(): TerminalEmulator {
        val emulator = TerminalEmulator(20, 4, 0)
        emulator.append("hello world\r\nsecond")
        return emulator
    }

    @Test
    fun `a one row range copies its columns inclusively`() {
        val emulator = terminal()

        assertEquals("hello", emulator.selectedText(TerminalSelection(0, 0, 0, 4)))
    }

    @Test
    fun `a multi row range runs to the end of the first row and the focus column of the last`() {
        val emulator = terminal()

        assertEquals("world\nsecon", emulator.selectedText(TerminalSelection(0, 6, 1, 4)))
    }

    @Test
    fun `a reverse drag normalises to the same text`() {
        val emulator = terminal()
        val drag = TerminalSelection(1, 4, 0, 6)

        assertEquals(0, drag.startRow)
        assertEquals(6, drag.startCol)
        assertEquals(1, drag.endRow)
        assertEquals(4, drag.endCol)
        assertEquals(
            emulator.selectedText(TerminalSelection(0, 6, 1, 4)),
            emulator.selectedText(drag),
        )
        assertEquals("world\nsecon", emulator.selectedText(drag))
    }

    @Test
    fun `trailing spaces are trimmed from every selected row`() {
        val emulator = TerminalEmulator(20, 4, 0)
        emulator.append("abc   \r\ndef  ")

        assertEquals("abc\ndef", emulator.selectedText(TerminalSelection(0, 0, 1, 5)))
    }

    @Test
    fun `a blank single cell copies as empty text`() {
        val emulator = terminal()

        assertEquals("", emulator.selectedText(TerminalSelection(1, 12, 1, 12)))
        assertEquals("s", emulator.selectedText(TerminalSelection(1, 0, 1, 0)))
    }

    @Test
    fun `an out of range selection clamps to the grid`() {
        val emulator = terminal()

        assertEquals(
            "hello world\nsecond",
            emulator.selectedText(TerminalSelection(-3, -4, 1, 99)),
        )
        assertEquals("", emulator.selectedText(TerminalSelection(-4, 0, -2, 4)))
        assertEquals("", emulator.selectedText(TerminalSelection(5, 0, 9, 4)))
    }
}
