package uk.xa0.ward.model

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AgentPresetsTest {

    @Test
    fun `the roster is read and sorted by name`() {
        val value = JSONObject().put("presets", JSONArray()
            .put(JSONObject()
                .put("name", "review")
                .put("system", "review the change")
                .put("model", "deepseek-flash"))
            .put(JSONObject()
                .put("name", "Build")
                .put("system", "")
                .put("model", "local")))

        val presets = parseAgentPresets(value)

        assertEquals(listOf("Build", "review"), presets.map { it.name })
        assertEquals("local", presets[0].model)
        assertEquals("review the change", presets[1].system)
    }

    @Test
    fun `entries without a name are skipped`() {
        val value = JSONObject().put("presets", JSONArray()
            .put(JSONObject().put("model", "local"))
            .put(JSONObject().put("name", "review")))

        val presets = parseAgentPresets(value)

        assertEquals(1, presets.size)
        assertEquals("review", presets.single().name)
    }

    @Test
    fun `a missing roster reads as no presets`() {
        assertTrue(parseAgentPresets(null).isEmpty())
        assertTrue(parseAgentPresets(JSONObject()).isEmpty())
    }

    @Test
    fun `an applied result names the preset`() {
        val outcome = agentPresetOutcome(
            requested = "review",
            result = JSONObject()
                .put("applied", true)
                .put("name", "review")
                .put("system", "read only")
                .put("model", "deepseek-flash"),
        )

        assertFalse(outcome.error)
        assertEquals("review", outcome.appliedName)
        assertTrue(outcome.message.contains("review"))
    }

    @Test
    fun `an unapplied result is not reported as applied`() {
        val outcome = agentPresetOutcome(
            requested = "review",
            result = JSONObject().put("applied", false),
        )

        assertTrue(outcome.error)
        assertNull(outcome.appliedName)
        assertTrue(outcome.message.contains("did not apply"))
    }

    @Test
    fun `a daemon error is carried into the outcome`() {
        val outcome = agentPresetOutcome(
            requested = "review",
            result = null,
            failure = "not_found: no such preset",
        )

        assertTrue(outcome.error)
        assertNull(outcome.appliedName)
        assertTrue(outcome.message.contains("not_found"))
    }
}
