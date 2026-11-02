/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.net

import org.json.JSONArray
import org.json.JSONObject
import uk.xa0.ward.proto.Cbor
import uk.xa0.ward.proto.WardApi
import uk.xa0.ward.proto.WardConnection
import uk.xa0.ward.proto.WardSession
import java.util.Base64
import java.util.concurrent.ConcurrentHashMap

/**
 * The DSH-shaped compatibility bridge over ward's method table.
 *
 * The copied screens speak DSH's JSON envelopes. Ward has a smaller, differently
 * shaped surface, so this is the one place that translates between them: a ward
 * session becomes a DSH list row, a ward approval becomes the `$events` waterfall
 * the attention centre already understands, and a ward prompt is assembled from
 * DSH's content parts. Everything ward serves is reachable from here; everything
 * ward does not serve answers an empty shape or a named refusal rather than
 * inventing state.
 */
class WardBridge(private val connection: WardConnection) {
    private val api = WardApi(connection)

    private data class ModelRoute(val provider: String, val model: String, val catalogId: String)
    private data class ApprovalRef(val session: String, val call: Long, val epoch: Long)

    private val models = ConcurrentHashMap<String, ModelRoute>()
    private val approvals = ConcurrentHashMap<String, ApprovalRef>()
    private val terminals = ConcurrentHashMap<String, JSONObject>()
    private val subagentParents = ConcurrentHashMap<String, String>()

    fun rememberApproval(eventId: String, session: String, call: Long, epoch: Long) {
        approvals[eventId] = ApprovalRef(session, call, epoch)
    }

    fun forgetApproval(eventId: String) {
        approvals.remove(eventId)
    }

    fun noteSubagent(parent: String, child: String) {
        if (parent.isNotEmpty() && child.isNotEmpty()) subagentParents[child] = parent
    }

    suspend fun call(method: String, args: JSONObject): Any? {
        val request = args.optJSONObject("request")
            ?: args.optJSONObject("_request")
            ?: args
        return when (method) {
            "session/list" -> JSONObject().put("items", sessionRows(
                api.listSessions(
                    limit = request.optInt("limit", 1000).coerceIn(1, 1000),
                    includeArchived = true,
                ).map { it },
            ))

            "session/create" -> {
                val route = routeOf(request.optString("model"))
                val session = api.createSession(
                    cwd = request.optString("cwd").takeIf { it.isNotBlank() },
                    model = route,
                )
                JSONObject().put("sessionId", session.id)
            }

            "session/get" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("sessionId", api.getSession(id).id)
            }

            "session/prompt" -> prompt(request)

            "session/cancel" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("cancelled", api.cancel(id))
            }

