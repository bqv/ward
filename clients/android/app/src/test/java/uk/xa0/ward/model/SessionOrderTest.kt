package uk.xa0.ward.model

import org.junit.Assert.assertEquals
import org.junit.Test
import uk.xa0.ward.SessionItem

/**
 * Holding a list's order still while somebody is reading it — the rule behind the
 * drawer not re-sorting under the reader — and fronting pinned rows without
 * disturbing either partition's order (the web's `sectionMembers`).
 */
class SessionOrderTest {

    private fun session(id: String, updatedAt: Long, blank: Boolean = false) = SessionItem(
        id = id,
        title = id,
        cwd = "/tmp",
        updatedAt = updatedAt,
        running = false,
        isSubagent = false,
        blank = blank,
    )

    private val a = session("a", 1)
    private val b = session("b", 2)
    private val c = session("c", 3)

    @Test
    fun `with nothing held the list is left alone`() {
        val live = listOf(c, b, a)
        assertEquals(live, holdOrder(live, emptyList()))
    }

    @Test
    fun `a held order is restored over the live one`() {
        // The roster now says c, b, a — the reader was looking at a, b, c.
        val held = holdOrder(listOf(c, b, a), held = listOf("a", "b", "c"))
        assertEquals(listOf(a, b, c), held)
    }

    @Test
    fun `a session that arrives while the order is held goes to the front`() {
        // Membership is never held: a stale row is worse than a moved one.
        val fresh = session("fresh", 9)
        val held = holdOrder(listOf(fresh, c, b, a), held = listOf("a", "b", "c"))
        assertEquals(listOf(fresh, a, b, c), held)
    }

    @Test
    fun `a session that leaves while the order is held is gone`() {
        val held = holdOrder(listOf(c, a), held = listOf("a", "b", "c"))
        assertEquals(listOf(a, c), held)
    }

    @Test
    fun `two sessions that both arrived keep the order they arrived in`() {
        val one = session("one", 9)
        val two = session("two", 8)
        val held = holdOrder(listOf(one, two, b, a), held = listOf("a", "b"))
        assertEquals(listOf(one, two, a, b), held)
    }

    @Test
    fun `no pins leaves the chosen order alone`() {
        val live = listOf(c, b, a)
        assertEquals(live, pinnedFirst(live, pinned = emptySet(), archived = emptySet()))
    }

    @Test
    fun `pinned rows lead and several pinned keep their caller order`() {
        // The caller's order is d, a, c, b; d and b are pinned, so they move up together
        // in that same relative order and the rest follows as it was.
        val d = session("d", 4)
        val ordered = listOf(d, a, c, b)
        assertEquals(
            listOf(d, b, a, c),
            pinnedFirst(ordered, pinned = setOf("d", "b"), archived = emptySet()),
        )
    }

    @Test
    fun `a pinned row keeps its place inside the partition when the order is held`() {
        // The composition the drawer performs: holdOrder picks the order first, then
        // pinnedFirst fronts it. `a` and `b` must stay a-then-b under the pinned `c`.
        val held = holdOrder(listOf(c, b, a), held = listOf("a", "b", "c"))
        assertEquals(listOf(c, a, b), pinnedFirst(held, pinned = setOf("c"), archived = emptySet()))
    }

    @Test
    fun `an archived pin does not lead`() {
        // The host refuses to pin an archived session, so a set that still names one is
        // stale. The archive fact wins: the row keeps its ordinary place and no other
        // pinned row is displaced.
        val ordered = listOf(a, b)
        assertEquals(
            ordered,
            pinnedFirst(ordered, pinned = setOf("a"), archived = setOf("a")),
        )
        // A live pin beside the stale one still leads normally.
        assertEquals(
            listOf(b, a),
            pinnedFirst(ordered, pinned = setOf("a", "b"), archived = setOf("a")),
        )
    }

    @Test
    fun `a blank placeholder stays first ahead of pinned rows`() {
        // Even when a pinned row is otherwise ahead of it in the caller's order.
        val blank = session("blank", 0, blank = true)
        assertEquals(
            listOf(blank, b, a),
            pinnedFirst(
                listOf(a, blank, b),
                pinned = setOf("b"),
                archived = emptySet(),
            ),
        )
    }

    @Test
    fun `a blank placeholder leads even when nothing is pinned`() {
        // The web's `sectionMembers` fronts placeholders unconditionally, so the rule
        // cannot hide behind the pin set being non-empty.
        val blank = session("blank", 0, blank = true)
        assertEquals(
            listOf(blank, a, b),
            pinnedFirst(listOf(a, b, blank), pinned = emptySet(), archived = emptySet()),
        )
    }
}
