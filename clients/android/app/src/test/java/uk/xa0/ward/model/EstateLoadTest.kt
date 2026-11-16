package uk.xa0.ward.model

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EstateLoadTest {

    private fun cards(json: String): List<EstateLoad> = estateCards(JSONObject(json))

    @Test
    fun `a running tenant becomes one chip named by its model and device`() {
        val rows = cards(
            """
            {"tenants":[{"name":"llama","model":"Qwen3-27B","device":"ROCm0",
             "group":"gpu","state":"running","in_flight":0}]}
            """.trimIndent(),
        )

        assertEquals(1, rows.size)
        val card = rows.single()
        assertEquals("ROCm0", card.slot)
        assertEquals("Qwen3-27B", card.model)
        assertEquals("resident", card.phase)
        assertEquals(false, card.active)
    }

    @Test
    fun `a resident chip spins while a request is in flight`() {
        val rows = cards(
            """
            {"tenants":[{"name":"llama","model":"Qwen3-27B","device":"ROCm0",
             "state":"running","in_flight":1}]}
            """.trimIndent(),
        )

        assertEquals(true, rows.single().active)
    }

    @Test
    fun `an idle or stopped tenant contributes no chip`() {
        val rows = cards(
            """
            {"tenants":[
              {"name":"llama","model":"A","state":"idle"},
              {"name":"llama2","model":"B","state":"stopped"},
              {"name":"llama3","model":"C","state":"running","device":"ROCm0"}
            ]}
            """.trimIndent(),
        )

        assertEquals(listOf("ROCm0"), rows.map { it.slot })
        assertEquals("C", rows.single().model)
    }

    @Test
    fun `a loading tenant without a loads entry still gets a loading chip`() {
        val rows = cards(
            """
            {"loads":[],
             "tenants":[{"name":"llama","model":"Qwen3-27B","device":"ROCm0",
              "state":"loading"}]}
            """.trimIndent(),
        )

        val card = rows.single()
        assertEquals("loading", card.phase)
        assertEquals("Qwen3-27B", card.model)
        assertEquals("ROCm0", card.slot)
    }

    @Test
    fun `a loads entry carries the progress its source can name`() {
        val rows = cards(
            """
            {"loads":[{"tenant":"llama","model":"Qwen3-27B","phase":"loading",
              "progress":0.42,"progress_source":"io_read_bytes","line":"reading"}],
             "tenants":[{"name":"llama","model":"old","device":"ROCm0","state":"loading"}]}
            """.trimIndent(),
        )

        val card = rows.single()
        assertEquals("Qwen3-27B", card.model)
        assertEquals(0.42f, card.progress!!, 0.001f)
        assertEquals("io_read_bytes", card.progressSource)
        assertEquals("reading", card.line)
    }

    @Test
    fun `a failed load carries its reason`() {
        val rows = cards(
            """
            {"loads":[{"slot":"llama","model":"Qwen3-27B","phase":"failed",
              "error":"the weights are unreadable"}],
             "tenants":[{"name":"llama","state":"loading"}]}
            """.trimIndent(),
        )

        val card = rows.single()
        assertEquals("failed", card.phase)
        assertEquals("the weights are unreadable", card.error)
    }

    @Test
    fun `a failed tenant without a load row names its last transition`() {
        val rows = cards(
            """
            {"tenants":[{"name":"llama","model":"Qwen3-27B","device":"ROCm0",
             "state":"failed","last_transition":"the weights are unreadable"}]}
            """.trimIndent(),
        )

        val card = rows.single()
        assertEquals("failed", card.phase)
        assertEquals("the weights are unreadable", card.error)
    }

    @Test
    fun `the aux group aggregates into one card`() {
        val rows = cards(
            """
            {"tenants":[
              {"name":"llamb-kev","model":"Kev","group":"aux","device":"GT1030",
               "state":"running","in_flight":0},
              {"name":"llamb-embed","model":"Qwen3-Embedding","group":"aux",
               "device":"GT1030","state":"running","in_flight":0},
              {"name":"llama","model":"Qwen3-27B","group":"gpu","device":"ROCm0",
               "state":"running","in_flight":0}
            ]}
            """.trimIndent(),
        )

        assertEquals(2, rows.size)
        val aux = rows.first { it.slot == "GT1030" }
        assertEquals("resident", aux.phase)
        assertEquals("Kev + Qwen3-Embedding", aux.model)
        assertEquals("ROCm0", rows.first { it.slot == "ROCm0" }.slot)
    }

    @Test
    fun `a failed card outranks a loading card for the same slot`() {
        val rows = estateCards(
            listOf(
                EstateLoad(slot = "llama", error = null, phase = "resident"),
                EstateLoad(slot = "llama", error = null, phase = "loading"),
                EstateLoad(slot = "llama", error = "boom", phase = "failed"),
            ),
        )

        assertEquals(listOf("failed"), rows.map { it.phase })
    }

    @Test
    fun `the status document carries tenants, loads and health`() {
        val status = parseEstateStatus(
            JSONObject(
                """
                {"version":"1.9.0","listen":"127.0.0.1:55555","vram_used_gib":17.5,
                 "media_in_flight":1,
                 "llm_health":{"degraded":true,"fails":2,"orphaned":false,
                   "restarting":false,"restarts":1},
                 "tenants":[{"name":"llama","model":"Qwen3-27B","device":"ROCm0",
                   "state":"running","in_flight":0,"cgroup":{"current_bytes":1024}}],
                 "loads":[{"tenant":"llama","model":"Qwen3-27B","phase":"loading"}]}
                """.trimIndent(),
            ),
        )

        assertEquals("1.9.0", status.version)
        assertEquals("127.0.0.1:55555", status.listen)
        assertEquals(17.5, status.vramUsedGib!!, 0.001)
        assertEquals(1, status.mediaInFlight)
        assertTrue(status.health.degraded)
        assertEquals(1, status.health.restarts)
        assertEquals(1, status.slots.size)
        assertEquals("ROCm0", status.slots.single().device)
        assertEquals("Qwen3-27B", status.slots.single().model)
        assertEquals(1024L, status.slots.single().cgroup?.currentBytes)
        assertEquals(1, status.loads.size)
        assertNotNull(status.loads.single().model)
        assertNull(status.idleTimeoutS)
    }
}
