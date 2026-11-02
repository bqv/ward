#include "SessionRailModel.h"

#include <QMetaType>
#include <QVariant>

#include <algorithm>
#include <functional>
#include <limits>

namespace {

// The three numbers the rail's arithmetic is built on, named where they are used
// so the caps in `walk` and in the sections read as the QML's own constants:
// the group cap (SessionSidebar.qml:915), the subagent cap (:922), and the depth
// guard both walkers carry (:704, :1521). The host caps its own nesting far
// below eight, so the guard only ever terminates a malformed roster.
constexpr int kCollapsedSessionLimit = 6;
constexpr int kCollapsedChildLimit = 8;
constexpr int kMaxDepth = 8;

/// The rank of a session the host's manual order does not name: JavaScript's
/// Number.MAX_SAFE_INTEGER, so every listed row sorts ahead of every unlisted
/// one and the unlisted ones keep the id tiebreak (:1352).
constexpr qint64 kUnlistedRank = 9007199254740991LL;

/// What the QML means by `=== true`.
///
/// Every flag test in the binding is a strict comparison: `archived === true`,
/// `pinned === true`, `running === true`, `expanded === true`. QVariant's own
/// toBool() would accept the *string* "true", which the bindings never do — so a
/// roster that sent `"running": "false"` would read as running here and as idle
/// in the QML, and the two shells would disagree about the one thing the rail
/// exists to show. The flag must be a boolean and it must be true.
bool jsTrue(const QVariant &value)
{
    return value.typeId() == QMetaType::Bool && value.toBool();
}

/// A number as the QML reads it.
///
/// `updatedAt` is compared by subtraction (:1362-1364) and
/// `scheduledTaskCount` goes through `Number()` (:2657), so both coerce: a
/// numeric string counts, and an absent value is 0 — which is the binding's own
/// `: 0` fallback. A non-numeric string becomes 0 rather than JavaScript's NaN;
/// the host sends a number, so this is a type mapping and not a rule change.
double jsNumber(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return 0.0;
    if (value.typeId() == QMetaType::QString)
        return value.toString().toDouble();
    if (value.canConvert<double>())
        return value.toDouble();
    return 0.0;
}

/// A string as `String(value)` reads it, except that absent and null are empty.
///
/// The QML's `String(undefined)` is the word "undefined", and the only places
/// that could show are keys of a roster the host never sends (a registry entry
/// with no `workspaceId`, a session with no `sessionId`). Carrying the word
/// through would put a literal "undefined" in a row's title, so it is the
/// JavaScript idiom that is dropped, not a rule.
QString jsString(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return QString();
    return value.toString();
}

/// One session row, or an empty map for an entry that is not a row at all.
///
/// The binding indexes `all[i].sessionId` without a null guard in several places
/// (`present`, :1409), so a malformed entry would throw there rather than being
/// skipped. Skipping it is the only useful reading of "no such row".
QVariantMap asSession(const QVariant &entry)
{
    const auto type = entry.typeId();
    if (type == QMetaType::QVariantMap)
        return entry.toMap();
    if (type == QMetaType::QVariantHash) {
        QVariantMap map;
        const QHash<QString, QVariant> hash = entry.toHash();
        for (auto it = hash.constBegin(); it != hash.constEnd(); ++it)
            map.insert(it.key(), it.value());
        return map;
    }
    return {};
}

QString sessionIdOf(const QVariantMap &session)
{
    return jsString(session.value(QStringLiteral("sessionId")));
}

/// The session's title as the rail draws it (:632-640): a child's readable
/// catalog label wins where the host sent one, then the stored title, and an
/// untitled session — common, and a blank row reads as a rendering fault — gets
/// a stable label. The label is only ever admitted for a child (:344-347), so a
/// fork keeps the title it was given rather than borrowing its parent's.
QString titleOf(const QVariantMap &session, const QHash<QString, QString> &childLabel)
{
    if (session.isEmpty())
        return QStringLiteral("Untitled");
    const QString labelled = childLabel.value(sessionIdOf(session));
    if (!labelled.isEmpty())
        return labelled;
    const QString title = jsString(session.value(QStringLiteral("title")));
    return title.isEmpty() ? QStringLiteral("Untitled") : title;
}

/// The session ids in one order: JavaScript's `<` on strings is a comparison of
/// UTF-16 code units, which is what QString::compare does case-sensitively.
/// (A numeric id would compare numerically in the QML and lexicographically
/// here; the host issues `session-<uuid>` strings.)
int compareIds(const QVariantMap &a, const QVariantMap &b)
{
    return QString::compare(sessionIdOf(a), sessionIdOf(b), Qt::CaseSensitive);
}

/// One descendant of a session, in the shape `descendantsIn` returns it
/// (:695-720): the direct children at depth 0, theirs at 1, and `parentId` the
/// immediate parent — which is half of the host's subagent address.
struct Descendant {
    QVariantMap session;
    int depth = 0;
    QString parentId;
};

/// Every descendant of `id`, depth-first (`descendantsIn`, :699-720).
///
/// The `seen` set and the depth cap are the QML's own: a roster that names a
/// parent twice, or a lineage that loops, terminates rather than spinning. The
/// walk is per CALL, so liveness under one session never depends on what was
/// walked for another.
QVector<Descendant> descendantsIn(const QHash<QString, QVector<QVariantMap>> &children,
                                  const QString &id)
{
    QVector<Descendant> out;
    QSet<QString> seen;
    std::function<void(const QString &, int)> walk = [&](const QString &parent, int depth) {
        const auto kids = children.constFind(parent);
        if (kids == children.constEnd() || depth > kMaxDepth)
            return;
        for (const QVariantMap &kid : kids.value()) {
            const QString key = sessionIdOf(kid);
            if (key.isEmpty() || seen.contains(key))
                continue;
            seen.insert(key);
            out.append(Descendant{kid, depth, parent});
            walk(key, depth + 1);
        }
    };
    walk(id, 0);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// construction and inputs
// ---------------------------------------------------------------------------

SessionRailModel::SessionRailModel(QObject *parent)
    : QObject(parent)
{
}

void SessionRailModel::setInputs(const Inputs &inputs)
{
    m_inputs = inputs;
    rebuild();
}

void SessionRailModel::setSessions(const QVariantList &sessions)
{
    m_inputs.sessions = sessions;
    rebuild();
}

void SessionRailModel::setWorkspaces(const QVariantList &workspaces)
{
    m_inputs.workspaces = workspaces;
    rebuild();
}

void SessionRailModel::setCurrentSessionId(const QString &sessionId)
{
    m_inputs.currentSessionId = sessionId;
    rebuild();
}

void SessionRailModel::setFilterText(const QString &text)
{
    m_inputs.filterText = text;
    rebuild();
}

void SessionRailModel::setSortOrder(SortOrder order)
{
    m_inputs.sortOrder = order;
    rebuild();
}

void SessionRailModel::setGroupByWorkspace(bool grouped)
{
    m_inputs.groupByWorkspace = grouped;
    rebuild();
}

void SessionRailModel::setArchivedFilter(ArchivedFilter filter)
{
    m_inputs.archivedFilter = filter;
    rebuild();
}

void SessionRailModel::setExpandedSessions(const QHash<QString, bool> &map)
{
    m_inputs.expandedSessions = map;
    rebuild();
}

void SessionRailModel::setCollapsedGroups(const QHash<QString, bool> &map)
{
    m_inputs.collapsedGroups = map;
    rebuild();
}

void SessionRailModel::setOpenedGroups(const QSet<QString> &keys)
{
    m_inputs.openedGroups = keys;
    rebuild();
}

void SessionRailModel::setOpenedSubagentGroups(const QSet<QString> &keys)
{
    m_inputs.openedSubagentGroups = keys;
    rebuild();
}

void SessionRailModel::setOpenedChildFolds(const QSet<QString> &parentIds)
{
    m_inputs.openedChildFolds = parentIds;
    rebuild();
}

void SessionRailModel::setOpenedChildSessionFolds(const QSet<QString> &parentIds)
{
    m_inputs.openedChildSessionFolds = parentIds;
    rebuild();
}

void SessionRailModel::setPendingSessionIds(const QStringList &ids)
{
    m_inputs.pendingSessionIds = ids;
    rebuild();
}

void SessionRailModel::setUnreadSessionIds(const QStringList &ids)
{
    m_inputs.unreadSessionIds = ids;
    rebuild();
}

// ---------------------------------------------------------------------------
// the shared predicates
// ---------------------------------------------------------------------------

bool SessionRailModel::isLive(const QVariantMap &session)
{
    // `running` or `waiting`: a subagent blocked on a question is the one the
    // reader most needs told about, and it is precisely not running (:1113-1117).
    return jsTrue(session.value(QStringLiteral("running")))
        || jsTrue(session.value(QStringLiteral("waiting")));
}

bool SessionRailModel::isSubagentChild(const QVariantMap &session)
{
    // No parent id, or a row naming itself, is not a child either: a lineage of
    // one is a row, not somebody's hidden row (:281-286).
    const QString id = sessionIdOf(session);
    const QString parent = jsString(session.value(QStringLiteral("parentSessionId")));
    if (parent.isEmpty() || parent == id)
        return false;
    // The host's own markers, not the parent link: a fork names the session it
    // branched from and is otherwise an ordinary top-level session (:287-293).
    if (jsString(session.value(QStringLiteral("origin"))) == QLatin1String("subagent"))
        return true;
    return !jsString(session.value(QStringLiteral("subagentMode"))).isEmpty();
}

QString SessionRailModel::parentIdOf(const QVariantMap &session)
{
    return isSubagentChild(session) ? jsString(session.value(QStringLiteral("parentSessionId")))
                                    : QString();
}

QString SessionRailModel::groupOf(const QVariantMap &session)
{
    // The host decides this — a Session is in a Workspace because the registry
    // says so — rather than by matching `cwd`, which splits one Workspace the
    // moment two share a directory (:974-984).
    return jsString(session.value(QStringLiteral("workspaceId")));
}

QString SessionRailModel::rowKey(const Row &row)
{
    // Identity across a rebuild of the array (:1924-1939). A parent's cap row
    // and its section's are different rows that can be on screen at once, which
    // is why they do not share a key.
    switch (row.kind) {
    case RowKind::GroupHeader:
        return QStringLiteral("h:") + row.groupKey;
    case RowKind::GroupMore:
        return QStringLiteral("m:") + row.groupKey;
    case RowKind::GroupMoreSubagents:
        return QStringLiteral("ma:") + row.groupKey;
    case RowKind::ChildMore:
        return QStringLiteral("c:") + row.parentKey;
    case RowKind::ChildMoreSubagents:
        return QStringLiteral("ca:") + row.parentKey;
    case RowKind::Session:
        return QStringLiteral("s:") + row.sessionId;
    }
    return QString();
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

void SessionRailModel::rebuild()
{
    m_rows.clear();
    m_liveGroups.clear();
    m_pinnedCount = 0;
    m_liveDescendantCount = 0;
    m_liveDescendantRunning = 0;
    m_liveDescendantWaiting = 0;

    // `allSessions` as the header's "N / M" counts it (:248-254, :2199-2200):
    // the length of the transport's list, whatever it holds.
    m_totalSessionCount = int(m_inputs.sessions.size());

    // ---- parentage and child labels, from the one flat list (:323-361) -----
    //
    // Two indexes, and the label is admitted for a child ALONE: a subagent's
    // stored title is its opening prompt, and the readable one lives in its
    // parent's catalog — but a fork also names a parent, and borrowing the name
    // of the session it was branched from would rename it.
    QHash<QString, QString> parentOf;
    QHash<QString, QString> childLabel;
    QVector<QVariantMap> roster;
    roster.reserve(m_inputs.sessions.size());
    for (const QVariant &entry : m_inputs.sessions) {
        const QVariantMap session = asSession(entry);
        const QString id = sessionIdOf(session);
        if (id.isEmpty())
            continue;
        roster.append(session);
        const QString parent = parentIdOf(session);
        if (!parent.isEmpty())
            parentOf.insert(id, parent);
        const QString label = jsString(session.value(QStringLiteral("label")));
        if (!parent.isEmpty() && !label.isEmpty())
            childLabel.insert(id, label);
    }

    // ---- the filter (:603-625) --------------------------------------------
    //
    // The archived choice is applied HERE rather than in the transport, so
    // switching it costs nothing: the rows are already in hand. The needle is
    // trimmed and lowercased once (:605), matches the title as drawn — which
    // means a child can be found by its catalog label — or the cwd, and never
    // the id.
    const QString needle = m_inputs.filterText.trimmed().toLower();
    QVector<QVariantMap> filtered;
    filtered.reserve(roster.size());
    for (const QVariantMap &session : roster) {
        const bool archived = jsTrue(session.value(QStringLiteral("archived")));
        if (m_inputs.archivedFilter == ArchivedFilter::Default && archived)
            continue;
        if (m_inputs.archivedFilter == ArchivedFilter::Only && !archived)
            continue;
        if (!needle.isEmpty()) {
            const QString cwd = jsString(session.value(QStringLiteral("cwd")));
            if (!titleOf(session, childLabel).toLower().contains(needle)
                && !cwd.toLower().contains(needle))
                continue;
        }
        filtered.append(session);
    }
    m_filteredSessionCount = filtered.size();

    // ---- the two sorts (:1347-1369) ---------------------------------------
    //
    // `updated` is the web UI's default: latest human prompt first, session id
    // ascending as the tiebreak — not the title, and not whatever order the host
    // happened to send. `manual` is the host's own durable accounting order,
    // which its `insertSessionBefore` rearranges.
    const auto workspaceMemberOrder = [this](const QString &workspaceId) {
        for (const QVariant &entry : m_inputs.workspaces) {
            const QVariantMap workspace = entry.toMap();
            if (jsString(workspace.value(QStringLiteral("workspaceId"))) == workspaceId)
                return workspace.value(QStringLiteral("sessionIds")).toList();
        }
        return QVariantList();
    };

    if (m_inputs.sortOrder == SortOrder::Manual) {
        // The QML asks `indexOf` per comparison, which is O(n·m) for one
        // rebuild; the answer is the same integer per row, so it is resolved
        // once here. `Array.indexOf` compares the raw values, with no
        // `session-` prefix normalisation — the host hands the order out in the
        // same form it hands the roster — so no normalising is done here either.
        QHash<QString, qint64> rank;
        rank.reserve(filtered.size());
        for (const QVariantMap &session : filtered) {
            const QVariant id = session.value(QStringLiteral("sessionId"));
            const QVariantList order = workspaceMemberOrder(groupOf(session));
            qint64 at = kUnlistedRank;
            for (int i = 0; i < order.size(); ++i) {
                if (order.at(i) == id) {
                    at = i;
                    break;
                }
            }
            rank.insert(sessionIdOf(session), at);
        }
        std::stable_sort(filtered.begin(), filtered.end(),
                         [&rank](const QVariantMap &a, const QVariantMap &b) {
                             const qint64 ra = rank.value(sessionIdOf(a), kUnlistedRank);
                             const qint64 rb = rank.value(sessionIdOf(b), kUnlistedRank);
                             if (ra != rb)
                                 return ra < rb;
                             return compareIds(a, b) < 0;
                         });
    } else {
        // JavaScript's sort is stable (ECMAScript 2019), which the tiebreak
        // depends on: two rows with the same timestamp and id keep their roster
        // order, so std::stable_sort is the honest equivalent. `delta` is
        // `b.updatedAt - a.updatedAt`, so a newer row compares negative and
        // sorts first (:1362-1368).
        std::stable_sort(filtered.begin(), filtered.end(),
                         [](const QVariantMap &a, const QVariantMap &b) {
                             const double delta = jsNumber(b.value(QStringLiteral("updatedAt")))
                                                - jsNumber(a.value(QStringLiteral("updatedAt")));
                             if (delta != 0.0)
                                 return delta < 0.0;
                             return compareIds(a, b) < 0;
                         });
    }

    // ---- the pinned partition (:1397-1407) --------------------------------
    //
    // A STABLE partition, pinned first, each part keeping the order it had. A
    // pin is therefore not an ordering of its own: in "last updated" the pinned
    // block is itself recency-sorted, and pin TIME never orders anything. The
    // host keeps its pin set newest-pinned-first, which is a trap rather than a
    // spec, and this deliberately does not read it.
    const auto pinnedFirst = [](const QVector<QVariantMap> &members) {
        QVector<QVariantMap> leading;
        QVector<QVariantMap> rest;
        for (const QVariantMap &member : members) {
            if (jsTrue(member.value(QStringLiteral("pinned"))))
                leading.append(member);
            else
                rest.append(member);
        }
        for (const QVariantMap &member : rest)
            leading.append(member);
        return leading;
    };

    // ---- who the filter left on the rail (:1409-1411) ---------------------
    QSet<QString> present;
    present.reserve(filtered.size());
    for (const QVariantMap &session : filtered)
        present.insert(sessionIdOf(session));

    // ---- the two absences a child's parent can have (:1446-1467) ----------
    //
    // Both forms of every id. The host stores a Session id with and without the
    // `session-` prefix, and a child's `parentSessionId` may name either — the
    // C++ projection indexes its child labels both ways for the same reason.
    QSet<QString> archivedIds;
    QSet<QString> rosterIds;
    for (const QVariantMap &row : roster) {
        const QString rosterId = sessionIdOf(row);
        if (rosterId.isEmpty())
            continue;
        const QString bare = rosterId.startsWith(QLatin1String("session-")) ? rosterId.mid(8)
                                                                          : rosterId;
        rosterIds.insert(bare);
        rosterIds.insert(QStringLiteral("session-") + bare);
        if (jsTrue(row.value(QStringLiteral("archived")))) {
            archivedIds.insert(bare);
            archivedIds.insert(QStringLiteral("session-") + bare);
        }
    }

    // ---- parentage indexed (:669-693) ------------------------------------
    //
    // Two indexes, because they answer two different questions. The tree's own
    // counts follow the FILTER — a filtered-out child is not a row, and a count
    // that disagrees with what unfolding shows is worse than no count. The
    // header's live count and a parent's liveness follow the whole ROSTER: a
    // running child that a search box happens to hide is still running, and
    // hiding it is the exact invisibility this rail exists to refuse.
    const auto indexPath = [](const QVector<QVariantMap> &rows) {
        QHash<QString, QVector<QVariantMap>> children;
        for (const QVariantMap &entry : rows) {
            const QString id = sessionIdOf(entry);
            if (id.isEmpty())
                continue;
            const QString key = parentIdOf(entry);
            if (key.isEmpty())
                continue;
            children[key].append(entry);
        }
        return children;
    };
    const QHash<QString, QVector<QVariantMap>> rosterChildren = indexPath(roster);
    const QHash<QString, QVector<QVariantMap>> filteredChildren = indexPath(filtered);

    // ---- what is live under a row (:1119-1143) ----------------------------
    //
    // Descendants, not just the child itself: hiding a quiet child that has a
    // running grandchild under it would hide the only row that says so. The
    // roster, not the filter: a fold, a cap or a search cannot be an excuse to
    // hide work.
    const auto liveUnder = [&rosterChildren](const QVariantMap &session) {
        if (isLive(session))
            return true;
        const QString key = sessionIdOf(session);
        if (!rosterChildren.contains(key))
            return false;
        for (const Descendant &below : descendantsIn(rosterChildren, key)) {
            if (isLive(below.session))
                return true;
        }
        return false;
    };

    // ---- a child the archived filter orphaned (:1471-1501) ---------------
    //
    // Three absences, and only the first two are the rule; the third was
    // measured on a real roster. A child whose parent the ARCHIVED filter
    // removed belongs to that filed-away conversation, not to a conversation of
    // its own — promoting it drew 69 settled subagents into Ungrouped. A child
    // whose parent the SEARCH took is promoted flat at the top instead: the box
    // must not hide a session it matched. The roster carries `archived` on every
    // row, so the two cases are distinguishable without asking the host again.
    // Five children on this box's roster name a parent with no row at all — its
    // record is gone while the archived set still names it — and they go the
    // same way as archived ones.
    //
    // Liveness outranks hiding, for the same reason it outranks both caps, and
    // the row being READ is never hidden, whatever it came from: a row that
    // disappears under the reader takes the rail's highlight with it.
    const auto hidesWithArchivedParent = [&](const QVariantMap &session) {
        if (m_inputs.archivedFilter != ArchivedFilter::Default)
            return false;
        if (!m_inputs.currentSessionId.isEmpty()
            && sessionIdOf(session) == m_inputs.currentSessionId)
            return false;
        const QString parent = parentOf.value(sessionIdOf(session));
        if (parent.isEmpty())
            return false; // not a child: a fork is an ordinary row
        if (!rosterIds.contains(parent))
            return !liveUnder(session);
        if (!archivedIds.contains(parent))
            return false; // on disk and not archived: the query took it, so promote
        return !liveUnder(session);
    };

    // ---- a Workspace's own name (:955-961) --------------------------------
    const auto workspaceLabel = [](const QVariantMap &workspace) {
        const QString title = jsString(workspace.value(QStringLiteral("title")));
        if (!title.isEmpty())
            return title;
        const QString path = jsString(workspace.value(QStringLiteral("path")));
        if (!path.isEmpty())
            return path;
        return jsString(workspace.value(QStringLiteral("workspaceId")));
    };

    // A group's name by key, for the folded rail's marks: "Ungrouped" is a real
    // group whose key is "", and a tooltip reading nothing at all would say the
    // mark was broken (:1236-1243).
    const auto groupLabel = [&](const QString &key) {
        for (const QVariant &entry : m_inputs.workspaces) {
            const QVariantMap workspace = entry.toMap();
            if (jsString(workspace.value(QStringLiteral("workspaceId"))) == key)
                return workspaceLabel(workspace);
        }
        return key.isEmpty() ? QStringLiteral("Ungrouped") : key;
    };

    // ---- the pins, from the roster (:1221-1229) --------------------------
    //
    // From the roster rather than from the rows on screen: a fold, the cap or a
    // filter must not make the strip claim there is nothing pinned. The flag is
    // the transport's, and an archived Session is never flagged.
    for (const QVariantMap &session : roster) {
        if (jsTrue(session.value(QStringLiteral("pinned"))))
            ++m_pinnedCount;
    }

    // ---- the groups with work in them (:1165-1212) -----------------------
    //
    // One entry per group, in roster order, counting live sessions WITH their
    // live descendants: a group whose only live rows are subagents is still a
    // group the reader will find work in. `running` separates blue from amber —
    // a group whose only live work is parked on a human is precisely not
    // running, and the two cannot share a colour.
    QSet<QString> rosterPresent;
    rosterPresent.reserve(roster.size());
    for (const QVariantMap &row : roster)
        rosterPresent.insert(sessionIdOf(row));

    QHash<QString, int> liveGroupIndex;
    for (const QVariantMap &session : roster) {
        // A subagent is its parent's work, not a second group's: counting it
        // twice would make two marks for one agent's delegation. A fork is a
        // group's member in its own right, which is why the test is the host's
        // word for a subagent and not "has a parent".
        const QString parentKey = parentIdOf(session);
        if (!parentKey.isEmpty() && rosterPresent.contains(parentKey))
            continue;
        if (!liveUnder(session))
            continue;
        const QString key = groupOf(session);
        int at = liveGroupIndex.value(key, -1);
        if (at < 0) {
            LiveGroup group;
            group.key = key;
            group.label = groupLabel(key);
            at = int(m_liveGroups.size());
            liveGroupIndex.insert(key, at);
            m_liveGroups.append(group);
        }
        int live = 1;
        bool inFlight = jsTrue(session.value(QStringLiteral("running")))
                     && !jsTrue(session.value(QStringLiteral("waiting")));
        for (const Descendant &below : descendantsIn(rosterChildren, sessionIdOf(session))) {
            if (!isLive(below.session))
                continue;
            ++live;
            if (jsTrue(below.session.value(QStringLiteral("running")))
                && !jsTrue(below.session.value(QStringLiteral("waiting"))))
                inFlight = true;
        }
        m_liveGroups[at].count += live;
        if (inFlight)
            m_liveGroups[at].running = true;
    }

    // ---- what is live under the session being read (:732-784) ------------
    //
    // Counted from the ROSTER, and archived rows count: filing a session away
    // says where it is kept, not that it stopped working.
    if (!m_inputs.currentSessionId.isEmpty()) {
        for (const Descendant &below : descendantsIn(rosterChildren, m_inputs.currentSessionId)) {
            if (!isLive(below.session))
                continue;
            ++m_liveDescendantCount;
            // The summary counts a session that is both running and waiting as
            // WAITING alone, so the chip's blue is only for work in flight
            // (:759-777, :1113 note).
            if (jsTrue(below.session.value(QStringLiteral("waiting"))))
                ++m_liveDescendantWaiting;
            else if (jsTrue(below.session.value(QStringLiteral("running"))))
                ++m_liveDescendantRunning;
        }
    }

    // ---- which group holds the followed session, once (:986-1009) --------
    //
    // `rows` asks the fold rule for every section, and each of those used to
    // walk the whole roster looking for the current session. `present` is kept
    // apart from the key because a session in no Workspace has the empty key,
    // which is a real answer and not "not found".
    bool currentPresent = false;
    QString currentGroup;
    if (!m_inputs.currentSessionId.isEmpty()) {
        for (const QVariantMap &session : filtered) {
            if (sessionIdOf(session) == m_inputs.currentSessionId) {
                currentPresent = true;
                currentGroup = groupOf(session);
                break;
            }
        }
    }

    const auto isGroupCollapsed = [&](const QString &key) {
        const auto saved = m_inputs.collapsedGroups.constFind(key);
        if (saved != m_inputs.collapsedGroups.constEnd())
            return saved.value();
        // In "only" mode the point is to look at what is archived, and there are
        // few enough of them that opening the groups which hold any is the whole
        // reason the mode exists (:1015-1020).
        if (m_inputs.archivedFilter == ArchivedFilter::Only)
            return false;
        // No preference recorded yet: the web UI opens only the group holding
        // the current session, but a client with no current session would then
        // present a rail of headers and nothing else, which reads as broken
        // (:1021-1027).
        if (m_inputs.currentSessionId.isEmpty())
            return false;
        return !(currentPresent && currentGroup == key);
    };

    // ---- a caret's state (:452-466) --------------------------------------
    //
    // Tri-state: an explicit decision wins (including "closed"), and a parent
    // that has children is open until someone closes it. Defaulting to closed
    // drew a delegation as a caret and nothing else.
    const auto isExpanded = [&](const QString &id) {
        const auto decision = m_inputs.expandedSessions.constFind(id);
        if (decision != m_inputs.expandedSessions.constEnd())
            return decision.value();
        const auto kids = rosterChildren.constFind(id);
        return kids != rosterChildren.constEnd() && !kids.value().isEmpty();
    };

    // ---- the session row's own marks (:2637-2781, :3058-3212) ------------
    QSet<QString> pending;
    for (const QString &id : m_inputs.pendingSessionIds)
        pending.insert(id);
    QSet<QString> unread;
    for (const QString &id : m_inputs.unreadSessionIds)
        unread.insert(id);

    const auto decorate = [&](Row &row, const QVariantMap &session) {
        row.sessionId = sessionIdOf(session);
        row.title = titleOf(session, childLabel);
        row.parentId = parentOf.value(row.sessionId);
        row.child = !row.parentId.isEmpty();
        row.groupKey = groupOf(session);
        row.archived = jsTrue(session.value(QStringLiteral("archived")));
        row.pinned = jsTrue(session.value(QStringLiteral("pinned")));
        row.scheduledTaskCount = int(jsNumber(session.value(QStringLiteral("scheduledTaskCount"))));
        row.running = jsTrue(session.value(QStringLiteral("running")));
        row.waiting = jsTrue(session.value(QStringLiteral("waiting")));
        // The QML compares the two values with `===`, so an empty current id
        // matches nothing rather than matching a row with no id (there is none).
        row.current = !m_inputs.currentSessionId.isEmpty()
                   && row.sessionId == m_inputs.currentSessionId;

        // Archived rows draw no status mark at all, and neither attention mark:
        // a filed-away conversation is a record, and a mark on it would ask for
        // a look it cannot be given (:3075-3077).
        const bool stateVisible = !row.archived;
        // Waiting on a person. Two sources, because they cover different halves
        // of the same fact: the roster's `waiting` counts a session's open
        // questions, the transport's pending waterfalls are the only record of
        // an approval (:3079-3087).
        row.parked = stateVisible && (row.waiting || pending.contains(row.sessionId));
        // Finished while the reader was elsewhere. It yields the slot to
        // `parked`, and the fact is not lost — the green mark appears as soon as
        // the question is answered (:3089-3097).
        row.unseen = stateVisible && !row.parked && unread.contains(row.sessionId);
        row.markWords = row.parked ? QStringLiteral("Waiting for your answer")
                      : row.unseen ? QStringLiteral("New since you last looked")
                                   : QString();

        // What is under this row, counted from the FILTERED roster so the
        // number is the tree's own and not a wider one's: it is what the caret
        // would disclose if it were opened (:2717-2752). The total is
        // deliberately not the number drawn — a dozen finished subagents must
        // not put a dozen beside the caret.
        for (const Descendant &below : descendantsIn(filteredChildren, row.sessionId)) {
            if (isLive(below.session))
                ++row.liveDescendants;
            if (jsTrue(below.session.value(QStringLiteral("running")))
                && !jsTrue(below.session.value(QStringLiteral("waiting"))))
                row.descendantRunning = true;
            if (jsTrue(below.session.value(QStringLiteral("waiting"))))
                row.descendantWaiting = true;
        }
    };

    // ---- the tree, flattened (:1516-1596) --------------------------------
    //
    // One flat array, depth-first, parents before children, which is what a
    // list view can render without a tree model. `seen` makes a roster that
    // names a parent twice, or a lineage that loops, terminate.
    QSet<QString> seen;
    std::function<void(const QVariantMap &, int)> walk = [&](const QVariantMap &session,
                                                            int depth) {
        const QString id = sessionIdOf(session);
        if (seen.contains(id) || depth > kMaxDepth)
            return;
        seen.insert(id);

        const bool hasChildren = filteredChildren.contains(id);
        Row row;
        row.kind = RowKind::Session;
        row.depth = depth;
        row.hasChildren = hasChildren;
        decorate(row, session);
        m_rows.append(row);

        if (!hasChildren || !isExpanded(id))
            return;

        const QVector<QVariantMap> children = filteredChildren.value(id);
        const bool subagentKidsOpen = m_inputs.openedChildFolds.contains(id);
        const bool sessionKidsOpen = m_inputs.openedChildSessionFolds.contains(id);

        QSet<QString> shownKidIds;
        QVector<QVariantMap> foldedSessionKids;
        QVector<QVariantMap> foldedSubagentKids;
        int sessionKids = 0;
        int subagentKids = 0;
        for (const QVariantMap &kid : children) {
            const bool subagent = isSubagentChild(kid);
            const bool shown =
                subagent ? (subagentKids < kCollapsedChildLimit || liveUnder(kid))
                         : (sessionKids < kCollapsedSessionLimit || liveUnder(kid));
            if (shown)
                shownKidIds.insert(sessionIdOf(kid));
            else if (subagent)
                foldedSubagentKids.append(kid);
            else
                foldedSessionKids.append(kid);
            if (subagent)
                ++subagentKids;
            else
                ++sessionKids;
        }
        for (const QVariantMap &kid : children) {
            const bool open =
                isSubagentChild(kid) ? subagentKidsOpen : sessionKidsOpen;
            if (open || shownKidIds.contains(sessionIdOf(kid)))
                walk(kid, depth + 1);
        }

        const auto appendChildCap = [&](RowKind kind,
                                        const QVector<QVariantMap> &folded, bool open) {
            if (folded.isEmpty())
                return;
            Row more;
            more.kind = kind;
            more.parentKey = id;
            more.groupKey = groupOf(session);
            more.depth = depth + 1;
            more.open = open;
            more.hidden = int(folded.size());
            m_rows.append(more);
        };
        appendChildCap(RowKind::ChildMore, foldedSessionKids, sessionKidsOpen);
        appendChildCap(RowKind::ChildMoreSubagents, foldedSubagentKids,
                       subagentKidsOpen);
    };

    // ---- the flat layout (:1598-1620) ------------------------------------
    if (!m_inputs.groupByWorkspace) {
        // The flat list is a section too, as far as the web is concerned: its
        // `deriveFlat` calls the same partition the grouped tree does, so a pin
        // leads here with no group header to lead under. Without this the pin
        // mark moved and the row did not.
        QVector<QVariantMap> topLevel;
        topLevel.reserve(filtered.size());
        for (const QVariantMap &session : filtered) {
            const QString parent = parentOf.value(sessionIdOf(session));
            if (!parent.isEmpty() && present.contains(parent))
                continue;
            if (hidesWithArchivedParent(session))
                continue;
            topLevel.append(session);
        }
        for (const QVariantMap &session : pinnedFirst(topLevel))
            walk(session, 0);
        return;
    }

    // ---- the Workspace sections (:1626-1709) -----------------------------
    //
    // One section per Workspace, in the HOST's registry order: that array is the
    // user's own arrangement, and ranking by activity instead would discard an
    // order the host stored and let people drag.
    QHash<QString, QVector<QVariantMap>> byGroup;
    QVector<QString> byGroupOrder;
    QVector<QVariantMap> ungrouped;
    for (const QVariantMap &session : filtered) {
        const QString parent = parentOf.value(sessionIdOf(session));
        if (!parent.isEmpty() && present.contains(parent))
            continue; // a child is reached through its parent, not a section
        // Dropped BEFORE the sections are built, so no header count, live count
        // or cap can describe a row that is not there.
        if (hidesWithArchivedParent(session))
            continue;
        const QString key = groupOf(session);
        if (key.isEmpty()) {
            ungrouped.append(session);
            continue;
        }
        if (!byGroup.contains(key)) {
            byGroupOrder.append(key);
            byGroup.insert(key, QVector<QVariantMap>());
        }
        byGroup[key].append(session);
    }

    struct Section {
        QString key;
        QString label;
        QString icon;
        QString iconFile;
        bool workspace = false;
        QVector<QVariantMap> members;
    };

    QVector<Section> sections;
    QVector<Section> virtualSections;
    QSet<QString> claimed;
    for (const QVariant &entry : m_inputs.workspaces) {
        const QVariantMap workspace = entry.toMap();
        const QString key = jsString(workspace.value(QStringLiteral("workspaceId")));
        claimed.insert(key);
        Section section;
        section.key = key;
        section.label = workspaceLabel(workspace);
        section.members = byGroup.value(key);
        if (jsTrue(workspace.value(QStringLiteral("virtual")))) {
            // The automations bucket, held back to the very end: it is a kind of
            // session rather than a place one lives, and a scheduled run's
            // session is history the moment it ends. Dropped entirely when
            // empty, which a real Workspace's header is not: there is no
            // workspace to lose.
            if (section.members.isEmpty())
                continue;
            section.icon = QStringLiteral("\u25c6");
            // Not a Workspace, so it gets no New Session +: `session/create`
            // would be handed an id the host never issued.
            section.workspace = false;
            virtualSections.append(section);
            continue;
        }
        // A real Workspace is rendered even when it holds no visible session:
        // the web UI does the same, and a group that vanishes reads as a
        // Workspace that was lost.
        section.icon = QStringLiteral("\u25a4");
        section.workspace = true;
        sections.append(section);
    }

    // A Session the registry does not account for — in no Workspace at all, or
    // left behind by one that was deleted. Appended only when it has members,
    // and always last but for the automations. The keys are walked in
    // first-appearance order, which is the order JavaScript's own object
    // iteration used.
    for (const QString &key : byGroupOrder) {
        if (claimed.contains(key))
            continue;
        for (const QVariantMap &member : byGroup.value(key))
            ungrouped.append(member);
    }
    if (!ungrouped.isEmpty()) {
        Section section;
        section.key = QString();
        section.label = QStringLiteral("Ungrouped");
        // Hollow, against the Workspaces' filled mark: sessions outside every
        // Workspace are still this machine's, but they belong to no directory.
        section.icon = QStringLiteral("\u25a1");
        section.iconFile = QStringLiteral("inbox.svg");
        section.workspace = false;
        section.members = ungrouped;
        sections.append(section);
    }
    for (const Section &section : virtualSections)
        sections.append(section);

    for (const Section &section : sections) {
        const bool collapsed = isGroupCollapsed(section.key);
        const QVector<QVariantMap> members = pinnedFirst(section.members);

        // The Workspace's own session count: top-level Sessions only, never a
        // subagent, never dependent on which folds are open (:1721-1743). A
        // subagent is its parent's work rather than a second session in the
        // Workspace, the parent row already carries how many are live under it,
        // and a count that moves when the reader merely looks inside is the
        // bug. A child the SEARCH box orphaned is a top-level member here and is
        // still not counted, because it is still a child.
        int total = 0;
        for (const QVariantMap &member : members) {
            if (!isSubagentChild(member))
                ++total;
        }

        Row header;
        header.kind = RowKind::GroupHeader;
        header.title = section.label;
        header.count = total;
        header.groupKey = section.key;
        header.expanded = !collapsed;
        header.icon = section.icon;
        header.iconFile = section.iconFile;
        header.workspace = section.workspace;
        // The badge a folded group keeps, from the same roster walk the 34 px
        // strip draws its marks from: the rows go, the signal stays, and a fold
        // can no longer read as a no-op. It is drawn only while folded — with
        // the rows on screen their own dots say it (:2754-2781).
        for (const LiveGroup &group : m_liveGroups) {
            if (group.key == section.key) {
                header.groupLiveCount = group.count;
                header.groupLiveRunning = group.running;
                break;
            }
        }
        m_rows.append(header);

        // A fold is the reader's decision and it is honoured: a folded group
        // draws its header and nothing under it, live rows included. Folding the
        // Workspace you are reading is legitimate — the rail quiet while you
        // read — and an earlier rule lifted live rows back out, so the click
        // looked like it had done nothing (:1759-1779).
        if (collapsed)
            continue;

        const bool sessionsOpen = m_inputs.openedGroups.contains(section.key);
        const bool subagentsOpen = m_inputs.openedSubagentGroups.contains(section.key);
        QSet<QString> shownMemberIds;
        QVector<QVariantMap> foldedSessions;
        QVector<QVariantMap> foldedSubagents;
        int unpinnedSessions = 0;
        int subagents = 0;
        for (const QVariantMap &member : members) {
            const bool subagent = isSubagentChild(member);
            const bool pinned = jsTrue(member.value(QStringLiteral("pinned")));
            const bool shown =
                pinned
                || (subagent ? (subagents < kCollapsedChildLimit || liveUnder(member))
                             : (unpinnedSessions < kCollapsedSessionLimit
                                || liveUnder(member)));
            if (shown)
                shownMemberIds.insert(sessionIdOf(member));
            else if (subagent)
                foldedSubagents.append(member);
            else
                foldedSessions.append(member);
            if (!pinned) {
                if (subagent)
                    ++subagents;
                else
                    ++unpinnedSessions;
            }
        }
        for (const QVariantMap &member : members) {
            const bool open = isSubagentChild(member) ? subagentsOpen : sessionsOpen;
            if (open || shownMemberIds.contains(sessionIdOf(member)))
                walk(member, 0);
        }

        const auto appendGroupCap = [&](RowKind kind,
                                        const QVector<QVariantMap> &folded, bool open) {
            if (folded.isEmpty())
                return;
            Row more;
            more.kind = kind;
            more.groupKey = section.key;
            more.open = open;
            more.hidden = int(folded.size());
            m_rows.append(more);
        };
        appendGroupCap(RowKind::GroupMore, foldedSessions, sessionsOpen);
        appendGroupCap(RowKind::GroupMoreSubagents, foldedSubagents, subagentsOpen);
    }
}
