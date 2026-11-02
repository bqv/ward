package uk.xa0.ward.extension.builtin

import androidx.compose.runtime.Composable
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.extension.ExtensionBuilder
import uk.xa0.ward.extension.ExtensionRegistry
import uk.xa0.ward.extension.FakeExtensionScope
import uk.xa0.ward.extension.RouterSession
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.model.LocalProvider
import uk.xa0.ward.model.RouterProbeResult
import uk.xa0.ward.model.RouterTarget
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.ExtensionTabContent
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The Router moved from a `ChatView` branch to an extension tab.
 *
 * What this can prove on the JVM: the contribution's id, label and order; that its
 * `visible` predicate follows `localRoute` through the registry's filter; and that
 * its content draws the router (or the unavailable state) from
 * `ExtensionScope.router`. What it cannot see is a device's rendering, a live host
 * probe or a loaded APK, and none of those is claimed here.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class RouterExtensionTest {

    @get:Rule
    val rule = createComposeRule()

    private val provider = LocalProvider(
        id = "dsh-local",
        settingsNs = "llm-pi-ai",
        displayName = "Local router",
        baseURL = "http://127.0.0.1:55558/v1",
    )

    private val target = RouterTarget(provider = provider, modelId = "Qwen3.5-35B")

    private val routerSession = RouterSession(
        target = target,
        probe = RouterProbeResult.Idle,
        effort = null,
        check = {},
    )

    private fun routerTab(): SessionTab {
        var contributed: SessionTab? = null
        RouterExtension().contribute(
            object : ExtensionBuilder {
                override fun sessionTab(tab: SessionTab) {
                    contributed = tab
                }
            },
        )
        return checkNotNull(contributed)
    }

    private fun registryWithRouter(): ExtensionRegistry {
        val registry = ExtensionRegistry()
        assertEquals(ExtensionRegistry.AddResult.Added, registry.add(RouterExtension()))
        return registry
    }

    @Test
    fun `the contribution is the Router tab at the old fourth seat`() {
        val tab = routerTab()
        assertEquals("router", tab.id)
        assertEquals("Router", tab.label)
        assertEquals(1400, tab.order)
    }

    @Test
    fun `the built-in set carries the Router and no Diagnostics tab`() {
        val registry = ExtensionRegistry()
        BuiltInExtensions.all.forEach { extension ->
            assertEquals(ExtensionRegistry.AddResult.Added, registry.add(extension))
        }

        assertTrue(registry.tabs().any { it.tab.id == "router" })
        assertFalse(registry.tabs().any { it.tab.label == "Diagnostics" })
    }

    @Test
    fun `the router tab is drawn only while the route is local`() {
        val registry = registryWithRouter()

        val local = FakeExtensionScope(id = "s1", localRoute = true)
        val cloud = FakeExtensionScope(id = "s1", localRoute = false)

        assertTrue(registry.tabs(local).any { it.tab.id == "router" })
        assertFalse(registry.tabs(cloud).any { it.tab.id == "router" })
    }

    @Test
    fun `the content renders the router from the scope`() {
        val tab = routerTab()
        val scope = FakeExtensionScope(id = "s1", localRoute = true, routerSession = routerSession)

        rule.setContent {
            DshTheme {
                ExtensionBody(tab, scope)
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("llama.cpp router").assertExists()
    }

    @Test
    fun `a null router fact renders the unavailable state`() {
        val tab = routerTab()
        val scope = FakeExtensionScope(id = "s1", localRoute = true, routerSession = null)

        rule.setContent {
            DshTheme {
                ExtensionBody(tab, scope)
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("The router is not available for this route.").assertExists()
    }

    @Composable
    private fun ExtensionBody(tab: SessionTab, scope: FakeExtensionScope) {
        ExtensionTabContent(tab = tab, scope = scope)
    }
}
