package uk.xa0.ward.ui.markdown

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.ClickableText
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import uk.xa0.ward.ui.theme.DshRadius
import androidx.compose.ui.layout.ContentScale
import androidx.compose.runtime.produceState
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.foundation.Image
import android.util.LruCache
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import uk.xa0.ward.model.ImageLoad
import uk.xa0.ward.ui.LightboxPage
import uk.xa0.ward.ui.LocalLightbox
import uk.xa0.ward.ui.clickableNoRipple
import uk.xa0.ward.ui.openLink
import uk.xa0.ward.ui.theme.DshColors
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/**
 * Renders the parsed markdown subset as real Compose text.
 *
 * No WebView, no TextView-with-HTML: every block becomes Compose layout, so
 * selection, theming and scrolling behave natively.
 */
/**
 * Parsed markdown for [text], remembered, cached, and — when it is long — parsed off
 * the main thread.
 *
 * Parsing is the expensive half of drawing a message: measured in `ParseCostTest`,
 * 1.5ms for a 2.4KB answer and 3.9ms for an 18KB one. Three things follow from that.
 *
 *  1. A frame is 11ms on this display. A long message that scrolls into view therefore
 *     spends a third of the frame it arrives in, and a scroll that brings three heavy
 *     rows in at once pays for all three on one frame — which is what the late frames
 *     in the recorder's histograms are.
 *  2. `remember(text)` does not survive the row leaving the composition, so flicking
 *     up and back down re-parsed everything on the way back. The cache makes a second
 *     visit free, and changes nothing on screen.
 *  3. Short documents are still parsed inline, because a fraction of a frame is not
 *     worth a frame of blank row. Long ones are parsed on a background thread and the
 *     row draws one frame later — a blank row for 11ms reads far better than a hitch
 *     that drops three frames, and the alternative is 4ms of every such frame.
 */
private const val INLINE_PARSE_CHARS = 4096

/** Bounded, keyed on the text itself: identical messages across sessions share it. */
private val parseCache = object : LruCache<String, List<MdBlock>>(48) {}

@Composable
private fun parsedMarkdown(text: String): List<MdBlock> {
    if (text.length <= INLINE_PARSE_CHARS) {
        return remember(text) {
            parseCache.get(text) ?: Markdown.parse(text).also { parseCache.put(text, it) }
        }
    }
    val cached = remember(text) { parseCache.get(text) }
    var parsed by remember(text) { mutableStateOf(cached) }
    LaunchedEffect(text) {
        if (parsed == null) {
            parsed = withContext(Dispatchers.Default) { Markdown.parse(text) }
                .also { parseCache.put(text, it) }
        }
    }
    return parsed.orEmpty()
}

@Composable
fun MarkdownText(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = DshTheme.colors.labelPrimary,
) {
    val blocks = parsedMarkdown(text)
    val colors = DshTheme.colors

    Column(modifier, verticalArrangement = Arrangement.spacedBy(DshSpacing.xl)) {
        blocks.forEach { block ->
            when (block) {
                is MdBlock.Paragraph -> InlineText(block.tokens, DshType.messageBody, color)

                is MdBlock.Heading -> InlineText(
                    tokens = block.tokens,
                    style = when (block.level) {
                        1 -> DshType.heading1
                        2 -> DshType.heading2
                        else -> DshType.heading3
                    },
                    color = color,
                )

                is MdBlock.Code -> CodeBlock(block.language, block.code)

                is MdBlock.BulletList -> ListBlock(block.items, ordered = false)

                is MdBlock.NumberedList -> ListBlock(block.items, ordered = true)

                is MdBlock.Table -> TableBlock(block.header, block.rows)

                is MdBlock.Quote -> Row(Modifier.fillMaxWidth()) {
                    Box(
                        Modifier
                            .width(2.dp)
                            .height(20.dp)
                            .background(colors.borderL4, RoundedCornerShape(1.dp)),
                    )
                    Spacer(Modifier.width(DshSpacing.lg))
                    InlineText(
                        block.tokens,
                        DshType.messageBody.copy(color = colors.labelSecondary),
                        colors.labelSecondary,
                        Modifier.weight(1f),
                    )
                }

                MdBlock.Rule -> Box(
                    Modifier
                        .fillMaxWidth()
                        .height(0.5.dp)
                        .background(colors.borderL3),
                )
            }
        }
    }
}

