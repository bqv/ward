/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.data

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/**
 * Ward has no host Workspace registry, so the Android client owns one.
 *
 * A Workspace is a path plus a title and an order. Membership is still derived
 * from each session's `cwd`, because that is the only membership fact Ward has;
 * the registry is what makes an empty Workspace, a title and a manual order
 * survive a restart instead of being rebuilt from whichever sessions exist.
 */
class LocalWorkspaceStore(context: Context) {
    data class Entry(val path: String, val title: String)

    private val prefs = context.applicationContext
        .getSharedPreferences("ward.workspaces", Context.MODE_PRIVATE)

    @Synchronized
    fun entries(): List<Entry> = decode(prefs.getString(KEY, null))

    @Synchronized
    fun add(path: String, title: String? = null): Entry {
        val normalized = normalize(path) ?: return Entry("", "")
        val current = entries().toMutableList()
        val existing = current.firstOrNull { it.path == normalized }
        if (existing != null) return existing
        val entry = Entry(normalized, title?.trim()?.takeIf { it.isNotEmpty() } ?: basename(normalized))
        current += entry
        save(current)
        return entry
    }

    @Synchronized
    fun rename(path: String, title: String): Boolean {
        val normalized = normalize(path) ?: return false
        val current = entries().toMutableList()
        val at = current.indexOfFirst { it.path == normalized }
        if (at < 0) return false
        current[at] = current[at].copy(title = title.trim())
        save(current)
        return true
    }

    @Synchronized
    fun delete(path: String): Boolean {
        val normalized = normalize(path) ?: return false
        val current = entries().toMutableList()
        val removed = current.removeAll { it.path == normalized }
        if (removed) save(current)
        return removed
    }

    @Synchronized
    fun move(path: String, before: String?): List<String> {
        val normalized = normalize(path) ?: return entries().map { it.path }
        val current = entries().toMutableList()
        val moving = current.firstOrNull { it.path == normalized } ?: return current.map { it.path }
        current.removeAll { it.path == normalized }
        val at = before?.let { anchor -> current.indexOfFirst { it.path == normalize(anchor) } } ?: -1
        if (at < 0) current += moving else current.add(at, moving)
        save(current)
        return current.map { it.path }
    }

    fun titleOf(path: String): String? =
        entries().firstOrNull { it.path == normalize(path) }?.title

    private fun save(entries: List<Entry>) {
        val array = JSONArray()
        entries.forEach { array.put(JSONObject().put("path", it.path).put("title", it.title)) }
        prefs.edit().putString(KEY, array.toString()).apply()
    }

    private fun decode(raw: String?): List<Entry> {
        if (raw.isNullOrBlank()) return emptyList()
        val array = runCatching { JSONArray(raw) }.getOrNull() ?: return emptyList()
        return (0 until array.length()).mapNotNull { index ->
            val row = array.optJSONObject(index) ?: return@mapNotNull null
            val path = normalize(row.optString("path")) ?: return@mapNotNull null
            Entry(path, row.optString("title").ifEmpty { basename(path) })
        }
    }

    private fun normalize(raw: String): String? {
        val trimmed = raw.trim()
        if (trimmed.isEmpty()) return null
        val without = trimmed.trimEnd('/')
        return if (without.isEmpty()) "/" else without
    }

    private fun basename(path: String): String =
        path.trimEnd('/').substringAfterLast('/').ifEmpty { path }

    private companion object {
        const val KEY = "entries"
    }
}
