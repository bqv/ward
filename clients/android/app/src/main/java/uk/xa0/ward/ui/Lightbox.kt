package uk.xa0.ward.ui

import android.graphics.BitmapFactory
import android.util.Base64
import android.util.LruCache
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BrokenImage
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.PointerInputScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onPlaced
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.dismiss
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlin.math.abs
import kotlin.math.max
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import uk.xa0.ward.data.ThemeMode
import uk.xa0.ward.model.FileDownloader
import uk.xa0.ward.model.ImageLoad
import uk.xa0.ward.model.LightboxGeometry
import uk.xa0.ward.model.MessageAttachment
import uk.xa0.ward.ui.components.FileDownloadControl
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/**
 * The full-screen image viewer: a controller the surfaces call, and the overlay it
 * raises.
 *
 * The web client has one: `@deepseek-ai/dsh-client-ui-attachment` names it the
 * "original-image lightbox", "a document-level modal preview over the shared mask",
 * opened by clicking a loaded message image and closed by Escape, a mask press or its
 * close control, restoring focus to the opener. Its published limits are that the
 * preview "renders the original at fit-to-viewport size only" and that there is "no zoom
 * or download in the lightbox". So this is the same surface with two deliberate
 * additions a phone earns — pinch/double-tap zoom, and paging through a message's or a
 * listing's set — plus the Download control the app already has. The vocabulary stays
 * the web's: the thumbnail says "View full screen", the control "Close image preview",
 * and the mask press is one way out rather than the only one.
 *
 * The overlay is composed once, at the window root, rather than inside each surface.
 * That is what makes it cover the chat header as well as the transcript, and it means a
 * new surface only has to know how to phrase a page, not where the viewer lives.
 */

/** The default loader's refusal, so an unwired surface fails in words rather than in black. */
internal const val NO_IMAGE_READER: String = "No image reader is wired."

/**
 * One picture the viewer can show.
 *
 * [load] is called on demand, once per visit to the page, so a set of hundred pictures
 * costs nothing until the reader pages to one. [title] is the caption and the fallback
 * description; [description] is the alt text when the transcript carried one. [savePath]
 * is a workspace path the Download control already knows how to take — null for an
 * attachment, which has an attachment id rather than a path and is not served by the
 * file download plumbing.
 */
data class LightboxPage(
    val key: String,
    val title: String,
    val description: String?,
    val savePath: String? = null,
    val load: suspend () -> ImageLoad,
)

/** What a controller is showing: the set, and where the reader entered it. */
data class LightboxRequest(val pages: List<LightboxPage>, val start: Int)

/**
 * Raises and dismisses the one overlay.
 *
 * Held at the window root and read through [LocalLightbox] by every surface that draws a
 * picture. [show] is a no-op for an empty set: a surface with nothing to show must not
 * open a black rectangle.
 */
@Stable
class LightboxController {

    var request: LightboxRequest? by mutableStateOf(null)
        private set

    fun show(pages: List<LightboxPage>, start: Int = 0) {
        if (pages.isEmpty()) return
        request = LightboxRequest(pages, start.coerceIn(0, pages.lastIndex))
    }

    fun close() {
        request = null
    }
}

/** The overlay's controller, provided once by the window root. */
val LocalLightbox = compositionLocalOf<LightboxController?> { null }

/** The identity of a message attachment across a rebuild of its row. */
internal fun attachmentPageKey(attachment: MessageAttachment): String =
    "attachment:${attachment.attachmentId}:${attachment.localData?.hashCode() ?: 0}"

/** The pages of one message's picture set, in attachment order. */
internal fun messageImagePages(
    pictures: List<MessageAttachment>,
    loadImage: suspend (String) -> ImageLoad,
): List<LightboxPage> = pictures.map { attachment ->
    val title = attachment.name.ifBlank { "Image" }
    LightboxPage(
        key = attachmentPageKey(attachment),
        title = title,
        description = title,
        load = { loadAttachmentImage(attachment, loadImage) },
    )
}

/**
 * One attachment's picture: staged bytes first, then the durable id.
 *
 * The order is the transcript's own (a staged picture is already on the device), and an
 * attachment with neither is a sentence rather than a spinner that never ends.
 */
