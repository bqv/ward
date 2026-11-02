package uk.xa0.ward.extension.builtin

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import uk.xa0.ward.extension.DshExtension
import uk.xa0.ward.extension.EXTENSION_ABI_VERSION
import uk.xa0.ward.extension.ExtensionBuilder
import uk.xa0.ward.extension.ExtensionScope
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.model.PRICED_PROVIDER
import uk.xa0.ward.ui.DeepSeekUsageTab

/** The extension id of the built-in Usage tab. */
const val USAGE_EXTENSION_ID: String = "uk.xa0.ward.builtin.usage"

/** The tab id inside [USAGE_EXTENSION_ID]. */
const val USAGE_TAB_ID: String = "usage"

/**
 * The DeepSeek **usage** readings as a built-in extension tab.
 *
 * This extension is the sole host of the DeepSeek-specific usage rows: the
 * off-peak/peak cost estimate, the subagent-family fold and the door to the
 * account balance. The token-usage popup keeps only the host's own token and
 * cache counts, because those are what the host reports; everything that is this
 * app's arithmetic on them, plus the balance route, lives here.
 *
 * The tab is offered only while the selected route is the official DeepSeek
 * provider — [ExtensionScope.providerId] is the gate the shell evaluates before it
 * draws the seat — so a local or other cloud route does not show an accounting
 * page that could not price it.
 *
 * The content draws [DeepSeekUsageTab] and reads its facts from
 * [ExtensionScope.usage]: the session's stats, the rate table for its current
 * route, the family fold and the balance door. A null usage — accounting that has
 * not arrived — draws the view's own unavailable state ("not reported") rather
 * than zeroes.
 */
class UsageExtension : DshExtension {

    override val id: String = USAGE_EXTENSION_ID

    override val title: String = "Usage"

    override val version: Int = EXTENSION_ABI_VERSION

    override fun contribute(builder: ExtensionBuilder) {
        builder.sessionTab(
            SessionTab(
                id = USAGE_TAB_ID,
                label = "Usage",
                order = 1300,
                visible = { scope -> scope.providerId.value == PRICED_PROVIDER },
                content = { scope -> UsageHost(scope) },
            ),
        )
    }
}

/**
 * The tab's host: observe the usage fact and the route's provider id, then hand
 * them to [DeepSeekUsageTab].
 *
 * The provider id is passed as the label so a priced row names the route its
 * rates belong to. Null usage is not an empty reading and is not turned into
 * zeroes here: the view words it as "not reported", and the balance door stays
 * available.
 */
@Composable
private fun UsageHost(scope: ExtensionScope) {
    val usage by scope.usage.collectAsState()
    val providerId by scope.providerId.collectAsState()

    DeepSeekUsageTab(
        stats = usage?.stats,
        rates = usage?.rates,
        family = usage?.family,
        onBalance = { usage?.openBalance?.invoke() },
        providerLabel = providerId,
        modifier = Modifier.fillMaxSize(),
    )
}
