package uk.xa0.ward.model

import androidx.compose.runtime.Immutable
import org.json.JSONArray
import org.json.JSONObject

/**
 * One plugin bundle, as the web UI's Plugins tab shows it.
 *
 * This is the **bundle** list, not the loader-entry inventory: the tab's model is
 * a profile package with one enable switch and an optional diagnostic, while
 * `pluginManager/listPlugins` answers individual loader entries (one row per
 * module, with a fiber phase and its own patch address). The web's Plugins tab is
 * built from `pluginManager/listBundles`; the entry inventory belongs to the
 * host's Settings surface, which this app does not port. The app therefore shows
 * bundles and never a component/entry list.
 *
 * **`pluginManager/listBundles` answers a bare JSON array, not an object.** That
 * is why the read goes through `DshClient.rpcRaw` and not `rpc`: `rpc` types its
 * answer as `JSONObject`, so an array would be silently read as `{}` and the
 * section would claim the deployment has no bundles at all. The Wire never wraps
 * this reply in a `bundles` field, so [parsePluginBundles] takes the value exactly
 * as `rpcRaw` returned it and treats anything that is not an array as "no answer"
 * rather than as an empty list — the caller distinguishes the two.
 *
 * Each item is `{name, version?, meta{title,description,icon,error}?, enabled,
 * installed, optional, removable, readOnlyReason?, error{code,diagnostic}?,
 * rows[], overrides[]}`. This model keeps only what the list draws: identity,
 * the two text fields, the enablement flags, the read-only reason and the error.
 * `rows` and `overrides` are deliberately ignored — the list must not grow back
 * into the entry inventory.
 *
 * The web tab's groups are `optional && !installed` ("Official", drawn first) and
 * everything else that is listed ("Installed"). Its visibility test also drops a
 * short list of built-in profile bundles (`@deepseek-ai/dsh-base` and the other
 * app bundles); this client has no such list and must not invent one, so it keeps
 * every row the host returned that is installed, optional, or carries an error.
 */
@Immutable
data class PluginBundle(
    /** The bundle's package name, and the `name` `setBundleEnabled` takes. */
    val name: String,
    /** The host's `version`, or null when it sent none. */
    val version: String?,
    /** `meta.title`, resolved; the package name when the host gave no usable title. */
    val title: String,
    /** `meta.description`, resolved, or null when the host gave none. */
    val description: String?,
    /** The bundle's enable switch, exactly as the host last reported it. */
    val enabled: Boolean,
    val installed: Boolean,
    /** Whether the installation ships this bundle as an optional official one. */
    val optional: Boolean,
    /** The host's `readOnlyReason` verbatim, or null when the bundle is switchable. */
    val readOnlyReason: String?,
    /** `error.code`, kept for a diagnostic that has no sentence. */
    val errorCode: String?,
    /** `error.diagnostic` or `meta.error`, resolved; null when the host gave no text. */
    val errorText: String?,
    /**
     * Whether the host's top-level `error` object was present. This is the web
     * tab's visibility test — `meta.error` is shown as text but does not turn a
     * bundle into an error row.
     */
    val hasError: Boolean,
) {
    /** The bundle has no switch: the host said why it cannot be addressed from here. */
    val readOnly: Boolean
        get() = readOnlyReason != null

    /** The web's Official group: an optional bundle the profile has not installed. */
    val official: Boolean
        get() = optional && !installed

    /** The web's list filter, minus its built-in profile-bundle denylist. */
    val listed: Boolean
        get() = installed || optional || hasError

    /**
     * The problem text the card shows: the host's diagnostic first, else its
     * `meta.error`, else the bare code. A code alone is still the host's own word
     * and is more honest than a silent "Problem" badge.
     */
    val errorSummary: String?
        get() = errorText ?: errorCode
}

/**
 * Parse `pluginManager/listBundles`' answer.
 *
 * [value] is whatever `rpcRaw` returned. A non-array (a malformed envelope, a host
 * that answers an object) parses to an empty list; the caller must *not* treat
 * that as an empty deployment, and [isPluginBundleAnswer] is how it tells the two
 * apart.
 */
fun parsePluginBundles(value: Any?): List<PluginBundle> {
    val array = value as? JSONArray ?: return emptyList()
    return (0 until array.length()).mapNotNull { index ->
        parsePluginBundle(array.optJSONObject(index))
    }
}

/** Whether an `rpcRaw` answer has the shape this bundle list expects at all. */
fun isPluginBundleAnswer(value: Any?): Boolean = value is JSONArray

/**
 * One bundle, or null when the host gave no `name`.
 *
 * Every other field is read defensively: a bundle missing `enabled` reads as off,
 * a missing `meta` falls back to its package name, and absent flags read false.
 * A bundle the host cannot name is dropped: `name` is the only handle
 * `setBundleEnabled` has, and several unnamed rows would share one Compose key.
 */