internal suspend fun loadAttachmentImage(
    attachment: MessageAttachment,
    loadImage: suspend (String) -> ImageLoad,
): ImageLoad {
    val staged = attachment.localData
    return when {
        !staged.isNullOrEmpty() -> decodeInlineImage(staged)
        attachment.attachmentId.isNotEmpty() -> loadImage(attachment.attachmentId)
        else -> ImageLoad.Failed("This picture has no bytes and no address to read them from.")
    }
}

/**
 * Decodes the inline base64 a staged attachment carries.
 *
 * `BitmapFactory` is asked off the main thread because a camera photo's decode is long
 * enough to drop frames, and every way this fails — a malformed payload, bytes the
 * platform refuses — is one sentence for the reader rather than a broken placeholder.
 */
internal suspend fun decodeInlineImage(encoded: String): ImageLoad =
    withContext(Dispatchers.Default) {
        try {
            val bytes = Base64.decode(encoded, Base64.DEFAULT)
            val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            if (bitmap == null) {
                ImageLoad.Failed("This picture's data could not be decoded on this device.")
            } else {
                ImageLoad.Ready(bitmap.asImageBitmap())
            }
        } catch (error: Throwable) {
            ImageLoad.Failed("This picture's data could not be read as a picture.")
        }
    }

/** Decodes one file's bytes, with the platform's refusal written for the reader. */
internal suspend fun decodeImageBytes(bytes: ByteArray): ImageLoad =
    withContext(Dispatchers.Default) {
        val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        if (bitmap == null) {
            ImageLoad.Failed("This device could not decode the picture.")
        } else {
            ImageLoad.Ready(bitmap.asImageBitmap())
        }
    }

/**
 * The backdrop: black in both themes, because a viewer is not a sheet.
 *
 * Opaque rather than a near-black veil: the overlay covers the chat header and the
 * Files surface, and 94% opacity still leaves a bright header legible through it —
 * which is the chrome a full-screen viewer must not show. The only transparency is the
 * fade while a downward drag is dismissing, where seeing the screen underneath is the
 * point.
 */
private val LightboxBackdrop = Color(0xFF000000)

/** Caption and hint inks are fixed, for the same reason: the surface under them is black. */
private val LightboxCaption = Color(0xEBFFFFFF)
private val LightboxHint = Color(0xA6FFFFFF)
private val LightboxError = Color(0xFFFF9A9A)

/**
 * The overlay: backdrop, picture, caption, pager, close control, and the gestures.
 *
 * Dismissal is deliberately over-determined. The close control is visible and labelled;
 * the system Back gesture and button close it through a [BackHandler]; a tap on the
 * backdrop closes it, mirroring the web; and a downward drag at fit closes it, while a
 * downward drag while zoomed pans instead, so the two never fight. The close control is
 * the one a TalkBack user is left with, which is why it is focused on open rather than
 * being one of several unlabelled targets.
 */
