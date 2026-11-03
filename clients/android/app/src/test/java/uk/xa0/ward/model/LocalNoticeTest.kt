package uk.xa0.ward.model

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LocalNoticeTest {

    private fun event(type: String, seq: Int, data: JSONObject = JSONObject()): JSONObject = JSONObject()
        .put("type", type)
        .put("seq", seq)
        .put("time", 1000L)
        .put("data", data)

    private fun journalError(seq: Int): JSONObject =
        event("agent/error", seq, JSONObject().put("message", "boom"))

    @Test
    fun `a notice added mid-turn lands after the row it followed and before the next real row`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(journalError(3))

        reducer.addLocalNotice("approval-request", "Approval requested · bash", "writes files", 2000L)
        reducer.applyEvent(journalError(7))

        val rows = reducer.snapshot()
        assertEquals(listOf(3, -1, 7), rows.map { it.seq })
        val notice = rows[1] as ChatEntry.Notice
        assertEquals("approval-request", notice.kind)
        assertEquals("Approval requested · bash", notice.text)
        assertEquals("writes files", notice.detail)
        assertEquals(2000L, notice.time)
        assertEquals(3, notice.localAnchorSeq)
        assertEquals(1L, notice.localOrder)
        assertTrue("a local notice never takes a real seq", notice.seq < 0)
    }

    @Test
    fun `two local notices keep the order they were added in and a later real event follows them`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(journalError(3))
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)
        reducer.addLocalNotice("approval-granted", "Approval granted · bash", null, 2000L)

        assertEquals(
            listOf("agent/error", "approval-request", "approval-granted"),
            reducer.snapshot().map { (it as ChatEntry.Notice).kind },
        )
        assertEquals(
            listOf(3, -1, -2),
            reducer.snapshot().map { it.seq },
        )

        reducer.applyEvent(journalError(4))

        assertEquals(
            listOf("agent/error", "approval-request", "approval-granted", "agent/error"),
            reducer.snapshot().map { (it as ChatEntry.Notice).kind },
        )
        assertEquals(listOf(3, -1, -2, 4), reducer.snapshot().map { it.seq })
    }

    @Test
    fun `no local notice is a back-fill cursor`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(journalError(3))
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        assertEquals(3, reducer.oldestSeq())

        reducer.reset()
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        assertNull(reducer.oldestSeq())
    }

    @Test
    fun `local notices never collide with an optimistic prompt's synthetic seq`() {
        val reducer = TranscriptReducer()
        reducer.addPending("rpc-1", "hello", emptyList())
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        val rows = reducer.snapshot()
        assertEquals(2, rows.size)
        val prompt = rows.filterIsInstance<ChatEntry.UserMessage>().single()
        val notice = rows.filterIsInstance<ChatEntry.Notice>().single()
        assertNotEquals(prompt.seq, notice.seq)
        assertTrue("the prompt row is still the optimistic one", prompt.pending)
        assertTrue("the prompt stays at the bottom", rows.last() === prompt)
    }

    @Test
    fun `reset clears local notices`() {
        val reducer = TranscriptReducer()
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        reducer.reset()

        assertEquals(emptyList<ChatEntry>(), reducer.snapshot())
    }
}
