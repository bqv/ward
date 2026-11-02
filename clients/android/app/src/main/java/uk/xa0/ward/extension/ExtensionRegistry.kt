package uk.xa0.ward.extension

/**
 * The loaded extensions and their contributions.
 *
 * Pure Kotlin on purpose: no Android type appears here, so the duplicate-id,
 * version and ordering rules are exercised by ordinary JVM tests rather than by a
 * device. The loader owns Android; this class owns identity.
 *
 * Registration is rejected — never thrown — for a blank id, a version this build
 * does not speak, a duplicate id, a contribution with a blank or repeated tab id,
 * or a `contribute` that throws. One bad extension must not stop the next one
 * from loading, so the caller records [AddResult.Rejected.reason] against that
 * extension and carries on.
 */
class ExtensionRegistry(private val abiVersion: Int = EXTENSION_ABI_VERSION) {

    /** One tab, tagged with the extension that contributed it. */
    data class RegisteredTab(
        val extensionId: String,
        val extensionTitle: String,
        val tab: SessionTab,
    )

    sealed interface AddResult {
        data object Added : AddResult
        data class Rejected(val reason: String) : AddResult
    }

    private data class Entry(val extension: DshExtension, val tabs: List<SessionTab>)

    private val entries = LinkedHashMap<String, Entry>()

    /**
     * Register one extension and collect its contributions.
     *
     * `contribute` runs here, once, and its tabs are held. A throw from it is
     * caught and turned into a rejection: the APK's code has already run by this
     * point, so the only safe failure is to drop its contributions and say so.
     */
    fun add(extension: DshExtension): AddResult {
        val id = extension.id
        if (id.isBlank()) return AddResult.Rejected("The extension has no id.")
        if (extension.version != abiVersion) {
            return AddResult.Rejected(
                "“${extension.title.ifBlank { id }}” speaks ABI version ${extension.version}; " +
                    "this build accepts $abiVersion.",
            )
        }
        if (entries.containsKey(id)) {
            return AddResult.Rejected("An extension with the id “$id” is already loaded.")
        }
        val tabs = mutableListOf<SessionTab>()
        val result = try {
            extension.contribute(object : ExtensionBuilder {
                override fun sessionTab(tab: SessionTab) {
                    if (tab.id.isBlank()) {
                        throw IllegalArgumentException("A session tab has no id.")
                    }
                    if (tabs.any { it.id == tab.id }) {
                        throw IllegalArgumentException(
                            "Session tab id “${tab.id}” was contributed twice by “$id”.",
                        )
                    }
                    tabs += tab
                }
            })
            AddResult.Added
        } catch (t: Throwable) {
            AddResult.Rejected(
                "“${extension.title.ifBlank { id }}” could not contribute: " +
                    (t.message ?: t::class.java.simpleName),
            )
        }
        if (result == AddResult.Added) {
            entries[id] = Entry(extension, tabs.toList())
        }
        return result
    }

    /** Drop one extension and every contribution it made. */
    fun remove(id: String): Boolean = entries.remove(id) != null

    fun clear() = entries.clear()

    /** Every loaded extension, in registration order. */
    fun list(): List<DshExtension> = entries.values.map { it.extension }

    fun find(id: String): DshExtension? = entries[id]?.extension

    fun ids(): List<String> = entries.keys.toList()

    /** The tabs one extension contributed, in contribution order. */
    fun tabsFor(id: String): List<SessionTab> = entries[id]?.tabs.orEmpty()

    /**
     * Every tab from every loaded extension: built-in ids and APK ids alike.
     *
     * Deterministic: `order`, then the tab id, then the extension id as a final
     * tie-break (two extensions may legitimately use the same tab id; the shell
     * still keys them apart). This is the ordering the session strip draws.
     */
    fun tabs(): List<RegisteredTab> =
        entries.values
            .flatMap { entry ->
                entry.tabs.map { tab ->
                    RegisteredTab(
                        extensionId = entry.extension.id,
                        extensionTitle = entry.extension.title,
                        tab = tab,
                    )
                }
            }
            .sortedWith(compareBy({ it.tab.order }, { it.tab.id }, { it.extensionId }))

    /**
     * The tabs the strip draws for [scope]: [tabs] filtered by each contribution's
     * own [SessionTab.visible] predicate, in the same order.
     */
    fun tabs(scope: ExtensionScope): List<RegisteredTab> =
        tabs().filter { it.tab.isVisibleIn(scope) }
}

/**
 * Whether this tab belongs on screen for [scope].
 *
 * The predicate is extension code evaluated during composition, so a throw hides
 * the tab rather than taking the strip down: an extension that cannot decide does
 * not earn a seat, and one broken predicate must not affect any other tab.
 */
fun SessionTab.isVisibleIn(scope: ExtensionScope): Boolean =
    runCatching { visible(scope) }.getOrDefault(false)
