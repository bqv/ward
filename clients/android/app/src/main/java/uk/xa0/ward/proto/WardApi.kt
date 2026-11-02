/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

/**
 * Every method the frozen protocol declares, with ward's parameter names.
 *
 * The M0/M1 core refuses the methods it has not reached with `not_supported`;
 * a client that asks anyway gets the daemon's own error rather than a client-side
 * guess about which milestone is mounted. That is the protocol's contract and it
 * is why this file wraps the whole table rather than a subset.
 */
class WardApi(private val connection: WardConnection) {

    // ---------------------------------------------------------------- sessions

    suspend fun createSession(cwd: String? = null, model: String? = null): WardSession {
        val params = cborMap(
            *buildList {
                cwd?.takeIf { it.isNotBlank() }?.let { add("cwd" to it.cbor()) }
                model?.takeIf { it.isNotBlank() }?.let { add("model" to it.cbor()) }
            }.toTypedArray(),
        )
        return WardSessionParser.of(connection.call("session.create", params).map("session")!!)
    }

    suspend fun listSessions(limit: Int = 100, includeArchived: Boolean? = null): List<WardSession> {
        val params = cborMap(
            "limit" to limit.cbor(),
            *buildList {
                includeArchived?.let { add("include_archived" to it.cbor()) }
            }.toTypedArray(),
        )
        return WardSessionParser.list(connection.call("session.list", params))
    }

    suspend fun getSession(id: String): WardSession =
        WardSessionParser.of(connection.call("session.get", cborMap("id" to id.cbor())).map("session")!!)

    suspend fun resume(id: String, from: Long? = 0): WardSubscription = connection.resume(id, from)

    suspend fun prompt(id: String, text: String, timeoutMs: Long = 20 * 60_000): Cbor =
        connection.call("session.prompt", cborMap("id" to id.cbor(), "text" to text.cbor()), timeoutMs)

    suspend fun cancel(id: String): Boolean =
        connection.call("session.cancel", cborMap("id" to id.cbor())).bool("cancelled") ?: false

    suspend fun fork(id: String, from: Long? = null): WardSession {
        val params = cborMap(
            "id" to id.cbor(),
            *buildList { from?.let { add("from" to it.cbor()) } }.toTypedArray(),
        )
        return WardSessionParser.of(connection.call("session.fork", params).map("session")!!)
    }

    suspend fun rename(id: String, name: String): WardSession =
        WardSessionParser.of(
            connection.call("session.rename", cborMap("id" to id.cbor(), "name" to name.cbor())).map("session")!!,
        )

    suspend fun pin(id: String, pinned: Boolean): WardSession =
        WardSessionParser.of(
            connection.call("session.pin", cborMap("id" to id.cbor(), "pinned" to pinned.cbor())).map("session")!!,
        )

    suspend fun archive(id: String, archived: Boolean): WardSession =
        WardSessionParser.of(
            connection.call("session.archive", cborMap("id" to id.cbor(), "archived" to archived.cbor())).map("session")!!,
        )

    suspend fun search(query: String, limit: Int = 100): List<WardSession> =
        WardSessionParser.list(
            connection.call("session.search", cborMap("query" to query.cbor(), "limit" to limit.cbor())),
        )

    suspend fun compact(id: String): Cbor =
        connection.call("session.compact", cborMap("id" to id.cbor()), 20 * 60_000)

    suspend fun checkpoint(id: String, label: String? = null): Cbor {
        val params = cborMap(
            "id" to id.cbor(),
            *buildList { label?.let { add("label" to it.cbor()) } }.toTypedArray(),
        )
        return connection.call("session.checkpoint", params)
    }

    suspend fun undo(id: String, to: Long? = null): WardSession {
        val params = cborMap(
            "id" to id.cbor(),
            *buildList { to?.let { add("to" to it.cbor()) } }.toTypedArray(),
        )
        return WardSessionParser.of(connection.call("session.undo", params).map("session")!!)
    }

