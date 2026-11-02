package uk.xa0.ward

import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The attention cap is a realistic wait, not a meeting timer.
 *
 * The reported defect was a card that vanished after minutes while the host still held
 * the waterfall; only restarting the app re-announced it. The two call-site constants
 * are therefore pinned at a day or more, so a change back to a minutes-long cap fails
 * here rather than on somebody's overnight approval.
 *
 * This asserts the configured setting only: no coroutine is run, no clock is advanced,
 * and nothing here proves the host's waterfall survives that long. The constants live
 * in a private companion, so they are read as the `const val` static fields the Kotlin
 * compiler emits on [AttentionCenter]; that keeps the production API closed while the
 * setting stays checkable.
 */
class AttentionTimeoutTest {

    private fun configured(name: String): Long =
        AttentionCenter::class.java.getDeclaredField(name).apply { isAccessible = true }.getLong(null)

    @Test
    fun approvalCapOutlastsARealisticWait() {
        val value = configured("APPROVAL_TIMEOUT_MS")
        assertTrue("APPROVAL_TIMEOUT_MS is ${value}ms, under a day", value >= DAY_MS)
    }

    @Test
    fun questionCapOutlastsARealisticWait() {
        val value = configured("QUESTION_TIMEOUT_MS")
        assertTrue("QUESTION_TIMEOUT_MS is ${value}ms, under a day", value >= DAY_MS)
    }

    private companion object {
        const val DAY_MS = 24L * 60 * 60 * 1000
    }
}
