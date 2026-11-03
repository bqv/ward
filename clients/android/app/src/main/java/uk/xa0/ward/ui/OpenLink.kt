package uk.xa0.ward.ui

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import java.net.URI
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

fun isWebLink(url: String): Boolean {
    val candidate = url.trim()
    if (candidate.isEmpty()) return false
    val parsed = runCatching { URI(candidate) }.getOrNull() ?: return false
    val scheme = parsed.scheme ?: return false
    if (!scheme.equals("http", ignoreCase = true) && !scheme.equals("https", ignoreCase = true)) {
        return false
    }
    return !parsed.host.isNullOrBlank()
}

private val pendingLink = androidx.compose.runtime.mutableStateOf<String?>(null)

fun openLink(context: Context, url: String) {
    if (isWebLink(url)) {
        pendingLink.value = url.trim()
        return
    }
    openExternally(context, url)
}

@Composable
fun LinkChoiceHost() {
    val url = pendingLink.value ?: return
    val context = LocalContext.current
    val colors = DshTheme.colors
    AlertDialog(
        onDismissRequest = { pendingLink.value = null },
        containerColor = colors.bgBase,
        title = {
            Text("Open link", color = colors.labelPrimary, style = DshType.titleMedium)
        },
        text = {
            Text(
                text = url,
                style = DshType.bodyMedium,
                color = colors.labelSecondary,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        },
        confirmButton = {
            TextButton(
                onClick = {
                    pendingLink.value = null
                    if (!launch(context, InternalBrowserActivity.intent(context, url))) {
                        unavailable(context, url)
                    }
                },
            ) {
                Text("In app", color = colors.accent)
            }
        },
        dismissButton = {
            Row {
                TextButton(
                    onClick = {
                        pendingLink.value = null
                        openExternally(context, url)
                    },
                ) {
                    Text("External app", color = colors.labelSecondary)
                }
                TextButton(onClick = { pendingLink.value = null }) {
                    Text("Cancel", color = colors.labelSecondary)
                }
            }
        },
    )
}

fun openExternally(context: Context, url: String) {
    val uri = runCatching { Uri.parse(url) }.getOrNull()
    if (uri == null || uri.scheme.isNullOrBlank()) {
        unavailable(context, url)
        return
    }
    if (!launch(context, Intent(Intent.ACTION_VIEW, uri))) unavailable(context, url)
}

private fun launch(context: Context, intent: Intent): Boolean {
    if (context !is Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    return try {
        context.startActivity(intent)
        true
    } catch (e: ActivityNotFoundException) {
        false
    } catch (e: SecurityException) {
        false
    }
}

private fun unavailable(context: Context, url: String) {
    val target = url.trim().ifEmpty { "this link" }
    Toast.makeText(context, "No app can open $target", Toast.LENGTH_SHORT).show()
}
