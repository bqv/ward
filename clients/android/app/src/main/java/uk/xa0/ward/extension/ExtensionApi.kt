package uk.xa0.ward.extension

import androidx.compose.runtime.Composable
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import uk.xa0.ward.model.FamilyUsage
import uk.xa0.ward.model.RouterProbeResult
import uk.xa0.ward.model.RouterTarget
import uk.xa0.ward.model.SessionStats
import uk.xa0.ward.model.TokenRates

/**
 * The Android extension ABI, version 2.
 *
 * **Internal, not a published SPI.** The class and package names, the method
 * shapes and the entry-point convention below may change without notice; this
 * version integer exists so an extension built for a different shape is rejected
 * instead of run. Runtime-loaded code is trusted exactly as much as the host
 * application: it executes in this app's process, under its uid, with its
 * cookies, its files and its Remote access. Trusting an extension is trusting its
 * author, and the certificate prompt in Settings says so.
 *
 * ## The entry point
 *
 * An extension APK must contain a public class named exactly [EXTENSION_ENTRY_CLASS]
 * (`uk.xa0.ward.extension.Entry`) with a public no-argument constructor, and it
 * must implement [DshExtension]. The class name is fixed because it is how the
 * loader finds the extension without reading resources or a manifest: loading
 * classes from an APK does not load its resources, so an XML or manifest-declared
 * entry point would need `AssetManager` surgery that this ABI deliberately does
 * not have.
 *
 * The loader refuses an extension whose [DshExtension.version] is not
 * [EXTENSION_ABI_VERSION]. This class/ABI being internal means the extension APK
 * and this app are built together; a third-party store, a published artefact and
 * binary compatibility are explicitly out of scope. In particular a `@Composable`
 * lambda's binary shape is the Compose compiler's, so an extension APK should be
 * built against this same ABI source with a compatible Compose compiler rather
 * than against a remembered signature.
 *
 * ## What an extension may contribute
 *
 * Additive only. The first slot is a session tab; built-in chrome, the
 * transcript, the composer and navigation are not replaceable. Compose cannot
 * let loaded code rewrite the host tree, so an extension supplies content for a
 * seat the shell draws, and the shell keeps layout, theme and hit targets.
 *
 * A tab may also say when it belongs on screen through [SessionTab.visible],
 * evaluated against the same live [ExtensionScope] its content receives. That is
 * what lets a route-specific tab — the Router on a local route, a provider's usage
 * tab on its own provider — appear and disappear with the session's selection
 * instead of being drawn on every route and apologising inside.
 *
 * Extensions have no resources or themes of their own: an extension view composes
 * under [uk.xa0.ward.ui.theme.DshTheme], with this app's colours and type.
 */

/** The only ABI version this build accepts. */
const val EXTENSION_ABI_VERSION: Int = 2

/**
 * The one class the loader instantiates from an extension APK.
 *
 * The package here is `uk.xa0.ward.extension`, not the extension's own package:
 * the fixed name is the ABI's one naming requirement. Do not add a class with this
 * name to this app — a [dalvik.system.DexClassLoader] delegates to its parent
 * first, so a parent class of the same name would shadow the APK's entry point.
 */
const val EXTENSION_ENTRY_CLASS: String = "uk.xa0.ward.extension.Entry"

/**
 * One installed extension.
 *
 * [id] is the stable identity used by `ConfigStore.disabledExtensionIds`; it must
 * be unique across every loaded extension (built-in and APK alike). [title] is the
 * name the Extensions section shows. [version] is the ABI version, not the APK's
 * `versionName`; it must equal [EXTENSION_ABI_VERSION].
 */
interface DshExtension {
    val id: String
    val title: String
    val version: Int

    /**
     * Declare this extension's contributions. Called once, during load, before
     * anything is composed. Any throw here fails this extension alone.
     */
    fun contribute(builder: ExtensionBuilder)
}

/** The contributions a [DshExtension] may add. Additive only. */
interface ExtensionBuilder {
    /** Add one tab to the session view strip. */
    fun sessionTab(tab: SessionTab)
}

