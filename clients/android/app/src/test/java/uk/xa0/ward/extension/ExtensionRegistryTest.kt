package uk.xa0.ward.extension

import androidx.compose.runtime.Composable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The registry's identity and ordering rules, with no Android in the test.
 *
 * The loader is Android code and cannot run here (a JVM cannot execute a
 * `DexClassLoader` APK, and `PackageManager` has no real archives under
 * Robolectric), but everything the loader *decides* after reading an archive is in
 * this class. These tests are the JVM's share of the harness: duplicate ids,
 * ABI-version refusal, a throwing `contribute`, and the `(order, tab id,
 * extension id)` tab order.
 */
class ExtensionRegistryTest {

    private class FakeExtension(
        override val id: String,
        override val title: String = id,
        override val version: Int = EXTENSION_ABI_VERSION,
        private val tabs: List<SessionTab> = emptyList(),
        private val explode: Boolean = false,
    ) : DshExtension {
        override fun contribute(builder: ExtensionBuilder) {
            if (explode) throw IllegalStateException("this extension is broken")
            tabs.forEach(builder::sessionTab)
        }
    }

    /** The content is never composed by these tests; only its presence is asserted. */
    private fun tab(
        id: String,
        order: Int = 1000,
        label: String = id,
        visible: (ExtensionScope) -> Boolean = { true },
    ): SessionTab = SessionTab(
        id = id,
        label = label,
        order = order,
        visible = visible,
        content = { _ -> },
    )

    @Test
    fun `a fake extension contributes its session tab`() {
        val registry = ExtensionRegistry()

        val result = registry.add(
            FakeExtension(
                id = "test.status",
                title = "Status",
                tabs = listOf(tab("status", label = "Status")),
            ),
        )

        assertEquals(ExtensionRegistry.AddResult.Added, result)
        assertEquals(listOf("status"), registry.tabs().map { it.tab.id })
        assertEquals("test.status", registry.tabs().single().extensionId)
        assertEquals("Status", registry.tabs().single().extensionTitle)
    }

    @Test
    fun `a duplicate extension id is rejected and the first one stays`() {
        val registry = ExtensionRegistry()
        assertEquals(
            ExtensionRegistry.AddResult.Added,
            registry.add(FakeExtension("test.one", title = "One")),
        )

        val duplicate = registry.add(FakeExtension("test.one", title = "Impostor"))

        assertTrue(duplicate is ExtensionRegistry.AddResult.Rejected)
        assertEquals("One", registry.find("test.one")?.title)
        assertEquals(listOf("test.one"), registry.ids())
    }

    @Test
    fun `an unknown ABI version is rejected`() {
        val registry = ExtensionRegistry()

        val result = registry.add(FakeExtension("test.future", version = EXTENSION_ABI_VERSION + 1))

        assertTrue(result is ExtensionRegistry.AddResult.Rejected)
        assertTrue((result as ExtensionRegistry.AddResult.Rejected).reason.contains("ABI version"))
        assertTrue(registry.ids().isEmpty())
    }

    @Test
    fun `a version-1 extension is rejected now that the ABI is 2`() {
        assertEquals(2, EXTENSION_ABI_VERSION)
        val registry = ExtensionRegistry()

        val result = registry.add(FakeExtension("test.older", version = 1))

        assertTrue(result is ExtensionRegistry.AddResult.Rejected)
    }

    @Test
    fun `a throwing contribute fails only that extension`() {
        val registry = ExtensionRegistry()
        val broken = registry.add(FakeExtension("test.broken", explode = true))
        val healthy = registry.add(FakeExtension("test.healthy", tabs = listOf(tab("ok"))))

        assertTrue(broken is ExtensionRegistry.AddResult.Rejected)
        assertEquals(ExtensionRegistry.AddResult.Added, healthy)
        assertEquals(listOf("test.healthy"), registry.ids())
    }

    @Test
    fun `a repeated tab id within one extension is rejected`() {
        val registry = ExtensionRegistry()

        val result = registry.add(
            FakeExtension("test.tabs", tabs = listOf(tab("same"), tab("same", label = "Again"))),
        )

        assertTrue(result is ExtensionRegistry.AddResult.Rejected)
        assertTrue(registry.ids().isEmpty())
    }

