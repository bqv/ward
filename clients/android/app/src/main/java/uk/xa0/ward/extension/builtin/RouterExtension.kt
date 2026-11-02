package uk.xa0.ward.extension.builtin

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import uk.xa0.ward.extension.DshExtension
import uk.xa0.ward.extension.EXTENSION_ABI_VERSION
import uk.xa0.ward.extension.ExtensionBuilder
import uk.xa0.ward.extension.ExtensionScope
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.ui.RouterTab
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/** The extension id of the built-in Router tab. */
const val ROUTER_EXTENSION_ID: String = "uk.xa0.ward.builtin.router"

/** The tab id inside [ROUTER_EXTENSION_ID]. */
const val ROUTER_TAB_ID: String = "router"

/**
 * The llama.cpp **router** behind the session's local model, as a built-in
 * extension tab.
 *
 * The tab is offered only while the selected route is the host's own local
 * router — [ExtensionScope.localRoute] is the gate the shell evaluates before it
 * draws the seat — and it goes away the moment a cloud model is selected, exactly
 * as the fourth built-in tab it replaces did. The content draws the same
 * [RouterTab] and takes its facts from [ExtensionScope.router]: the target, the
 * last probe bound to that route, the effort label and the check itself. There is
 * no ViewModel in this file, which is the point of the move: the Router is now a
 * sample of the extension ABI rather than a special case in the screen.
 *
 * A null [ExtensionScope.router] — the moment between the route changing and the
 * scope's fact updating, or a shell that could not derive one — draws a short
 * unavailable state rather than an empty or stale router screen.
 */
class RouterExtension : DshExtension {

    override val id: String = ROUTER_EXTENSION_ID

    override val title: String = "Router"

    override val version: Int = EXTENSION_ABI_VERSION

    override fun contribute(builder: ExtensionBuilder) {
        builder.sessionTab(
            SessionTab(
                id = ROUTER_TAB_ID,
                label = "Router",
                order = 1400,
                visible = { scope -> scope.localRoute.value },
                content = { scope -> RouterHost(scope) },
            ),
        )
    }
}

/** The built-in extensions: the same list the runtime registers before the APK scan. */
object BuiltInExtensions {
    val all: List<DshExtension> = listOf(
        UsageExtension(),
        RouterExtension(),
    )
}

/**
 * The tab's host: observe the router fact, ask the question when the seat opens,
 * and hand the answer to [RouterTab].
 *
 * Opening the tab is what asks the question, and re-opening it asks again: a
 * reachability answer is only worth reading with the time it was taken. The
 * session and the route are keys too — two sessions can select the same local
 * route, and moving between them is a new "is my router up now", not a reason to
 * show the other one's answer.
 */
@Composable
private fun RouterHost(scope: ExtensionScope) {
    val sessionId by scope.sessionId.collectAsState()
    val router by scope.router.collectAsState()
    val session = router

    LaunchedEffect(session?.target?.provider?.id, session?.target?.modelId, sessionId) {
        session?.check?.invoke()
    }

    if (session == null) {
        RouterUnavailable()
        return
    }
    RouterTab(
        target = session.target,
        probe = session.probe,
        effort = session.effort,
        onCheck = session.check,
        modifier = Modifier.fillMaxSize(),
    )
}

@Composable
private fun RouterUnavailable() {
    val colors = DshTheme.colors
    Column(
        Modifier
            .fillMaxSize()
            .padding(DshSpacing.xl),
    ) {
        Text("Router", style = DshType.titleMedium, color = colors.labelPrimary)
        Text(
            text = "The router is not available for this route.",
            style = DshType.bodySmall,
            color = colors.labelTertiary,
            modifier = Modifier.padding(top = DshSpacing.sm),
        )
    }
}