@Composable
fun LightboxOverlay(
    request: LightboxRequest,
    downloader: FileDownloader? = null,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    var index by remember(request) { mutableIntStateOf(request.start) }
    val page = request.pages[index]
    val view = remember(request) { LightboxView() }
    // One offset each for the paging drag and the dismissal drag. Both are kept across
    // pages so a page can slide in from the side the reader sent it to.
    val swipe = remember(request) { DragOffset() }
    val dismiss = remember(request) { DragOffset() }
    // At most three decoded pages are held: enough for back-and-forth reading, not
    // enough for a folder of photographs to become an out-of-memory report.
    val cache = remember(request) { LruCache<String, ImageLoad>(3) }
    val loaded = remember(page.key) { mutableStateOf(cache.get(page.key)) }
    var attempt by remember(page.key) { mutableIntStateOf(0) }
    var downloadMessage by remember(page.key) { mutableStateOf<String?>(null) }
    val closeFocus = remember { FocusRequester() }

    LaunchedEffect(page.key, attempt) {
        val held = cache.get(page.key)
        if (held != null) {
            loaded.value = held
            return@LaunchedEffect
        }
        loaded.value = null
        val result = page.load()
        cache.put(page.key, result)
        // The page may have moved on while the read was in flight; a stale result must
        // not overwrite the picture the reader is now looking at.
        if (loaded.value == null) loaded.value = result
    }

    BackHandler { onClose() }
    // The close control is the one target a TalkBack user is left with, so it takes focus
    // on open. The request rides on placement rather than on an effect: a focus requester
    // only resolves to a node once the modifier carrying it has been attached, and an
    // effect dispatched from this composition can run before that, which throws instead of
    // waiting. Placement cannot happen earlier than that.
    var closeFocused by remember { mutableStateOf(false) }

    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .testTag("lightbox")
            .semantics {
                dismiss {
                    onClose()
                    true
                }
                // Paging is a swipe, and a horizontal swipe is exactly what TalkBack
                // claims for itself: without these the other pictures of a set are
                // unreachable with a screen reader, however reachable they are by finger.
                if (request.pages.size > 1) {
                    customActions = listOf(
                        CustomAccessibilityAction("Next image") {
                            if (index < request.pages.lastIndex) {
                                index += 1
                                view.reset()
                                true
                            } else {
                                false
                            }
                        },
                        CustomAccessibilityAction("Previous image") {
                            if (index > 0) {
                                index -= 1
                                view.reset()
                                true
                            } else {
                                false
                            }
                        },
                    )
                }
            },
    ) {
        val viewportWidth = constraints.maxWidth.toFloat()
        val viewportHeight = constraints.maxHeight.toFloat()
        val dismissal = if (viewportHeight > 0f) {
            (dismiss.value / viewportHeight).coerceIn(0f, 1f)
        } else {
            0f
        }
        val backdrop = LightboxBackdrop.copy(
            alpha = (1f - 0.35f * dismissal).coerceIn(0f, 1f),
        )
        val density = LocalDensity.current
        val bitmap = (loaded.value as? ImageLoad.Ready)?.bitmap
        val fit = if (bitmap == null) {
            1f
        } else {
            LightboxGeometry.fitScale(bitmap.width, bitmap.height, viewportWidth, viewportHeight)
        }
        val drawWidthPx = (bitmap?.width ?: 0) * fit
        val drawHeightPx = (bitmap?.height ?: 0) * fit

        // The gesture handler needs the drawn size to clamp against, and the size is only
        // known once the picture has decoded: hand it over after layout rather than
        // teaching the handler to decode.
        SideEffect {
            view.contentWidth = drawWidthPx
            view.contentHeight = drawHeightPx
            view.panX = LightboxGeometry.clampPan(view.panX, drawWidthPx, viewportWidth, view.zoom)
            view.panY = LightboxGeometry.clampPan(view.panY, drawHeightPx, viewportHeight, view.zoom)
        }

        Box(
            Modifier
                .fillMaxSize()
                .background(backdrop)
                .clipToBounds()
                // A modal barrier, not a wish: everything under this overlay — the
                // transcript's LazyColumn, the drawer's edge gesture, a row — is still
                // hit-tested, because Compose delivers a pointer to overlapping nodes and
                // lets the topmost one win by consuming. The Main pass runs leaf-to-root,
                // so the overlay's own children (the close control, Download, Retry, the
                // gesture handler) have already taken what they wanted by the time this
                // runs; consuming here is what stops a drag from scrolling the chat
                // underneath and leaving the transcript displaced when the viewer closes.
                .pointerInput(Unit) {
                    awaitPointerEventScope {
                        while (true) {
                            awaitPointerEvent().changes.forEach { it.consume() }
                        }
                    }
                },
        ) {
            val canPrevious = index > 0
            val canNext = index < request.pages.lastIndex

            Box(
                Modifier
                    .fillMaxSize()
                    .pointerInput(page.key, request.pages.size, canPrevious, canNext, bitmap != null) {
                        lightboxGestures(
                            view = view,
                            swipe = swipe,
                            dismiss = dismiss,
                            pageCount = request.pages.size,
                            canPrevious = canPrevious,
                            canNext = canNext,
                            hasPicture = bitmap != null,
                            onPage = { direction ->
                                index += direction
                                view.reset()
                            },
                            onClose = onClose,
                            settleScope = scope,
                        )
                    }
                    .testTag("lightbox-image-area"),
                contentAlignment = Alignment.Center,
            ) {
                val current = loaded.value
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap,
                        contentDescription = page.description?.takeIf { it.isNotBlank() } ?: page.title,
                        contentScale = ContentScale.FillBounds,
                        modifier = Modifier
                            .size(
                                width = with(density) { drawWidthPx.toDp() },
                                height = with(density) { drawHeightPx.toDp() },
                            )
                            .graphicsLayer {
                                scaleX = view.zoom
                                scaleY = view.zoom
                                translationX = view.panX + swipe.value
                                translationY = view.panY + dismiss.value
                            }
                            .testTag("lightbox-image"),
                    )
                } else if (current is ImageLoad.Failed) {
                    LightboxFailure(
                        page = page,
                        message = current.message,
                        onRetry = {
                            cache.remove(page.key)
                            attempt += 1
                        },
                    )
                } else {
                    LightboxLoading(page)
                }
            }

            // The caption, the pager and the save control sit over the picture rather
            // than taking a row out of it: a viewer that shrinks the photograph to make
            // room for its own chrome has stopped being full screen.
            Row(
                Modifier
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .padding(horizontal = DshSpacing.lg, vertical = DshSpacing.md),
                horizontalArrangement = Arrangement.End,
            ) {
                Box(
                    Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .background(Color(0x66000000))
                        .focusRequester(closeFocus)
                        // Only the first placement takes focus; a later one is the reader
                        // resizing or turning the device, not the overlay opening.
                        .onPlaced {
                            if (!closeFocused) {
                                closeFocused = true
                                closeFocus.requestFocus()
                            }
                        }
                        .clickableNoRipple(onClickLabel = "Close image preview", onClick = onClose)
                        .testTag("lightbox-close"),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        imageVector = Icons.Rounded.Close,
                        contentDescription = "Close image preview",
                        tint = LightboxCaption,
                        modifier = Modifier.size(20.dp),
                    )
                }
            }

            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .background(
                        Brush.verticalGradient(
                            listOf(Color.Transparent, Color.Black.copy(alpha = 0.78f)),
                        ),
                    )
                    .navigationBarsPadding()
                    .padding(horizontal = DshSpacing.xl, vertical = DshSpacing.lg),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = page.title,
                    style = DshType.bodySmall,
                    color = LightboxCaption,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.testTag("lightbox-title"),
                )
                // A single picture has no pager, so it gets no counter: "1 of 1" is the
                // sort of chrome that makes a viewer feel busier than the picture.
                if (request.pages.size > 1) {
                    Spacer(Modifier.height(DshSpacing.xxs))
                    Text(
                        text = "${index + 1} of ${request.pages.size}",
                        style = DshType.micro,
                        color = LightboxHint,
                        modifier = Modifier.testTag("lightbox-position"),
                    )
                }
                val savePath = page.savePath
                if (savePath != null && downloader != null) {
                    Spacer(Modifier.height(DshSpacing.xxs))
                    // The same control the Files header and the delivered cards draw, so
                    // a picture saved here goes through the one download and save-dialog
                    // path rather than a second one that only the viewer knows. The
                    // viewer is black in either theme, so the control is drawn with the
                    // dark palette: the light theme's link ink would vanish on it.
                    DshTheme(themeMode = ThemeMode.DARK) {
                        FileDownloadControl(
                            path = savePath,
                            downloader = downloader,
                            onMessage = { downloadMessage = it },
                        )
                    }
                }
                downloadMessage?.let { message ->
                    Spacer(Modifier.height(DshSpacing.xxs))
                    Text(
                        text = message,
                        style = DshType.micro,
                        color = LightboxError,
                        textAlign = TextAlign.Center,
                    )
                }
            }
        }
    }
}

