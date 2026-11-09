package uk.xa0.ward.model

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionStatsMergeTest {

    private val previous = SessionStats(
        hasStats = true,
        turns = 2,
        steps = 5,
        llmMs = 4_000L,
        toolMs = 1_000L,
        ttftMs = 300L,
        ttftSteps = 3,
        decodeMs = 2_000L,
        decodeTokens = 80L,
        hasUsage = true,
        uncachedInputTokens = 100L,
        cacheReadTokens = 200L,
        cacheWriteTokens = 50L,
        outputTokens = 40L,
    )

    @Test
    fun `a tokenUsage-only payload keeps the previous timing`() {
        val values = JSONObject().put(
            "tokenUsage",
            JSONObject()
                .put("uncachedInputTokens", 7L)
                .put("cacheReadTokens", 11L)
                .put("cacheWriteTokens", 3L)
                .put("outputTokens", 13L),
        )

        val merged = parseSessionStats(values, previous)!!

        assertEquals(previous.decodeMs, merged.decodeMs)
        assertEquals(previous.decodeTokens, merged.decodeTokens)
        assertEquals(previous.tokensPerSecondText, merged.tokensPerSecondText)
        assertEquals("40", merged.tokensPerSecondText)
        assertEquals(previous.steps, merged.steps)
        assertEquals(previous.turns, merged.turns)
        assertTrue(merged.hasStats)
        assertTrue(merged.hasUsage)
        assertEquals(7L, merged.uncachedInputTokens)
        assertEquals(11L, merged.cacheReadTokens)
        assertEquals(3L, merged.cacheWriteTokens)
        assertEquals(13L, merged.outputTokens)
        assertEquals(34L, merged.totalTokens)
    }

    @Test
    fun `a sessionStats-only payload keeps the previous usage`() {
        val values = JSONObject().put(
            "sessionStats",
            JSONObject()
                .put("turns", 4)
                .put("steps", 9)
                .put("llmMs", 5_000L)
                .put("toolMs", 600L)
                .put("ttftMs", 400L)
                .put("ttftSteps", 2)
                .put("decodeMs", 1_500L)
                .put("decodeTokens", 90L),
        )

        val merged = parseSessionStats(values, previous)!!

        assertEquals(previous.totalTokens, merged.totalTokens)
        assertEquals(previous.uncachedInputTokens, merged.uncachedInputTokens)
        assertEquals(previous.cacheReadTokens, merged.cacheReadTokens)
        assertEquals(previous.cacheWriteTokens, merged.cacheWriteTokens)
        assertEquals(previous.outputTokens, merged.outputTokens)
        assertTrue(merged.hasUsage)
        assertTrue(merged.hasTokenActivity)
        assertEquals(9, merged.steps)
        assertEquals(4, merged.turns)
        assertEquals(1_500L, merged.decodeMs)
        assertEquals("60", merged.tokensPerSecondText)
    }

    @Test
    fun `neither half present returns null`() {
        val values = JSONObject().put("title", "x")

        assertNull(parseSessionStats(values, previous))
        assertNull(parseSessionStats(values))
        assertNull(parseSessionStats(null, previous))

        val usageOnly = JSONObject().put("tokenUsage", JSONObject().put("outputTokens", 7L))
        val fresh = parseSessionStats(usageOnly)!!
        assertFalse(fresh.hasStats)
        assertEquals(0, fresh.steps)
        assertEquals(0L, fresh.decodeMs)
        assertNull(fresh.tokensPerSecondText)
        assertEquals(7L, fresh.outputTokens)
    }

    @Test
    fun `both halves present replaces both`() {
        val values = JSONObject()
            .put(
                "sessionStats",
                JSONObject()
                    .put("turns", 1)
                    .put("steps", 2)
                    .put("decodeMs", 500L)
                    .put("decodeTokens", 100L),
            )
            .put("tokenUsage", JSONObject().put("outputTokens", 7L))

        val merged = parseSessionStats(values, previous)!!

        assertEquals(2, merged.steps)
        assertEquals(1, merged.turns)
        assertEquals(500L, merged.decodeMs)
        assertEquals(100L, merged.decodeTokens)
        assertEquals("200", merged.tokensPerSecondText)
        assertEquals(0L, merged.llmMs)
        assertEquals(0L, merged.toolMs)
        assertEquals(0, merged.ttftSteps)
        assertEquals(7L, merged.outputTokens)
        assertEquals(0L, merged.uncachedInputTokens)
        assertEquals(0L, merged.cacheReadTokens)
        assertEquals(0L, merged.cacheWriteTokens)
        assertEquals(7L, merged.totalTokens)
        assertTrue(merged.hasStats)
        assertTrue(merged.hasUsage)
    }
}