    suspend fun setModel(id: String, model: String): WardSession =
        WardSessionParser.of(
            connection.call("session.set_model", cborMap("id" to id.cbor(), "model" to model.cbor())).map("session")!!,
        )

    suspend fun setTools(id: String, tools: Cbor): Cbor =
        connection.call("session.set_tools", cborMap("id" to id.cbor(), "tools" to tools))

    suspend fun setPolicy(id: String, policy: Cbor): Cbor =
        connection.call("session.set_policy", cborMap("id" to id.cbor(), "policy" to policy))

    suspend fun upgrade(id: String): Cbor =
        connection.call("session.upgrade", cborMap("id" to id.cbor()))

    suspend fun resync(id: String): Cbor =
        connection.call("session.resync", cborMap("id" to id.cbor()))

    // ---------------------------------------------------------------- models

    suspend fun modelList(): List<Cbor.Map> =
        connection.call("model.list").map("models")?.asArray()?.items
            ?.mapNotNull { it as? Cbor.Map }.orEmpty()

    suspend fun selectModel(id: String): Cbor =
        connection.call("model.select", cborMap("id" to id.cbor()))

    suspend fun probeModel(id: String): Cbor =
        connection.call("model.probe", cborMap("id" to id.cbor()), 30_000)

    // ---------------------------------------------------------------- approvals

    suspend fun respondApproval(session: String, call: Long, epoch: Long, allow: Boolean): Boolean =
        connection.call(
            "approval.respond",
            cborMap(
                "session" to session.cbor(),
                "call" to call.cbor(),
                "epoch" to epoch.cbor(),
                "allow" to allow.cbor(),
            ),
            60_000,
        ).bool("resolved") ?: true

    // ---------------------------------------------------------------- stats

    suspend fun statsSession(id: String): Cbor.Map =
        connection.call("stats.session", cborMap("id" to id.cbor())).asMap()

    suspend fun statsGlobal(): Cbor.Map = connection.call("stats.global", Cbor.Map(emptyList())).asMap()

    // ---------------------------------------------------------------- attachments

    suspend fun addAttachment(id: String, name: String, data: Cbor): Cbor.Map =
        connection.call(
            "attachment.add",
            cborMap("id" to id.cbor(), "name" to name.cbor(), "data" to data),
            120_000,
        ).asMap()

    suspend fun listAttachments(id: String): List<Cbor.Map> =
        connection.call("attachment.list", cborMap("id" to id.cbor())).asArray().items
            .mapNotNull { it as? Cbor.Map }

    // ---------------------------------------------------------------- terminal

    suspend fun openTerminal(id: String, command: String? = null, rows: Int? = null, cols: Int? = null): Cbor {
        val params = cborMap(
            "id" to id.cbor(),
            *buildList {
                command?.let { add("command" to it.cbor()) }
                rows?.let { add("rows" to it.cbor()) }
                cols?.let { add("cols" to it.cbor()) }
            }.toTypedArray(),
        )
        return connection.call("terminal.open", params)
    }

    suspend fun writeTerminal(terminal: String, data: String): Long =
        connection.call(
            "terminal.write",
            cborMap("terminal" to terminal.cbor(), "data" to data.cbor()),
        ).long("written") ?: 0L

    suspend fun resizeTerminal(terminal: String, rows: Int, cols: Int): Boolean =
        connection.call(
            "terminal.resize",
            cborMap("terminal" to terminal.cbor(), "rows" to rows.cbor(), "cols" to cols.cbor()),
        ).bool("resized") ?: false

    suspend fun closeTerminal(terminal: String): Boolean =
        connection.call("terminal.close", cborMap("terminal" to terminal.cbor())).bool("closed") ?: false

    // ---------------------------------------------------------------- jobs

    suspend fun listJobs(): List<Cbor.Map> =
        connection.call("job.list", Cbor.Map(emptyList())).map("jobs")?.asArray()?.items
            ?.mapNotNull { it as? Cbor.Map }.orEmpty()

