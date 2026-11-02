package uk.xa0.ward.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
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

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class, qualifiers = "w411dp-h891dp")
class SessionsDrawerWorkspaceActionsTest {

    @get:Rule
    val rule = createComposeRule()

    private fun workspace(id: String, title: String) = WorkspaceItem(
        id = id,
        path = "/work/$id",
        title = title,
        sessionIds = emptyList(),
    )

    private fun session(id: String) = SessionItem(
        id = id,
        title = "A session",
        cwd = "/work/elsewhere",
        updatedAt = 1_000L,
        running = false,
        isSubagent = false,
    )

    @Composable
    private fun drawer(
        workspaces: List<WorkspaceItem> = emptyList(),
        sessions: List<SessionItem> = emptyList(),
        grouping: DrawerGrouping = DrawerGrouping.WORKSPACE,
        onRenameWorkspace: (String, String) -> Unit = { _, _ -> },
        onDeleteWorkspace: (String) -> Unit = {},
    ) {
        SessionsDrawer(
            sessions = sessions,
            workspaces = workspaces,
            workspacesPhase = WorkspaceListPhase.Ready,
            archivedSessionIds = emptySet(),
            completedSessionIds = emptySet(),
            currentId = null,
            drawerOpen = true,
            loading = false,
            onSelect = {},
            onNew = {},
            onRefresh = {},
            onSettings = {},
            onAbout = {},
            grouping = grouping,
            orderByUpdated = false,
            showArchived = false,
            collapsedSections = emptySet(),
            onGrouping = {},
            onOrderByUpdated = {},
            onShowArchived = {},
            onCollapsedSections = {},
            onRenameWorkspace = onRenameWorkspace,
            onDeleteWorkspace = onDeleteWorkspace,
        )
    }

    @Test
    fun `a real workspace menu offers rename and delete`() {
        rule.setContent {
            DshTheme {
                drawer(workspaces = listOf(workspace("w1", "Alpha")))
            }
        }
        rule.waitForIdle()

        rule.onNodeWithContentDescription("Workspace actions").performClick()
        rule.onNodeWithText("Rename").assertExists()
        rule.onNodeWithText("Delete workspace").assertExists()
    }

    @Test
    fun `Delete asks first and only confirms through the destructive action`() {
        var deleted: String? = null
        rule.setContent {
            DshTheme {
                drawer(
                    workspaces = listOf(workspace("w1", "Alpha")),
                    onDeleteWorkspace = { deleted = it },
                )
            }
        }
        rule.waitForIdle()

        rule.onNodeWithContentDescription("Workspace actions").performClick()
        rule.onNodeWithText("Delete workspace").performClick()
        rule.onNodeWithText("Delete workspace “Alpha”?").assertExists()
        rule.onNodeWithText("Cancel").performClick()
        assertNull(deleted)

        rule.onNodeWithContentDescription("Workspace actions").performClick()
        rule.onNodeWithText("Delete workspace").performClick()
        rule.onNodeWithText("Delete").performClick()
        assertEquals("w1", deleted)
    }

    @Test
    fun `synthetic groups have no workspace actions`() {
        rule.setContent {
            DshTheme {
                drawer(
                    sessions = listOf(session("s1")),
                    grouping = DrawerGrouping.FLAT,
                )
            }
        }
        rule.waitForIdle()

        rule.onAllNodesWithContentDescription("Workspace actions").assertCountEquals(0)
    }

    @Test
    fun `the ungrouped header has no workspace actions`() {
        rule.setContent {
            DshTheme {
                drawer(sessions = listOf(session("s1")))
            }
        }
        rule.waitForIdle()

        rule.onNodeWithText("Ungrouped").assertExists()
        rule.onAllNodesWithContentDescription("Workspace actions").assertCountEquals(0)
    }
}
