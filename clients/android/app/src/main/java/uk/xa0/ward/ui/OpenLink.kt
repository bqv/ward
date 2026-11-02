package uk.xa0.ward.ui

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import java.net.URI

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

fun openLink(context: Context, url: String) {
    if (isWebLink(url)) {
        val launched = launch(context, InternalBrowserActivity.intent(context, url))
        if (!launched) unavailable(context, url)
        return
    }
    openExternally(context, url)
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
