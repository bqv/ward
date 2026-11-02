package uk.xa0.ward.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.SessionItem
import uk.xa0.ward.WorkspaceItem
import uk.xa0.ward.model.DrawerGrouping
import uk.xa0.ward.model.WorkspaceListPhase
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The two independent "Show N more" controls a Workspace section draws.
 *
 * A section holds two kinds of row — its top-level sessions and the subagents a
 * parent's caret has disclosed — and they page separately: six sessions and eight
 * subagents. This pins the wiring the pure caps cannot see: that tapping one
 * control moves only its own counter, that the labels count their own kind, and
 * that a subagent whose root session has been paged away is not drawn without its
 * parent chain.
 *
 * The surface is deliberately tall enough to compose the whole paged section: a
 * control below the fold would both fail to exist in the semantics tree and never
 * receive [performClick]'s injected touch, which would make a cap look correct for
 * the wrong reason. Text metrics are not measured anywhere — every assertion is a
 * label, a count or a row's presence.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class, qualifiers = "w411dp-h1400dp")
class SessionsDrawerShowMoreTest {

    @get:Rule
    val rule = createComposeRule()

    private fun session(
        id: String,
        subagent: Boolean = false,
        parent: String? = null,
    ) = SessionItem(
        id = id,
        title = id,
        cwd = "/home/user/var/work/dsh-android",
        updatedAt = 1_000L,
        running = false,
        isSubagent = subagent,
        parentSessionId = parent,
    )

    private fun workspace(vararg sessionIds: String) = WorkspaceItem(
        id = "ws1",
        path = "/home/user/var/work/dsh-android",
        title = "dsh-android",
        sessionIds = sessionIds.toList(),
    )

    /** Eight top-level sessions, the first with [subagents] children. */
    private fun roster(subagents: Int): List<SessionItem> {
        val tops = (1..8).map { session("s$it") }
        val children = (1..subagents).map { session("a$it", subagent = true, parent = "s1") }
        return tops + children
    }

    private fun workspaceOf(roster: List<SessionItem>) =
        workspace(*roster.filterNot { it.isSubagent }.map { it.id }.toTypedArray())

    @Composable
    private fun drawer(
        sessions: List<SessionItem>,
        pinned: Set<String> = emptySet(),
    ) {
        SessionsDrawer(
            sessions = sessions,
            workspaces = listOf(workspaceOf(sessions)),
            workspacesPhase = WorkspaceListPhase.Ready,
            archivedSessionIds = emptySet(),
            pinnedSessionIds = pinned,
            completedSessionIds = emptySet(),
            currentId = null,
            drawerOpen = true,
            loading = false,
            onSelect = {},
            onNew = {},
            onRefresh = {},
            onSettings = {},
            onAbout = {},
            grouping = DrawerGrouping.WORKSPACE,
            orderByUpdated = false,
            showArchived = false,
            collapsedSections = emptySet(),
            onGrouping = {},
            onOrderByUpdated = {},
            onShowArchived = {},
            onCollapsedSections = {},
        )
    }

    /** Disclose the first session's ten subagents, leaving both counters closed. */
    private fun discloseTenSubagents(sessions: List<SessionItem>) {
        rule.setContent { DshTheme { drawer(sessions) } }
        rule.waitForIdle()
        rule.onNodeWithContentDescription("Show subagents").performClick()
        rule.waitForIdle()
    }

    private fun text(value: String) = rule.onNodeWithText(value, useUnmergedTree = true)

    /**
     * The handlers live on the footer's Box, and both controls are the same words
     * once open. Their bounds say which is which: the sessions footer is drawn
     * before the subagents one, so the topmost is the session counter.
     */
    private fun clickFooter(text: String, fromTop: Int) {
        val nodes = rule.onAllNodesWithText(text, useUnmergedTree = true)
        val indices = 0 until nodes.fetchSemanticsNodes().size
        val ordered = indices.sortedBy { nodes[it].fetchSemanticsNode().boundsInRoot.top }
        nodes[ordered[fromTop]].performClick()
        rule.waitForIdle()
    }

    @Test
    fun `a section labels its two pages by kind, and its header still counts top-level sessions only`() {
        discloseTenSubagents(roster(subagents = 10))

        text("Show 2 more sessions").assertExists()
        text("Show 2 more subagents").assertExists()
        // Eight of the ten disclosed children are on screen; the ninth is paged.
        text("a8").assertExists()
        text("a9").assertDoesNotExist()
        // The header's figure is the Workspace's top-level members, not rows drawn.
        text("8").assertExists()
    }

    @Test
    fun `the session control reveals eight sessions and still caps subagents at eight`() {
        discloseTenSubagents(roster(subagents = 10))

        text("Show 2 more sessions").performClick()
        rule.waitForIdle()

        text("s7").assertExists()
        text("s8").assertExists()
        text("Show 2 more sessions").assertDoesNotExist()
        rule.onAllNodesWithText("Show less", useUnmergedTree = true).assertCountEquals(1)
        // The session tap must not have moved the subagent page.
        text("Show 2 more subagents").assertExists()
        text("a9").assertDoesNotExist()
    }

