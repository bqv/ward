package uk.xa0.ward.ui

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.components.TurnStatusRow
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The running-turn row's words, supplied and fallen back.
 *
 * The row is where the host plugin's live phase lands, so the two things worth
 * pinning are that a supplied label is what the row draws and that null, blank or
 * whitespace falls back to the app's own "Deep diving..." — the degradation path
 * for a host without the plugin.
 *
 * This asserts the semantics tree only. The row paints a brand-blue shimmer
 * (`rememberInfiniteTransition`), and Robolectric cannot measure text anyway
 * (`.work/docs/traps.md`): where a longer plugin label wraps, ellipsises or
 * collides with the elapsed clock is a device claim, not one this harness can
 * make. The clock is therefore frozen so the infinite animation cannot keep the
 * test from ever going idle; the elapsed text itself is still asserted.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class TurnStatusRowTest {

    @get:Rule
    val rule = createComposeRule()

    private fun show(elapsedMs: Long?, status: String?) {
        rule.mainClock.autoAdvance = false
        rule.setContent {
            DshTheme { TurnStatusRow(elapsedMs, status) }
        }
        rule.mainClock.advanceTimeByFrame()
        rule.waitForIdle()
    }

    @Test
    fun `a supplied live label is the row's text`() {
        show(null, "Requesting model")

        rule.onNodeWithText("Requesting model").assertExists()
        rule.onNodeWithText("Deep diving...").assertDoesNotExist()
    }

    @Test
    fun `a null label falls back to the app's own copy`() {
        show(null, null)

        rule.onNodeWithText("Deep diving...").assertExists()
    }

    @Test
    fun `a blank label falls back to the app's own copy`() {
        show(null, "   ")

        rule.onNodeWithText("Deep diving...").assertExists()
    }

    /** The elapsed clock is unchanged by the label, and still drawn beside it. */
    @Test
    fun `the elapsed clock survives a live label`() {
        show(15_000, "Running tool")

        rule.onNodeWithText("Running tool").assertExists()
        rule.onNodeWithText("15s").assertExists()
    }
}
