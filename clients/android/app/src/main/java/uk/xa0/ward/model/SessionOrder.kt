package uk.xa0.ward.model

import uk.xa0.ward.SessionItem

/**
 * Orders a session list so that anything already in [held] keeps the place it had,
 * and anything new goes to the front.
 *
 * The roster arrives freshly sorted by `updatedAt` on every refresh, so a session
 * that receives a message jumps to the top of its group — while the drawer is open
 * and somebody is reading it. The recorder caught that as the list moving with
 * nothing driving it: shifts of two and three rows with only 85-95% of the visible
 * rows surviving, and no gesture and no programmatic scroll behind any of them.
 *
 * Holding the order while the drawer is open costs nothing a reader can see, because
 * the only thing deferred is a re-sort they did not ask for. Membership is never
 * held: a session that arrives or is archived while the drawer is open is shown or
 * hidden straight away, since a stale row is worse than a moved one. A row with no
 * held place is therefore new, and goes to the front, which is where a new session
 * belongs anyway.
 *
 * `sortedWith` is stable, so rows that share a rank — the new ones, and any two the
 * roster delivered in the same order — keep the order they arrived in.
 */
fun holdOrder(sessions: List<SessionItem>, held: List<String>): List<SessionItem> {
    if (held.isEmpty()) return sessions
    val rank = HashMap<String, Int>(held.size)
    held.forEachIndexed { index, id -> rank[id] = index }
    return sessions.sortedWith(compareBy { rank[it.id] ?: -1 })
}

/**
 * Fronts this list's pinned sessions, keeping the supplied order inside each partition.
 *
 * This is the web's `sectionMembers`
 * (`dsh-client-ui-workspace/lib/client.js`): provisional New Session rows first, then
 * the unarchived pinned rows, then everything else. It is applied *after* the caller
 * has chosen the order — the roster's recency, a Workspace's host manual order, or
 * [holdOrder]'s held order — so each partition keeps the order the caller decided on,
 * and it never re-sorts inside one. Flat/List mode, every Workspace section and
 * Ungrouped all run this identical fold; the drawer must not grow a second rule.
 *
 * Archived rows are **not** leading candidates even when a pin set still names them.
 * The host refuses to pin an archived session and draws no mark on an archived row, so
 * a set held across an archive is stale about that id (`sessionNode` already suppresses
 * the mark for the same reason). Letting a stale pin reorder a saved row would make the
 * archive filter and the pin set disagree about where a row belongs; here the archive
 * state, which is the current fact, wins, and the row simply takes its place among the
 * unpinned ones.
 *
 * @param sessions rows in the caller's chosen order.
 * @param pinned the host's registry-global pin set.
 * @param archived the host's archive set; a pin on one of these ids is not honoured.
 * @return the same rows, partitioned; no row is added, dropped or duplicated.
 */
fun pinnedFirst(
    sessions: List<SessionItem>,
    pinned: Set<String>,
    archived: Set<String>,
): List<SessionItem> {
    val placeholders = ArrayList<SessionItem>()
    val leading = ArrayList<SessionItem>()
    val rest = ArrayList<SessionItem>()
    for (session in sessions) {
        when {
            session.blank -> placeholders += session
            session.id in pinned && session.id !in archived -> leading += session
            else -> rest += session
        }
    }
    // Nothing to front: the caller's list is already the answer, and returning it as-is
    // keeps the common case from copying a roster of a couple of thousand rows.
    if (leading.isEmpty() && placeholders.isEmpty()) return sessions
    return placeholders + leading + rest
}
