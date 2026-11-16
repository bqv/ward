package uk.xa0.ward.model

import org.json.JSONObject

data class EstateLoad(
    val slot: String,
    val model: String? = null,
    val error: String?,
    val progress: Float? = null,
    val progressSource: String? = null,
    val line: String? = null,
    val phase: String? = null,
    val active: Boolean = false,
)

fun estateCards(loads: List<EstateLoad>): List<EstateLoad> {
    val cards = ArrayList<EstateLoad>()
    for ((slot, entries) in loads.groupBy { it.slot }) {
        val card = if (slot == "llamb") {
            entries.firstOrNull { it.phase == "loading" || it.phase == "queued" }
        } else {
            entries.minByOrNull { phaseRank(it.phase) }
        }
        if (card != null) cards += card
    }
    return cards
}

private fun phaseRank(phase: String?): Int = when (phase) {
    "failed" -> 0
    "loading", "queued" -> 1
    "resident" -> 2
    else -> 3
}

private fun readyState(state: String?): Boolean = state == "ready" || state == "running"

private fun idleState(state: String?): Boolean =
    state == "idle" || state == "cold" || state == "stopped"

fun estateCards(status: JSONObject): List<EstateLoad> {
    val loads = estateLoads(status)
    val bySlot = loads.groupBy { it.slot }
    val tenants = status.arr("tenants")
    val residents = LinkedHashMap<String, List<String>?>()
    val inFlight = HashMap<String, Int>()
    val activeByName = HashMap<String, Boolean>()
    val groupByName = HashMap<String, String>()
    val deviceByName = HashMap<String, String>()
    val stateByName = HashMap<String, String>()
    val failureByName = HashMap<String, String>()
    val modelByName = HashMap<String, String>()
    val names = ArrayList<String>()
    if (tenants != null) {
        for (index in 0 until tenants.length()) {
            val entry = tenants.optJSONObject(index) ?: continue
            val name = entry.str("name")
            if (name.isBlank()) continue
            if (name !in names) names += name
            inFlight[name] = entry.int("in_flight", 0)
            activeByName[name] = entry.bool("active")
            groupByName[name] = entry.str("group")
            deviceByName[name] = entry.str("device")
            stateByName[name] = entry.str("state")
            failureByName[name] = entry.str("last_error").ifBlank { entry.str("last_transition") }
            modelByName[name] = entry.str("model")
            if (!entry.has("resident_models") || entry.isNull("resident_models")) continue
            val models = entry.arr("resident_models") ?: continue
            residents[name] = (0 until models.length())
                .map { models.optString(it) }
                .filter { it.isNotBlank() }
        }
    }
    for (name in bySlot.keys) if (name !in names) names += name

    fun isAux(name: String): Boolean =
        name == "llamb" || groupByName[name] == "aux" || deviceByName[name] == "GT1030"

    val auxNames = names.filter(::isAux)
    val cards = ArrayList<EstateLoad>()
    if (auxNames.isNotEmpty()) {
        val auxEntries = auxNames.flatMap { bySlot[it].orEmpty() }
        val auxLoading = auxEntries.firstOrNull { it.phase == "loading" || it.phase == "queued" }
        val auxFailed = auxEntries.firstOrNull { it.phase == "failed" }
        val auxModels = auxNames.flatMap { name ->
            residents[name].orEmpty().ifEmpty {
                if (readyState(stateByName[name])) {
                    listOf(modelByName[name]?.takeIf { it.isNotBlank() } ?: name)
                } else {
                    emptyList()
                }
            }
        }.distinct()
        val auxActive = auxNames.any { name ->
            activeByName[name] == true || (inFlight[name] ?: 0) > 0
        }
        val auxBooting = auxNames.firstOrNull { name ->
            stateByName[name] == "loading" || stateByName[name] == "booting"
        }
        val card = auxLoading ?: auxFailed ?: auxBooting?.let { name ->
            EstateLoad(
                slot = "GT1030",
                model = modelByName[name]?.takeIf { it.isNotBlank() },
                error = failureByName[name]?.takeIf { it.isNotBlank() },
                phase = "loading",
            )
        } ?: if (auxModels.isNotEmpty()) {
            EstateLoad(
                slot = "GT1030",
                model = auxModels.joinToString(" + "),
                error = null,
                phase = "resident",
                active = auxActive,
            )
        } else {
            null
        }
        if (card != null) cards += card
    }

    for (slot in names.filterNot { isAux(it) }) {
        val entries = bySlot[slot].orEmpty()
        val state = stateByName[slot]
        val loading = entries.any { it.phase == "loading" || it.phase == "queued" }
        if (idleState(state) && !loading) continue
        val device = deviceByName[slot]?.takeIf { it.isNotBlank() } ?: slot
        val model = modelByName[slot]?.takeIf { it.isNotBlank() }
        val active = activeByName[slot] == true || (inFlight[slot] ?: 0) > 0
        val card = entries.firstOrNull { it.phase == "failed" }
            ?: entries.firstOrNull { it.phase == "loading" || it.phase == "queued" }
            ?: if (state == "failed") {
                EstateLoad(
                    slot = device,
                    model = model,
                    error = failureByName[slot]?.takeIf { it.isNotBlank() } ?: "Load failed",
                    phase = "failed",
                )
            } else if (residents[slot].orEmpty().isNotEmpty()) {
                residents[slot]?.firstOrNull()?.let { resident ->
                    EstateLoad(
                        slot = device,
                        model = resident,
                        error = null,
                        phase = "resident",
                        active = active,
                    )
                }
            } else if (readyState(state)) {
                EstateLoad(
                    slot = device,
                    model = model ?: slot,
                    error = null,
                    phase = "resident",
                    active = active,
                )
            } else if (state == "loading" || state == "booting") {
                EstateLoad(
                    slot = device,
                    model = model,
                    error = failureByName[slot]?.takeIf { it.isNotBlank() },
                    phase = "loading",
                )
            } else if (residents.containsKey(slot)) {
                null
            } else {
                entries.firstOrNull { it.phase == "ready" || it.phase == null }
            }
        if (card != null) cards += card
    }
    return cards
}

