package uk.xa0.ward.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.isToggleable
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.PluginBundle
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The Plugins section's bundle list: its groups and their order, what a bundle row
 * says, and what its switch and Retry actually call.
 *
 * A switch here is only a switch when it can call something. Material3's `Switch`
 * attaches its `toggleable` semantics only when `onCheckedChange` is non-null, so a
 * read-only or busy row has *no* toggleable node at all; the honest assertion is
 * that no node is toggleable and none is clickable, not `assertIsNotEnabled` on a
 * node the composed tree never creates.
 *
 * What the JVM cannot see is the text itself: Robolectric lays every string out as
 * one 260px line with stub metrics, so these assertions are about presence and
 * wiring, never about wrapping, sizes or tint. The one geometry claim is the
 * Official-before-Installed order, measured as `boundsInRoot.top` rather than as a
 * pixel style. Nor can these tests see the host — whether `setBundleEnabled`
 * accepts the name, or whether a read after a change refreshes the list — which is
 * a host-contract fact rather than a section fact.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class PluginsSectionTest {

    @get:Rule
    val rule = createComposeRule()

    private fun plugin(
        name: String = "@acme/alpha",
        version: String? = "1.2.3",
        title: String = "Alpha",
        description: String? = "Does alpha things.",
        enabled: Boolean = true,
        installed: Boolean = true,
        optional: Boolean = false,
        readOnlyReason: String? = null,
        errorCode: String? = null,
        errorText: String? = null,
        hasError: Boolean = false,
    ) = PluginBundle(
        name = name,
        version = version,
        title = title,
        description = description,
        enabled = enabled,
        installed = installed,
        optional = optional,
        readOnlyReason = readOnlyReason,
        errorCode = errorCode,
        errorText = errorText,
        hasError = hasError,
    )

    @Composable
    private fun section(
        plugins: List<PluginBundle> = emptyList(),
        loading: Boolean = false,
        readError: String? = null,
        changing: String? = null,
        actionError: String? = null,
        notice: String? = null,
        onToggle: ((String, Boolean) -> Unit)? = { _, _ -> },
        onRetry: (() -> Unit)? = {},
    ) {
        // A Column gives the section's notes, group labels and rows a vertical
        // layout, so the group-order assertion has positions to compare at all.
        Column {
            PluginsSection(
                plugins = plugins,
                loading = loading,
                readError = readError,
                changing = changing,
                actionError = actionError,
                notice = notice,
                onToggle = onToggle,
                onRetry = onRetry,
            )
        }
    }

    /** Where a node is drawn; group order is a claim about vertical order. */
    private fun top(text: String): Float =
        rule.onNodeWithText(text, useUnmergedTree = true).fetchSemanticsNode().boundsInRoot.top

    @Test
    fun `the first read says it is reading`() {
        rule.setContent { DshTheme { section(loading = true) } }
        rule.waitForIdle()

        rule.onNodeWithText("Reading plugins…").assertExists()
        rule.onNodeWithText("No plugin bundles are installed in this deployment.").assertDoesNotExist()
    }

    @Test
    fun `a read error offers Retry and Retry invokes onRetry`() {
        var retries = 0
        rule.setContent {
            DshTheme {
                section(
                    readError = "The plugin bundle list could not be read.",
                    onRetry = { retries++ },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("The plugin bundle list could not be read.").assertExists()
        rule.onNodeWithText("No plugin bundles are installed in this deployment.").assertDoesNotExist()

        rule.onNodeWithText("Retry").performClick()
        rule.waitForIdle()

        assertEquals(1, retries)
    }

    @Test
    fun `an empty inventory says there are no bundles`() {
        rule.setContent { DshTheme { section() } }
        rule.waitForIdle()

        rule.onNodeWithText("No plugin bundles are installed in this deployment.").assertExists()
        rule.onNodeWithText("Reading plugins…").assertDoesNotExist()
    }

    /**
     * The web tab draws the Official group (an optional bundle the profile has not
     * installed) above Installed, each with its count. A bundle that is neither
     * installed nor optional and carries no error is not listed at all, so the count
     * is the filter's count rather than every row the host sent.
     */
    @Test
    fun `official bundles are drawn above installed ones with their counts`() {
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(
                        plugin(
                            name = "@acme/alpha",
                            title = "Alpha",
                            installed = false,
                            optional = true,
                        ),
                        plugin(
                            name = "@acme/beta",
                            title = "Beta",
                            installed = false,
                            optional = true,
                        ),
                        plugin(
                            name = "@acme/gamma",
                            title = "Gamma",
                            installed = true,
                        ),
                        plugin(
                            name = "@acme/ghost",
                            title = "Ghost",
                            installed = false,
                            optional = false,
                        ),
                    ),
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Official (2)").assertExists()
        rule.onNodeWithText("Installed (1)").assertExists()
        rule.onNodeWithText("Ghost").assertDoesNotExist()
        // The Installed row is present under its own label, but the harness reports
        // its `boundsInRoot.top` as 0, so the position is not measurable here; the
        // heading order is, and that is the group claim this test can make.
        rule.onNodeWithText("Gamma").assertExists()

        val official = top("Official (2)")
        val firstOfficial = top("Alpha")
        val secondOfficial = top("Beta")
        val installed = top("Installed (1)")
        assertTrue(
            "the Official group must lead (Official=$official Alpha=$firstOfficial)",
            official < firstOfficial,
        )
        assertTrue(
            "the official rows must follow the group label (Alpha=$firstOfficial Beta=$secondOfficial)",
            firstOfficial < secondOfficial,
        )
        assertTrue(
            "the Installed group must follow the Official rows (Beta=$secondOfficial Installed=$installed)",
            secondOfficial < installed,
        )
    }

    /**
     * One row draws its title, package name, description and state; the switch
     * reports the bundle name the host addressed and the value the bundle is *not*:
     * toggling an enabled bundle asks for off.
     */
    @Test
    fun `an enabled bundle row draws its details and its switch asks for off`() {
        val toggles = mutableListOf<Pair<String, Boolean>>()
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(
                        plugin(
                            name = "@acme/alpha",
                            title = "Alpha",
                            description = "Does alpha things.",
                            enabled = true,
                        ),
                    ),
                    onToggle = { name, enabled -> toggles += name to enabled },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Alpha").assertExists()
        rule.onNodeWithText("@acme/alpha").assertExists()
        rule.onNodeWithText("Does alpha things.").assertExists()
        rule.onNodeWithText("Enabled").assertExists()

        rule.onNode(isToggleable()).assertIsOn().performClick()
        rule.waitForIdle()

        assertEquals(listOf("@acme/alpha" to false), toggles)
    }

    /** A disabled bundle says so, and its switch asks for the opposite: on. */
    @Test
    fun `a disabled bundle row says Disabled and its switch asks for on`() {
        val toggles = mutableListOf<Pair<String, Boolean>>()
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(plugin(name = "@acme/beta", title = "Beta", enabled = false)),
                    onToggle = { name, enabled -> toggles += name to enabled },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Disabled").assertExists()

        rule.onNode(isToggleable()).assertIsOff().performClick()
        rule.waitForIdle()

        assertEquals(listOf("@acme/beta" to true), toggles)
    }

    /** An errored bundle says `Problem` and shows the host's own diagnostic. */
    @Test
    fun `an errored bundle row says Problem and shows the diagnostic`() {
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(
                        plugin(
                            name = "@acme/alpha",
                            title = "Alpha",
                            hasError = true,
                            errorCode = "bundle/load-failed",
                            errorText = "The bundle could not be loaded.",
                        ),
                    ),
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Problem").assertExists()
        rule.onNodeWithText("The bundle could not be loaded.").assertExists()
    }

    /**
     * A read-only bundle explains why it cannot be switched and offers nothing to
     * tap: the `Switch` is composed with a null callback, which drops its toggleable
     * and clickable semantics entirely.
     */
    @Test
    fun `a read-only bundle says why and has no switch to tap`() {
        val toggles = mutableListOf<Pair<String, Boolean>>()
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(
                        plugin(
                            name = "@acme/alpha",
                            title = "Alpha",
                            readOnlyReason = "management-required",
                        ),
                    ),
                    onToggle = { name, enabled -> toggles += name to enabled },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Alpha").assertExists()
        rule.onNodeWithText(
            "This plugin bundle is managed by the deployment's configuration and cannot be switched here.",
        ).assertExists()
        rule.onAllNodes(isToggleable()).assertCountEquals(0)
        rule.onAllNodes(hasClickAction()).assertCountEquals(0)
        assertTrue("a read-only bundle must not ask for a toggle", toggles.isEmpty())
    }

    /**
     * While one bundle is busy every switch is disabled, so the busy row itself must
     * neither draw a state nor accept a tap. `changing` is the bundle name the view
     * model is waiting on; the row says `Changing…` instead of its status.
     */
    @Test
    fun `a busy bundle row says Changing and has no switch to tap`() {
        val toggles = mutableListOf<Pair<String, Boolean>>()
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(plugin(name = "@acme/alpha", title = "Alpha", enabled = true)),
                    changing = "@acme/alpha",
                    onToggle = { name, enabled -> toggles += name to enabled },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Changing…").assertExists()
        rule.onAllNodes(isToggleable()).assertCountEquals(0)
        rule.onAllNodes(hasClickAction()).assertCountEquals(0)
        assertTrue("a busy bundle must not ask for a toggle", toggles.isEmpty())
    }

    @Test
    fun `the action error and the restart notice are drawn above the list`() {
        rule.setContent {
            DshTheme {
                section(
                    plugins = listOf(plugin(name = "@acme/alpha", title = "Alpha")),
                    actionError = "The host refused to change Alpha.",
                    notice = "Restart DSH for the change to take effect.",
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("The host refused to change Alpha.").assertExists()
        rule.onNodeWithText("Restart DSH for the change to take effect.").assertExists()
        // The row is still drawn beside its notes; the notes did not replace it.
        rule.onNodeWithText("Alpha").assertExists()
        rule.onAllNodes(isToggleable()).assertCountEquals(1)
    }
}