/** Bullet / numbered / task list, with indentation for nested entries. */
@Composable
private fun ListBlock(items: List<ListItem>, ordered: Boolean) {
    val colors = DshTheme.colors
    Column(verticalArrangement = Arrangement.spacedBy(DshSpacing.xs)) {
        items.forEachIndexed { index, item ->
            Row(Modifier.padding(start = (item.depth * 16).dp)) {
                val marker = when {
                    item.checked == true -> "✓"
                    item.checked == false -> "○"
                    ordered -> "${index + 1}."
                    else -> "•"
                }
                Text(
                    text = marker,
                    style = DshType.messageBody,
                    color = when {
                        item.checked == true -> colors.success
                        item.checked == false -> colors.labelTertiary
                        else -> colors.labelTertiary
                    },
                )
                Spacer(Modifier.width(DshSpacing.md))
                InlineText(
                    tokens = item.tokens,
                    style = DshType.messageBody,
                    color = if (item.checked == true) colors.labelTertiary else colors.labelPrimary,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

/**
 * Tables scroll horizontally with fixed-width columns rather than squeezing into
 * the phone's width — the web UI sizes columns to content, which a 360dp viewport
 * cannot emulate readably.
 */
@Composable
private fun TableBlock(
    header: List<List<InlineToken>>,
    rows: List<List<List<InlineToken>>>,
) {
    val colors = DshTheme.colors
    val shape = RoundedCornerShape(12.dp)
    Box(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
        Column(
            Modifier
                .clip(shape)
                .border(0.5.dp, colors.borderL1, shape),
        ) {
            TableRow(header, header = true)
            rows.forEach { row ->
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(0.5.dp)
                        .background(colors.borderL1),
                )
                TableRow(row, header = false)
            }
        }
    }
}

@Composable
private fun TableRow(cells: List<List<InlineToken>>, header: Boolean) {
    val colors = DshTheme.colors
    Row(Modifier.background(if (header) colors.codeBanner else Color.Transparent)) {
        cells.forEach { cell ->
            Box(
                Modifier
                    .width(160.dp)
                    .padding(horizontal = DshSpacing.lg, vertical = DshSpacing.md),
            ) {
                InlineText(
                    tokens = cell,
                    style = DshType.bodyMedium.copy(lineHeight = 22.sp),
                    color = colors.labelPrimary,
                )
            }
        }
    }
}

/**
 * The same run, with its pictures drawn instead of described.
 *
 * A picture cannot live inside a `Text`, so a run containing one is broken at the
 * pictures: the words either side keep their own `Text`, and the picture is drawn
 * between them. Everything without an image — which is almost everything — stays one
 * `Text` and one layout.
 *
 * The alt text is what a reader sees until the bytes land, and what stays if they never
 * do: a broken picture is not worth a red box, but silence is worth less than the
 * sentence the author wrote to describe it.
 */
@Composable
private fun InlineWithImages(
    tokens: List<InlineToken>,
    style: androidx.compose.ui.text.TextStyle,
    color: Color,
    modifier: Modifier = Modifier,
) {
    // The runs are emitted inline rather than through a local `flushRun()`: a nested
    // function cannot invoke a composable, and both of these are composables.
    Column(modifier, verticalArrangement = Arrangement.spacedBy(DshSpacing.sm)) {
        var run = ArrayList<InlineToken>()
        tokens.forEach { token ->
            if (token.style == InlineStyle.IMAGE) {
                if (run.isNotEmpty()) {
                    InlineText(run.toList(), style, color)
                    run = ArrayList()
                }
                MarkdownImage(url = token.url.orEmpty(), alt = token.text, style = style)
            } else {
                run += token
            }
        }
        if (run.isNotEmpty()) InlineText(run.toList(), style, color)
    }
}

/**
 * One picture from a message body.
 *
 * The bytes come from [LocalImageLoad], which the app provides: the renderer has no
 * business knowing whether a markdown picture is an `https://` address or a path in the
 * session's workspace.
 *
 * A picture that resolved is a control, like a message attachment: a markdown picture
 * is the same picture however it was written, and a phone has no hover to hide a second
 * affordance. A picture that did not resolve keeps the alt text it always showed — there
 * is no image to open, and the sentence the author wrote is the honest fallback.
 */
@Composable
private fun MarkdownImage(
    url: String,
    alt: String,
    style: androidx.compose.ui.text.TextStyle,
) {
    val colors = DshTheme.colors
    val load = LocalImageLoad.current
    val lightbox = LocalLightbox.current
    val state by produceState<ImageLoad?>(initialValue = null, url) {
        value = if (url.isBlank()) {
            ImageLoad.Failed("This picture has no address.")
        } else {
            load(url)
        }
    }
    val image = (state as? ImageLoad.Ready)?.bitmap
    if (image == null) {
        Text(text = alt.ifBlank { url }, style = style, color = colors.labelTertiary)
        return
    }
    // The alt text is the description when the author wrote one; otherwise the file name
    // is the only true thing to call it by, and "Image" is the last resort.
    val fileName = url.substringAfterLast('/').substringBefore('?').substringBefore('#')
    val description = alt.ifBlank { fileName }.ifBlank { "Image" }
    val remote = url.startsWith("http://") || url.startsWith("https://")
    val shape = RoundedCornerShape(DshRadius.md)
    Image(
        bitmap = image,
        contentDescription = description,
        contentScale = ContentScale.FillWidth,
        modifier = Modifier
            .fillMaxWidth()
            .clip(shape)
            .clickableNoRipple(
                enabled = lightbox != null,
                onClickLabel = "View full screen",
            ) {
                lightbox?.show(
                    listOf(
                        LightboxPage(
                            key = "markdown:$url",
                            title = description,
                            description = description,
                            // A workspace path is also a file the Download control can
                            // take; a remote address is not, and offering a save there
                            // would read the URL as a path.
                            savePath = url.takeIf { !remote },
                            load = { load(url) },
                        ),
                    ),
                )
            }
            .border(0.5.dp, colors.borderL1, shape),
    )
}

/**
 * How a markdown picture gets its pixels.
 *
 * A composition local rather than a parameter because the renderer is reached from a
 * dozen places — messages, reasoning, tool output — and every one of them would
 * otherwise have to thread a fetcher through. The app provides it once. The answer is
 * [ImageLoad] rather than nullable bytes so the lightbox can name the reason a picture
 * is missing; the inline renderer only looks at [ImageLoad.Ready].
 */
val LocalImageLoad = compositionLocalOf<suspend (String) -> ImageLoad> {
    { ImageLoad.Failed("No image reader is wired.") }
}

@Composable
private fun InlineText(
    tokens: List<InlineToken>,
    style: androidx.compose.ui.text.TextStyle,
    color: Color,
    modifier: Modifier = Modifier,
) {
    // A run with a picture in it is broken up; see [InlineWithImages].
    if (tokens.any { it.style == InlineStyle.IMAGE }) {
        InlineWithImages(tokens, style, color, modifier)
        return
    }
    val colors = DshTheme.colors
    val context = LocalContext.current

    val annotated: AnnotatedString = remember(tokens, color, colors) {
        buildAnnotatedString {
            tokens.forEach { token ->
                val span = when (token.style) {
                    InlineStyle.NORMAL -> SpanStyle(color = color)
                    InlineStyle.BOLD -> SpanStyle(color = color, fontWeight = FontWeight.Bold)
                    InlineStyle.ITALIC -> SpanStyle(color = color, fontStyle = FontStyle.Italic)
                    InlineStyle.BOLD_ITALIC -> SpanStyle(
                        color = color,
                        fontWeight = FontWeight.Bold,
                        fontStyle = FontStyle.Italic,
                    )

                    InlineStyle.CODE -> SpanStyle(
                        color = color,
                        fontFamily = FontFamily.Monospace,
                        background = colors.inlineCode,
                    )

                    InlineStyle.LINK -> SpanStyle(color = colors.link, textDecoration = TextDecoration.Underline)
                    InlineStyle.STRIKE -> SpanStyle(color = color, textDecoration = TextDecoration.LineThrough)
                    // Unreachable: a run reaching this builder has had its pictures
                    // taken out. The alt text is the honest fallback if one ever does.
                    InlineStyle.IMAGE -> SpanStyle(color = colors.labelTertiary)
                }
                if (token.url != null) {
                    pushStringAnnotation(tag = "url", annotation = token.url)
                    withStyle(span) { append(token.text) }
                    pop()
                } else {
                    withStyle(span) { append(token.text) }
                }
            }
        }
    }

    ClickableText(
        text = annotated,
        style = style,
        modifier = modifier,
        onClick = { offset ->
            annotated.getStringAnnotations("url", offset, offset)
                .firstOrNull()
                ?.let { openLink(context, it.item) }
        },
    )
}

/** A fenced code block: banner with language + copy, then a scrolling mono body. */
@Composable
fun CodeBlock(language: String?, code: String) {
    val colors = DshTheme.colors
    val clipboard = LocalClipboardManager.current
    var copied by remember { mutableStateOf(false) }

    LaunchedEffect(copied) {
        if (copied) {
            delay(1600)
            copied = false
        }
    }

    val shape = RoundedCornerShape(12.dp)
    Column(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(colors.codeBlock)
            .border(0.5.dp, colors.borderL1, shape),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .background(colors.codeBanner)
                .padding(start = DshSpacing.lg, end = DshSpacing.sm, top = DshSpacing.sm, bottom = DshSpacing.sm),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = language?.lowercase() ?: "code",
                style = DshType.codeSmall,
                color = colors.labelTertiary,
                modifier = Modifier.weight(1f),
            )
            Box(
                Modifier
                    .size(24.dp)
                    .clip(RoundedCornerShape(DshSpacing.sm))
                    .clickableNoRipple { clipboard.setText(AnnotatedString(code)); copied = true },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    imageVector = if (copied) Icons.Rounded.Check else Icons.Rounded.ContentCopy,
                    contentDescription = "Copy code",
                    tint = if (copied) colors.success else colors.labelTertiary,
                    modifier = Modifier.size(14.dp),
                )
            }
        }

        val highlighted = remember(code, colors) { highlightCode(code, colors) }
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
        ) {
            Text(
                text = highlighted,
                style = DshType.code,
                color = colors.labelPrimary,
                softWrap = false,
                modifier = Modifier.padding(DshSpacing.lg),
            )
        }
    }
}

