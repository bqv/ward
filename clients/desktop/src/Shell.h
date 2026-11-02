#pragma once

#include <QHash>
#include <QRect>
#include <QUrl>
#include <QVariant>
#include <QWidget>

class AcpClient;
class ClientFacade;
class Composer;
class GoalEditDialog;
class GoalRow;
class HeadlessClient;
class HeadlessPage;
class ManagementPage;
class TranscriptView;
class SettingsDialog;
class ChangesDialog;
class FilesDock;
class TrajectoryPage;
class Lightbox;
class PendingBand;
class QAction;
class QMenu;
class QQmlEngine;
class QStackedWidget;
class QVBoxLayout;
class QVariantAnimation;
class PaneHost;
class AcpPage;
class WebPage;
class SessionRail;
class ShellHeader;
class StatusStrip;
class TerminalPanel;

/// The full-window panes' files and clients, as `main.cpp` knows them.
///
/// They arrive as one argument rather than six so that a slice which adds a
/// fourth page adds a field here instead of growing the constructor's tail
/// again. The clients are borrowed: `main.cpp` owns them, the panes read their
/// state, and this shell only drives their lifecycle (plan §4.3's "the shell
/// keeps its Connections to the client").
struct ShellPanes
{
    QUrl acp;
    QUrl headless;
    QUrl webUrl;
    AcpClient *acpClient = nullptr;
    HeadlessClient *headlessClient = nullptr;
};

/// The Widgets shell: a plain `QWidget` top-level whose chrome is three bands.
///
/// Not a `QMainWindow` on purpose (plan §5, stage 1): the application's header and
/// footer are custom, there is no menu bar, and `QMainWindow`'s status bar, dock
/// widgets and toolbar would add behaviours the design does not want. A stack is
/// still the right container because the full-window surfaces — web, ACP,
/// headless and the management pages — are pages that replace the chat page
/// exactly as they replace the QML content item today.
///
/// The window's bands, top to bottom:
///
///   header            39 px, the application's bar (ShellHeader), above the stack
///   the current page   the chat page, or one of the full-window pages (web, ACP,
///                     headless, management)
///   status strip      24 px, the conversation's bar (StatusStrip), chat page only
///   composer footer   the composer and the dock stack above it; 49 px at rest
///                     (35 px field + the QML footer's 10 px above, 4 px below),
///                     growing with the field to 120 px and shrinking back
///
/// The chat page itself is `[ rail 240 | conversation expanding ]` above a footer
/// whose height is the composer's plus whatever the docks add, so the arithmetic
/// is 39 + content + 24 + (composer + docks). The footer is the QML's `footer:`
/// band (main.qml:943): the composer at the bottom of the window, full width, and
/// above it the dock stack the session's surfaces grow into — the goal row, the
/// pending band and the terminal (TerminalPanel), and in the QML the files dock.
/// `filesDock` is reserved and zero-height.
///
/// The full-window pages are the pages themselves now: web, ACP and management
/// are `PaneHost`s filling the stack, and the headless pane is a real
/// `HeadlessPage` widget; the header's toggles drive the same open / minimise /
/// close state machine `main.qml` keeps in `openTrapdoor()` and its siblings.
/// "Minimise" hides the page and keeps everything behind it — the web page and
/// (when this build has one) its Servo view, the ACP child and its sessions,
/// the headless conversation — while "close", the pane's own button, is the
/// exit that unloads: the web page's `shutRequested` seam, the ACP child's
/// `shutdown()`, the headless client's `reset()`. Escape is the QML's own
/// guarded exit, one page at a time.
///
/// The management page is the fourth page and the only one with no header
/// toggle: the rail's two menu entries open it, on the Automation or the Plugins
/// tab. It has one exit (its own Close and Escape both return to chat) because
/// there is nothing behind it to unload — its catalogues live in the transport
/// and its Delete is a shell action, confirmed by a shell dialog.
class Shell : public QWidget
{
    Q_OBJECT

public:
    /// `engine` is the shell's one shared `QQmlEngine`, borrowed and outliving
    /// every pane. `facade` is the typed view of the selected transport; every
    /// value the chrome draws comes from it, never from a QML context property.
    /// `panes` names the full-window pane files and lends the two client objects
    /// their pages are written against.
    Shell(ClientFacade *facade, const ShellPanes &panes, QWidget *parent = nullptr);

