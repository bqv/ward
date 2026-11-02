package uk.xa0.ward.ui.components

import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.ChatEntry
import uk.xa0.ward.model.NoticeSeverity
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * An unknown host event is a tiny one-line activity note, not a JSON card.
 *
 * The rendered size and colour are a device claim — Robolectric's stub metrics
 * cannot see either — so what is asserted here is the disclosure behaviour: the
 * payload is absent until the label is tapped, present after it, gone again on
 * the second tap, and a row with no payload is not tappable at all. That is the
 * part the "tiny" treatment must not break: collapsed stays one line, and the
 * raw JSON never becomes the default face.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class NoticeEventRowTest {

    @get:Rule
    val rule = createComposeRule()

    private fun notice(detail: String?) = ChatEntry.Notice(
        seq = 1,
        kind = "event:x-local/loop",
        text = "X local loop",
        time = 1000L,
        severity = NoticeSeverity.INFO,
        detail = detail,
    )

    @Test
    fun `the label is one line until tapped, then the payload shows`() {
        val payload = """{"phase":"live","loop":3}"""
        rule.setContent { DshTheme { NoticeRow(notice(payload)) } }
        rule.waitForIdle()

        rule.onNodeWithText("X local loop").assertExists()
        rule.onNodeWithText(payload).assertDoesNotExist()

        rule.onNodeWithText("X local loop").performClick()
        rule.waitForIdle()
        rule.onNodeWithText(payload).assertExists()

        rule.onNodeWithText("X local loop").performClick()
        rule.waitForIdle()
        rule.onNodeWithText(payload).assertDoesNotExist()
    }

    @Test
    fun `a row with no payload has nothing to tap`() {
        rule.setContent { DshTheme { NoticeRow(notice(null)) } }
        rule.waitForIdle()

        rule.onNodeWithText("X local loop").assertExists()
        rule.onAllNodes(hasClickAction()).assertCountEquals(0)
    }
}
