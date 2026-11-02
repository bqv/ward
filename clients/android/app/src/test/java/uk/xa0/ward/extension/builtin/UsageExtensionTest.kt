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
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.extension.SessionUsage
import uk.xa0.ward.model.FamilyUsage
import uk.xa0.ward.model.SessionStats
import uk.xa0.ward.model.TokenRates
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.ExtensionTabContent
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The Usage readings moved from the token-usage popup to their own extension tab.
 *
 * What this can prove on the JVM: the contribution's id, label and order; that its
 * `visible` predicate follows `providerId` through the registry's filter; and that
 * its content draws the DeepSeek usage view, with its unavailable state when no
 * usage reading has arrived. It cannot see a device's rendering, a live host or a
 * loaded APK, and none of those is claimed here.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class UsageExtensionTest {

    @get:Rule
    val rule = createComposeRule()

    private val stats = SessionStats(
        hasStats = true,
        turns = 3,
        steps = 7,
        hasUsage = true,
        uncachedInputTokens = 1_000,
        cacheReadTokens = 8_000,
        cacheWriteTokens = 500,
        outputTokens = 2_000,
    )

    private val rates = TokenRates(cacheHitInput = 0.04, cacheMissInput = 2.0, output = 8.0)

    private val family = FamilyUsage(
        members = 2,
        counted = 1,
        tokens = 9_000,
        inFigure = 2,
        unreported = 1,
        cost = null,
        unpriced = 0,
    )

    private val usage = SessionUsage(
        stats = stats,
        rates = rates,
        family = family,
        openBalance = {},
    )

    private fun usageTab(): SessionTab {
        var contributed: SessionTab? = null
        UsageExtension().contribute(
            object : ExtensionBuilder {
                override fun sessionTab(tab: SessionTab) {
                    contributed = tab
                }
            },
        )
        return checkNotNull(contributed)
    }

    private fun registryWithUsage(): ExtensionRegistry {
        val registry = ExtensionRegistry()
        assertEquals(ExtensionRegistry.AddResult.Added, registry.add(UsageExtension()))
        return registry
    }

    @Test
    fun `the contribution is the Usage tab before the Router seat`() {
        val extension = UsageExtension()
        assertEquals("uk.xa0.ward.builtin.usage", extension.id)
        assertEquals("Usage", extension.title)

        val tab = usageTab()
        assertEquals("usage", tab.id)
        assertEquals("Usage", tab.label)
        assertEquals(1300, tab.order)
    }

    @Test
    fun `the built-in set carries the Usage tab`() {
        val registry = ExtensionRegistry()
        BuiltInExtensions.all.forEach { extension ->
            assertEquals(ExtensionRegistry.AddResult.Added, registry.add(extension))
        }

        assertTrue(registry.tabs().any { it.tab.id == "usage" })
    }

    @Test
    fun `the usage tab is drawn only for the official DeepSeek provider`() {
        val registry = registryWithUsage()

        val deepSeek = FakeExtensionScope(id = "s1", provider = "deepseek-official")
        val local = FakeExtensionScope(id = "s1", provider = "dsh-local")
        val unnamed = FakeExtensionScope(id = "s1", provider = null)

        assertTrue(registry.tabs(deepSeek).any { it.tab.id == "usage" })
        assertFalse(registry.tabs(local).any { it.tab.id == "usage" })
        assertFalse(registry.tabs(unnamed).any { it.tab.id == "usage" })
    }

    @Test
    fun `the content renders the DeepSeek usage view from the scope`() {
        val tab = usageTab()
        val scope = FakeExtensionScope(
            id = "s1",
            provider = "deepseek-official",
            sessionUsage = usage,
        )

        rule.setContent {
            DshTheme {
                ExtensionBody(tab, scope)
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Session usage").assertExists()
        rule.onNodeWithText("DeepSeek cost").assertExists()
        rule.onNodeWithText("Rates for deepseek-official.").assertExists()
        rule.onNodeWithText("Family, as reported").assertExists()
        rule.onNodeWithText("Balance & top up").assertExists()
    }

    @Test
    fun `an absent usage fact renders the unavailable state`() {
        val tab = usageTab()
        val scope = FakeExtensionScope(
            id = "s1",
            provider = "deepseek-official",
            sessionUsage = null,
        )

        rule.setContent {
            DshTheme {
                ExtensionBody(tab, scope)
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("The host has not reported usage for this session.").assertExists()
    }

    @Composable
    private fun ExtensionBody(tab: SessionTab, scope: FakeExtensionScope) {
        ExtensionTabContent(tab = tab, scope = scope)
    }
}
