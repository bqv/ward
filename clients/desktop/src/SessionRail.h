#pragma once

// SessionRail — the session rail as a real `QWidget` over `SessionRailModel`.
//
// This is `SessionSidebar.qml` re-housed: the filter, the header row, the tree of
// rows and the marks the delegate paints. The RULES that decide what the rows
// are live in `SessionRailModel` (stage 3a), so this class holds no sort, no
// grouping and no fold arithmetic — it feeds the model its inputs, draws
// `rows()` and turns a gesture into one of the signals the shell answers.
//
// WHERE THE BOUNDARY IS. The rail owns its content, its own two folds (the
// sections' and the caret's) and the keyboard. The SHELL owns its rectangle: the
// 240/34 splitter, the 140 ms transition and the transcript margin that follows
// it are the shell's, because the chat page's layout is the shell's (plan §5,
// stage 3; Shell::toggleRail()). The rail is told `setExpanded()` and simply
// paints the shape that fits the width it is given — below
// `expandedWidth() / 2` it shows the 34 px strip, so the animation never draws
// a 240 px header into a 100 px cell.
//
// MENUS ARE NOT HERE. A `QMenu` is a `Qt::Popup` window and therefore a shell
// surface (plan §4.4), exactly as the status strip's three menus are
// (StatusStrip.cpp's `shellMenu`). The rail asks — `overflowMenuRequested()`,
// `rowMenuRequested()` with the row's rectangle in global coordinates — and the
// shell builds, parents and places the menu with the same helpers.
//
// The row adapter is deliberately small: it exposes `SessionRailModel::rows()`
// as a `QAbstractListModel` so a `QListView` can address a row by index. The
// fields a row paints live on the model, so the delegate reads the `Row` rather
// than a role per field: one source of truth, no second copy to fall out of
// step.

#include "SessionRailModel.h"

#include <QHash>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class ClientFacade;
class QAbstractItemView;
class QLabel;
class QListView;
class QStackedWidget;
class QTimer;

class SessionRail : public QWidget
{
    Q_OBJECT

public:
    /// `facade` is the shell's typed view of the transport: the roster, the
    /// Workspaces, the followed session and the two mark sets are read from it,
    /// never from a QML context property. `iconDir` is the installed QML tree's
    /// `icons/` directory — the rail's pin and clock marks are the same two SVGs
    /// the QML rail draws, so the two shells cannot drift apart.
    SessionRail(ClientFacade *facade, const QString &iconDir, QWidget *parent = nullptr);
    ~SessionRail() override;

    // ---- the rectangle the shell gives us ----------------------------------
    //
    // The numbers are the QML rail's own `expandedWidth`/`collapsedWidth`, and
    // the 140 ms is main.qml's `Behavior on width { NumberAnimation { duration:
    // 140 } }`. They live here so the shell does not keep a second copy; the
    // ANIMATION itself is the shell's, because the splitter and the transcript's
    // margin are its layout.
    static int expandedWidth();    // 240
    static int collapsedWidth();   // 34
    static int animationMs();      // 140
    static int rowHeight();        // 45, the QML delegate's `height: 45`
    static int capRowHeight();

    /// The fold the shell has asked for. The strip's own shape follows the width
    /// it is given, so a collapse in flight is drawn as a strip as soon as the
    /// animation crosses the threshold — see the width test in resizeEvent().
    bool expanded() const { return m_expanded; }
    void setExpanded(bool expanded);

    // ---- the shell's two anchors ------------------------------------------
    //
    // The overflow menu hangs from the button that asked for it, and a row menu
    // from the row's rectangle. A `QListView` row is not a widget, so the rail
    // maps the rectangle itself — it is the only side that can.
    QWidget *overflowAnchor() const;
    QRect rowGlobalRect(const QString &sessionId) const;

    /// For the shell's probe and for a later slice's scroll anchor; the view is
    /// an implementation detail otherwise.
    QAbstractItemView *rowView() const;

