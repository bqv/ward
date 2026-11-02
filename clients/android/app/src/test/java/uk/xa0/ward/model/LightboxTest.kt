package uk.xa0.ward.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The lightbox's arithmetic: fitting, clamping, zoom focus, paging and dismissal.
 *
 * These are the rules a reader only ever notices by their absence — a picture dragged
 * off into black, a pinch that flings a face past the edge, a short drag that pages
 * when they meant to cancel. Each one is a pure function of four or five numbers, so
 * each one can be pinned here without a screen, a gesture or a decoded bitmap.
 *
 * What these cannot show is the gesture that *feeds* them: whether Compose reports a
 * pinch, a drag or a tap as expected, and whether the pointer stream is consumed by the
 * right handler. That is device work, and it is called out in the report.
 */
class LightboxTest {

    private fun assertClose(expected: Float, actual: Float, message: String = "") {
        assertEquals(message, expected, actual, 0.001f)
    }

    @Test
    fun `a picture larger than the viewport is scaled down to fit`() {
        assertClose(0.5f, LightboxGeometry.fitScale(1000, 500, 500f, 500f))
        assertClose(0.625f, LightboxGeometry.fitScale(800, 1600, 500f, 1000f))
    }

    @Test
    fun `a picture smaller than the viewport is not blown up`() {
        assertClose(1f, LightboxGeometry.fitScale(200, 100, 500f, 500f))
    }

    @Test
    fun `square and empty geometry fits without dividing by zero`() {
        assertClose(1f, LightboxGeometry.fitScale(0, 100, 500f, 500f))
        assertClose(1f, LightboxGeometry.fitScale(100, 100, 0f, 500f))
    }

    @Test
    fun `a fitted picture has no pan room on either axis`() {
        assertClose(0f, LightboxGeometry.panLimit(400f, 500f, 1f))
        assertClose(0f, LightboxGeometry.clampPan(120f, 400f, 500f, 1f))
        assertClose(0f, LightboxGeometry.clampPan(0f, 400f, 400f, 1f))
    }

    @Test
    fun `a zoomed picture may move by half its overflow`() {
        // 500px of fitted content scaled twice is 1000px in a 500px viewport: 250 each side.
        assertClose(250f, LightboxGeometry.panLimit(500f, 500f, 2f))
        assertClose(250f, LightboxGeometry.clampPan(900f, 500f, 500f, 2f))
        assertClose(-250f, LightboxGeometry.clampPan(-900f, 500f, 500f, 2f))
        assertClose(40f, LightboxGeometry.clampPan(40f, 500f, 500f, 2f))
    }

    @Test
    fun `zoom keeps the point under the fingers still`() {
        // Content 500px in a 500px viewport; focus 100px right of centre; 1x to 2x.
        val pan = LightboxGeometry.zoomPan(
            pan = 0f,
            focus = 100f,
            centroidMovement = 0f,
            fromZoom = 1f,
            toZoom = 2f,
            contentSize = 500f,
            viewportSize = 500f,
        )
        assertClose(-100f, pan)
        // The content point under the focus: before u = (100 - 0) / 1 = 100; after
        // -100 + 2 * 100 = 100, still under the same screen point.
        assertClose(100f, pan + 2f * 100f)
    }

    @Test
    fun `a moving pinch centroid is followed by the picture`() {
        val pan = LightboxGeometry.zoomPan(
            pan = 0f,
            focus = 120f,
            centroidMovement = 20f,
            fromZoom = 1f,
            toZoom = 2f,
            contentSize = 500f,
            viewportSize = 500f,
        )
        // The previous centroid was at 100. The content point that was under it is
        // u = 100, and after the zoom it must sit under the new centroid at 120.
        assertClose(120f, pan + 2f * 100f)
    }

