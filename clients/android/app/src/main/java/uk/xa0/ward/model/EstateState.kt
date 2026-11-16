package uk.xa0.ward.model

import org.json.JSONArray
import org.json.JSONObject

const val ESTATE_STATUS_METHOD: String = "estate.status"

const val ESTATE_POLL_MS: Long = 2_000

data class EstateHealth(
    val degraded: Boolean,
    val fails: Int,
    val orphaned: Boolean,
    val restarting: Boolean,
    val restarts: Int,
)

data class EstateCgroup(
    val currentBytes: Long?,
    val highBytes: Long?,
    val maxBytes: Long?,
    val oomKills: Long?,
)

data class EstateSlot(
    val name: String,
    val up: Boolean,
    val booting: Boolean,
    val policy: String?,
    val group: String?,
    val pid: Long?,
    val uptimeS: Double?,
    val inFlight: Int,
    val restarts: Int,
    val listen: String?,
    val residentModels: List<String>,
    val cgroup: EstateCgroup?,
    val state: String? = null,
    val model: String? = null,
    val device: String? = null,
    val kind: String? = null,
    val nCtx: Int? = null,
    val nParams: Long? = null,
    val sizeBytes: Long? = null,
    val quant: String? = null,
    val alias: String? = null,
    val vramMb: Double? = null,
    val lastError: String? = null,
    val lastTransition: String? = null,
)

data class EstateStatus(
    val version: String?,
    val listen: String?,
    val vramUsedGib: Double?,
    val idleTimeoutS: Double?,
    val mediaInFlight: Int,
    val health: EstateHealth,
    val slots: List<EstateSlot>,
    val loads: List<EstateLoad>,
)

fun parseEstateStatus(reply: JSONObject): EstateStatus = EstateStatus(
    version = reply.str("version").takeIf { it.isNotEmpty() },
    listen = reply.str("listen").takeIf { it.isNotEmpty() },
    vramUsedGib = reply.doubleOrNull("vram_used_gib"),
    idleTimeoutS = reply.doubleOrNull("idle_timeout_s"),
    mediaInFlight = reply.int("media_in_flight"),
    health = parseHealth(reply.obj("llm_health") ?: reply.obj("health")),
    slots = parseSlots(reply.arr("tenants")),
    loads = estateLoads(reply),
)

private fun parseHealth(health: JSONObject?): EstateHealth = EstateHealth(
    degraded = health.bool("degraded"),
    fails = health.int("fails"),
    orphaned = health.bool("orphaned"),
    restarting = health.bool("restarting"),
    restarts = health.int("restarts"),
)

private fun parseSlots(array: JSONArray?): List<EstateSlot> {
    if (array == null) return emptyList()
    val slots = ArrayList<EstateSlot>()
    for (index in 0 until array.length()) {
        val entry = array.optJSONObject(index) ?: continue
        val name = entry.str("name")
        if (name.isBlank()) continue
        slots += EstateSlot(
            name = name,
            up = entry.bool("up"),
            booting = entry.bool("booting"),
            policy = entry.str("policy").takeIf { it.isNotEmpty() },
            group = entry.str("group").takeIf { it.isNotEmpty() },
            pid = entry.longOrNull("pid")?.takeIf { it > 0 },
            uptimeS = entry.doubleOrNull("uptime_s"),
            inFlight = entry.int("in_flight"),
            restarts = entry.int("restarts"),
            listen = entry.str("listen").takeIf { it.isNotEmpty() },
            residentModels = entry.strings("resident_models"),
            cgroup = parseCgroup(entry.obj("cgroup")),
            state = entry.str("state").takeIf { it.isNotEmpty() },
            model = entry.str("model").takeIf { it.isNotEmpty() },
            device = entry.str("device").takeIf { it.isNotEmpty() },
            kind = entry.str("kind").takeIf { it.isNotEmpty() },
            nCtx = entry.int("n_ctx").takeIf { it > 0 },
            nParams = entry.longOrNull("n_params")?.takeIf { it > 0 },
            sizeBytes = entry.longOrNull("size_bytes")?.takeIf { it > 0 },
            quant = entry.str("quant").takeIf { it.isNotEmpty() },
            alias = entry.str("alias").takeIf { it.isNotEmpty() },
            vramMb = entry.doubleOrNull("vram_mb"),
            lastError = entry.str("last_error").takeIf { it.isNotEmpty() },
            lastTransition = entry.str("last_transition").takeIf { it.isNotEmpty() },
        )
    }
    return slots
}

private fun parseCgroup(cgroup: JSONObject?): EstateCgroup? {
    if (cgroup == null) return null
    return EstateCgroup(
        currentBytes = cgroup.longOrNull("current_bytes"),
        highBytes = cgroup.longOrNull("high_bytes"),
        maxBytes = cgroup.longOrNull("max_bytes"),
        oomKills = cgroup.longOrNull("oom_kills"),
    )
}

private fun JSONObject?.longOrNull(key: String): Long? {
    val source = this ?: return null
    if (!source.has(key) || source.isNull(key)) return null
    return source.optLong(key)
}

private fun JSONObject?.doubleOrNull(key: String): Double? {
    val source = this ?: return null
    if (!source.has(key) || source.isNull(key)) return null
    return source.optDouble(key)
}

private fun JSONObject?.strings(key: String): List<String> {
    val array = this?.optJSONArray(key) ?: return emptyList()
    return (0 until array.length()).mapNotNull { index ->
        array.optString(index).takeIf { it.isNotEmpty() }
    }
}