@Composable
private fun LightboxLoading(page: LightboxPage) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier.testTag("lightbox-loading"),
    ) {
        CircularProgressIndicator(
            color = LightboxCaption,
            strokeWidth = 2.dp,
            modifier = Modifier.size(30.dp),
        )
        Spacer(Modifier.height(DshSpacing.md))
        Text(
            text = "Loading ${page.title}…",
            style = DshType.bodySmall,
            color = LightboxHint,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(horizontal = DshSpacing.xxl),
        )
    }
}

/**
 * The dead end, said out loud.
 *
 * The icon alone is the empty rectangle this replaces; the host's own reason (or the
 * device's) is the line under it, and Retry is offered because most of these are a
 * dropped connection or a file that was replaced mid-read rather than a permanent fault.
 */
@Composable
private fun LightboxFailure(page: LightboxPage, message: String, onRetry: () -> Unit) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .padding(horizontal = DshSpacing.xxl)
            .testTag("lightbox-failure"),
    ) {
        Icon(
            imageVector = Icons.Rounded.BrokenImage,
            contentDescription = null,
            tint = LightboxHint,
            modifier = Modifier.size(34.dp),
        )
        Spacer(Modifier.height(DshSpacing.md))
        Text(
            text = "Could not load this image.",
            style = DshType.bodyMedium,
            color = LightboxCaption,
            textAlign = TextAlign.Center,
        )
        if (message.isNotBlank()) {
            Spacer(Modifier.height(DshSpacing.xs))
            Text(
                text = message,
                style = DshType.bodySmall,
                color = LightboxHint,
                textAlign = TextAlign.Center,
            )
        }
        Spacer(Modifier.height(DshSpacing.md))
        Text(
            text = "Retry",
            style = DshType.bodySmall,
            color = LightboxCaption,
            modifier = Modifier
                .clip(RoundedCornerShape(DshSpacing.xs))
                .clickableNoRipple(onClickLabel = "Try loading ${page.title} again", onClick = onRetry)
                .padding(horizontal = DshSpacing.lg, vertical = DshSpacing.sm)
                .testTag("lightbox-retry"),
        )
    }
}