/**
 * A session tab contribution.
 *
 * [id] is unique within its extension and is what a future "remember the open
 * tab" or per-tab preference would key on; the shell composes it with the
 * extension's id, so two extensions may both use `"status"` without colliding.
 * [order] is the tab's seat: extension tabs are drawn after every built-in tab
 * and sorted by `order`, then [id]. [label] is the strip's word.
 *
 * [visible] decides whether the strip draws this tab right now. It is evaluated
 * against the live [ExtensionScope] and re-evaluated as the scope's facts change,
 * so a route-specific tab disappears when the reader selects another route. It
 * throws only by mistake; the shell treats a throwing predicate as "not visible"
 * rather than letting one extension take the strip down.
 *
 * [content] runs with a live [ExtensionScope] and is recomposed by the host.
 */
data class SessionTab(
    val id: String,
    val label: String,
    val order: Int = 1000,
    val visible: (ExtensionScope) -> Boolean = { true },
    val content: @Composable (ExtensionScope) -> Unit,
)

/**
 * The llama.cpp router behind the route the session will run.
 *
 * [target], [probe] and [effort] are the same facts the composer's model trigger
 * and the router's own check use: the host's declared endpoint, the last
 * reachability answer bound to that route, and the active reasoning effort's
 * label. [check] asks the host to reach the endpoint again; it is the same call
 * the tab's own button makes.
 */
data class RouterSession(
    val target: RouterTarget,
    val probe: RouterProbeResult,
    /** The active effort's own label, resolved by the app; null when there is none. */
    val effort: String?,
    val check: () -> Unit,
)

/**
 * The open session's usage accounting, as the token panel reads it.
 *
 * Every part is the app's own derivation, not a new wire call: [stats] is the
 * session's folded projections, [rates] is the price table for its current route
 * (null when that route has no published price), and [family] folds the subagent
 * family under the session. [openBalance] opens the balance sheet and refreshes
 * it, exactly as the token panel's own door does.
 */
data class SessionUsage(
    val stats: SessionStats?,
    val rates: TokenRates?,
    val family: FamilyUsage?,
    val openBalance: () -> Unit,
)

/**
 * What an extension may see and do while its content is composed.
 *
 * A deliberately narrow surface — not the view model, not the app's screen, client
 * or transport classes. The values it carries are plain model data the app owns,
 * and the same scope instance is passed to every contribution from one extension.
 *
 * The facts below are **app-owned and read-only**: the extension observes them,
 * never writes them. They are `StateFlow`s rather than values because each one
 * changes while the tab is open — a session switch, a different model, a router
 * check finishing, usage arriving — and a tab must see the new value without being
 * recreated. An extension must treat `null` as *unavailable*, not as zero or as
 * "nothing happened": a null [providerId] is a route this client cannot name yet,
 * a null [RouterSession] is no router on this route, and a null [SessionUsage] is
 * accounting that has not arrived.
 */
interface ExtensionScope {
    /**
     * The session the tab is being shown in, or null on the pending new-session
     * seat. A tab that needs a session should render that fact rather than assume
     * one; the value changes in place when the reader switches sessions.
     */
    val sessionId: StateFlow<String?>

    /**
     * The provider id of the route the session will run — `"deepseek-official"`,
     * `"dsh-local"` — or null while no route is named. The same id the composer's
     * model trigger names, derived from the host's own model selection.
     */
    val providerId: StateFlow<String?>

    /**
     * True while the selected route's declared endpoint is this host's own
     * loopback, which is the fact that places the Router tab. It is false for a
     * cloud route, for a pending switch to one, and for a selection the host's
     * settings document does not describe; null provider facts read as false here.
     */
    val localRoute: StateFlow<Boolean>

    /**
     * The router behind the selected route, or null when that route is not local.
     * The value changes when the route changes and when a check updates its probe.
     */
    val router: StateFlow<RouterSession?>

    /**
     * The open session's token and cost reading, or null when the shell has no
     * reading at all. Carries the family fold and the balance sheet's door, and
     * updates in place as the projections, the route's rates or the family move.
     */
    val usage: StateFlow<SessionUsage?>

    /**
     * One unary host Remote call, carrying the same authentication, route and
     * transport as the app's own calls. Returns the reply's `value` untouched
     * (`JSONObject`, array, string, number, null), because some host methods answer
     * with a bare array. Throws on a host refusal or transport failure, exactly as
     * [uk.xa0.ward.net.DshClient.rpcRaw] does; an extension must handle that
     * failure itself.
     */
    suspend fun rpc(method: String, args: JSONObject): Any?
}
