package uk.xa0.ward.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import uk.xa0.ward.model.PluginBundle
import uk.xa0.ward.model.pluginReadOnlyReasonLabel
import uk.xa0.ward.ui.theme.DshRadius
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/**
 * The Settings sheet's Plugins section: the profile's plugin bundles with one
 * enable switch per bundle.
 *
 * This is the web Plugins tab's model, not the host's loader-entry inventory. A
 * bundle is a package with one `enabled` switch; the individual modules it loads
 * used to be drawn here as rows, and are deliberately not drawn at all — the tab
 * this section ports shows bundles. Nothing here installs, removes or updates a
 * bundle, because this correction is about matching the tab's switch list, not
 * its management actions.
 *
 * The web tab draws "Official" first (an optional bundle the profile has not
 * installed) and "Installed" second (everything else it lists), each with a
 * count; this section keeps that order and count. A bundle's status is the host's
 * `enabled`; a bundle with an `error` says `Problem` and shows the host's own
 * diagnostic, and one with a `readOnlyReason` shows the host's reason instead of
 * a working switch.
 *
 * The switch follows the last bundle list the host sent. A tap does not move it:
 * the view model re-reads the list after every accepted change and only that read
 * may change the drawn value, so `restart-required` (stored, not yet in effect)
 * and `overridden` cannot look like a completed flip. While a change is in flight
 * the row says `Changing…` and every switch is disabled, which is what keeps a
 * second tap from racing it.
 */
@Composable
internal fun PluginsSection(
    plugins: List<PluginBundle>,
    loading: Boolean,
    readError: String?,
    /** The `name` of the bundle being changed, or null when nothing is. */
    changing: String?,
    /** The host's wording for the last refused toggle; drawn above the list. */
    actionError: String?,
    /** A successful change the host says needs a restart before it is in effect. */
    notice: String?,
    onToggle: ((String, Boolean) -> Unit)?,
    onRetry: (() -> Unit)?,
) {
    SectionNote("Configure and inspect the plugin bundles installed in this deployment.")

    // "Wired" is the caller handing this section a read, a read's state, or rows.
    // A caller that passed none of it (the defaults that keep older call sites
    // compiling) must not be told the deployment has no bundles: that is a claim
    // only a real answer can support.
    val wired = onRetry != null || loading || readError != null || plugins.isNotEmpty()
    if (!wired) return

    // A toggle's refusal is about an action the reader just took, so it sits above
    // the list where they will see it rather than under the row they tapped.
    actionError?.let { SectionErrorNote(it) }
    notice?.let { SectionNoticeNote(it) }

    if (readError != null) {
        PluginReadError(readError, onRetry)
    }

    // The first read has nothing to show yet. A later read keeps its rows, so this
    // is not drawn over a list the reader is already using.
    if (loading && plugins.isEmpty() && readError == null) {
        SectionNote("Reading plugins…")
        return
    }

    // The web's list filter, minus its built-in profile-bundle denylist, which
    // this app does not have. The groups and their order are the web tab's:
    // Official (optional, not installed) first, then Installed (everything else).
    val listed = plugins.filter { it.listed }
    val official = listed.filter { it.official }
    val installed = listed.filter { !it.official }

    if (listed.isEmpty()) {
        if (readError == null) SectionNote("No plugin bundles are installed in this deployment.")
        return
    }

    if (official.isNotEmpty()) {
        GroupLabel("Official (${official.size})")
        official.forEach { bundle ->
            PluginBundleCard(
                bundle = bundle,
                busy = changing != null,
                changing = changing == bundle.name,
                onToggle = onToggle,
            )
        }
    }
    if (installed.isNotEmpty()) {
        GroupLabel("Installed (${installed.size})")
        installed.forEach { bundle ->
            PluginBundleCard(
                bundle = bundle,
                busy = changing != null,
                changing = changing == bundle.name,
                onToggle = onToggle,
            )
        }
    }
}

