package uk.xa0.ward.ui

import android.graphics.Bitmap
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import kotlinx.coroutines.CompletableDeferred
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.DownloadResult
import uk.xa0.ward.model.FileDownloader
import uk.xa0.ward.model.ImageLoad
import uk.xa0.ward.scroll.HarnessApplication
import uk.xa0.ward.ui.theme.DshTheme

/**
 * The viewer's own composable: what a reader sees, and the ways out it offers.
 *
 * ## What these can and cannot show
 *
 * They can show the chrome that is *decided in composition*: which caption and pager are
 * drawn, that a single picture has no "1 of 1", that the close control carries a label,
 * that a missing picture states the host's reason and offers Retry, and that the save
 * control appears only where a path and a downloader exist. They can also show that the
 * close control actually closes, because closing is a state change rather than a gesture.
 *
 * They cannot show any of the touch rules. A pinch, a pan, a swipe, a double-tap and a
 * downward drag are pointer streams whose velocity and timing come from the platform;
 * Robolectric's clock and its stub display make a synthesised swipe a statement about the
 * harness rather than about a finger. The arithmetic those gestures feed is pinned in
 * `model/LightboxTest`; the gesture *recognition* — and whether Compose reports a pinch
 * as a pinch at all — is device work, and is named as such in the report. Nor can these
 * show that the backdrop reaches under the status bar and the chat header: that is
 * insets and real layout, which a 320dp Robolectric screen does not model.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class LightboxOverlayTest {

    @get:Rule
    val rule = createComposeRule()

    private val bitmap by lazy {
        Bitmap.createBitmap(240, 120, Bitmap.Config.ARGB_8888).asImageBitmap()
    }

    private fun page(
        key: String,
        title: String = key,
        description: String? = title,
        savePath: String? = null,
        load: suspend () -> ImageLoad = { ImageLoad.Failed("No bytes in this test.") },
    ) = LightboxPage(
        key = key,
        title = title,
        description = description,
        savePath = savePath,
        load = load,
    )

    private fun show(
        pages: List<LightboxPage>,
        start: Int = 0,
        downloader: FileDownloader? = null,
        onClose: () -> Unit = {},
    ) {
        rule.setContent {
            DshTheme {
                Box(Modifier.fillMaxSize()) {
                    LightboxOverlay(
                        request = LightboxRequest(pages, start),
                        downloader = downloader,
                        onClose = onClose,
                    )
                }
            }
        }
        rule.waitForIdle()
    }

    private fun downloader() = FileDownloader { _, _ -> DownloadResult.Refused("Not in this test.") }

    @Test
    fun `a single picture shows its name and description but no pager`() {
        show(
            listOf(
                page(
                    key = "one",
                    title = "screenshot.png",
                    description = "A screenshot of the settings screen",
                    load = { ImageLoad.Ready(bitmap) },
                ),
            ),
        )
        rule.onNodeWithText("screenshot.png").assertIsDisplayed()
        rule.onNodeWithContentDescription("A screenshot of the settings screen").assertExists()
        // "1 of 1" is the chrome a single picture does not need.
        rule.onNodeWithTag("lightbox-position").assertDoesNotExist()
        rule.onNodeWithTag("lightbox-loading").assertDoesNotExist()
    }

    @Test
    fun `a set shows which page of how many`() {
        show(
            listOf(
                page("one", "one.png"),
                page("two", "two.png"),
                page("three", "three.png"),
            ),
            start = 1,
        )
        rule.onNodeWithText("two.png").assertIsDisplayed()
        rule.onNodeWithText("2 of 3").assertIsDisplayed()
    }

    @Test
    fun `the close control is labelled and closes the viewer`() {
        var closed = false
        show(listOf(page("one")), onClose = { closed = true })
        val close = rule.onNodeWithContentDescription("Close image preview")
        close.assertExists()
        close.performClick()
        rule.waitForIdle()
        assertEquals(true, closed)
    }

    @Test
    fun `a picture that failed says why and retries`() {
        var attempts = 0
        show(
            listOf(
                page(
                    key = "one",
                    title = "photo.jpg",
                    load = {
                        attempts += 1
                        if (attempts == 1) {
                            ImageLoad.Failed("The host refused this picture.")
                        } else {
                            ImageLoad.Ready(bitmap)
                        }
                    },
                ),
            ),
        )
        rule.onNodeWithText("Could not load this image.").assertIsDisplayed()
        rule.onNodeWithText("The host refused this picture.").assertIsDisplayed()
        rule.onNodeWithTag("lightbox-retry").performClick()
        rule.waitForIdle()
        assertEquals(2, attempts)
        rule.onNodeWithText("Could not load this image.").assertDoesNotExist()
        rule.onNodeWithContentDescription("photo.jpg").assertExists()
    }

    @Test
    fun `a picture is loading until its bytes resolve`() {
        val gate = CompletableDeferred<ImageLoad>()
        show(listOf(page("one", load = { gate.await() })))
        // A reader is never left with an unexplained black rectangle while the host reads.
        rule.onNodeWithTag("lightbox-loading").assertExists()
        gate.complete(ImageLoad.Ready(bitmap))
        rule.waitForIdle()
        rule.onNodeWithTag("lightbox-loading").assertDoesNotExist()
        rule.onNodeWithContentDescription("one").assertExists()
    }

    @Test
    fun `a path with a downloader offers the one save control`() {
        show(
            listOf(page("one", savePath = "/w/a.png")),
            downloader = downloader(),
        )
        rule.onNodeWithText("Download").assertExists()
    }

    @Test
    fun `an attachment with no path offers no save control`() {
        show(listOf(page("one")), downloader = downloader())
        rule.onNodeWithText("Download").assertDoesNotExist()
    }

    @Test
    fun `a viewer with no downloader offers no save control`() {
        show(listOf(page("one", savePath = "/w/a.png")), downloader = null)
        rule.onNodeWithText("Download").assertDoesNotExist()
    }
}
