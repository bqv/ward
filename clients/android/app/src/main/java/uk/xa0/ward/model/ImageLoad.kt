package uk.xa0.ward.model

import androidx.compose.ui.graphics.ImageBitmap

/**
 * One picture's pixels, or the reason they are not on screen.
 *
 * The app already had a loader that answered `null` for every way an image could fail —
 * no session, a refused read, bytes the host would not send, a decode the device could
 * not perform. The thumbnails only ever needed "is there a picture?", so the shape was
 * enough there. A full-screen viewer is different: it has room for the sentence, and a
 * black rectangle with no explanation is the defect this type exists to make impossible.
 *
 * [Failed] carries the app's own wording, not a stack trace: it is drawn under the
 * picture's title in the lightbox, where a reader is already looking, and every caller
 * that cannot be more specific still has to say something true.
 */
sealed interface ImageLoad {

    /** Decoded pixels, ready to draw. */
    data class Ready(val bitmap: ImageBitmap) : ImageLoad

    /** Nothing to draw, and [message] says why in a reader's words. */
    data class Failed(val message: String) : ImageLoad
}
