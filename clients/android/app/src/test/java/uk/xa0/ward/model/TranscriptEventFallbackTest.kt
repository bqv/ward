package uk.xa0.ward.model

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The opt-in fallback for journal types the reducer does not know.
 *
 * A host can emit event types this client has never heard of — a plugin's own
 * loop phases are the reported case — and the reducer has no branch for them.
 * The fallback is **off by default** (`showUnknownEvents = false`): it is a
 * diagnostic view, not normal transcript content, and the owner found the rows
 * noisy. When switched on, every non-blank unknown type becomes one small
 * activity note: a human label derived from the type by [eventLabel], with its
 * raw payload kept in [ChatEntry.Notice.detail] behind a tap rather than on the
 * default face. Known branches and blank types behave the same either way.
 *
 * Note on the styling the model half cannot assert: the row is drained as
 * `micro` type in `labelCaption`, one line with an ellipsis (see `NoticeRow`'s
 * `UnknownEventRow`). This JVM suite verifies the flag, the label and the
 * detail; the one-line, no-icon behaviour is exercised in
 * `NoticeEventRowTest`, and what the text actually looks like is a device claim,
 * not one made here.
 */
class TranscriptEventFallbackTest {

    private fun event(type: String, seq: Int, data: JSONObject = JSONObject()) = JSONObject()
        .put("type", type)
        .put("seq", seq)
        .put("time", 1000L)
        .put("data", data)

    @Test
    fun `an unknown event is ignored while the flag is off, which is the default`() {
        val reducer = TranscriptReducer()
        reducer.applyEvent(
            event(
                "x-local/loop",
                4,
                JSONObject()
                    .put("phase", "live")
                    .put("loop", 3),
            ),
        )

        assertEquals(emptyList<ChatEntry>(), reducer.snapshot())
    }

    @Test
    fun `an unknown plugin event becomes a human label and keeps its payload when enabled`() {
        val reducer = TranscriptReducer()
        reducer.showUnknownEvents = true
        reducer.applyEvent(
            event(
                "x-local/loop",
                4,
                JSONObject()
                    .put("phase", "live")
                    .put("loop", 3),
            ),
        )

        val notice = reducer.snapshot().single() as ChatEntry.Notice
        assertEquals("event:x-local/loop", notice.kind)
        assertEquals("X local loop", notice.text)
        assertEquals(NoticeSeverity.INFO, notice.severity)
        val detail = notice.detail.orEmpty()
        assertTrue(detail.contains("phase"))
        assertTrue(detail.contains("live"))
        assertTrue(detail.contains("loop"))
    }

    @Test
    fun `eventLabel splits delimiters and camel case, capitalising only the first word`() {
        assertEquals("Llm retry", eventLabel("llm/retry"))
        assertEquals("Model request retry", eventLabel("model/request-retry"))
        assertEquals("X local loop", eventLabel("x-local/loop"))
        assertEquals("Plugin phase two now v3", eventLabel("plugin/phase_two-now.v3"))
        assertEquals("State change", eventLabel("stateChange"))
        assertEquals("Http response retry", eventLabel("HTTPResponseRetry"))
        // Only the first word is capitalised: an acronym stays lower-case after it.
        assertEquals("Some id token", eventLabel("some/ID-token"))
    }

    @Test
    fun `a known event does not become an extra row even with the fallback on`() {
        val reducer = TranscriptReducer()
        reducer.showUnknownEvents = true
        reducer.applyEvent(event("turn/start", 1, JSONObject().put("turn", 1)))

        // `turn/start` is bookkeeping, not a row; the fallback must not turn its
        // handled branch into a second line in the chat log.
        assertEquals(emptyList<ChatEntry>(), reducer.snapshot())
    }

    @Test
    fun `a blank event type is ignored even with the fallback on`() {
        val reducer = TranscriptReducer()
        reducer.showUnknownEvents = true
        reducer.applyEvent(event("", 1))
        reducer.applyEvent(JSONObject().put("seq", 2).put("time", 1000L))

        assertEquals(emptyList<ChatEntry>(), reducer.snapshot())
    }

    @Test
    fun `a very large payload is capped`() {
        val reducer = TranscriptReducer()
        reducer.showUnknownEvents = true
        reducer.applyEvent(
            event("x-local/loop", 1, JSONObject().put("blob", "x".repeat(20_000))),
        )

        val detail = (reducer.snapshot().single() as ChatEntry.Notice).detail.orEmpty()
        assertEquals(UNKNOWN_EVENT_DETAIL_LIMIT, detail.length)
        assertTrue(detail.endsWith("(truncated)"))
    }
}