    int rowCount() const;

    // ---- state the shell's menus read and write ---------------------------
    bool terminalSupported() const { return m_terminalSupported; }
    void setTerminalSupported(bool supported);
    bool terminalActive() const { return m_terminalActive; }
    void setTerminalActive(bool active);

    SessionRailModel::SortOrder sortOrder() const { return m_sortOrder; }
    void setSortOrder(SessionRailModel::SortOrder order);
    bool groupByWorkspace() const { return m_groupByWorkspace; }
    void setGroupByWorkspace(bool grouped);
    SessionRailModel::ArchivedFilter archivedFilter() const { return m_archivedFilter; }
    void setArchivedFilter(SessionRailModel::ArchivedFilter filter);
    /// The two menu entries are cycles rather than submenus: the QML's "Sort" and
    /// "Archived" rows flip their value, so the shell asks for the next one.
    void cycleSortOrder();
    void cycleArchivedFilter();

    /// The overflow button's lit state while the shell's menu is open.
    void setOverflowMenuOpen(bool open);
    /// The shell's row menu was dismissed; the row's ⋯ stops being held open.
    void noteRowMenuClosed();

    /// The rail's notice line: one wrapped sentence in `danger` when it reports a
    /// failure and `dim` when it states a fact. Fed by the transport's
    /// `railNotice` signal and by the shell, and cleared after the QML's pause.
    void showNotice(const QString &text, bool error);

signals:
    /// The header's fold control. The shell owns the animation.
    void foldToggleRequested();
    /// The terminal toggle; only the terminal has a dock in this shell, and the
    /// other two pane toggles are not emitted until their slices exist.
    void terminalToggleRequested();

    void workspaceNewRequested();
    void sessionNewRequested();
    void sessionNewInWorkspaceRequested(const QString &workspaceId);

    /// A row was chosen: by click or by Enter. `parentId` is non-empty only for a
    /// subagent child, which the host opens by the tagged address.
    void sessionActivated(const QString &sessionId, const QString &parentId, bool child);

    void renameRequested(const QString &sessionId, const QString &storedTitle);
    void copyIdRequested(const QString &sessionId);
    void forkRequested(const QString &sessionId);
    void pinRequested(const QString &sessionId, bool pinned);
    void archiveRequested(const QString &sessionId, bool archived);

    void settingsRequested();
    void automationRequested();
    void pluginsRequested();
    void refreshRequested();
    void restartRequested();