fun parsePluginBundle(row: JSONObject?): PluginBundle? {
    val source = row ?: return null
    val name = source.str("name").trim()
    if (name.isEmpty()) return null
    val meta = source.obj("meta")
    val error = source.obj("error")
    return PluginBundle(
        name = name,
        version = source.str("version").trim().ifEmpty { null },
        title = metaText(meta?.opt("title")) ?: name,
        description = metaText(meta?.opt("description")),
        enabled = source.bool("enabled"),
        installed = source.bool("installed"),
        optional = source.bool("optional"),
        readOnlyReason = source.str("readOnlyReason").trim().ifEmpty { null },
        errorCode = error?.str("code")?.trim()?.ifEmpty { null },
        errorText = metaText(error?.opt("diagnostic")) ?: metaText(meta?.opt("error")),
        hasError = error != null,
    )
}

/**
 * The reader-facing text for one `meta` value: a plain string, or a locale object
 * whose `en` entry wins and whose first non-blank string value is the fallback
 * (org.json keeps an object's own key order, so "first" is the host's order, not
 * the client's). Anything else — a number, an array, an empty object — is no text.
 */
private fun metaText(value: Any?): String? {
    when (value) {
        is String -> return value.trim().ifEmpty { null }
        is JSONObject -> {
            val en = value.opt("en")
            if (en is String && en.isNotBlank()) return en.trim()
            val keys = value.keys()
            while (keys.hasNext()) {
                val candidate = value.opt(keys.next())
                if (candidate is String && candidate.isNotBlank()) return candidate.trim()
            }
        }
    }
    return null
}

/**
 * The sentence shown instead of a switch for a read-only bundle.
 *
 * The two reasons the contract defines get the app's own explanation; an unknown
 * reason still disables the switch (the host said it is read-only) and says only
 * that, rather than inventing a cause.
 */
fun pluginReadOnlyReasonLabel(reason: String): String = when (reason) {
    "management-required" ->
        "This plugin bundle is managed by the deployment's configuration and cannot be switched here."
    "unaddressable" ->
        "The host cannot address this plugin bundle from here, so it cannot be switched."
    else -> "This plugin bundle cannot be switched from here."
}

/**
 * One `pluginManager/setBundleEnabled` answer, as far as the sheet needs it.
 *
 * The method takes **top-level** `{name, enabled}` arguments — not a nested
 * `request` object — because that is the shape the host's gateway validates, and
 * `name` is the bundle's package name. Answers are the same `ChangeResult` shape
 * as `setPluginEnabled`: `changed`, `application`
 * (`applied` | `restart-required` | `overridden` | `failed` | `cancelled`),
 * `stage`, `target`, an optional `enabled`, an optional `error{code,diagnostic}`,
 * and `warnings[]`.
 *
 * A `ChangeResult` is not the new bundle list. Whether a switch may be drawn as
 * flipped is the *reload's* answer, never this reply's: a `restart-required`
 * change is stored but not in effect, an `overridden` one was superseded, and a
 * `failed` one left the bundle where it was. See
 * `DshViewModel.setBundleEnabled`.
 */
@Immutable
data class PluginChange(
    val changed: Boolean,
    val application: String,
    val target: String,
    val enabled: Boolean?,
    val errorCode: String?,
    val errorDiagnostic: String?,
    val warnings: List<String>,
) {
    /**
     * The host's own refusal wording, or null when the change is not a refusal.
     *
     * A diagnostic is preferred over a bare code because it is a sentence the
     * reader can act on; the code is still carried when the host gave no
     * diagnostic. A `cancelled` change is reported as such rather than as a
     * failure, since the host deliberately did nothing.
     */
    fun failureText(): String? = when (application) {
        "failed" -> errorDiagnostic ?: errorCode ?: "The host could not change this bundle."
        "cancelled" -> errorDiagnostic ?: "The host cancelled the change."
        else -> null
    }

    /** The change took effect, or is stored and waiting for a host restart. */
    val accepted: Boolean
        get() = changed && application != "failed" && application != "cancelled"
}

/** Read the `ChangeResult` one `setBundleEnabled` call answered with. */
fun readPluginChange(reply: JSONObject?): PluginChange {
    val error = reply?.obj("error")
    val warnings = reply?.arr("warnings")?.let { array ->
        (0 until array.length()).mapNotNull { index ->
            val item = array.opt(index)
            when (item) {
                is String -> item.takeIf { it.isNotBlank() }
                is JSONObject -> metaText(item.opt("message")) ?: item.str("code").ifBlank { null }
                else -> null
            }
        }
    }.orEmpty()
    return PluginChange(
        changed = reply.bool("changed"),
        application = reply.str("application").trim().lowercase(),
        target = reply.str("target"),
        enabled = reply?.opt("enabled")?.let { it as? Boolean },
        errorCode = error?.str("code")?.ifBlank { null },
        errorDiagnostic = error?.str("diagnostic")?.trim()?.ifBlank { null },
        warnings = warnings,
    )
}
