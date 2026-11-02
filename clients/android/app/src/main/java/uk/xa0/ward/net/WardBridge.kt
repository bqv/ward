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

    fun rememberApproval(eventId: String, session: String, call: Long, epoch: Long) {
        approvals[eventId] = ApprovalRef(session, call, epoch)
    }

    fun forgetApproval(eventId: String) {
        approvals.remove(eventId)
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

            "job/list" -> JSONObject().put("jobs", JSONArray(api.listJobs().map { it.toJson() }))
            "job/kill" -> JSONObject().put("killed", api.killJob(request.optString("job").ifEmpty { request.optString("id") }))

            "schedule/catalog" -> JSONArray()
            "schedule/history" -> JSONObject().put("items", JSONArray())
            "schedule/delete" -> JSONObject().put("deleted", api.deleteSchedule(
                request.optString("schedule").ifEmpty { request.optString("id") },
            ))

            "fileUploads/upload" -> upload(request)

            "\$events/result" -> answerApproval(args)

            "terminal/create" -> api.openTerminal(
                request.optString("sessionId").ifEmpty { request.optString("agentId") },
                request.optString("command").takeIf { it.isNotBlank() },
                request.optInt("rows", 0).takeIf { it > 0 },
                request.optInt("cols", 0).takeIf { it > 0 },
            ).toJson()

            "terminal/write" -> JSONObject().put(
                "written",
                api.writeTerminal(
                    request.optString("terminalId").ifEmpty { request.optString("terminal") },
                    request.optString("data"),
                ),
            )

            "terminal/resize" -> JSONObject().put(
                "resized",
                api.resizeTerminal(
                    request.optString("terminalId").ifEmpty { request.optString("terminal") },
                    request.optInt("rows", 24),
                    request.optInt("cols", 80),
                ),
            )

            "terminal/close" -> JSONObject().put(
                "closed",
                api.closeTerminal(request.optString("terminalId").ifEmpty { request.optString("terminal") }),
            )

            // Ward has no Workspace registry, plugin bundles, command catalogue or
            // settings document. These empty shapes are the honest answer: the
            // screens render "none configured" rather than an error banner that
            // would be about the app, not the host.
            "settings/describe" -> JSONObject()
                .put("settings", JSONObject())
                .put("localProviders", JSONObject())
            "settings/update" -> JSONObject()
            "pluginManager/listBundles" -> JSONArray()
            "pluginManager/setBundleEnabled" -> JSONObject()
            "commands/list" -> JSONArray()
            "agentPresets/list" -> JSONArray()
            "permissionPresets/catalog" -> JSONObject().put("presets", JSONArray())
            "directoryPicker/list" -> JSONObject()
                .put("path", request.optString("path"))
                .put("home", request.optString("path"))
                .put("crumbs", JSONArray())
                .put("entries", JSONArray())
                .put("truncated", false)
            "workspaceFiles/list" -> JSONObject()
                .put("path", request.optString("path"))
                .put("entries", JSONArray())
                .put("truncated", false)
            "workspaceFiles/read" -> JSONObject().put("text", "")
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
        val path = request.optString("path")
        if (path.isBlank()) return ByteArray(0)
        val result = api.readFile(path).asMap()
        return result.bytes("data") ?: result.bytes("bytes") ?: ByteArray(0)
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
            out.put(JSONObject()
                .put("sessionId", session.id)
                .put("cwd", session.cwd)
                .put("updatedAt", session.updatedMs ?: session.created)
                .put("running", false)
                .put("agentAvailable", true)
                .put("blank", false)
                .put("origin", "agent")
                .put("projections", JSONObject().put("values", values)))
        }
        return out
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