    @Test
    fun `zooming near the edge clamps and the focus is the part that gives`() {
        val pan = LightboxGeometry.zoomPan(
            pan = 0f,
            focus = 400f,
            centroidMovement = 0f,
            fromZoom = 1f,
            toZoom = 5f,
            contentSize = 500f,
            viewportSize = 500f,
        )
        // Unclamped this would be -1600; the limit at 5x is (2500 - 500) / 2 = 1000.
        assertClose(-1000f, pan)
    }

    @Test
    fun `dragging left asks for the next page and right for the previous`() {
        assertEquals(1, LightboxGeometry.swipePage(-150f, 500f, 0f))
        assertEquals(-1, LightboxGeometry.swipePage(150f, 500f, 0f))
    }

    @Test
    fun `a short slow drag stays on the page`() {
        assertNull(LightboxGeometry.swipePage(-40f, 500f, 0f))
        assertNull(LightboxGeometry.swipePage(40f, 500f, 0f))
        assertNull(LightboxGeometry.swipePage(-20f, 500f, 0f))
    }

    @Test
    fun `a flick pages without covering a fifth of the width`() {
        assertEquals(1, LightboxGeometry.swipePage(-40f, 500f, -1200f))
        assertEquals(-1, LightboxGeometry.swipePage(40f, 500f, 1200f))
    }

    @Test
    fun `a flick against the drag direction does not page`() {
        assertNull(LightboxGeometry.swipePage(-40f, 500f, 1200f))
        assertNull(LightboxGeometry.swipePage(40f, 500f, -1200f))
    }

    @Test
    fun `a flick still needs some travel so a twitch is not a page turn`() {
        assertNull(LightboxGeometry.swipePage(-10f, 500f, -3000f))
    }

    @Test
    fun `a downward drag dismisses and an upward one never does`() {
        assertTrue(LightboxGeometry.dismiss(150f, 500f, 0f))
        assertFalse(LightboxGeometry.dismiss(50f, 500f, 0f))
        assertFalse(LightboxGeometry.dismiss(-150f, 500f, -2000f))
        assertFalse(LightboxGeometry.dismiss(0f, 500f, 0f))
    }

    @Test
    fun `a downward flick dismisses sooner`() {
        assertTrue(LightboxGeometry.dismiss(50f, 500f, 1200f))
        assertFalse(LightboxGeometry.dismiss(10f, 500f, 1200f))
    }

    @Test
    fun `a tap on the picture is not a tap on the backdrop`() {
        assertTrue(
            LightboxGeometry.containsPoint(
                x = 450f, y = 350f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 0f, panY = 0f,
                contentWidth = 400f, contentHeight = 200f,
                zoom = 1f,
            ),
        )
        assertFalse(
            LightboxGeometry.containsPoint(
                x = 460f, y = 350f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 0f, panY = 0f,
                contentWidth = 400f, contentHeight = 200f,
                zoom = 1f,
            ),
        )
    }

    @Test
    fun `the drawn rectangle follows the zoom and the pan`() {
        // Twice as wide means a point 300px from centre is now over the picture.
        assertTrue(
            LightboxGeometry.containsPoint(
                x = 550f, y = 250f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 0f, panY = 0f,
                contentWidth = 400f, contentHeight = 200f,
                zoom = 2f,
            ),
        )
        // Panned 100px right, the picture's centre is at 350; 700 is inside (350 + 400).
        assertTrue(
            LightboxGeometry.containsPoint(
                x = 700f, y = 250f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 100f, panY = 0f,
                contentWidth = 400f, contentHeight = 200f,
                zoom = 2f,
            ),
        )
        assertFalse(
            LightboxGeometry.containsPoint(
                x = 760f, y = 250f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 100f, panY = 0f,
                contentWidth = 400f, contentHeight = 200f,
                zoom = 2f,
            ),
        )
    }

    @Test
    fun `nothing is inside a picture that has no size`() {
        assertFalse(
            LightboxGeometry.containsPoint(
                x = 250f, y = 250f,
                viewportWidth = 500f, viewportHeight = 500f,
                panX = 0f, panY = 0f,
                contentWidth = 0f, contentHeight = 0f,
                zoom = 1f,
            ),
        )
    }
}
