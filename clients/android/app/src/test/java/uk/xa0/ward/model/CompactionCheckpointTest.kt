package uk.xa0.ward.model

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class CompactionCheckpointTest {

    private fun event(type: String, seq: Int, data: JSONObject = JSONObject()): JSONObject = JSONObject()
        .put("type", type)
        .put("seq", seq)
        .put("time", 1000L)
        .put("data", data)

    private fun summaryEvent(seq: Int, id: String): JSONObject = event(
        "compaction/summary",
        seq,
        JSONObject()
            .put("compactionId", id)
            .put("shadowedSeqs", JSONArray().put(1).put(2))
            .put("shadowedTokenCount", 15000)
            .put("summary", JSONArray().put(JSONObject().put("type", "text").put("text", "the old turns"))),
    )

    private fun automaticCheckpoint(seq: Int, id: String): JSONObject = event(
        "user/message",
        seq,
        JSONObject()
            .put(
                "content",
                JSONArray().put(JSONObject().put("type", "text").put("text", "the old turns, rewritten")),
            )
            .put("source", JSONObject().put("kind", "compact-checkpoint").put("compactionId", id)),
    )

    private fun commandCheckpoint(seq: Int, id: String): JSONObject = event(
        "user/message",
        seq,
        JSONObject()
            .put(
                "content",
                JSONArray().put(JSONObject().put("type", "text").put("text", "the old turns, rewritten")),
            )
            .put(
                "source",
                JSONObject()
                    .put("plugin", "compact")
                    .put("compactionId", id)
                    .put("sourceCommandId", "command-1"),
            ),
    )

    @Test
    fun `an automatic checkpoint is one compaction marker, never a context notice`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(automaticCheckpoint(10, "c1"))
        reducer.applyEvent(summaryEvent(11, "c1"))

        val rows = reducer.snapshot()
        assertEquals(1, rows.size)
        val notice = rows.single() as ChatEntry.Notice
        assertEquals("compaction", notice.kind)
        assertEquals("Context compacted · 2 history items (~15k tokens)", notice.text)
        assertEquals("the old turns", notice.detail)
        assertEquals(10, notice.seq)
    }

    @Test
    fun `the marker is drawn when the summary landed before its checkpoint`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(summaryEvent(11, "c1"))
        reducer.applyEvent(automaticCheckpoint(10, "c1"))

        assertEquals(1, reducer.snapshot().size)
        val notice = reducer.snapshot().single() as ChatEntry.Notice
        assertEquals("compaction", notice.kind)
        assertEquals("Context compacted · 2 history items (~15k tokens)", notice.text)
        assertEquals("the old turns", notice.detail)
    }

    @Test
    fun `a durable replay of one checkpoint does not add a second marker`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(automaticCheckpoint(10, "c1"))
        reducer.applyEvent(summaryEvent(11, "c1"))
        reducer.applyEvent(automaticCheckpoint(10, "c1"))

        assertEquals(1, reducer.snapshot().size)
    }

    @Test
    fun `a command checkpoint keeps its own compact title`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(commandCheckpoint(10, "c1"))
        reducer.applyEvent(summaryEvent(11, "c1"))

        val notice = reducer.snapshot().single() as ChatEntry.Notice
        assertEquals("compact · 2 history items (~15k tokens)", notice.text)
    }
}
