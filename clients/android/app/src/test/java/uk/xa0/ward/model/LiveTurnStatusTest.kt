package uk.xa0.ward.model

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The force-compact plugin's live running phase, parsed off `settings/describe`.
 *
 * The wire shapes here are the ones the plugin actually publishes: a
 * `namespaces` array of namespace views, the plugin's view under
 * `falling-ts-force-compact`, its `value.liveUi` object carrying
 * `{phase, text, textId}`. Nothing on this path may throw, because the reply is
 * external input and the row it drives is cosmetic; the malformed cases below
 * are as much the subject as the happy path.
 */
class LiveTurnStatusTest {

    /** One `settings/describe` reply carrying [liveUi] under the plugin's namespace. */
    private fun reply(liveUi: Any?): JSONObject {
        val view = JSONObject().put("ns", LiveTurnStatus.NAMESPACE)
        if (liveUi != null) view.put("value", JSONObject().put(LiveTurnStatus.FIELD, liveUi))
        return JSONObject().put("namespaces", org.json.JSONArray().put(view))
    }

    private fun replyWith(phase: String, textId: String? = null, text: String = "") =
        reply(
            JSONObject()
                .put("phase", phase)
                .put("text", text)
                .apply { if (textId != null) put("textId", textId) },
        )

    // ------------------------------------------------------------- known phases

    @Test
    fun `requesting maps to the plugin's copy`() {
        assertEquals("Requesting model", parseLiveTurnStatus(replyWith("requesting", "requesting")))
    }

    @Test
    fun `responding maps to the plugin's copy`() {
        assertEquals("Model responding", parseLiveTurnStatus(replyWith("responding", "responding")))
    }

    @Test
    fun `tooling maps to the plugin's copy`() {
        assertEquals("Running tool", parseLiveTurnStatus(replyWith("tooling", "tooling")))
    }

    @Test
    fun `compressing maps to the plugin's banner`() {
        assertEquals("[Compacting…]", parseLiveTurnStatus(replyWith("compressing", "compressing")))
    }

    @Test
    fun `done maps to the compaction banner`() {
        assertEquals("[Compaction complete!]", parseLiveTurnStatus(replyWith("done", "done")))
    }

    /**
     * The plugin's wire `text` does not override known copy: a diagnostic string
     * riding beside a known id must not reach the running line.
     */
    @Test
    fun `a known phase ignores the wire text`() {
        assertEquals(
            "Requesting model",
            parseLiveTurnStatus(replyWith("requesting", "requesting", text = "raw wire text")),
        )
    }

    /** A build that sends no `textId` is still understood from `phase`. */
    @Test
    fun `phase is the fallback when textId is absent`() {
        assertEquals("Running tool", parseLiveTurnStatus(replyWith("tooling")))
        assertEquals("Running tool", parseLiveTurnStatus(replyWith("tooling", textId = "")))
    }

    // ----------------------------------------------------------------- the clear

    @Test
    fun `end clears the row`() {
        assertNull(parseLiveTurnStatus(replyWith("end", "end")))
    }

    /** `end` wins even if a stale text is still beside it. */
    @Test
    fun `end wins over a stale text`() {
        assertNull(parseLiveTurnStatus(replyWith("end", "end", text = "Compacting…")))
    }

    // ------------------------------------------------------------------ absence

    @Test
    fun `a null reply has no live status`() {
        assertNull(parseLiveTurnStatus(null))
    }

    @Test
    fun `a reply with no namespaces has no live status`() {
        assertNull(parseLiveTurnStatus(JSONObject()))
        assertNull(parseLiveTurnStatus(JSONObject().put("namespaces", org.json.JSONArray())))
    }

    /** The plugin is not loaded: its namespace is simply absent. */
    @Test
    fun `a missing plugin namespace has no live status`() {
        val other = JSONObject()
            .put("ns", "ui-theme")
            .put("value", JSONObject().put("preference", "dark"))
        val reply = JSONObject().put("namespaces", org.json.JSONArray().put(other))
        assertNull(parseLiveTurnStatus(reply))
    }

    @Test
    fun `a namespace with no liveUi value has no live status`() {
        assertNull(parseLiveTurnStatus(reply(null)))
        assertNull(parseLiveTurnStatus(reply(JSONObject())))
    }

    @Test
    fun `a blank liveUi has no live status`() {
        assertNull(parseLiveTurnStatus(reply(JSONObject().put("phase", "").put("textId", ""))))
        assertNull(parseLiveTurnStatus(reply(JSONObject().put("phase", "  ").put("text", " "))))
    }

    // ------------------------------------------------------------------ unknown

    /** A newer phase this client does not know, with usable wire text, is shown. */
    @Test
    fun `an unknown phase falls back to non-blank wire text`() {
        assertEquals(
            "Waiting for a slot",
            parseLiveTurnStatus(replyWith("queued", "queued", text = "Waiting for a slot")),
        )
    }

    /** No known key and no usable text: the app's own label, not a blank line. */
    @Test
    fun `an unknown phase with blank text has no live status`() {
        assertNull(parseLiveTurnStatus(replyWith("queued", "queued", text = "")))
        assertNull(parseLiveTurnStatus(replyWith("queued", "queued", text = "   ")))
    }

    /** An unknown id beside a phase this client knows still resolves from the phase. */
    @Test
    fun `an unknown textId may resolve from a known phase`() {
        assertEquals(
            "Model responding",
            parseLiveTurnStatus(replyWith("responding", "responding-v2")),
        )
    }

    /** A field with no phase and no id, but usable text, is still a label. */
    @Test
    fun `a text-only liveUi falls back to its text`() {
        assertEquals("Working", parseLiveTurnStatus(reply(JSONObject().put("text", "Working"))))
    }

    // ----------------------------------------------------------------- malformed

    /**
     * Wrongly-shaped nodes degrade to null instead of throwing: a scalar where the
     * namespace array belongs, a namespace view that is not an object, and a
     * `liveUi` that is a string.
     */
    @Test
    fun `unexpected shapes degrade to null without throwing`() {
        assertNull(parseLiveTurnStatus(JSONObject().put("namespaces", "not-an-array")))
        val scalarView = JSONObject()
            .put("namespaces", org.json.JSONArray().put("not-an-object"))
        assertNull(parseLiveTurnStatus(scalarView))
        assertNull(parseLiveTurnStatus(reply("not-an-object")))
        assertNull(
            parseLiveTurnStatus(
                JSONObject().put(
                    "namespaces",
                    org.json.JSONArray().put(
                        JSONObject().put("ns", LiveTurnStatus.NAMESPACE).put("value", "not-an-object"),
                    ),
                ),
            ),
        )
        assertNull(
            parseLiveTurnStatus(
                JSONObject().put(
                    "namespaces",
                    org.json.JSONArray().put(
                        JSONObject().put("ns", LiveTurnStatus.NAMESPACE)
                            .put("value", JSONObject().put("liveUi", org.json.JSONObject.NULL)),
                    ),
                ),
            ),
        )
    }
}