            "session/fork" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                val from = request.optLong("atSeq", 0L).takeIf { it > 0 }
                JSONObject().put("sessionId", api.fork(id, from).id)
            }

            "session/rename" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                val title = request.optString("title").ifEmpty { request.optString("name") }
                JSONObject().put("sessionId", api.rename(id, title).id)
            }

            "session/search" -> {
                val query = request.optString("query")
                val hits = if (query.isBlank()) emptyList() else api.search(query, 20)
                JSONObject()
                    .put("items", JSONArray(hits.map {
                        JSONObject().put("sessionId", it.id).put("snippet", it.name ?: it.cwd)
                    }))
                    .put("hasMore", false)
            }

            "session/compact" -> api.compact(request.optString("sessionId").ifEmpty { request.optString("id") })
                .toJson()

            "session/checkpoint" -> api.checkpoint(
                request.optString("sessionId").ifEmpty { request.optString("id") },
                request.optString("label").takeIf { it.isNotBlank() },
            ).toJson()

            "session/modelCatalog" -> modelCatalog()

            "session/selectModel" -> selectModel(request)

            "workspace/archiveSession" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("sessionId", api.archive(id, true).id)
            }

            "workspace/unarchiveSession" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("sessionId", api.archive(id, false).id)
            }

            "workspace/pinSession" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("sessionId", api.pin(id, true).id)
            }

            "workspace/unpinSession" -> {
                val id = request.optString("sessionId").ifEmpty { request.optString("id") }
                JSONObject().put("sessionId", api.pin(id, false).id)
            }

            "subagents/prompt" -> {
                val session = request.optString("sessionId").ifEmpty { request.optString("parentSessionId") }
                val text = textOf(request.optJSONArray("content"))
                api.promptSubagent(session, text).toJson()
            }

            "subagents/interruptByParent" -> {
                val session = request.optString("sessionId").ifEmpty { request.optString("childSessionId") }
                JSONObject().put("interrupted", api.interruptSubagent(session))
            }

            "job/list" -> {
                val session = request.optString("sessionId")
                val jobs = api.listJobs().filter { session.isEmpty() || it.text("session") == session }
                JSONObject().put("jobs", JSONArray(jobs.map { jobRow(it) }))
            }
            "job/kill" -> JSONObject().put(
                "killed",
                api.killJob(
                    request.optString("job").ifEmpty { request.optString("jobId") }
                        .ifEmpty { request.optString("id") },
                ),
            )

            "schedule/catalog" -> JSONArray(api.listSchedules().map { scheduleRow(it) })
            "schedule/history" -> JSONObject().put("items", JSONArray())
            "schedule/delete" -> JSONObject().put("deleted", api.deleteSchedule(
                request.optString("schedule").ifEmpty { request.optString("id") },
            ))

            "fileUploads/upload" -> upload(request)

            "\$events/result" -> answerApproval(args)

            "terminal/environment" -> terminalEnvironment(
                args.optString("agentId").ifEmpty { args.optString("sessionId") }
                    .ifEmpty { request.optString("agentId") },
            )

            "terminal/shells" -> JSONArray().put(JSONObject()
                .put("path", "sh")
                .put("args", JSONArray().put("-i"))
                .put("name", "Shell"))

            "terminal/list" -> {
                val owner = args.optString("sessionId").ifEmpty { args.optString("agentId") }
                    .ifEmpty { request.optString("sessionId") }
                JSONArray(terminals.values.filter { it.optString("owner") == owner })
            }

            "terminal/create" -> createTerminal(
                args.optString("agentId").ifEmpty { args.optString("sessionId") },
                args.optJSONObject("request") ?: request,
            )

            "terminal/write" -> JSONObject().put(
                "written",
                api.writeTerminal(
                    terminalIdOf(args),
                    args.optString("data").ifEmpty { request.optString("data") },
                ),
            )

            "terminal/resize" -> JSONObject().put(
                "resized",
                api.resizeTerminal(
                    terminalIdOf(args),
                    args.optInt("rows", 24),
                    args.optInt("cols", 80),
                ),
            )

            "terminal/close" -> {
                val terminal = terminalIdOf(args)
                terminals.remove(terminal)
                JSONObject().put("closed", api.closeTerminal(terminal))
            }

            "terminal/rename" -> JSONObject()

            // Ward has no Workspace registry, plugin bundles, command catalogue or
            // settings document. These empty shapes are the honest answer: the
            // screens render "none configured" rather than an error banner that
            // would be about the app, not the host.
            "settings/describe" -> JSONObject()
                .put("settings", JSONObject())
                .put("localProviders", JSONObject())
            "settings/update" -> JSONObject()
            "pluginManager/listBundles" -> JSONArray(api.listModules().map { moduleRow(it) })
            "pluginManager/setBundleEnabled" -> {
                val name = request.optString("name")
                if (request.optBoolean("enabled", false) && name.isNotEmpty()) {
                    api.reloadModule(name).toJson()
                } else {
                    JSONObject()
                }
            }
            "commands/list" -> JSONArray()
            "agentPresets/list" -> JSONArray()
            "permissionPresets/catalog" -> permissionCatalog()
            "commands/execute" -> runCommand(args)
            "directoryPicker/list" -> JSONObject()
                .put("path", request.optString("path"))
                .put("home", request.optString("path"))
                .put("crumbs", JSONArray())
                .put("entries", JSONArray())
                .put("truncated", false)
            "workspaceFiles/list" -> listFiles(request)
            "workspaceFiles/read" -> readFileText(request)
            "workspaceFiles/stat" -> JSONObject()
            "schedule/set" -> JSONObject().put(
                "schedule",
                api.setSchedule(
                    request.optString("sessionId").ifEmpty { request.optString("session") },
                    request.optString("cron"),
                    request.optString("text"),
                ).toJson(),
            )
            "goals/clear" -> api.clearGoal(
                request.optString("sessionId").ifEmpty { request.optString("session") },
            ).toJson()
            "goals/edit", "goals/pause", "goals/resume" -> JSONObject()
            "session/updateQueue" -> JSONObject()
            "session/attachment" -> JSONObject()
            "llm/discoverModels", "localRouter/status" -> JSONObject()
            else -> throw DshRpcException(
                "not_supported",
                "ward does not serve $method",
                null,
            )
        }
    }

    suspend fun callBytes(method: String, args: JSONObject): ByteArray {
        if (method != "workspaceFiles/readBytes") return ByteArray(0)
        val request = args.optJSONObject("request") ?: args
        val session = request.optString("workspaceFileScopeId").ifEmpty { request.optString("sessionId") }
        val path = request.optString("path")
        if (session.isBlank() || path.isBlank()) return ByteArray(0)
        val range = request.optJSONObject("options")?.optJSONObject("range")
            ?: request.optJSONObject("range")
        val offset = range?.optLong("offset", 0L) ?: 0L
        val asked = range?.optInt("length", 0)?.takeIf { it > 0 } ?: DEFAULT_BLOB_BYTES
        val length = asked.coerceIn(1, MAX_BLOB_REQUEST)
        val chunks = java.io.ByteArrayOutputStream(length)
        var at = offset
        var remaining = length
        while (remaining > 0) {
            val want = minOf(remaining, MAX_BLOB_BYTES)
            val result = api.readFileBlob(session, path, at, want)
            val data = result.bytes("data") ?: break
            if (data.isEmpty()) break
            chunks.write(data)
            at += data.size
            remaining -= data.size
            if (data.size < want) break
        }
        return multipartBytes(chunks.toByteArray())
    }

    private suspend fun listFiles(request: JSONObject): JSONObject {
        val session = request.optString("workspaceFileScopeId").ifEmpty { request.optString("sessionId") }
        val raw = request.optString("path")
        val path = if (raw.isEmpty() || raw == ".") "" else raw
        val result = api.listFiles(session, path)
        val entries = JSONArray()
        result.array("entries")?.items?.forEach { item ->
            val entry = (item as? Cbor.Map) ?: return@forEach
            entries.put(JSONObject()
                .put("name", entry.text("name").orEmpty())
                .put("type", when (entry.text("kind")) {
                    "dir" -> "directory"
                    "file" -> "file"
                    else -> "other"
                })
                .put("size", entry.long("size") ?: 0L))
        }
        return JSONObject()
            .put("path", result.text("path") ?: raw.ifEmpty { "." })
            .put("entries", entries)
            .put("truncated", result.bool("truncated") ?: false)
    }

    private suspend fun readFileText(request: JSONObject): JSONObject {
        val session = request.optString("workspaceFileScopeId").ifEmpty { request.optString("sessionId") }
        val path = request.optString("path")
        val result = api.readFileText(session, path)
        val text = result.text("text").orEmpty()
        return JSONObject()
            .put("text", text)
            .put("lines", text.count { it == '\n' } + if (text.endsWith('\n')) 0 else 1)
            .put("eof", true)
            .put("bytes", result.long("size") ?: text.toByteArray(Charsets.UTF_8).size.toLong())
            .put("absolutePath", path)
    }

    private fun multipartBytes(data: ByteArray): ByteArray {
        val head = "--ward\r\nContent-Disposition: form-data; name=\"bytes-0\"\r\n\r\n"
            .toByteArray(Charsets.ISO_8859_1)
        val tail = "\r\n--ward--\r\n".toByteArray(Charsets.ISO_8859_1)
        return head + data + tail
    }

    private suspend fun prompt(request: JSONObject): JSONObject {
        val session = request.optString("sessionId").ifEmpty { request.optString("id") }
        val content = request.optJSONArray("content")
        val text = textOf(content)
        content?.let { parts ->
            for (i in 0 until parts.length()) {
                val part = parts.optJSONObject(i) ?: continue
                if (part.optString("type") == "image") {
                    val data = part.optString("data")
                    if (data.isNotEmpty()) {
                        runCatching {
                            api.addAttachment(
                                session,
                                part.optString("name").ifEmpty { "image" },
                                Cbor.Bytes(Base64.getDecoder().decode(data)),
                            )
                        }
                    }
                }
            }
        }
        api.prompt(session, text)
        return JSONObject().put("sessionId", session)
    }

    private suspend fun upload(request: JSONObject): JSONObject {
        val session = request.optString("agentId").ifEmpty { request.optString("sessionId") }
        val body = request.optJSONObject("request") ?: request
        val name = body.optString("name").ifEmpty { "file" }
        val data = body.optString("data")
        if (session.isNotBlank() && data.isNotEmpty()) {
            api.addAttachment(session, name, Cbor.Bytes(Base64.getDecoder().decode(data)))
        }
        return JSONObject().put("receiptId", name)
    }

    private suspend fun answerApproval(args: JSONObject): JSONObject {
        val eventId = args.optString("eventId")
        val ref = approvals.remove(eventId) ?: return JSONObject()
        val outcome = args.optJSONObject("outcome")
        val allow = outcome?.optString("value") != "rejected"
        api.respondApproval(ref.session, ref.call, ref.epoch, allow)
        return JSONObject().put("resolved", true)
    }

    private suspend fun modelCatalog(): JSONObject {
        val entries = api.modelList()
        models.clear()
        val groups = LinkedHashMap<String, MutableList<JSONObject>>()
        for (entry in entries) {
            val provider = entry.text("provider").orEmpty().ifEmpty { "ward" }
            val model = entry.text("model").orEmpty().ifEmpty { entry.text("id").orEmpty() }
            val catalogId = entry.text("id").orEmpty().ifEmpty { model }
            models["$provider|$model"] = ModelRoute(provider, model, catalogId)
            groups.getOrPut(provider) { mutableListOf() } += JSONObject()
                .put("id", model)
                .put("name", model)
                .put("reasoning", JSONObject().put("efforts", JSONArray()))
        }
        val array = JSONArray()
        groups.forEach { (provider, rows) ->
            array.put(JSONObject().put("id", provider).put("name", provider).put("models", JSONArray(rows)))
        }
        val first = models.values.firstOrNull()
        return JSONObject()
            .put("groups", array)
            .put(
                "default",
                if (first == null) JSONObject.NULL else JSONObject()
                    .put("provider", first.provider)
                    .put("model", first.model),
            )
    }

    private suspend fun selectModel(request: JSONObject): JSONObject {
        val session = request.optString("sessionId").ifEmpty { request.optString("id") }
        val provider = request.optString("provider")
        val model = request.optString("model")
        val catalogId = models["$provider|$model"]?.catalogId ?: model
        api.setModel(session, catalogId)
        return JSONObject()
            .put("selected", JSONObject().put("provider", provider).put("model", model))
    }

    /**
     * Ward's three presets behind the DSH permission chip's names. The mapping is
     * not cosmetic: `default` asks before a write, `reads-only` denies it and
     * `workspace` asks too but also requires a read before an overwrite. There is
     * no full-access preset in ward, so that DSH option is not offered.
     */
    private suspend fun permissionCatalog(): JSONObject {
        val policy = api.policyList()
        val options = JSONArray()
        policy.array("presets")?.items?.forEach { item ->
            val preset = item as? Cbor.Map ?: return@forEach
            val id = preset.text("id").orEmpty()
            if (id.isEmpty()) return@forEach
            options.put(JSONObject()
                .put("value", dshPresetOf(id))
                .put("name", preset.text("name").orEmpty().ifEmpty { dshPresetOf(id) })
                .put("description", preset.text("description").orEmpty()))
        }
        return JSONObject()
            .put("options", options)
            .put("current", dshPresetOf(policy.text("current").orEmpty().ifEmpty { "default" }))
    }

    private suspend fun runCommand(args: JSONObject): JSONObject {
        val line = args.optString("line").trim()
        val preset = line.removePrefix("/permission ").trim()
        if (line.startsWith("/permission ") && preset.isNotEmpty()) {
            val wardPreset = wardPresetOf(preset)
                ?: return JSONObject().put("result", JSONObject()
                    .put("kind", "error")
                    .put("text", "ward has no $preset access mode"))
            api.policySelect(wardPreset)
            return JSONObject().put("result", JSONObject()
                .put("kind", "text")
                .put("text", "Access mode: ${dshPresetOf(wardPreset)}"))
        }
        if (line == "/plan off") {
            return JSONObject().put("result", JSONObject()
                .put("kind", "text")
                .put("text", "Plan mode is not a ward switch"))
        }
        return JSONObject().put("result", JSONObject()
            .put("kind", "error")
            .put("text", "ward does not serve host command $line"))
    }

    private fun dshPresetOf(ward: String): String = when (ward) {
        "reads-only" -> "read-only"
        "workspace" -> "workspace-write"
        "default" -> "default"
        else -> ward
    }

    private fun wardPresetOf(dsh: String): String? = when (dsh) {
        "read-only", "reads-only" -> "reads-only"
        "workspace-write", "workspace" -> "workspace"
        "default" -> "default"
        else -> null
    }

    private suspend fun terminalEnvironment(sessionId: String): JSONObject {
        val session = api.getSession(sessionId)
        return JSONObject()
            .put("cwd", session.cwd)
            .put("maxInputBytes", 64 * 1024)
            .put("maxCols", 500)
            .put("maxRows", 200)
            .put("scrollback", 1000)
    }

    private suspend fun createTerminal(sessionId: String, request: JSONObject): JSONObject {
        val cols = request.optInt("cols", 80).coerceIn(1, 500)
        val rows = request.optInt("rows", 24).coerceIn(1, 200)
        val shell = request.optString("shellPath").takeIf { it.isNotBlank() } ?: "sh"
        val opened = api.openTerminal(sessionId, shell, rows, cols)
        val id = opened.text("terminal").orEmpty()
        android.util.Log.d("WardBridge", "terminal/open session=$sessionId shell=$shell id=$id")
        val info = JSONObject()
            .put("id", id)
            .put("title", "Shell")
            .put("shell", JSONObject().put("path", shell).put("args", JSONArray()))
            .put("cwd", "")
            .put("cols", cols)
            .put("rows", rows)
            .put("state", "running")
            .put("owner", sessionId)
        if (id.isNotEmpty()) terminals[id] = info
        return info
    }

    private fun terminalIdOf(request: JSONObject): String =
        request.optString("id").ifEmpty { request.optString("terminalId") }.ifEmpty { request.optString("terminal") }

    private fun moduleRow(item: Cbor.Map): JSONObject {
        val name = item.text("name").orEmpty()
        val valid = item.bool("valid") != false
        val row = JSONObject()
            .put("name", name)
            .put("version", item.text("version").orEmpty())
            .put("meta", JSONObject().put("title", name))
            .put("enabled", valid)
            .put("installed", true)
            .put("optional", false)
            .put("readOnlyReason", "ward reloads modules as one registry; there is no per-module switch")
        if (!valid) {
            row.put("error", JSONObject()
                .put("code", "invalid")
                .put("diagnostic", "the module failed its registry validation"))
        }
        return row
    }

    private fun jobRow(item: Cbor.Map): JSONObject {
        val state = item.text("state").orEmpty().ifEmpty { "running" }
        return JSONObject()
            .put("id", item.text("job").orEmpty())
            .put("kind", "turn")
            .put("label", "turn")
            .put("owner", item.text("session").orEmpty())
            .put("status", state)
            .put("startedAt", 0L)
            .put("output", JSONObject().put("total", 0))
    }

    private fun scheduleRow(item: Cbor.Map): JSONObject {
        val next = item.long("next_ms") ?: 0L
        val at = if (next > 0) java.time.Instant.ofEpochMilli(next).toString() else ""
        return JSONObject()
            .put("id", item.text("schedule").orEmpty())
            .put("sessionId", item.text("session").orEmpty())
            .put("title", item.text("cron").orEmpty())
            .put("prompt", "")
            .put("status", "active")
            .put("kind", "cron")
            .put("expression", item.text("cron").orEmpty())
            .put("scheduledAt", at)
    }

    private fun routeOf(model: String): String? {
        if (model.isBlank()) return null
        return models.values.firstOrNull { "${it.provider}|${it.model}" == model }?.catalogId
            ?: models.values.firstOrNull { it.model == model }?.catalogId
            ?: model
    }

    private fun sessionRows(sessions: List<WardSession>): JSONArray {
        val out = JSONArray()
        sessions.forEach { session ->
            val values = JSONObject().put("title", session.name ?: "")
            val selected = models.values.firstOrNull { it.catalogId == session.model }
            if (selected != null) {
                values.put(
                    "modelSelection",
                    JSONObject().put(
                        "next",
                        JSONObject().put("provider", selected.provider).put("model", selected.model),
                    ),
                )
            }
            val parent = subagentParents[session.id]
            out.put(JSONObject()
                .put("sessionId", session.id)
                .put("cwd", session.cwd)
                .put("updatedAt", session.updatedMs ?: session.created)
                .put("running", false)
                .put("agentAvailable", true)
                .put("blank", false)
                .put("origin", if (parent != null) "subagent" else "agent")
                .put("archived", session.archived)
                .put("pinned", session.pinned)
                .apply { if (parent != null) put("parentSessionId", parent) }
                .put("projections", JSONObject().put("values", values)))
        }
        return out
    }

    private companion object {
        const val DEFAULT_BLOB_BYTES = 256 * 1024
        const val MAX_BLOB_BYTES = 1024 * 1024
        const val MAX_BLOB_REQUEST = 4 * 1024 * 1024
    }

    private fun textOf(content: JSONArray?): String {
        if (content == null) return ""
        val builder = StringBuilder()
        for (i in 0 until content.length()) {
            val part = content.optJSONObject(i) ?: continue
            if (part.optString("type") == "text") {
                if (builder.isNotEmpty()) builder.append('\n')
                builder.append(part.optString("text"))
            }
        }
        return builder.toString()
    }

    private fun Cbor.toJson(): Any = when (this) {
        is Cbor.Map -> JSONObject().also { out ->
            entries.forEach { (key, value) -> out.put(key, value.toJson()) }
        }
        is Cbor.Array -> JSONArray(items.map { it.toJson() })
        is Cbor.Text -> value
        is Cbor.UInt -> value
        is Cbor.NInt -> value
        is Cbor.Bool -> value
        is Cbor.Bytes -> Base64.getEncoder().encodeToString(value)
        Cbor.Null -> JSONObject.NULL
    }

    private operator fun JSONObject.get(key: String): Any? = opt(key)
}