    /// The shell builds the menus (see the header comment). `globalRect` is the
    /// row's rectangle in global coordinates, so the shell can place the menu
    /// under it without reaching into the view.
    void overflowMenuRequested();
    void rowMenuRequested(const QRect &globalRect, const QString &sessionId,
                          const QString &storedTitle, bool pinned, bool archived);
    /// The list moved under an open row menu, which would otherwise hang over
    /// whatever scrolled into its place.
    void rowMenuDismissRequested();

private Q_SLOTS:
    /// The transport's `railNotice(QString,bool)`, forwarded to showNotice().
    /// A slot rather than a lambda because the signal exists only on the
    /// transport's own meta-object, which is reached through the old-style
    /// connect (the facade does not carry the notice).
    void onRailNotice(const QString &text, bool error) { showNotice(text, error); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class Rows;
    class Delegate;
    class Button;
    class Filter;
    class CollapsedMarks;

    // ---- model input -------------------------------------------------------
    //
    // One rebuild per transport change: the inputs are assembled into a single
    // `SessionRailModel::Inputs` and handed over with `setInputs()`, which is the
    // model's batch door — a per-fact setter would rebuild the tree five times
    // for one host update.
    void refreshFromFacade();
    void reloadRows();
    void updateHeader();
    void updateEmptyState();
    void positionEmptyState();
    void rebuildCollapsedMarks();

    /// The row under an index, or null.
    const SessionRailModel::Row *rowAt(int index) const;

    // ---- the two folds, and their store -----------------------------------
    //
    // Read back at construction through the transport's own invokables, exactly
    // as the QML does (railExpandedSessions/railCollapsed), and written through
    // on every gesture. The formats are the QML's own:
    //   "<1|0>:<key>"     the row's fold (a section header, or a caret)
    //   "1o:<key>"        the second fold on the same row ("Show N more")
    // A bare key in the group store is the oldest shape and still reads as
    // folded.
    void restoreFolds();
    void persistExpansion();
    void persistGroups();
    static bool parseEntry(const QString &entry, bool bareIsAFold, QString *key,
                           bool *value, bool *openedOut);

    void toggleGroup(const QString &key);
    void toggleSessionFold(const QString &sessionId);
    void toggleChildFold(const QString &parentId, bool subagents);
    void setGroupOpened(const QString &key, bool subagents, bool opened);

    // ---- gestures ---------------------------------------------------------
    void openRow(int index);
    void requestRowMenu(int index);
    void revealFirstPinned();
    void revealLiveGroup(const QString &key);
    QString storedTitleFor(const QString &sessionId) const;
    QString rowKeyAt(int index) const;
    int indexForKey(const QString &key) const;

    void setStackForWidth(int width);
    void updateSpinTimer();

    ClientFacade *m_facade;
    QString m_iconDir;

    SessionRailModel m_model;
    Rows *m_rowsModel = nullptr;

    /// The two shapes, side by side in a stack: the 240 px tree and the 34 px
    /// strip. Only one is up at a time, and which one follows the WIDTH rather
    /// than the requested state, so the animation never crushes the tree.
    QStackedWidget *m_stack = nullptr;
    QWidget *m_expandedPage = nullptr;
    QWidget *m_collapsedPage = nullptr;

    QLabel *m_title = nullptr;
    QLabel *m_count = nullptr;
    Button *m_foldButton = nullptr;
    Button *m_workspaceNewButton = nullptr;
    Button *m_overflowButton = nullptr;
    Button *m_collapsedFoldButton = nullptr;
    Button *m_collapsedTerminalButton = nullptr;
    QLabel *m_notice = nullptr;
    Filter *m_filter = nullptr;
    QListView *m_list = nullptr;
    QLabel *m_emptyLabel = nullptr;
    Button *m_emptyNewButton = nullptr;
    CollapsedMarks *m_collapsedMarks = nullptr;
    QTimer *m_spinTimer = nullptr;
    QTimer *m_noticeTimer = nullptr;

    SessionRailModel::SortOrder m_sortOrder = SessionRailModel::SortOrder::Updated;
    bool m_groupByWorkspace = true;
    SessionRailModel::ArchivedFilter m_archivedFilter =
        SessionRailModel::ArchivedFilter::Default;

    bool m_expanded = true;
    bool m_terminalSupported = false;
    bool m_terminalActive = false;
    bool m_filtering = false;

    // The persisted folds, in the model's own shapes.
    QHash<QString, bool> m_expandedSessions;
    QHash<QString, bool> m_collapsedGroups;
    QSet<QString> m_openedGroups;
    QSet<QString> m_openedSubagentGroups;
    QSet<QString> m_openedChildFolds;
    QSet<QString> m_openedChildSessionFolds;
    bool m_foldsRestored = false;
    bool m_foldWarningSaid = false;

    /// The row the pointer is over, for the hover-only controls (the row's ⋯,
    /// the header's +). `QListView` has no hover index, and a delegate that drew
    /// them unconditionally would put a control on every row at once.
    int m_hoveredRow = -1;
    int m_menuRow = -1;
    QString m_menuSessionId;

    /// The spinner's quarter turn, advanced by a timer only while a visible row
    /// is actually running — the QML's `RotationAnimator` in one shared clock.
    int m_spinAngle = 0;

    int m_loggedRows = -1;
};
