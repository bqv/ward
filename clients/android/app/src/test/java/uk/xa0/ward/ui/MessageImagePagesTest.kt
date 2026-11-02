package uk.xa0.ward.ui

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import uk.xa0.ward.model.ImageLoad
import uk.xa0.ward.model.MessageAttachment
import uk.xa0.ward.scroll.HarnessApplication

/**
 * The set a message's picture opens: which pictures, in which order, and what each one
 * reads as.
 *
 * This is the part of the viewer the overlay test cannot see. The overlay is handed a
 * list and draws whatever it is given; the question here is whether the list *is* the
 * message's pictures in attachment order, because the "2 of 3" the reader sees and the
 * page the next swipe lands on both come from that order rather than from the tap.
 *
 * ## What these can and cannot show
 *
 * They can show the membership, the order, the captions, that a page asks for the id its
 * own attachment carries and no other, and that an attachment with neither bytes nor an
 * id fails in words. They cannot show a decoded picture: the staging path goes through
 * the platform's decoder, whose answer under Robolectric is a statement about the shadow
 * rather than about a phone, so the picture cases here assert on the id that was asked
 * for instead of on pixels.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = HarnessApplication::class)
class MessageImagePagesTest {

    private fun image(id: String, name: String = id) = MessageAttachment(
        kind = "image",
        attachmentId = id,
        name = name,
        bytes = 1024,
        mediaType = "image/png",
    )

    @Test
    fun `the pages are the message's pictures in the order they were attached`() {
        val pages = messageImagePages(
            listOf(image("a", "one.png"), image("b", "two.png"), image("c", "three.png")),
            loadImage = { ImageLoad.Failed("Not asked in this test.") },
        )
        assertEquals(3, pages.size)
        assertEquals(listOf("one.png", "two.png", "three.png"), pages.map { it.title })
        // Distinct keys, or the overlay's per-page cache would answer one page from
        // another's pixels.
        assertEquals(3, pages.map { it.key }.distinct().size)
    }

    @Test
    fun `a page asks for its own attachment's id and nothing else`() = runBlocking {
        val asked = mutableListOf<String>()
        val pages = messageImagePages(
            listOf(image("a"), image("b"), image("c")),
            loadImage = { id ->
                asked += id
                ImageLoad.Failed("The host refused this picture.")
            },
        )
        val answer = pages[1].load()
        assertEquals(listOf("b"), asked)
        assertEquals("The host refused this picture.", (answer as ImageLoad.Failed).message)
    }

    @Test
    fun `an attachment with neither bytes nor an id fails in words`() = runBlocking {
        val empty = MessageAttachment(
            kind = "image",
            attachmentId = "",
            name = "gone.png",
            bytes = 0,
            mediaType = "image/png",
        )
        val pages = messageImagePages(
            listOf(empty),
            loadImage = { ImageLoad.Failed("Not asked in this test.") },
        )
        val answer = pages.single().load() as ImageLoad.Failed
        assertTrue(answer.message.contains("no bytes and no address"))
    }

    @Test
    fun `a key survives the row being rebuilt with the same attachment`() {
        val attached = image("a", "one.png")
        assertEquals(attachmentPageKey(attached), attachmentPageKey(attached.copy()))
        // Same id, different staged bytes: an edited row must not reuse the old key.
        val staged = attached.copy(attachmentId = "", localData = "Zm9v")
        assertTrue(attachmentPageKey(attached) != attachmentPageKey(staged))
    }
}
