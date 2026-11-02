package uk.xa0.ward.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import uk.xa0.ward.model.FamilyUsage
import uk.xa0.ward.model.PRICE_SOURCE
import uk.xa0.ward.model.SessionStats
import uk.xa0.ward.model.TokenRates
import uk.xa0.ward.model.costRange
import uk.xa0.ward.model.formatCostRange
import uk.xa0.ward.model.formatExactTokens
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/**
 * The Usage extension tab: the DeepSeek-specific readings that left the token-usage
 * popup, laid out as one page.
 *
 * This view is the sole host of those readings: the token-usage popup
 * ([uk.xa0.ward.ui.components.SessionStatsPills]) draws only the host's own token
 * and cache counts, while the cost estimate, the subagent family and the balance
 * door live here and nowhere else.
 *
 * A view and nothing more: it makes no RPC and reads no balance of its own. The
 * caller owns the model route, the published rates and the family fold, and
 * [onBalance] is the only way out of the page.
 *
 * The popup keeps the host's own token and cache counts because those are what the
 * host reports; this page adds the readings that are this app's arithmetic on them —
 * the off-peak/peak cost estimate and the subagent-family total — plus the route to
 * the account balance. Those three were in the popup and are deliberately not there
 * any more, so the popup says only what the host counted.
 *
 * @param stats the open session's projections, or null while the host has not sent
 *   them. Null and an empty reading are the same unavailable state, and it is
 *   worded as "not reported" rather than drawn as zeroes, because a missing
 *   projection is not a session that used nothing.
 * @param rates the published rates for the session's current route, or null when
 *   this app has no price for it. Null removes the cost section rather than pricing
 *   the tokens from another provider's list.
 * @param family the open session's whole subagent family, or null. A family with no
 *   members draws no section: there is nothing to report, which is not the same
 *   fact as a family whose usage has not arrived.
 * @param onBalance opens the balance and top-up sheet.
 * @param providerLabel the route the rates belong to, when the caller knows it; it
 *   is shown beside the price source so a cost row can be attributed. Null omits
 *   the attribution.
 */
@Composable
fun DeepSeekUsageTab(
    stats: SessionStats?,
    rates: TokenRates?,
    family: FamilyUsage?,
    onBalance: () -> Unit,
    providerLabel: String? = null,
    modifier: Modifier = Modifier,
) {
    val colors = DshTheme.colors
    val reported = stats?.takeIf { it.visible }

    Column(
        modifier
            .fillMaxWidth()
            .background(colors.bgBase)
            .verticalScroll(rememberScrollState())
            .padding(bottom = DshSpacing.xxl),
    ) {
        SectionHeading("Session usage")
        if (reported == null) {
            SectionNote("The host has not reported usage for this session.")
        } else {
            StatRow("Total", exactCount(reported.totalTokens))
            reported.cacheHitPercent?.let { StatRow("Cache hit", "$it%") }
            StatRow("Uncached input", exactCount(reported.uncachedInputTokens))
            StatRow("Cached input", exactCount(reported.cacheReadTokens))
            // A session that never wrote cache drops the row, as the popup did.
            if (reported.cacheWriteTokens != 0L) {
                StatRow("Cache write", exactCount(reported.cacheWriteTokens))
            }
            StatRow("Output", exactCount(reported.outputTokens))
        }

        // The session's own estimate, priced only when a table matched its route.
        if (reported != null && rates != null) {
            val priced = costRange(
                cacheHitTokens = reported.cacheReadTokens,
                // A cache write is an input token that did not come from the
                // cache, and the published list has no third input rate.
                cacheMissTokens = reported.uncachedInputTokens + reported.cacheWriteTokens,
                outputTokens = reported.outputTokens,
                rates = rates,
            )
            SectionHeading("DeepSeek cost")
            providerLabel?.takeIf { it.isNotBlank() }?.let {
                SectionNote("Rates for $it.")
            }
            StatRow("Cost (off-peak – peak)", formatCostRange(priced))
            SectionNote(PRICE_SOURCE)
        }

        // Under the session's own reading, never in place of it: the rows above are
        // the host's count for this log, the family is a sum over other logs.
        if (family != null && family.members > 0) {
            SectionHeading("Subagent family")
            StatRow(family.label, exactCount(family.tokens))
            StatRow("Subagents", family.memberCountText)
            family.cost?.let { StatRow("Family cost (off-peak – peak)", formatCostRange(it)) }
            family.notes.forEach { SectionNote(it) }
        }

        // Shown whether or not a price table matched: topping up is useful even for
        // an unpriced route, and this is the door to the account behind the reading.
        SectionAction(
            label = "Balance & top up",
            onClick = onBalance,
            modifier = Modifier
                .padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.sm)
                .testTag("deepseek-usage-balance"),
        )
    }
}

/** Web `message.turnUsage.count`: `{count} tok`. */
private fun exactCount(value: Long): String = "${formatExactTokens(value)} tok"

/** One `dt`/`dd` pair: tertiary label left, tabular value right. */
@Composable
private fun StatRow(label: String, value: String) {
    val colors = DshTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = DshSpacing.xxl, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = DshType.bodySmall,
            color = colors.labelTertiary,
            modifier = Modifier.weight(1f),
        )
        Spacer(Modifier.width(DshSpacing.xl))
        Text(
            text = value,
            style = DshType.bodySmall,
            color = colors.labelSecondary,
            maxLines = 1,
        )
    }
}

/**
 * A tappable action line, in the sheets' own accent. Recreated here rather than
 * widened in `BalanceSheet`, where it is private and has no second caller.
 */
@Composable
private fun SectionAction(
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = DshTheme.colors
    TextButton(
        onClick = onClick,
        modifier = modifier,
        contentPadding = PaddingValues(horizontal = 0.dp, vertical = DshSpacing.xs),
    ) {
        Text(label, color = colors.link, style = DshType.bodyMedium)
    }
}
