package uk.xa0.ward.ui.components

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.PRICE_SOURCE
import uk.xa0.ward.model.SessionStats
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The token-usage popup keeps the host's own counts and nothing DeepSeek-specific.
 *
 * The cost range, its price source, the subagent family and the balance door all
 * live in the Usage extension tab ([uk.xa0.ward.ui.DeepSeekUsageTab]). This test
 * opens the popup and proves it draws the rows the host reports while none of
 * those DeepSeek-specific rows appear.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class SessionStatsPillsTest {

    @get:Rule
    val rule = createComposeRule()

    @Test
    fun `the popup draws host counts and no longer draws cost, family or balance`() {
        val stats = SessionStats(
            hasStats = true,
            turns = 3,
            steps = 7,
            hasUsage = true,
            uncachedInputTokens = 1_000,
            cacheReadTokens = 8_000,
            cacheWriteTokens = 500,
            outputTokens = 2_000,
        )
        rule.setContent {
            DshTheme {
                SessionStatsPills(stats = stats)
            }
        }
        rule.waitForIdle()

        rule.onNodeWithTag(STATS_USAGE_CHIP_TAG, useUnmergedTree = true).performClick()
        rule.waitForIdle()

        rule.onNodeWithText("Token usage").assertExists()
        rule.onNodeWithText("11,500 tok").assertExists()
        rule.onNodeWithText("Uncached input").assertExists()
        rule.onNodeWithText("Cached input").assertExists()
        rule.onNodeWithText("Output").assertExists()

        rule.onNodeWithText("Cost (off-peak – peak)").assertDoesNotExist()
        rule.onNodeWithText(PRICE_SOURCE).assertDoesNotExist()
        rule.onNodeWithText("Subagent family").assertDoesNotExist()
        rule.onNodeWithText("Family, as reported").assertDoesNotExist()
        rule.onNodeWithText("Subagents").assertDoesNotExist()
        rule.onNodeWithText("Balance & top up").assertDoesNotExist()
    }
}
