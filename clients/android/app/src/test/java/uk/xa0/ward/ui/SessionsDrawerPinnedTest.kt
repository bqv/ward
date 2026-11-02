package uk.xa0.ward.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
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
 * The drawer's pinned-session mark, from the host's pin set to the drawn row.
 *
 * A pure test can pin the set semantics; this pins the *wiring* the drawer owns:
 * that it reads [SessionsDrawer]'s `pinnedSessionIds` for the row it holds, that
 * an archived row suppresses the mark, and that the mark sits inside the row's
 * hit target without taking the tap away from it.
 *
 * The row's overflow menu is the other half of that wiring: the drawer offers
 * the one verb the row's state allows, and the archived rule is enforced by
 * omitting both pin verbs rather than by drawing a disabled one.
 *
 * Ordering is wired here too, because it is a `remember` block the JVM cannot call
 * directly: a pinned row must be drawn above the unpinned ones in List mode and in a
 * Workspace section, the caller's order must survive inside each partition, and an
 * archived id a stale set still names must not lead. The pure rule is
 * `SessionOrderTest.pinnedFirst`; these tests hold the drawer to calling it.
 *
 * The pin glyph itself is not measurable on the JVM — there are no pixels and no
 * vector metrics here — so the assertion is the same fact spoken: the icon's
 * content description, which is what a screen reader announces in place of the
 * glyph. A row that stops drawing the mark therefore fails even though a change
 * to its tint alone would pass unnoticed.
 *
 * What these cannot see is the host: that the registry's set is unarchived-only,
 * that `workspace/pinSession` refuses an archived session, and that a pin reaches
 * this drawer at all are host-contract facts, not drawer facts. The archived test
 * pins the client half of that rule — a stale set must not outvote the archive
 * state drawn beside it — and says nothing about the refusal upstream.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class, qualifiers = "w411dp-h891dp")
class SessionsDrawerPinnedTest {

    @get:Rule
    val rule = createComposeRule()

    /** The mark's spoken label, as `SessionRow` draws it. */
    private val pinMark = "Pinned"

    private fun session(id: String) = SessionItem(
        id = id,
        title = id,
        cwd = "/home/user/var/work/dsh-android",
        updatedAt = 1_000L,
        running = false,
        isSubagent = false,
    )

    @Composable
    private fun drawer(
        sessions: List<SessionItem>,
        pinned: Set<String> = emptySet(),
        archived: Set<String> = emptySet(),
        showArchived: Boolean = false,
        workspaces: List<WorkspaceItem> = emptyList(),
        grouping: DrawerGrouping = DrawerGrouping.FLAT,
        onSelect: (String) -> Unit = {},
        onPin: (String) -> Unit = {},
        onUnpin: (String) -> Unit = {},
    ) {
        SessionsDrawer(
            sessions = sessions,
            workspaces = workspaces,
            workspacesPhase = WorkspaceListPhase.Ready,
            archivedSessionIds = archived,
            pinnedSessionIds = pinned,
            completedSessionIds = emptySet(),
            currentId = null,
            drawerOpen = true,
            loading = false,
            onSelect = onSelect,
            onNew = {},
            onRefresh = {},
            onSettings = {},
            onAbout = {},
            grouping = grouping,
            orderByUpdated = false,
            showArchived = showArchived,
            collapsedSections = emptySet(),
            onGrouping = {},
            onOrderByUpdated = {},
            onShowArchived = {},
            onCollapsedSections = {},
            onPin = onPin,
            onUnpin = onUnpin,
        )
    }

    /** Where a row is drawn; the pin partition is a claim about vertical order. */
    private fun top(text: String): Float =
        rule.onNodeWithText(text, useUnmergedTree = true).fetchSemanticsNode().boundsInRoot.top

