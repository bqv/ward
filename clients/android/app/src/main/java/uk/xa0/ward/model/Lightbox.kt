package uk.xa0.ward.model

import kotlin.math.abs
import kotlin.math.min

/**
 * The lightbox's finger arithmetic, without a screen.
 *
 * Everything here is a decision that can be made from four numbers — the viewport, the
 * picture's fitted size, how far it has been scaled, and how far it has been dragged —
 * so it is decided where it can be tested rather than inside a pointer-input lambda
 * where it can only be felt. The composable owns the gestures and the drawing; this
 * owns the limits they must obey.
 *
 * Three of the rules are the ones a reader notices when they are wrong:
 *
 *  - **A fitted picture cannot be panned at all.** The limit is zero whenever the
 *    scaled content is no larger than the viewport, so the image cannot be dragged off
 *    into empty space at rest and a vertical drag at fit is not a pan.
 *  - **A zoomed picture is clamped on each axis independently.** A tall portrait can
 *    still be panned vertically once it is taller than the screen, while staying centred
 *    horizontally, which is what "the image cannot leave the screen" means in practice.
 *  - **Zoom keeps the point under the fingers still.** The pan is recomputed about the
 *    pinch focus, then clamped, so pinching a face does not fling it off the edge.
 *
 * The paging and dismissal thresholds are here for the same reason: a horizontal swipe
 * that pages, and a vertical one that dismisses, are the two ways a gesture at fit can
 * end, and neither may be mistaken for the other.
 */
object LightboxGeometry {

    /** Fit-to-viewport. Below this the image would be smaller than the screen for no gain. */
    const val MIN_ZOOM: Float = 1f

    /** Far enough in to read a screenshot's text, short of turning pixels into mush. */
    const val MAX_ZOOM: Float = 5f

    /** Double-tap's sensible magnified size: readable, and still navigable by panning. */
    const val DOUBLE_TAP_ZOOM: Float = 2.5f

    /** Fraction of the width a drag must cover to change page without a flick. */
    const val PAGE_FRACTION: Float = 0.2f

    /** Fraction of the width a flick may shorten the distance to, down to [PAGE_FLICK_MIN]. */
    const val PAGE_FLICK_FRACTION: Float = 0.06f

    /** Pixels per second that turn a short drag into a page turn. */
    const val PAGE_FLICK_VELOCITY: Float = 900f

    /** Fraction of the height a downward drag must cover to dismiss without a flick. */
    const val DISMISS_FRACTION: Float = 0.2f

    /** Fraction of the height a downward flick may shorten the distance to. */
    const val DISMISS_FLICK_FRACTION: Float = 0.05f

    /** Pixels per second that turn a short downward drag into a dismissal. */
    const val DISMISS_FLICK_VELOCITY: Float = 900f

    /**
     * How large the picture is drawn at rest, as a fraction of its own pixels.
     *
     * **Never above 1**: a small picture is shown at its own size rather than blown up
     * to fill a tablet, which is the web's rule for its original-image preview too.
     * A picture larger than the viewport is scaled down until its longer edge fits.
     */
    fun fitScale(
        imageWidth: Int,
        imageHeight: Int,
        viewportWidth: Float,
        viewportHeight: Float,
    ): Float {
        if (imageWidth <= 0 || imageHeight <= 0) return 1f
        if (viewportWidth <= 0f || viewportHeight <= 0f) return 1f
        return min(
            1f,
            min(viewportWidth / imageWidth.toFloat(), viewportHeight / imageHeight.toFloat()),
        )
    }

    /**
     * The most a picture of [contentSize] scaled by [zoom] may be moved from centre on
     * one axis: zero while it fits, half the overflow once it does not.
     */
    fun panLimit(contentSize: Float, viewportSize: Float, zoom: Float): Float {
        if (contentSize <= 0f || viewportSize <= 0f) return 0f
        val scaled = contentSize * zoom
        return if (scaled > viewportSize) (scaled - viewportSize) / 2f else 0f
    }