private fun highlightCode(code: String, colors: DshColors): AnnotatedString {
    return buildAnnotatedString {
        code.split('\n').forEachIndexed { index, line ->
            if (index > 0) append("\n")
            SyntaxHighlighter.highlightLine(line).forEach { span ->
                withStyle(SpanStyle(color = codeColor(span.kind, colors))) { append(span.text) }
            }
        }
    }
}

/**
 * One line of code as coloured spans, for a reader that draws its own line numbers.
 *
 * The Files preview is a list of numbered rows, one per line, so it cannot use
 * [CodeBlock] — but it must use the same grammar and the same palette, or the same file
 * would be coloured two different ways in two places. This is that shared half.
 *
 * It is a claim about the *line*, not about the file: the highlighter has no state
 * between lines, so a block comment opened on one line and closed on another is
 * coloured as punctuation on the line it opens and as ordinary text after. Nothing is
 * removed — the concatenation of the spans is always the line exactly as it arrived,
 * which `MarkdownHighlightTest` holds to — so a wrong colour is a wrong colour and
 * never a lost character.
 */
fun highlightCodeLine(line: String, colors: DshColors): AnnotatedString = buildAnnotatedString {
    SyntaxHighlighter.highlightLine(line).forEach { span ->
        withStyle(SpanStyle(color = codeColor(span.kind, colors))) { append(span.text) }
    }
}

/** The palette's one answer for a span kind. */
private fun codeColor(kind: SyntaxHighlighter.Kind, colors: DshColors): Color = when (kind) {
    SyntaxHighlighter.Kind.PLAIN -> colors.labelPrimary
    SyntaxHighlighter.Kind.KEYWORD -> colors.synKeyword
    SyntaxHighlighter.Kind.STRING -> colors.synString
    SyntaxHighlighter.Kind.COMMENT -> colors.synComment
    SyntaxHighlighter.Kind.NUMBER -> colors.synConstant
    SyntaxHighlighter.Kind.FUNCTION -> colors.synFunction
    SyntaxHighlighter.Kind.PUNCTUATION -> colors.synPunctuation
}
