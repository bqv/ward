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
    fun `a local notice sorts after every real event and keeps its words`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(journalError(3))

        reducer.addLocalNotice("approval-request", "Approval requested · bash", "writes files", 2000L)

        val rows = reducer.snapshot()
        assertEquals(2, rows.size)
        val notice = rows.last() as ChatEntry.Notice
        assertEquals("approval-request", notice.kind)
        assertEquals("Approval requested · bash", notice.text)
        assertEquals("writes files", notice.detail)
        assertEquals(2000L, notice.time)
        assertTrue("above the journal's seq", notice.seq > 3)
    }

    @Test
    fun `no local notice is a back-fill cursor`() {
        val reducer = TranscriptReducer()
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        assertNull(reducer.oldestSeq())
    }

    @Test
    fun `two local notices keep the order they were added in`() {
        val reducer = TranscriptReducer()
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)
        reducer.addLocalNotice("approval-granted", "Approval granted · bash", null, 2000L)

        val kinds = reducer.snapshot().map { (it as ChatEntry.Notice).kind }
        assertEquals(listOf("approval-request", "approval-granted"), kinds)
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
    }

    @Test
    fun `reset clears local notices`() {
        val reducer = TranscriptReducer()
        reducer.addLocalNotice("approval-request", "Approval requested · bash", null, 1000L)

        reducer.reset()

        assertEquals(emptyList<ChatEntry>(), reducer.snapshot())
    }
}
