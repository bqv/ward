package uk.xa0.ward.ui

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.CostRange
import uk.xa0.ward.model.FamilyUsage
import uk.xa0.ward.model.METHOD_NOTE
import uk.xa0.ward.model.PRICE_SOURCE
import uk.xa0.ward.model.SessionStats
import uk.xa0.ward.model.TokenRates
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The Usage extension tab's sections and the one action it hands upward.
 *
 * A wiring test, not a visual one: Robolectric's stub metrics make an absolute
 * height or a line break meaningless. What it does show is that the host counts,
 * the priced range and its source, the family reading, the balance door and the
 * unavailable wording are each drawn where the tab says they are.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class DeepSeekUsageTabTest {

    @get:Rule
    val rule = createComposeRule()

    private companion object {
        val STATS = SessionStats(
            hasStats = true,
            turns = 3,
            steps = 7,
            hasUsage = true,
            uncachedInputTokens = 1_000,
            cacheReadTokens = 8_000,
            cacheWriteTokens = 500,
            outputTokens = 2_000,
        )

        val RATES = TokenRates(cacheHitInput = 0.04, cacheMissInput = 2.0, output = 8.0)

        val FAMILY = FamilyUsage(
            members = 2,
            counted = 1,
            tokens = 9_000,
            inFigure = 2,
            unreported = 1,
            cost = CostRange(offPeak = 0.5, peak = 1.0),
            unpriced = 0,
        )
    }

    private fun render(
        stats: SessionStats? = STATS,
        rates: TokenRates? = RATES,
        family: FamilyUsage? = FAMILY,
        providerLabel: String? = null,
        onBalance: () -> Unit = {},
    ) {
        rule.setContent {
            DshTheme {
                DeepSeekUsageTab(
                    stats = stats,
                    rates = rates,
                    family = family,
                    onBalance = onBalance,
                    providerLabel = providerLabel,
                )
            }
        }
        rule.waitForIdle()
    }

    @Test
    fun `host counts and the priced range are shown`() {
        render(providerLabel = "DeepSeek official")

        rule.onNodeWithText("Session usage").assertExists()
        rule.onNodeWithText("Total").assertExists()
        rule.onNodeWithText("11,500 tok").assertExists()
        rule.onNodeWithText("Cache hit").assertExists()
        rule.onNodeWithText("84%").assertExists()
        rule.onNodeWithText("Uncached input").assertExists()
        rule.onNodeWithText("1,000 tok").assertExists()
        rule.onNodeWithText("Cached input").assertExists()
        rule.onNodeWithText("8,000 tok").assertExists()
        rule.onNodeWithText("Cache write").assertExists()
        rule.onNodeWithText("500 tok").assertExists()
        rule.onNodeWithText("Output").assertExists()
        rule.onNodeWithText("2,000 tok").assertExists()

        rule.onNodeWithText("DeepSeek cost").assertExists()
        rule.onNodeWithText("Rates for DeepSeek official.").assertExists()
        rule.onNodeWithText("Cost (off-peak – peak)").assertExists()
        rule.onNodeWithText(PRICE_SOURCE).assertExists()
    }

    @Test
    fun `no rates and no family leaves only the host counts and the balance door`() {
        render(rates = null, family = null)

        rule.onNodeWithText("DeepSeek cost").assertDoesNotExist()
        rule.onNodeWithText("Cost (off-peak – peak)").assertDoesNotExist()
        rule.onNodeWithText(PRICE_SOURCE).assertDoesNotExist()
        rule.onNodeWithText("Subagent family").assertDoesNotExist()
        rule.onNodeWithText("Balance & top up").assertExists()
    }

    @Test
    fun `the family rows are shown when the family has members`() {
        render()

        rule.onNodeWithText("Subagent family").assertExists()
        rule.onNodeWithText("Family, as reported").assertExists()
        rule.onNodeWithText("9,000 tok").assertExists()
        rule.onNodeWithText("Subagents").assertExists()
        rule.onNodeWithText("1 of 2").assertExists()
        rule.onNodeWithText("Family cost (off-peak – peak)").assertExists()
        rule.onNodeWithText("¥0.50 – ¥1.00").assertExists()
        rule.onNodeWithText(METHOD_NOTE).assertExists()
    }

    @Test
    fun `a family with no members draws no family section`() {
        render(family = FAMILY.copy(members = 0, counted = 0, unreported = 0))

        rule.onNodeWithText("Subagent family").assertDoesNotExist()
        rule.onNodeWithText("Family, as reported").assertDoesNotExist()
    }

    @Test
    fun `Balance and top up invokes the callback`() {
        var clicks = 0
        render(onBalance = { clicks += 1 })

        // The door sits at the bottom of a scrolling page: in Robolectric's
        // viewport it can be off-screen, so bring it into view before the click.
        rule.onNodeWithTag("deepseek-usage-balance")
            .performScrollTo()
            .performClick()

        assertEquals(1, clicks)
    }

    @Test
    fun `a missing usage projection says not reported rather than zero`() {
        render(stats = null)

        rule.onNodeWithText("The host has not reported usage for this session.").assertExists()
        rule.onNodeWithText("Total").assertDoesNotExist()
        rule.onNodeWithText("0 tok").assertDoesNotExist()
        rule.onNodeWithText("Cost (off-peak – peak)").assertDoesNotExist()
    }

    @Test
    fun `an invisible reading says not reported rather than zero`() {
        render(stats = SessionStats(steps = 0, hasUsage = false))

        rule.onNodeWithText("The host has not reported usage for this session.").assertExists()
        rule.onNodeWithText("Total").assertDoesNotExist()
        rule.onNodeWithText("0 tok").assertDoesNotExist()
        rule.onNodeWithText("DeepSeek cost").assertDoesNotExist()
    }
}
