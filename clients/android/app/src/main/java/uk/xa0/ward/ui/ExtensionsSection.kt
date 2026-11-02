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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import uk.xa0.ward.extension.ExtensionTrust
import uk.xa0.ward.extension.runtime.ExtensionRow
import uk.xa0.ward.extension.runtime.ExtensionsUiState
import uk.xa0.ward.extension.runtime.InboxExtension
import uk.xa0.ward.extension.runtime.PendingExtensionInstall
import uk.xa0.ward.ui.theme.DshRadius
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/**
 * Settings → **Extensions**: installed runtime extensions, their local switches,
 * install/remove/import actions and each extension's own failure text.
 *
 * This is a different thing from the **Plugins** section above it. A plugin is
 * host-side Cordis state and the switch changes it for every client; an
 * extension is an APK in this phone's private storage and its switch is local to
 * this install. The copy says so rather than leaving the reader to infer it from
 * two adjacent switch lists.
 *
 * The trust dialog is the security boundary this section exists for. Runtime-loaded
 * APKs execute with this app's uid, cookies and host access, so the first install
 * shows the package and the signing certificate's SHA-256 and pins it; an update
 * must match. The dialog names both; the APK is never trusted from its own label
 * alone.
 */
@Composable
internal fun ExtensionsSection(
    state: ExtensionsUiState,
    /** Opens the system file picker. Null leaves the action out (tests/read-only). */
    onInstall: (() -> Unit)? = null,
    onRescan: (() -> Unit)? = null,
    onToggle: ((String, Boolean) -> Unit)? = null,
    onRemove: ((String) -> Unit)? = null,
    onImport: ((InboxExtension) -> Unit)? = null,
    onTrust: (() -> Unit)? = null,
    onCancelTrust: (() -> Unit)? = null,
    onClearMessage: (() -> Unit)? = null,
) {
    SectionNote(
        "Extensions are APKs loaded at runtime. They run in this app's process with " +
            "its access, so only install code you trust. Their switches are local to " +
            "this phone; the Plugins section above is host state shared by every client.",
    )

    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.sm),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (state.loading && state.rows.isEmpty()) {
            Text(
                "Reading extensions…",
                style = DshType.bodySmall,
                color = DshTheme.colors.labelTertiary,
                modifier = Modifier.weight(1f),
            )
        } else {
            Spacer(Modifier.weight(1f))
        }
        if (onInstall != null) {
            ActionText("Install APK", onInstall)
            Spacer(Modifier.width(DshSpacing.md))
        }
        if (onRescan != null) {
            ActionText("Rescan", onRescan)
        }
    }

    state.error?.let { error ->
        SectionErrorNote(error)
        onClearMessage?.let { clear -> ActionText("Dismiss", clear) }
    }
    state.notice?.let { SectionNote(it) }

    if (!state.loading && state.rows.isEmpty()) {
        SectionNote("No extensions are installed.")
    }

    if (state.rows.isNotEmpty()) {
        GroupLabel("Installed (${state.rows.size})")
        state.rows.forEach { row ->
            ExtensionCard(
                row = row,
                busy = state.busy,
                onToggle = onToggle,
                onRemove = onRemove,
            )
        }
    }

    state.pending?.let { pending ->
        TrustDialog(
            pending = pending,
            onTrust = onTrust,
            onCancel = onCancelTrust,
        )
    }

    // The inbox is a staging area for `adb push`; it is read only when Rescan is
    // pressed. Candidates still go through the same trust dialog.
    GroupLabel("Import from adb")
    SectionNote(
        "Push an APK to this app's external files directory, " +
            "extensions/inbox/, then press Rescan. Nothing is watched or installed " +
            "automatically.",
    )
    if (state.inbox.isEmpty()) {
        SectionNote("No APKs in the import directory.")
    } else {
        state.inbox.forEach { candidate ->
            InboxCard(candidate = candidate, busy = state.busy, onImport = onImport)
        }
    }
}

