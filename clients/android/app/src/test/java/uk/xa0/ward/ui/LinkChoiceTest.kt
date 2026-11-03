package uk.xa0.ward.ui

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ApplicationProvider
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class, qualifiers = "w411dp-h891dp")
class LinkChoiceTest {

    @get:Rule
    val rule = createComposeRule()

    private val link = "https://example.com/a/very/long/path?q=1"

    private fun host() {
        rule.setContent {
            DshTheme {
                LinkChoiceHost()
            }
        }
        rule.waitForIdle()
    }

    @Test
    fun `a web link asks before it opens`() {
        openLink(ApplicationProvider.getApplicationContext(), link)
        host()

        rule.onNodeWithText("Open link").assertExists()
        rule.onNodeWithText(link).assertExists()
        rule.onNodeWithText("In app").assertExists()
        rule.onNodeWithText("External app").assertExists()
        rule.onNodeWithText("Cancel").assertExists()
    }

    @Test
    fun `cancel clears the pending link`() {
        openLink(ApplicationProvider.getApplicationContext(), link)
        host()

        rule.onNodeWithText("Cancel").performClick()

        rule.onNodeWithText("Open link").assertDoesNotExist()
    }
}
