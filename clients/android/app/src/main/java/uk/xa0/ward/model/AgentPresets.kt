/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.model

import org.json.JSONObject

data class AgentPreset(
    val name: String,
    val system: String,
    val model: String,
)

fun parseAgentPresets(value: JSONObject?): List<AgentPreset> {
    val rows = value?.optJSONArray("presets") ?: return emptyList()
    return (0 until rows.length()).mapNotNull { index ->
        val row = rows.optJSONObject(index) ?: return@mapNotNull null
        val name = row.str("name")
        if (name.isEmpty()) {
            null
        } else {
            AgentPreset(name = name, system = row.str("system"), model = row.str("model"))
        }
    }.sortedBy { it.name.lowercase() }
}

data class AgentPresetApplied(
    val applied: Boolean,
    val name: String,
    val system: String,
    val model: String,
)

fun parseAgentPresetApply(value: JSONObject?): AgentPresetApplied? {
    val source = value ?: return null
    return AgentPresetApplied(
        applied = source.optBoolean("applied", false),
        name = source.str("name"),
        system = source.str("system"),
        model = source.str("model"),
    )
}

data class AgentPresetOutcome(
    val appliedName: String?,
    val message: String,
    val error: Boolean,
)

fun agentPresetOutcome(
    requested: String,
    result: JSONObject?,
    failure: String? = null,
): AgentPresetOutcome {
    if (!failure.isNullOrEmpty()) {
        return AgentPresetOutcome(
            appliedName = null,
            message = failure,
            error = true,
        )
    }
    val applied = parseAgentPresetApply(result)
    if (applied == null || !applied.applied) {
        return AgentPresetOutcome(
            appliedName = null,
            message = "the daemon did not apply the preset",
            error = true,
        )
    }
    val name = applied.name.ifEmpty { requested }
    return AgentPresetOutcome(
        appliedName = name,
        message = "Agent preset $name applied.",
        error = false,
    )
}
