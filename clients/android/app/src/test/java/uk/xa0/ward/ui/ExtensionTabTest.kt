package uk.xa0.ward.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.extension.FakeExtensionScope
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The session strip's extension seat, driven through the same composables
 * `ChatScreen` uses: [SessionTabs] draws the label and [ExtensionTabContent]
 * composes the tab with a scope.
 *
 * What this proves is the wiring that is pure UI: an extension label appears
 * beside a built-in one, clicking it selects it, and its content runs with a real
 * `ExtensionScope` whose `sessionId` reaches the content. What it cannot see is
 * [uk.xa0.ward.ui.ChatScreen]'s own body selection (that needs the view model and a
 * connection) or anything about a `DexClassLoader` APK — the JVM cannot execute
 * one. Those are device/build facts, not harness facts, and are reported as
 * unverified.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class ExtensionTabTest {

    @get:Rule
    val rule = createComposeRule()

    private val status = SessionTab(
        id = "status",
        label = "Status",
        order = 2000,
        content = { scope ->
            val sessionId by scope.sessionId.collectAsState()
            Text("Session ${sessionId ?: "none"}")
        },
    )

    @Composable
    private fun Harness() {
        var selected by remember { mutableStateOf("chat") }
        Column {
            SessionTabs(
                selectedKey = selected,
                tabs = listOf(
                    SessionTabItem("chat", "Chat"),
                    SessionTabItem("ext:test:status", "Status"),
                ),
                onSelect = { selected = it },
            )
            if (selected == "ext:test:status") {
                ExtensionTabContent(
                    tab = status,
                    scope = FakeExtensionScope("session-1"),
                )
            } else {
                Text("Chat body")
            }
        }
    }

    @Test
    fun `an extension tab appears, selects, and its content sees the session`() {
        rule.setContent { DshTheme { Harness() } }
        rule.waitForIdle()

        rule.onNodeWithText("Chat").assertExists()
        rule.onNodeWithText("Status").assertExists()
        rule.onNodeWithText("Chat body").assertExists()

        // The seat is after the built-in one, measured as horizontal position
        // rather than as a pixel size (the harness has stub text metrics).
        val chatLeft = rule.onNodeWithText("Chat").fetchSemanticsNode().boundsInRoot.left
        val statusLeft = rule.onNodeWithText("Status").fetchSemanticsNode().boundsInRoot.left
        assertTrue("an extension tab must follow the built-in strip", chatLeft < statusLeft)

        rule.onNode(hasText("Status") and hasClickAction()).performClick()
        rule.waitForIdle()

        rule.onNodeWithText("Chat body").assertDoesNotExist()
        rule.onNodeWithText("Session session-1").assertExists()
    }
}
