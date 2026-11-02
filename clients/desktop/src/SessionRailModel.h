#pragma once

// SessionRailModel — the session rail's rows, computed with no way to draw them.
//
// This is `SessionSidebar.qml`'s `rows` binding (SessionSidebar.qml:1334-1823),
// together with the derived facts that binding reads and the delegate draws:
// the filter (:603), the child-label index (:323), the two sorts (:1347), the
// pinned-first partition (:1397), the archived-parent rule (:1471), the tree
// walk and its two caps (:1516), the Workspace sections (:1626), the per-section
// header counts and live badges (:1711), and the per-row marks the delegate
// computes from the session (:2637-2781, :3058-3212).
//
// WHY IT IS A CLASS OF ITS OWN. Stage 3 replaces the QML rail with a Widgets
// view over this model, and stage 3's acceptance criterion is row-for-row parity
// with the QML rail (plan §5 stage 3). During the change both must be fed the
// same roster and agree, so the RULES live here once and the QML keeps its copy
// only while the QML shell exists (plan §10a). The rules are ported, not
// paraphrased: every derivation below names the QML line it came from, and the
// places where the QML was surprising say so in the comment.
//
// The split is enforced by what is ABSENT: no QWidget, no QQuickItem, no
// QAbstractItemModel, no QML property lookup, no colour and no measurement. A
// view injects the transport's lists through `Inputs`, reads `rows()`, and draws
// them. The QML-free seam is the same one `TerminalEmulator` keeps (see
// src/TerminalEmulator.h), so the offscreen dump in `~/tmp` can build rows with
// no window at all.
//
// A `Row` is a LAYOUT fact, not a copy of a session: it carries the id, the
// title as the rail draws it, the counts and the marks. Anything a menu acts on
// (rename's stored title, pin's write) is looked up in the transport by
// `sessionId` when it is needed, so a row that is a moment stale cannot act on
// the wrong session.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class SessionRailModel : public QObject
{
    Q_OBJECT

public:
    /// How a section orders its sessions (SessionSidebar.qml:797-805).
    enum class SortOrder {
        /// The web UI's default: latest human prompt first, session id ascending
        /// as the tiebreak (:1362-1368).
        Updated,
        /// The host's own durable accounting order, the one its
        /// `insertSessionBefore` rearranges (:1348-1360).
        Manual,
    };

    /// What to do with archived Sessions (:812-816). The web UI's own three
    /// states: hiding them is the default, being unable to see them at all is
    /// not.
    enum class ArchivedFilter { Default, Show, Only };

    /// What a row is. The QML distinguishes these with the flags `header`,
    /// `more` and `childMore` on one flat array (:1745, :1578, :1807) — the
    /// delegate's `isHeader` / `isMore` / `isChildMore` (:2623-2628). A kind is
    /// the same information with the shape stated, so no combination of flags
    /// can mean two things.
    enum class RowKind {
        GroupHeader,  ///< a Workspace section's header, or Ungrouped, or Automations
        Session,      ///< one session, top-level or nested under a parent
        ChildMore,    ///< "Show N more" for one parent's folded subagents
        GroupMore,    ///< "Show N more" for one section's folded sessions
        ChildMoreSubagents,
        GroupMoreSubagents,
    };

    static bool isGroupCap(RowKind kind)
    {
        return kind == RowKind::GroupMore || kind == RowKind::GroupMoreSubagents;
    }

    static bool isChildCap(RowKind kind)
    {
        return kind == RowKind::ChildMore || kind == RowKind::ChildMoreSubagents;
    }

    static bool isCap(RowKind kind) { return isGroupCap(kind) || isChildCap(kind); }

    static bool capIsSubagents(RowKind kind)
    {
        return kind == RowKind::GroupMoreSubagents || kind == RowKind::ChildMoreSubagents;
    }

    /// One group's live work, for a folded section's header badge and the
    /// collapsed rail's 34 px strip (SessionSidebar.qml:1165-1212). Ordered as
    /// the roster is ordered, not sorted here.
    struct LiveGroup {
        QString key;
        QString label;
        int count = 0;
        bool running = false;
    };

    /// One row of the rail.
    struct Row {
        RowKind kind = RowKind::Session;
        /// One indent step per level, 0 for a section header and for the
        /// section's own cap row; a parent's cap row sits at `parent.depth + 1`
        /// (:1589, and the delegate's `depth` at :2634-2636).
        int depth = 0;
        /// A header's label, a session's title as the rail draws it (a child's
        /// catalog label wins, then the stored title, then "Untitled"; :632-640),
        /// and empty for a cap row — the view composes "Show N more" itself so
        /// the wording stays with the drawing.
        QString title;
        QString sessionId;
        /// The immediate parent, for a session that is a subagent child; empty
        /// for a fork and for anything top-level (:1525-1535).
        QString parentId;
        /// A header's or cap row's section key, and a session's Workspace id —
        /// "" is a real section (Ungrouped) and a real answer (:982-984).
        QString groupKey;
        /// A cap row's owner when it is a parent's: the parent's session id
        /// (:1584). Empty for a section's cap row.
        QString parentKey;
        /// A header's kind mark: a Workspace is a directory, a schedule is a
        /// kind of session, Ungrouped belongs to no directory (:1671-1706).
        QString icon;
        QString iconFile;

        /// A header's own session count: top-level Sessions only, never a
        /// subagent, never dependent on a fold (:1740-1743).
        int count = 0;
        /// A cap row's count of the rows the cap is holding (:1594, :1814).
        int hidden = 0;

        /// A header for a real registry Workspace, which may offer New Session;
        /// false for Ungrouped and the automations bucket (:1752-1756).
        bool workspace = false;
        /// A header's fold state, open when true (:1750).
        bool expanded = false;
        /// A cap row's direction: the cap is off, so the row reads "Show less"
        /// (:1810-1813, :1590-1593).
        bool open = false;
        /// A session that is somebody's subagent: it opens by the tagged
        /// address, not the plain one (:1534).
        bool child = false;
        /// Whether the FILTERED tree has children under this session, which is
        /// what draws the caret (:1536).
        bool hasChildren = false;
        /// This is the session the transcript is showing (:2637-2638).
        bool current = false;
        /// The host's pin flag, read from the row; an archived row is never
        /// pinned and the transport has already cleared it (:2643-2645).
        bool pinned = false;
        bool archived = false;
        /// How many scheduled tasks deliver into this session, folded from the
        /// host's schedule catalog onto the roster row (:2654-2657).
        int scheduledTaskCount = 0;

        /// This session's own turn is in flight, and it is parked on a person.
        bool running = false;
        bool waiting = false;
        /// Waiting on a person: the amber mark. The roster's `waiting` or the
        /// transport's pending waterfalls, either is enough (:3084-3087).
        bool parked = false;
        /// Finished while the reader was elsewhere: the green mark, which
        /// yields its slot to `parked` (:3095-3097).
        bool unseen = false;
        /// The word for whichever of the two marks is drawn (:3101-3103).
        QString markWords;

        /// Active descendants in the FILTERED tree — the number beside the
        /// caret, which the delegate computes from `filteredChildren`
        /// (:2720-2752). Not the total: a dozen finished subagents must not put
        /// a dozen there.
        int liveDescendants = 0;
        bool descendantRunning = false;
        bool descendantWaiting = false;

        /// A folded header's live badge, drawn only while the section is
        /// folded (:2761-2781). Zero when the group has no live work.
        int groupLiveCount = 0;
        bool groupLiveRunning = false;
    };

    /// Everything `rows` reads, in one place, so a caller can hand over a whole
    /// state rather than a sequence of setters that each rebuild.
    struct Inputs {
        /// The transport's session list (`dshTransport.sessions`; :248-254).
        /// Each entry is the host's own summary: `sessionId`, `title`, `cwd`,
        /// `updatedAt`, `workspaceId`, `archived`, `pinned`, `running`,
        /// `waiting`, `scheduledTaskCount`, and for a subagent child
        /// `parentSessionId`, `origin`, `subagentMode` and the catalog `label`.
        QVariantList sessions;
        /// The host's Workspace registry, in the host's own order
        /// (`dshTransport.workspaces`; :949-951), each entry `{workspaceId,
        /// path, title, sessionIds, virtual}`.
        QVariantList workspaces;
        /// The session the transcript is showing (`dshTransport.sessionId`).
        /// Empty when nothing is followed, which is a real state: the rail then
        /// opens every section (:1025-1026).
        QString currentSessionId;
        /// The filter box's text, untrimmed; the model trims and lowercases it
        /// exactly as the binding does (:605).
        QString filterText;

        SortOrder sortOrder = SortOrder::Updated;
        bool groupByWorkspace = true;
        ArchivedFilter archivedFilter = ArchivedFilter::Default;

        /// A row's caret, keyed by session id. TRI-STATE on purpose: a key that
        /// is present is an explicit decision (including `false`), and a key
        /// that is absent means "nobody has said", which opens a parent that has
        /// children (:452-466).
        QHash<QString, bool> expandedSessions;
        /// A section header's fold, keyed by section key. Absent follows the
        /// default rule in `isGroupCollapsed` (:1011-1028).
        QHash<QString, bool> collapsedGroups;
        /// Sections whose own "Show N more" has been opened out (:928).
        QSet<QString> openedGroups;
        QSet<QString> openedSubagentGroups;
        /// Parents whose subagent cap has been opened out (:944).
        QSet<QString> openedChildFolds;
        QSet<QString> openedChildSessionFolds;

        /// The transport's pending waterfalls, as ids: an approval is not a
        /// projection, so this is the only record of one (:66-72).
        QStringList pendingSessionIds;
        /// Sessions that finished a turn while the reader was elsewhere
        /// (:77-83).
        QStringList unreadSessionIds;
    };

    explicit SessionRailModel(QObject *parent = nullptr);

    /// Replace every input and rebuild once.
    void setInputs(const Inputs &inputs);
    const Inputs &inputs() const { return m_inputs; }

    // ---- the individual inputs -----------------------------------------------
    //
    // Each one rebuilds immediately, which is what the QML binding did: every
    // one of these was a property `rows` read, so every change recomputed the
    // array. A caller changing several at once should use `setInputs`, which
    // rebuilds once; the view slice will need a batch door for a fold gesture
    // and this is where it goes.
    void setSessions(const QVariantList &sessions);
    void setWorkspaces(const QVariantList &workspaces);
    void setCurrentSessionId(const QString &sessionId);
    void setFilterText(const QString &text);
    void setSortOrder(SortOrder order);
    void setGroupByWorkspace(bool grouped);
    void setArchivedFilter(ArchivedFilter filter);
    void setExpandedSessions(const QHash<QString, bool> &map);
    void setCollapsedGroups(const QHash<QString, bool> &map);
    void setOpenedGroups(const QSet<QString> &keys);
    void setOpenedSubagentGroups(const QSet<QString> &keys);
    void setOpenedChildFolds(const QSet<QString> &parentIds);
    void setOpenedChildSessionFolds(const QSet<QString> &parentIds);
    void setPendingSessionIds(const QStringList &ids);
    void setUnreadSessionIds(const QStringList &ids);

    // ---- the result ----------------------------------------------------------

    /// The ordered rows, depth first, parents before children. Rebuilt by any
    /// input change; a pointer to this vector is valid until the next rebuild.
    const QVector<Row> &rows() const { return m_rows; }
    int rowCount() const { return m_rows.size(); }

    /// The filter's own two numbers, for the rail header's "N / M" while a
    /// filter is narrowing the tree (SessionSidebar.qml:2199-2200).
    int filteredSessionCount() const { return m_filteredSessionCount; }
    int totalSessionCount() const { return m_totalSessionCount; }
    bool filtering() const { return !m_inputs.filterText.trimmed().isEmpty(); }

    /// How many roster rows the host's pin set flags — from the ROSTER, so a
    /// fold, a cap or a filter cannot make the strip claim there is nothing
    /// pinned (:1221-1229).
    int pinnedCount() const { return m_pinnedCount; }

    /// What is live UNDER the session being read, counted from the roster, for
    /// the rail header's chip and its sheet (:732-784). A session blocked on a
    /// question counts as live: it is precisely not running, and it is the one
    /// that most needs saying.
    int liveDescendantCount() const { return m_liveDescendantCount; }
    int liveDescendantRunningCount() const { return m_liveDescendantRunning; }
    int liveDescendantWaitingCount() const { return m_liveDescendantWaiting; }
    bool liveDescendantRunning() const { return m_liveDescendantRunning > 0; }

    /// One entry per group holding live work, in roster order, for the folded
    /// rail's marks and a folded header's badge (:1165-1212).
    const QVector<LiveGroup> &liveGroups() const { return m_liveGroups; }

    /// A row's identity across a rebuild of the array: `h:<key>`, `m:<key>`,
    /// `c:<parent id>`, `s:<session id>` (:1924-1939). The scroll anchors that
    /// use it are NOT ported here — they belong with the view, which is what
    /// owns `contentY` and the row heights (see the report).
    static QString rowKey(const Row &row);

    // ---- predicates the view and the model share -----------------------------

    /// The rail's one notion of "active": a session doing work now, or one
    /// parked on a person (:1113-1117).
    static bool isLive(const QVariantMap &session);

    /// Whether a roster row is a subagent child, by the HOST's rule: `origin`
    /// says "subagent", or the row carries a non-empty `subagentMode`. A parent
    /// id alone is not a child — a fork names the session it branched from and
    /// is otherwise an ordinary top-level session (:278-294).
    static bool isSubagentChild(const QVariantMap &session);

    /// The parent a row actually hangs under, or empty for anything else — the
    /// one door every parentage decision goes through (:301-303).
    static QString parentIdOf(const QVariantMap &session);

    /// A session's Workspace key (`workspaceId`, "" when it has none): the
    /// host deciding, not a match on `cwd` (:982-984).
    static QString groupOf(const QVariantMap &session);

    /// Recompute `rows()` and the derived counts from `inputs()`.
    void rebuild();

private:
    Inputs m_inputs;
    QVector<Row> m_rows;
    QVector<LiveGroup> m_liveGroups;
    int m_totalSessionCount = 0;
    int m_filteredSessionCount = 0;
    int m_pinnedCount = 0;
    int m_liveDescendantCount = 0;
    int m_liveDescendantRunning = 0;
    int m_liveDescendantWaiting = 0;
};
