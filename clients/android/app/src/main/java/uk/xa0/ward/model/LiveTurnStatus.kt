package uk.xa0.ward.model

import org.json.JSONObject

/**
 * The running-turn line a host plugin can drive, read from `settings/describe`.
 *
 * `@falling-ts/dsh-force-compact` publishes one settings namespace,
 * [NAMESPACE], whose `liveUi` field is `{phase, text, textId}`. The plugin's own
 * web client paints its running line from that object, so the app can show the
 * same live phase instead of its fixed "Deep diving..." — but only when the
 * plugin (and therefore the namespace) is actually present.
 *
 * This parser is deliberately total: a host without the plugin, a namespace
 * shape this client does not know, a field that has gone missing and a value
 * that is plain JSON null all answer null, which the caller treats as "use the
 * app's own label". Nothing here may throw — the reply is external input, and a
 * cosmetic label is never worth a crash.
 */
object LiveTurnStatus {

    /** The plugin's settings namespace; absent from `namespaces` when it is not loaded. */
    const val NAMESPACE = "falling-ts-force-compact"

    /** The field inside [NAMESPACE] that carries the live phase. */
    const val FIELD = "liveUi"

    /**
     * The plugin's English copy, keyed by its `textId` (and by `phase` when the
     * plugin sent no id). This is the web client's own wording, so the phone and
     * a browser show the same sentence for the same phase.
     */
    internal val PHASE_COPY: Map<String, String> = mapOf(
        "requesting" to "Requesting model",
        "responding" to "Model responding",
        "tooling" to "Running tool",
        "compressing" to "[Compacting…]",
        "done" to "[Compaction complete!]",
    )

    /**
     * The phase the plugin uses to clear the line. It is spelled out rather than
     * implied by an empty `text`, because an explicit `end` must win even if a
     * stale text is still riding beside it.
     */
    private const val END = "end"
}

/**
 * Parse the live running-turn label out of one `settings/describe` reply.
 *
 * Returns the label to draw, or null for every case in which the app must fall
 * back to its own "Deep diving...": no reply, no [LiveTurnStatus.NAMESPACE]
 * (the plugin is not loaded), no `value.liveUi` object, a blank `liveUi`, the
 * `end` phase, or a phase this client does not know and that came with no
 * usable text of its own.
 *
 * The fallback rule for an unknown phase: the plugin's own `text` is used only
 * when it is non-blank. That keeps a *newer* plugin phase legible without this
 * client being taught it, while never painting a blank or whitespace field —
 * the plugin's `end` is exactly an empty text, so a blank text is the clear
 * signal, not a label.
 *
 * Known phases always use [LiveTurnStatus.PHASE_COPY] rather than the wire's
 * `text`, so the app's copy cannot drift with whatever diagnostic wording a
 * plugin build happens to carry.
 */
fun parseLiveTurnStatus(reply: JSONObject?): String? = runCatching {
    val namespaces = reply.arr("namespaces") ?: return@runCatching null
    val view = (0 until namespaces.length())
        .mapNotNull { namespaces.optJSONObject(it) }
        .firstOrNull { it.str("ns") == LiveTurnStatus.NAMESPACE }
        ?: return@runCatching null
    val live = view.obj("value")?.obj(LiveTurnStatus.FIELD)
        ?: return@runCatching null

    val textId = live.str("textId").trim()
    val phase = live.str("phase").trim()
    // The plugin's id is the most specific name; `phase` is the fallback for a
    // build that sends no id.
    val key = textId.ifEmpty { phase }
    // No phase and no id is the blank field; the plugin's text is the only thing
    // left that could say anything, and blank text is the clear, not a label.
    if (key.isEmpty()) {
        return@runCatching live.str("text").takeIf { it.isNotBlank() }
    }
    if (key == "end") return@runCatching null
    LiveTurnStatus.PHASE_COPY[key]?.let { return@runCatching it }
    // An unknown id may still be paired with a phase this client knows; trust the
    // known half rather than either giving up or echoing the wire's text.
    if (key != phase && phase.isNotEmpty()) {
        if (phase == "end") return@runCatching null
        LiveTurnStatus.PHASE_COPY[phase]?.let { return@runCatching it }
    }
    // Unknown phase: the plugin's own text, but only when it says something.
    live.str("text").takeIf { it.isNotBlank() }
}.getOrNull()