/**
 * The reader's zoom and pan for the picture on screen.
 *
 * A class rather than four `remember`ed floats so the gesture lambda can mutate it
 * without the composition restarting the pointer handler on every frame: this is state
 * the gestures own, and Compose only has to draw it.
 */
@Stable
private class LightboxView {

    var zoom by mutableFloatStateOf(LightboxGeometry.MIN_ZOOM)
    var panX by mutableFloatStateOf(0f)
    var panY by mutableFloatStateOf(0f)

    /** The picture's drawn size at fit, in pixels; zero until a decode lands. */
    var contentWidth by mutableFloatStateOf(0f)
    var contentHeight by mutableFloatStateOf(0f)

    val isZoomed: Boolean get() = zoom > LightboxGeometry.MIN_ZOOM + 0.001f

    fun reset() {
        zoom = LightboxGeometry.MIN_ZOOM
        panX = 0f
        panY = 0f
    }

    fun panBy(dx: Float, dy: Float, viewportWidth: Float, viewportHeight: Float) {
        panX = LightboxGeometry.clampPan(panX + dx, contentWidth, viewportWidth, zoom)
        panY = LightboxGeometry.clampPan(panY + dy, contentHeight, viewportHeight, zoom)
    }

    fun zoomAround(
        focusX: Float,
        focusY: Float,
        movementX: Float,
        movementY: Float,
        from: Float,
        to: Float,
        viewportWidth: Float,
        viewportHeight: Float,
    ) {
        panX = LightboxGeometry.zoomPan(
            pan = panX,
            focus = focusX,
            centroidMovement = movementX,
            fromZoom = from,
            toZoom = to,
            contentSize = contentWidth,
            viewportSize = viewportWidth,
        )
        panY = LightboxGeometry.zoomPan(
            pan = panY,
            focus = focusY,
            centroidMovement = movementY,
            fromZoom = from,
            toZoom = to,
            contentSize = contentHeight,
            viewportSize = viewportHeight,
        )
        zoom = to
    }

    fun zoomTo(focusX: Float, focusY: Float, target: Float, viewportWidth: Float, viewportHeight: Float) {
        val to = target.coerceIn(LightboxGeometry.MIN_ZOOM, LightboxGeometry.MAX_ZOOM)
        zoomAround(focusX, focusY, 0f, 0f, zoom, to, viewportWidth, viewportHeight)
    }
}

/**
 * A drag offset: the distance the paging or dismissal drag has been pulled off centre.
 *
 * A plain float rather than an `Animatable` because the gesture writes it, and
 * `awaitEachGesture` runs its body in a restricted coroutine scope where a suspending
 * function such as `Animatable.snapTo` cannot be called. Assignment is not suspending, so
 * the finger can move the picture directly; settling is an animation in `settleScope`,
 * which is outside the gesture because it has to outlive the finger being lifted.
 */
@Stable
private class DragOffset {

    var value by mutableFloatStateOf(0f)
}