    @Test
    fun `the subagent control reveals all ten subagents without revealing the extra sessions`() {
        discloseTenSubagents(roster(subagents = 10))

        text("Show 2 more subagents").performClick()
        rule.waitForIdle()

        text("a9").assertExists()
        text("a10").assertExists()
        text("Show 2 more subagents").assertDoesNotExist()
        rule.onAllNodesWithText("Show less", useUnmergedTree = true).assertCountEquals(1)
        // The subagent tap must not have moved the session page.
        text("s7").assertDoesNotExist()
        text("s8").assertDoesNotExist()
        text("Show 2 more sessions").assertExists()
    }

    @Test
    fun `show less resets only its own kind`() {
        discloseTenSubagents(roster(subagents = 10))
        text("Show 2 more sessions").performClick()
        rule.waitForIdle()
        text("Show 2 more subagents").performClick()
        rule.waitForIdle()
        rule.onAllNodesWithText("Show less", useUnmergedTree = true).assertCountEquals(2)
        text("a10").assertExists()

        // Sessions footer first: the subagent page must stay open behind it.
        clickFooter("Show less", fromTop = 0)
        text("s7").assertDoesNotExist()
        text("Show 2 more sessions").assertExists()
        text("a10").assertExists()
        rule.onAllNodesWithText("Show less", useUnmergedTree = true).assertCountEquals(1)

        // The remaining "Show less" is the subagent counter's.
        clickFooter("Show less", fromTop = 0)
        text("a9").assertDoesNotExist()
        text("Show 2 more subagents").assertExists()
        // The session counter is still closed: the second reset did not reopen it.
        text("s7").assertDoesNotExist()
        text("Show 2 more sessions").assertExists()
    }

    @Test
    fun `a subagent is not drawn once its parent session is paged away`() {
        // Only s7 discloses a child, so the caret is unambiguous once s7 is drawn.
        val sessions = (1..8).map { session("s$it") } +
            session("a1", subagent = true, parent = "s7")
        rule.setContent { DshTheme { drawer(sessions) } }
        rule.waitForIdle()

        // The child's parent is behind the session page, so neither is drawn yet.
        rule.onNodeWithContentDescription("Show subagents").assertDoesNotExist()

        text("Show 2 more sessions").performClick()
        rule.waitForIdle()
        rule.onNodeWithContentDescription("Show subagents").performClick()
        rule.waitForIdle()
        text("a1").assertExists()

        // Collapsing the session page takes the parent away; the child must go with it.
        clickFooter("Show less", fromTop = 0)
        text("s7").assertDoesNotExist()
        text("a1").assertDoesNotExist()
        text("Show 2 more sessions").assertExists()
    }

    @Test
    fun `pins do not consume the six unpinned session slots`() {
        // Eight unpinned sessions plus two pins. The pinned ids sit late in the
        // roster's own order, so the only reason they are on screen is the exemption.
        val sessions = (1..10).map { session("s$it") }
        val pinned = setOf("s3", "s5")

        rule.setContent { DshTheme { drawer(sessions, pinned) } }
        rule.waitForIdle()

        // Both pins plus the first six unpinned; s9 and s10 are what is still hidden.
        text("s3").assertExists()
        text("s5").assertExists()
        text("s8").assertExists()
        text("s9").assertDoesNotExist()
        text("s10").assertDoesNotExist()
        text("Show 2 more sessions").assertExists()
        // Four would be the remainder if the two pins had eaten sessions' slots.
        text("Show 4 more sessions").assertDoesNotExist()

        text("Show 2 more sessions").performClick()
        rule.waitForIdle()

        // Expanding reveals all eight unpinned, and the pins stay.
        text("s9").assertExists()
        text("s10").assertExists()
        text("s3").assertExists()
        text("s5").assertExists()
        rule.onAllNodesWithText("Show less", useUnmergedTree = true).assertCountEquals(1)
    }

    @Test
    fun `a pin later in the section is drawn and is not counted`() {
        // `pinnedFirst` front-loads pins on screen, so a mid-list pin cannot be
        // produced through the composable; the page rule is exercised directly.
        val rows = (1..9).map { TreeRow(session("s$it"), 0) }
        val page = pageSection(
            rows = rows,
            sessionsExpanded = false,
            subagentsExpanded = false,
            pinnedSessionIds = setOf("s4", "s9"),
        )

        // Both pins are drawn wherever they sit, and the order is untouched.
        assertEquals(
            listOf("s1", "s2", "s3", "s4", "s5", "s6", "s7", "s9"),
            page.rows.map { it.session.id },
        )
        // Seven unpinned against six slots leaves exactly one hidden; the pins
        // add nothing to the count.
        assertEquals(1, page.hiddenSessions)
    }
}