    /** [pan] brought back inside [panLimit]. */
    fun clampPan(
        pan: Float,
        contentSize: Float,
        viewportSize: Float,
        zoom: Float,
    ): Float {
        val limit = panLimit(contentSize, viewportSize, zoom)
        return pan.coerceIn(-limit, limit)
    }

    /**
     * The pan that keeps the content point under the fingers fixed while the scale
     * changes from [fromZoom] to [toZoom].
     *
     * [focus] is the pinch centroid, and [centroidMovement] how far that centroid itself
     * moved since the previous event, both in pixels **relative to the viewport's
     * centre**. The two are kept apart because the point that must stay under the
     * fingers is the one that was under the *previous* centroid.
     */
    fun zoomPan(
        pan: Float,
        focus: Float,
        centroidMovement: Float,
        fromZoom: Float,
        toZoom: Float,
        contentSize: Float,
        viewportSize: Float,
    ): Float {
        if (fromZoom <= 0f) return 0f
        val previousFocus = focus - centroidMovement
        val ratio = toZoom / fromZoom
        val moved = focus + (pan - previousFocus) * ratio
        return clampPan(moved, contentSize, viewportSize, toZoom)
    }

    /**
     * Which page a horizontal drag asks for: `+1` is the next page, `-1` the previous,
     * null stays put.
     *
     * A drag leftward (negative) is the next page, as a pager runs. A flick only has to
     * cover [PAGE_FLICK_FRACTION] of the width, so a quick short swipe is not swallowed
     * by the distance rule; a slow short drag is, which is what keeps a settle-back from
     * changing page.
     */
    fun swipePage(drag: Float, width: Float, velocityX: Float): Int? {
        if (width <= 0f || drag == 0f) return null
        val direction = if (drag < 0f) 1 else -1
        val distance = abs(drag) / width
        if (distance >= PAGE_FRACTION) return direction
        // Short of the distance rule, only a flick that agrees with the drag's direction
        // counts. A drag that reversed at the last instant is not a flick at all.
        val velocityAgrees =
            if (direction == 1) {
                velocityX <= -PAGE_FLICK_VELOCITY
            } else {
                velocityX >= PAGE_FLICK_VELOCITY
            }
        return if (distance >= PAGE_FLICK_FRACTION && velocityAgrees) direction else null
    }

    /**
     * Whether a downward drag is a dismissal.
     *
     * Downward only: an upward drag at fit has nothing to reveal, and closing on it
     * would make the gesture feel like it fired by accident.
     */
    fun dismiss(drag: Float, height: Float, velocityY: Float): Boolean {
        if (height <= 0f || drag <= 0f) return false
        val distance = drag / height
        if (distance >= DISMISS_FRACTION) return true
        return velocityY >= DISMISS_FLICK_VELOCITY && distance >= DISMISS_FLICK_FRACTION
    }

    /**
     * Whether a tap landed on the picture rather than the backdrop around it.
     *
     * A tap outside is "the backdrop was pressed", which is how the web's preview
     * closes. It has to know the drawn rectangle, so it takes the same numbers the
     * clamp does: the fitted size, the zoom and the pan, in pixels.
     */
    fun containsPoint(
        x: Float,
        y: Float,
        viewportWidth: Float,
        viewportHeight: Float,
        panX: Float,
        panY: Float,
        contentWidth: Float,
        contentHeight: Float,
        zoom: Float,
    ): Boolean {
        if (contentWidth <= 0f || contentHeight <= 0f) return false
        val halfWidth = contentWidth * zoom / 2f
        val halfHeight = contentHeight * zoom / 2f
        val centreX = viewportWidth / 2f + panX
        val centreY = viewportHeight / 2f + panY
        return abs(x - centreX) <= halfWidth && abs(y - centreY) <= halfHeight
    }
}