@Composable
private fun ExtensionCard(
    row: ExtensionRow,
    busy: Boolean,
    onToggle: ((String, Boolean) -> Unit)?,
    onRemove: ((String) -> Unit)?,
) {
    val colors = DshTheme.colors
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
                text = row.title,
                style = DshType.bodyMedium,
                color = colors.labelPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (!row.builtIn) {
                Text(
                    text = row.packageName,
                    style = DshType.code,
                    color = colors.labelTertiary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                text = buildString {
                    if (row.builtIn) append("Built in") else append("APK ")
                    row.version?.let { append(" · ABI v$it") }
                    row.apkVersionName?.takeIf { it.isNotBlank() }?.let { append(" · $it") }
                },
                style = DshType.micro,
                color = colors.labelTertiary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            row.certSha256?.let { fingerprint ->
                Text(
                    text = "SHA-256 ${ExtensionTrust.display(fingerprint)}",
                    style = DshType.codeSmall,
                    color = colors.labelCaption,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                text = when {
                    row.failure != null -> "Problem"
                    row.enabled -> "Enabled"
                    else -> "Disabled"
                },
                style = DshType.micro,
                color = when {
                    row.failure != null -> colors.error
                    row.enabled -> colors.success
                    else -> colors.labelCaption
                },
            )
            row.failure?.let { failure ->
                Text(
                    text = failure,
                    style = DshType.bodySmall,
                    color = colors.error,
                    maxLines = 5,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (row.removable && onRemove != null) {
                ActionText("Remove", { onRemove(row.packageName) })
            }
        }
        Spacer(Modifier.width(DshSpacing.md))
        if (row.loaded) {
            Switch(
                checked = row.enabled,
                onCheckedChange = if (onToggle != null && !busy) {
                    { checked -> onToggle(row.extensionId!!, checked) }
                } else {
                    null
                },
                enabled = onToggle != null && !busy,
            )
        }
    }
}

@Composable
private fun InboxCard(
    candidate: InboxExtension,
    busy: Boolean,
    onImport: ((InboxExtension) -> Unit)?,
) {
    val colors = DshTheme.colors
    val shape = RoundedCornerShape(DshRadius.card)
    val decision = candidate.decision
    val importable = candidate.failure == null &&
        decision != null &&
        decision !is ExtensionTrust.Decision.Mismatch &&
        onImport != null &&
        !busy
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
                text = candidate.title ?: candidate.path.substringAfterLast('/'),
                style = DshType.bodyMedium,
                color = colors.labelPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            candidate.packageName?.let { packageName ->
                Text(
                    text = packageName,
                    style = DshType.code,
                    color = colors.labelTertiary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            candidate.apkVersionName?.takeIf { it.isNotBlank() }?.let { version ->
                Text(version, style = DshType.micro, color = colors.labelTertiary)
            }
            candidate.certSha256?.let { fingerprint ->
                Text(
                    text = "SHA-256 ${ExtensionTrust.display(fingerprint)}",
                    style = DshType.codeSmall,
                    color = colors.labelCaption,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                text = when {
                    candidate.failure != null -> candidate.failure
                    decision is ExtensionTrust.Decision.Mismatch ->
                        "Certificate does not match the pin"
                    decision is ExtensionTrust.Decision.Pinned -> "Known signing certificate"
                    decision is ExtensionTrust.Decision.FirstUse -> "New signing certificate"
                    else -> "Unread"
                } ?: "Unread",
                style = DshType.micro,
                color = if (candidate.failure != null ||
                    decision is ExtensionTrust.Decision.Mismatch
                ) {
                    colors.error
                } else {
                    colors.labelTertiary
                },
                maxLines = 4,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Spacer(Modifier.width(DshSpacing.md))
        if (importable) {
            ActionText("Import", { onImport?.invoke(candidate) })
        }
    }
}

@Composable
private fun TrustDialog(
    pending: PendingExtensionInstall,
    onTrust: (() -> Unit)?,
    onCancel: (() -> Unit)?,
) {
    AlertDialog(
        onDismissRequest = { onCancel?.invoke() },
        title = {
            Text(if (pending.update) "Trust this extension update?" else "Trust this extension?")
        },
        text = {
            Column {
                Text("${pending.title} (${pending.packageName})", style = DshType.bodyMedium)
                pending.apkVersionName?.takeIf { it.isNotBlank() }?.let { version ->
                    Text("Version $version", style = DshType.bodySmall)
                }
                Text(
                    "Signing certificate SHA-256:",
                    style = DshType.bodySmall,
                    modifier = Modifier.padding(top = DshSpacing.md),
                )
                Text(
                    ExtensionTrust.display(pending.certSha256),
                    style = DshType.codeSmall,
                )
                Text(
                    if (pending.update) {
                        "This package is already pinned to a certificate. Installing is " +
                            "allowed only because this APK presents the same one."
                    } else {
                        "This install will pin that certificate. Every later update must " +
                            "present the same one. The extension runs in this app's process " +
                            "with its cookies, files and host access."
                    },
                    style = DshType.bodySmall,
                    modifier = Modifier.padding(top = DshSpacing.md),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onTrust?.invoke() }, enabled = onTrust != null) {
                Text(if (pending.update) "Trust update" else "Trust and install")
            }
        },
        dismissButton = {
            TextButton(onClick = { onCancel?.invoke() }) {
                Text("Cancel")
            }
        },
    )
}

@Composable
private fun ActionText(label: String, onClick: () -> Unit) {
    Text(
        text = label,
        style = DshType.bodySmall,
        color = DshTheme.colors.link,
        maxLines = 1,
        modifier = Modifier
            .clip(RoundedCornerShape(DshRadius.sm))
            .clickableNoRipple(onClick = onClick)
            .padding(DshSpacing.sm),
    )
}