fun estateLoads(status: JSONObject): List<EstateLoad> {
    val loads = ArrayList<EstateLoad>()
    val running = status.arr("loads")
    if (running != null) {
        for (index in 0 until running.length()) {
            val entry = running.optJSONObject(index) ?: continue
            val slot = entry.str("slot").ifBlank { entry.str("tenant") }
            val model = entry.str("model")
            if (slot.isBlank() && model.isBlank()) continue
            loads += EstateLoad(
                slot = slot.ifBlank { "model" },
                model = model.takeIf { it.isNotBlank() },
                error = entry.str("error").takeIf { it.isNotBlank() },
                progress = if (entry.has("progress") && !entry.isNull("progress")) {
                    entry.optDouble("progress").toFloat().coerceIn(0f, 1f)
                } else {
                    null
                },
                progressSource = entry.str("progress_source").takeIf { it.isNotBlank() },
                line = entry.str("line").takeIf { it.isNotBlank() },
                phase = entry.str("phase").takeIf { it.isNotBlank() },
                active = entry.optBoolean("active", false),
            )
        }
        if (loads.isNotEmpty()) return loads
    }
    val tenants = status.arr("tenants") ?: return emptyList()
    for (index in 0 until tenants.length()) {
        val entry = tenants.optJSONObject(index) ?: continue
        if (!entry.bool("booting")) continue
        loads += EstateLoad(
            slot = entry.str("name").takeIf { it.isNotBlank() } ?: "model",
            model = null,
            error = entry.str("last_error").takeIf { it.isNotBlank() },
        )
    }
    return loads
}