/** One bundle: identity, description, status, host error/read-only reason, switch. */
@Composable
private fun PluginBundleCard(
    bundle: PluginBundle,
    busy: Boolean,
    changing: Boolean,
    onToggle: ((String, Boolean) -> Unit)?,
) {
    val colors = DshTheme.colors
    val switchable = onToggle != null && !bundle.readOnly
    val switchEnabled = switchable && !busy
    val shape = RoundedCornerShape(DshRadius.card)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.sm)
            .clip(shape)
            .background(colors.codeBlock)
            .border(0.5.dp, colors.borderL1, shape)
            .padding(DshSpacing.lg),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = bundle.title,
                style = DshType.bodyMedium,
                color = colors.labelPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            // The package name earns its own monospace line only when the host gave
            // a display title; otherwise the title above already *is* the name.
            if (bundle.name != bundle.title) {
                Text(
                    text = bundle.name,
                    style = DshType.code,
                    color = colors.labelTertiary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            // `meta.description` is optional; when it merely repeats the package
            // name there is already a line above for that, so it is not repeated.
            bundle.description?.takeIf {
                it.isNotBlank() && it != bundle.name && it != bundle.title
            }?.let { description ->
                Text(
                    text = description,
                    style = DshType.bodySmall,
                    color = colors.labelTertiary,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                text = if (changing) "Changing…" else bundleStatusLabel(bundle),
                style = DshType.micro,
                color = when {
                    bundle.hasError -> colors.error
                    !bundle.enabled -> colors.labelCaption
                    else -> colors.success
                },
            )
            // The host's own diagnostic for a bundle it could not read or load. It
            // is the page's real error, so it is shown verbatim rather than inferred
            // from the status alone.
            bundle.errorSummary?.let { error ->
                Text(
                    text = error,
                    style = DshType.bodySmall,
                    color = colors.error,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            bundle.readOnlyReason?.let { reason ->
                Text(
                    text = pluginReadOnlyReasonLabel(reason),
                    style = DshType.micro,
                    color = colors.labelCaption,
                )
            }
        }
        Spacer(Modifier.width(DshSpacing.md))
        Switch(
            checked = bundle.enabled,
            // Null rather than a no-op lambda: a disabled switch must not claim a
            // tap it will ignore.
            onCheckedChange = if (switchEnabled) {
                { onToggle?.invoke(bundle.name, !bundle.enabled) }
            } else {
                null
            },
            enabled = switchEnabled,
        )
    }
}

/**
 * The bundle's status word. An error outranks enablement, because a bundle the
 * host cannot read is a problem whether or not the profile selected it.
 */
private fun bundleStatusLabel(bundle: PluginBundle): String = when {
    bundle.hasError -> "Problem"
    bundle.enabled -> "Enabled"
    else -> "Disabled"
}

/** The read failure and the reader's own retry, on one line beside its wording. */
@Composable
private fun PluginReadError(readError: String, onRetry: (() -> Unit)?) {
    val colors = DshTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = readError,
            style = DshType.bodySmall,
            color = colors.error,
            modifier = Modifier.weight(1f),
        )
        if (onRetry != null) {
            Spacer(Modifier.width(DshSpacing.md))
            Text(
                text = "Retry",
                style = DshType.bodySmall,
                color = colors.link,
                maxLines = 1,
                modifier = Modifier
                    .clip(RoundedCornerShape(DshRadius.sm))
                    .clickableNoRipple(onClick = onRetry)
                    .padding(DshSpacing.sm),
            )
        }
    }
}

/** A successful-but-not-yet-visible change, in the sheet's neutral voice. */
@Composable
private fun SectionNoticeNote(text: String) {
    Text(
        text = text,
        style = DshType.bodySmall,
        color = DshTheme.colors.labelSecondary,
        modifier = Modifier.padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.xs),
    )
}