    /**
     * A row the host's set names wears the mark. The title is asserted beside it so
     * a missing mark cannot be excused by a row that never rendered.
     */
    @Test
    fun `a pinned session's row carries the pin mark`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    pinned = setOf("s1"),
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("s1", useUnmergedTree = true).assertExists()
        rule.onAllNodesWithContentDescription(pinMark, useUnmergedTree = true).assertCountEquals(1)
    }

    /**
     * The set is the only pin fact the drawer has, so an empty answer must draw no
     * mark. Where the unarchived list carries many rows, a mark that leaked onto an
     * unnamed row would make the set meaningless.
     */
    @Test
    fun `an unpinned session's row carries no pin mark`() {
        rule.setContent {
            DshTheme {
                drawer(sessions = listOf(session("s1")))
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("s1", useUnmergedTree = true).assertExists()
        rule.onAllNodesWithContentDescription(pinMark, useUnmergedTree = true).assertCountEquals(0)
    }

    /**
     * The host cannot pin an archived session, but a set held across an archive can
     * still name one. The saved row is drawn and must draw no pin: the archive state
     * beside it is the current fact, and a stale set must not contradict it.
     */
    @Test
    fun `an archived pinned session draws no pin mark`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    pinned = setOf("s1"),
                    archived = setOf("s1"),
                    showArchived = true,
                )
            }
        }
        rule.waitForIdle()

        // The row itself is still there — this is a suppressed mark, not a hidden row.
        rule.onNodeWithText("s1", useUnmergedTree = true).assertExists()
        rule.onAllNodesWithContentDescription(pinMark, useUnmergedTree = true).assertCountEquals(0)
    }

    /**
     * The mark is presentational. A tap on the pinned row's title must still reach
     * `onSelect`: the icon sits inside the row's hit target, so an icon that took
     * pointer input for itself, or a pinned row whose click was replaced by a pin
     * action, would leave the tap unanswered.
     */
    @Test
    fun `the pin mark does not swallow a tap meant for the row`() {
        var selected: String? = null
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    pinned = setOf("s1"),
                    onSelect = { selected = it },
                )
            }
        }
        rule.waitForIdle()

        // The pinned row is the one being tapped; the mark is on it.
        rule.onAllNodesWithContentDescription(pinMark, useUnmergedTree = true).assertCountEquals(1)
        rule.onNodeWithText("s1").performClick()
        rule.waitForIdle()

        assertEquals("s1", selected)
    }

    /**
     * An unpinned row offers the verb that changes its state, and only that verb:
     * choosing "Pin session" must report the row's own id to `onPin`, and a
     * mis-wired item that also fired `onUnpin` would leave the host's set
     * unchanged while the drawer believed it had asked.
     */
    @Test
    fun `an unpinned row's menu pins the session and never unpins it`() {
        val pinned = mutableListOf<String>()
        val unpinned = mutableListOf<String>()
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    onPin = { pinned += it },
                    onUnpin = { unpinned += it },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithContentDescription("Session actions").performClick()
        rule.waitForIdle()

        rule.onNodeWithText("Unpin session").assertDoesNotExist()
        rule.onNodeWithText("Pin session").assertExists()
        rule.onNodeWithText("Pin session").performClick()
        rule.waitForIdle()

        assertEquals(listOf("s1"), pinned)
        assertTrue("choosing Pin session must not unpin", unpinned.isEmpty())
    }

    /**
     * The mirror of the unpinned case. A pinned row must offer the inverse verb,
     * or the only pin action available would be one the host already recorded;
     * choosing it must reach `onUnpin` with that row's id and never `onPin`.
     */
    @Test
    fun `a pinned row's menu unpins the session and never pins it`() {
        val pinned = mutableListOf<String>()
        val unpinned = mutableListOf<String>()
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    pinned = setOf("s1"),
                    onPin = { pinned += it },
                    onUnpin = { unpinned += it },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithContentDescription("Session actions").performClick()
        rule.waitForIdle()

        rule.onNodeWithText("Pin session").assertDoesNotExist()
        rule.onNodeWithText("Unpin session").assertExists()
        rule.onNodeWithText("Unpin session").performClick()
        rule.waitForIdle()

        assertEquals(listOf("s1"), unpinned)
        assertTrue("choosing Unpin session must not pin", pinned.isEmpty())
    }

    /**
     * The host refuses to pin an archived session, so an archived row must offer
     * no pin verb at all — the menu's own rule, not the host's refusal seen later.
     * The stale-set case is the one that matters: an archived row the set still
     * names must offer neither "Pin session" nor "Unpin session".
     *
     * `Restore` is asserted present so a menu that failed to open cannot pass by
     * offering nothing at all.
     */
    @Test
    fun `an archived row's menu offers neither pin verb`() {
        val pinned = mutableListOf<String>()
        val unpinned = mutableListOf<String>()
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    pinned = setOf("s1"),
                    archived = setOf("s1"),
                    showArchived = true,
                    onPin = { pinned += it },
                    onUnpin = { unpinned += it },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithContentDescription("Session actions").performClick()
        rule.waitForIdle()

        // The menu is open: the verbs that do not depend on pin state are there.
        rule.onNodeWithText("Restore").assertExists()
        rule.onNodeWithText("Pin session").assertDoesNotExist()
        rule.onNodeWithText("Unpin session").assertDoesNotExist()
        assertTrue("an archived row must not pin", pinned.isEmpty())
        assertTrue("an archived row must not unpin", unpinned.isEmpty())
    }

    /**
     * The web's `deriveFlat` fronts pinned rows over the flat order. The roster order
     * is s1, s2, s3 and s3 is pinned, so the drawer must draw s3 first and leave s1
     * before s2 — a pin partitions the list, it does not sort inside a partition.
     *
     * `boundsInRoot` is what the JVM harness can measure: the rows' vertical order,
     * not their pixels or text metrics.
     */
    @Test
    fun `list mode draws a pinned row ahead of the unpinned ones`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1"), session("s2"), session("s3")),
                    pinned = setOf("s3"),
                )
            }
        }
        rule.waitForIdle()

        val pinned = top("s3")
        val first = top("s1")
        val second = top("s2")
        assertTrue("pinned s3 must draw above s1 (s3=$pinned s1=$first)", pinned < first)
        assertTrue("the unpinned rows must keep their order (s1=$first s2=$second)", first < second)
    }

    /**
     * The same rule inside a Workspace section, applied after the host's manual order:
     * `sessionIds` says s2, s1, s3 and s3 is pinned, so the section must draw
     * s3, s2, s1. This is the half a flat-only fix would miss, and it also shows the
     * manual order surviving inside each partition rather than being re-sorted.
     */
    @Test
    fun `a workspace section draws a pinned row ahead and keeps its manual order`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1"), session("s2"), session("s3")),
                    workspaces = listOf(
                        WorkspaceItem(
                            id = "w1",
                            path = "/home/user/var/work/dsh-android",
                            title = "dsh-android",
                            sessionIds = listOf("s2", "s1", "s3"),
                        ),
                    ),
                    grouping = DrawerGrouping.WORKSPACE,
                    pinned = setOf("s3"),
                )
            }
        }
        rule.waitForIdle()

        val pinned = top("s3")
        val manualFirst = top("s2")
        val manualSecond = top("s1")
        assertTrue(
            "pinned s3 must lead the section (s3=$pinned s2=$manualFirst)",
            pinned < manualFirst,
        )
        assertTrue(
            "the manual order must hold inside the unpinned partition " +
                "(s2=$manualFirst s1=$manualSecond)",
            manualFirst < manualSecond,
        )
    }

    /**
     * A stale set naming an archived row must not reorder it. The drawer leaves
     * Archived on so the row is drawn at all; the roster already puts `s2` before the
     * archived `s1`, so a pin that still named `s1` would be the only thing able to
     * lift it. The drawn order must stay the roster's.
     */
    @Test
    fun `an archived pinned row does not lead`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s2"), session("s1")),
                    pinned = setOf("s1"),
                    archived = setOf("s1"),
                    showArchived = true,
                )
            }
        }
        rule.waitForIdle()

        val live = top("s2")
        val archived = top("s1")
        assertTrue(
            "archived pinned s1 must not lead s2 (s2=$live s1=$archived)",
            live < archived,
        )
    }
}