    @Test
    fun `tabs are ordered by order, then tab id, then extension id`() {
        val registry = ExtensionRegistry()
        // Deliberately registered out of order: ordering must not depend on the
        // scan order of the extension directory.
        registry.add(
            FakeExtension(
                "test.b",
                tabs = listOf(tab("z", order = 10), tab("a", order = 20)),
            ),
        )
        registry.add(
            FakeExtension(
                "test.a",
                tabs = listOf(tab("z", order = 10), tab("m", order = 10)),
            ),
        )

        assertEquals(
            listOf("test.a:m", "test.a:z", "test.b:z", "test.b:a"),
            registry.tabs().map { "${it.extensionId}:${it.tab.id}" },
        )
    }

    @Test
    fun `tabs are filtered by their own visible predicate`() {
        val registry = ExtensionRegistry()
        registry.add(
            FakeExtension(
                "test.router",
                tabs = listOf(
                    tab("router", label = "Router", visible = { scope -> scope.localRoute.value }),
                    tab("status", label = "Status"),
                ),
            ),
        )
        val local = FakeExtensionScope(id = "s1", localRoute = true)
        val cloud = FakeExtensionScope(id = "s1", localRoute = false)

        assertEquals(listOf("router", "status"), registry.tabs(local).map { it.tab.id })
        assertEquals(listOf("status"), registry.tabs(cloud).map { it.tab.id })
    }

    @Test
    fun `a provider-specific tab follows the provider fact`() {
        val registry = ExtensionRegistry()
        registry.add(
            FakeExtension(
                "test.usage",
                tabs = listOf(
                    tab(
                        "usage",
                        visible = { scope -> scope.providerId.value == "deepseek-official" },
                    ),
                ),
            ),
        )

        assertEquals(
            listOf("usage"),
            registry.tabs(FakeExtensionScope(provider = "deepseek-official")).map { it.tab.id },
        )
        assertTrue(registry.tabs(FakeExtensionScope(provider = "dsh-local")).isEmpty())
    }

    @Test
    fun `a throwing visible predicate hides only its own tab`() {
        val registry = ExtensionRegistry()
        registry.add(
            FakeExtension(
                "test.broken",
                tabs = listOf(
                    tab("broken", visible = { error("no") }),
                    tab("fine"),
                ),
            ),
        )

        assertEquals(listOf("fine"), registry.tabs(FakeExtensionScope()).map { it.tab.id })
    }

    @Test
    fun `removing an extension drops its tabs`() {
        val registry = ExtensionRegistry()
        registry.add(FakeExtension("test.one", tabs = listOf(tab("one"))))
        registry.add(FakeExtension("test.two", tabs = listOf(tab("two"))))

        assertTrue(registry.remove("test.one"))

        assertFalse(registry.remove("test.one"))
        assertEquals(listOf("test.two"), registry.ids())
        assertEquals(listOf("two"), registry.tabs().map { it.tab.id })
    }

    @Test
    fun `a blank id is rejected`() {
        val registry = ExtensionRegistry()
        val result = registry.add(FakeExtension("", title = "Nameless"))
        assertTrue(result is ExtensionRegistry.AddResult.Rejected)
    }
}

/** A scope a pure test can hand to a tab without an Android client. */
internal class FakeExtensionScope(
    id: String? = "session-1",
    provider: String? = null,
    localRoute: Boolean = false,
    routerSession: RouterSession? = null,
    sessionUsage: SessionUsage? = null,
) : ExtensionScope {
    override val sessionId: StateFlow<String?> = MutableStateFlow(id)
    override val providerId: StateFlow<String?> = MutableStateFlow(provider)
    override val localRoute: StateFlow<Boolean> = MutableStateFlow(localRoute)
    override val router: StateFlow<RouterSession?> = MutableStateFlow(routerSession)
    override val usage: StateFlow<SessionUsage?> = MutableStateFlow(sessionUsage)

    override suspend fun rpc(method: String, args: JSONObject): Any? =
        JSONObject().put("method", method)
}

/** Kept so the fake content type is exercised by the compiler in this source set. */
@Composable
internal fun fakeExtensionContent(scope: ExtensionScope) {
    scope.sessionId.value
}