/** Animates this offset to [target] and leaves it there; runs outside the gesture scope. */
private suspend fun DragOffset.animateTo(target: Float, durationMillis: Int) {
    animate(
        initialValue = value,
        targetValue = target,
        animationSpec = tween(durationMillis = durationMillis),
    ) { frame, _ ->
        value = frame
    }
    value = target
}

/** Which gesture a drag has become; `MAYBE` is the finger down before touch slop. */
private enum class DragMode { MAYBE, PAGE, DISMISS, PAN, PINCH }

/**
 * One gesture from down to up: pinch, pan, page, dismiss, double-tap or a backdrop tap.
 *
 * The order of the branches is the rule:
 *
 *  1. Two fingers are always a pinch — zoom about their centroid and pan with it.
 *  2. One finger while zoomed is always a pan, clamped to the picture's edges.
 *  3. One finger at fit is decided by direction once touch slop is crossed: horizontal
 *     pages, vertical dismisses. Below slop the gesture is still a prospective tap, so a
 *     double-tap can zoom and a tap outside the picture can close.
 *
 * Nothing is consumed until the gesture is one of those: the close control, the caption's
 * Download control and the failure's Retry are all nested above this handler and must
 * receive their own taps.
 */
private suspend fun PointerInputScope.lightboxGestures(
    view: LightboxView,
    swipe: DragOffset,
    dismiss: DragOffset,
    pageCount: Int,
    canPrevious: Boolean,
    canNext: Boolean,
    /** False while loading or failed: a backdrop tap must not dismiss those states. */
    hasPicture: Boolean,
    onPage: (Int) -> Unit,
    onClose: () -> Unit,
    settleScope: CoroutineScope,
) {
    val viewportWidth = size.width.toFloat()
    val viewportHeight = size.height.toFloat()
    var lastTapTime = 0L
    var lastTapPosition = Offset.Zero
    var settle: Job? = null

    awaitEachGesture {
        // A drag that arrives while the previous one is settling takes over: the settle
        // animation is a job in settleScope, so cancelling it is what stops the offset
        // from being animated out from under this finger.
        settle?.cancel()
        val down = awaitFirstDown(requireUnconsumed = false)
        // A nested control (the close button, Retry, the Download control) has already
        // taken this press. This handler sits behind them and must not also read it as a
        // backdrop tap, or tapping Retry would retry and close at once.
        if (down.isConsumed) return@awaitEachGesture
        val tracker = VelocityTracker()
        tracker.addPosition(down.uptimeMillis, down.position)

        var mode = DragMode.MAYBE
        var total = Offset.Zero
        var pinched = false
        var lastPosition = down.position
        var lastTime = down.uptimeMillis

        do {
            val event = awaitPointerEvent()
            val pressed = event.changes.filter { it.pressed }
            if (mode == DragMode.MAYBE && pressed.any { it.isConsumed }) {
                return@awaitEachGesture
            }
            if (pressed.size >= 2) {
                pinched = true
                mode = DragMode.PINCH
                val zoomChange = event.calculateZoom()
                val centroid = event.calculateCentroid(useCurrent = true)
                val movement = event.calculatePan()
                val from = view.zoom
                val to = (from * zoomChange).coerceIn(
                    LightboxGeometry.MIN_ZOOM,
                    LightboxGeometry.MAX_ZOOM,
                )
                if (to != from) {
                    view.zoomAround(
                        focusX = centroid.x - viewportWidth / 2f,
                        focusY = centroid.y - viewportHeight / 2f,
                        movementX = movement.x,
                        movementY = movement.y,
                        from = from,
                        to = to,
                        viewportWidth = viewportWidth,
                        viewportHeight = viewportHeight,
                    )
                } else if (movement != Offset.Zero) {
                    view.panBy(movement.x, movement.y, viewportWidth, viewportHeight)
                }
                event.changes.forEach { it.consume() }
            } else if (pressed.size == 1) {
                val change = pressed.first()
                val delta = change.position - change.previousPosition
                lastPosition = change.position
                lastTime = change.uptimeMillis
                tracker.addPosition(change.uptimeMillis, change.position)
                if (view.isZoomed) {
                    // While zoomed there is no paging and no dismissal: a drag is the
                    // pan the zoom exists for, and a downward one must not close the
                    // viewer out from under a reader inspecting an edge.
                    mode = DragMode.PAN
                    view.panBy(delta.x, delta.y, viewportWidth, viewportHeight)
                    change.consume()
                } else {
                    total += delta
                    if (mode == DragMode.MAYBE && total.getDistance() > viewConfiguration.touchSlop) {
                        mode = if (abs(total.x) >= abs(total.y)) DragMode.PAGE else DragMode.DISMISS
                    }
                    when (mode) {
                        DragMode.PAGE -> {
                            // A set of one has nothing to page to, so the picture stays
                            // put; at either end of a set the drag meets resistance
                            // rather than sliding into a gap.
                            val drag = when {
                                total.x < 0f && !canNext -> total.x * 0.3f
                                total.x > 0f && !canPrevious -> total.x * 0.3f
                                else -> total.x
                            }
                            if (pageCount > 1) swipe.value = drag
                            change.consume()
                        }

                        DragMode.DISMISS -> {
                            // Upward travel at fit is swallowed rather than moving the
                            // picture, which is the "not stranded" half of the rule.
                            dismiss.value = max(total.y, 0f)
                            change.consume()
                        }

                        else -> Unit
                    }
                }
            }
        } while (event.changes.any { it.pressed })

        when (mode) {
            DragMode.PAGE -> {
                val velocity = tracker.calculateVelocity().x
                val direction = LightboxGeometry.swipePage(swipe.value, viewportWidth, velocity)
                val allowed = when (direction) {
                    1 -> canNext
                    -1 -> canPrevious
                    else -> false
                }
                if (direction != null && allowed) {
                    settle = settleScope.launch {
                        val target = if (direction == 1) -viewportWidth else viewportWidth
                        swipe.animateTo(target, durationMillis = 160)
                        onPage(direction)
                        // The incoming page starts on the far side and slides into place,
                        // so a page turn reads as a turn rather than a cut.
                        swipe.value = -target
                        swipe.animateTo(0f, durationMillis = 160)
                    }
                } else {
                    settle = settleScope.launch {
                        swipe.animateTo(0f, durationMillis = 160)
                    }
                }
            }

            DragMode.DISMISS -> {
                val velocity = tracker.calculateVelocity().y
                if (LightboxGeometry.dismiss(dismiss.value, viewportHeight, velocity)) {
                    settle = settleScope.launch {
                        dismiss.animateTo(viewportHeight, durationMillis = 180)
                        onClose()
                    }
                } else {
                    settle = settleScope.launch {
                        dismiss.animateTo(0f, durationMillis = 160)
                    }
                }
            }

            DragMode.MAYBE -> {
                // No slop crossed and no pinch: a tap, not a drag. It is a double-tap if
                // it lands soon enough and near enough to the previous one, and a
                // backdrop press if it missed the picture.
                val elapsed = lastTime - down.uptimeMillis
                val isTap = !pinched && elapsed <= viewConfiguration.longPressTimeoutMillis
                if (isTap) {
                    val since = lastTime - lastTapTime
                    val earliestDouble = viewConfiguration.doubleTapMinTimeMillis
                    val latestDouble = viewConfiguration.doubleTapTimeoutMillis
                    val double = lastTapTime != 0L &&
                        since in earliestDouble..latestDouble &&
                        (lastPosition - lastTapPosition).getDistance() <=
                        viewConfiguration.touchSlop * 2f
                    if (double) {
                        lastTapTime = 0L
                        val focusX = lastPosition.x - viewportWidth / 2f
                        val focusY = lastPosition.y - viewportHeight / 2f
                        val target = if (view.isZoomed) {
                            LightboxGeometry.MIN_ZOOM
                        } else {
                            LightboxGeometry.DOUBLE_TAP_ZOOM
                        }
                        view.zoomTo(focusX, focusY, target, viewportWidth, viewportHeight)
                    } else {
                        lastTapTime = lastTime
                        lastTapPosition = lastPosition
                        val onPicture = LightboxGeometry.containsPoint(
                            x = lastPosition.x,
                            y = lastPosition.y,
                            viewportWidth = viewportWidth,
                            viewportHeight = viewportHeight,
                            panX = view.panX,
                            panY = view.panY,
                            contentWidth = view.contentWidth,
                            contentHeight = view.contentHeight,
                            zoom = view.zoom,
                        )
                        if (hasPicture && !onPicture) onClose()
                    }
                }
            }

            DragMode.PAN, DragMode.PINCH -> Unit
        }
    }
}