    /// The retained panes, for the coordinator's visible run and for the slices
    /// that will give them inputs. The rail is no longer a retained pane: it is
    /// a real widget over the C++ rows model (stage 3b).
    TranscriptView *conversation() const { return m_conversation; }
    SessionRail *rail() const { return m_rail; }

    /// The full-window pages, for the same visible run. Each is the page itself:
    /// the stack holds the `PaneHost` or `HeadlessPage`, not a wrapper around it.
    WebPage *webPage() const { return m_webPage; }
    AcpPage *acpPage() const { return m_acpPage; }
    HeadlessPage *headlessPage() const { return m_headlessPage; }
    ManagementPage *managementPage() const { return m_managementPage; }

    /// The tab in front of the management page: "" while it is not up,
    /// "automation" or "plugins" while it is. The page itself is one stack page;
    /// this is the input that tells its two halves apart.
    QString managementTab() const { return m_managementTab; }

    /// The page currently on the stack: "chat", "web", "acp", "headless" or
    /// "management" (the last one on either of its two tabs).
    QString currentPage() const { return m_currentPage; }

    Composer *composer() const { return m_composer; }
    PendingBand *pendingBandSlot() const { return m_pendingBandSlot; }
    GoalRow *goalRow() const { return m_goalRow; }
    TerminalPanel *terminalPanel() const { return m_terminalDockSlot; }
    QWidget *filesDockSlot() const { return m_filesDockSlot; }

protected:
    // Window geometry is the one piece of state a desktop window has to remember.
    // The same QSettings keys as the QML shell (`window/width`, `window/height`,
    // `window/x`, `window/y`), so a person who runs both shells keeps their
    // window; the position is saved on move and the size on resize rather than
    // both only at close, and close writes the pair one last time.
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static QString iconDirectory();
    QWidget *buildChatPage();
    /// Construct the full-window pages and answer their requests. Their root
    /// items start hidden, because `main.qml` gives each pane
    /// `visible: window.xOpen` and the pane's own focus rule keys off it.
    void buildFullWindowPages(const ShellPanes &panes);
    void openImageLightbox(const QVariant &source, const QVariant &alt,
                           qreal fallbackWidth, qreal fallbackHeight);
    void showPage(const QString &page);

    // ---- the full-window pages' state machine ------------------------------
    //
    // Four verbs, named after the QML functions they replace rather than after
    // the stack: a toggle press is `openTrapdoor()` when the page is down and
    // `minimiseTrapdoor()` when it is up; a pane's own close is
    // `closeTrapdoor()` (and its ACP/headless siblings). `openPage()` is the
    // header's press when down, `minimisePage()` the header's press when up and
    // Escape's answer for web and headless, `closePage()` the pane's own exit
    // and Escape's answer for ACP.
    void togglePage(const QString &page);
    void openPage(const QString &page);
    void minimisePage(const QString &page);
    void closePage(const QString &page);

    /// The management page has no toggle of its own, so it has its own opener:
    /// `main.qml`'s `openManagementPage(name)`. One stack page, and `tab` is the
    /// pane's `page` input. Opening it puts the three panes away by the verbs
    /// their own openers use against each other (web and headless minimise, ACP
    /// closes), and `openPage()` closes it in return — one page at a time.
    void openManagementPage(const QString &tab);
    void showSettingsDialog();
    void showChangesDialog();
    void showGoalEditDialog(const QString &objective, int maxGoalRounds);
    /// The refresh `main.qml`'s opener performs once: the namespace the tab reads,
    /// asked of the transport by name because the facade deliberately does not
    /// carry the schedule and plugin surfaces (plan §3.3).
    /// The management page's Delete: a shell dialog, then the transport's
    /// `deleteScheduleTask`. The pane asks and never performs it.
    void deleteManagementTask(const QString &taskId, const QString &taskTitle);
    /// Escape: the QML's four guarded `Shortcut`s, collapsed into one action
    /// because the widgets stack can only ever have one page up.
    void escapeCurrentPage();