    suspend fun killJob(job: String): Boolean =
        connection.call("job.kill", cborMap("job" to job.cbor())).bool("killed") ?: false

    // ---------------------------------------------------------------- schedules

    suspend fun listSchedules(): List<Cbor.Map> =
        connection.call("schedule.list", Cbor.Map(emptyList())).map("schedules")?.asArray()?.items
            ?.mapNotNull { it as? Cbor.Map }.orEmpty()

    suspend fun setSchedule(session: String, cron: String, text: String): Cbor =
        connection.call("schedule.set", cborMap(
            "session" to session.cbor(),
            "cron" to cron.cbor(),
            "text" to text.cbor(),
        ))

    suspend fun deleteSchedule(schedule: String): Boolean =
        connection.call("schedule.delete", cborMap("schedule" to schedule.cbor())).bool("deleted") ?: false

    // ---------------------------------------------------------------- goals

    suspend fun setGoal(session: String, text: String): Cbor =
        connection.call("goal.set", cborMap("session" to session.cbor(), "text" to text.cbor()))

    suspend fun clearGoal(session: String): Cbor =
        connection.call("goal.clear", cborMap("session" to session.cbor()))

    // ---------------------------------------------------------------- subagents

    suspend fun promptSubagent(session: String, text: String, timeoutMs: Long = 20 * 60_000): Cbor =
        connection.call(
            "subagent.prompt",
            cborMap("session" to session.cbor(), "text" to text.cbor()),
            timeoutMs,
        )

    suspend fun interruptSubagent(session: String): Boolean =
        connection.call("subagent.interrupt", cborMap("session" to session.cbor())).bool("interrupted") ?: false

    // ---------------------------------------------------------------- modules and files

    suspend fun listModules(): List<Cbor.Map> =
        connection.call("module.list", Cbor.Map(emptyList())).map("modules")?.asArray()?.items
            ?.mapNotNull { it as? Cbor.Map }.orEmpty()

    suspend fun reloadModule(name: String): Cbor =
        connection.call("module.reload", cborMap("name" to name.cbor()))

    suspend fun readFile(path: String): Cbor =
        connection.call("file.read", cborMap("path" to path.cbor()), 120_000)

    suspend fun uploadFile(path: String, data: Cbor): Cbor =
        connection.call("file.upload", cborMap("path" to path.cbor(), "data" to data), 120_000)

    // ---------------------------------------------------------------- settings and policy

    suspend fun getSettings(): Cbor.Map =
        connection.call("settings.get", Cbor.Map(emptyList())).asMap()

    suspend fun setSettings(settings: Cbor): Cbor =
        connection.call("settings.set", cborMap("settings" to settings))

    suspend fun checkPolicy(tool: String, payload: Cbor): Cbor =
        connection.call("policy.check", cborMap("tool" to tool.cbor(), "payload" to payload))

    suspend fun previewPrompt(id: String): Cbor =
        connection.call("prompt.preview", cborMap("id" to id.cbor()))

    suspend fun recall(query: String, k: Int = 5): Cbor =
        connection.call("recall.search", cborMap("query" to query.cbor(), "k" to k.cbor()), 30_000)

    suspend fun mcp(request: String): Cbor =
        connection.call("mcp.call", cborMap("request" to request.cbor()), 60_000)

    // ---------------------------------------------------------------- devices

    suspend fun pairDevice(scope: String = "phone"): Cbor =
        connection.call("device.pair", cborMap("scope" to scope.cbor()))

    suspend fun enrolDevice(token: String, csr: String): Cbor =
        connection.call("device.enrol", cborMap("token" to token.cbor(), "csr" to csr.cbor()))

    suspend fun listDevices(): List<Cbor.Map> =
        connection.call("device.list", Cbor.Map(emptyList())).map("devices")?.asArray()?.items
            ?.mapNotNull { it as? Cbor.Map }.orEmpty()

    suspend fun revokeDevice(serial: String): Boolean =
        connection.call("device.revoke", cborMap("serial" to serial.cbor())).bool("revoked") ?: false
}