    // ---- the rail's rectangle, and its two menus ---------------------------
    //
    // The rail owns its content; the SHELL owns the splitter. The 240/34 widths
    // and the 140 ms are the rail's own constants (SessionRail::expandedWidth()
    // and friends), because they are the QML rail's numbers, but the animation
    // runs here: it also has to carry the dock stack's left inset, which is the
    // shell's layout and the transcript's margin.
    void toggleRail();
    void applyRailWidth(int width);
    /// The rail's terminal toggle and the overflow menu's Terminal entry are one
    /// verb: open the dock when it is closed, close it when it is open.
    void toggleTerminalDock();
    /// The two `QMenu`s the rail asks for. They are the shell's — a popup is a
    /// `Qt::Popup` window and no pane owns one (plan §4.4) — and they are built
    /// with the same helpers the status strip's three use.
    void openRailOverflowMenu();
    void openRailRowMenu(const QRect &globalRow, const QString &sessionId,
                         const QString &storedTitle, bool pinned, bool archived);
    /// The rail's own actions, each forwarded to the transport by name because
    /// the facade deliberately carries only the chrome's subset (plan §3.3).
    void renameRailSession(const QString &sessionId, const QString &storedTitle);
    void callRailTransport(const char *method, const QString &argument = QString());
    /// Focus the chat composer after a pane is put away, which is the QML's
    /// `input.forceActiveFocus()` in every minimise and close function.
    void focusChatComposer();

    void restoreWindowGeometry();
    void saveWindowGeometry(bool position, bool size);

    ClientFacade *m_facade;
    ShellHeader *m_header;
    QStackedWidget *m_pages;
    StatusStrip *m_strip = nullptr;
    TranscriptView *m_conversation = nullptr;
    SessionRail *m_rail = nullptr;
    /// The dock stack's layout, whose left margin follows the rail's width. Held
    /// because the animation writes it and it is built in buildChatPage().
    QVBoxLayout *m_dockLayout = nullptr;
    /// The rail's menu that is open, if any: the rail asks for a row menu, the
    /// shell owns the surface, and the rail must be told when it goes away so a
    /// row's ⋯ stops being held open.
    QMenu *m_openRailMenu = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    ChangesDialog *m_changesDialog = nullptr;
    GoalEditDialog *m_goalEditDialog = nullptr;
    /// The splitter's one animation, so a second press retargets it rather than
    /// stacking two timelines on the same fixed width.
    QVariantAnimation *m_railAnimation = nullptr;
    /// The fold in force, which is what the animation is moving towards.
    bool m_railExpanded = true;
    WebPage *m_webPage = nullptr;
    AcpPage *m_acpPage = nullptr;
    HeadlessPage *m_headlessPage = nullptr;
    ManagementPage *m_managementPage = nullptr;
    FilesDock *m_filesDock = nullptr;
    TrajectoryPage *m_trajectory = nullptr;
    QStackedWidget *m_conversationStack = nullptr;
    /// The tab in front of the management page, kept here because the page's
    /// `page` input is written by this side and the state machine has to know
    /// whether an opening is a fresh page or a switch between its two halves.
    QString m_managementTab;
    AcpClient *m_acpClient = nullptr;           // borrowed; the ACP pane's child
    HeadlessClient *m_headlessClient = nullptr; // borrowed; the headless pane's child
    QUrl m_webUrl;
    QAction *m_escape = nullptr;
    Lightbox *m_lightbox = nullptr;
    Composer *m_composer = nullptr;
    PendingBand *m_pendingBandSlot = nullptr;
    GoalRow *m_goalRow = nullptr;
    TerminalPanel *m_terminalDockSlot = nullptr;
    QWidget *m_filesDockSlot = nullptr;
    QHash<QString, QWidget *> m_pageByName;
    QString m_currentPage;

    /// True once the saved geometry has been applied. Until then a resize is the
    /// constructor's own default and must not overwrite the stored size before it
    /// is read.
    bool m_geometryRestored = false;
};
