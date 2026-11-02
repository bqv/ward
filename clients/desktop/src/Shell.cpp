#include "Shell.h"

#include "ChangesDialog.h"
#include "SettingsDialog.h"

#include "AcpClient.h"
#include "AcpPage.h"
#include "ClientFacade.h"
#include "Composer.h"
#include "FilesDock.h"
#include "GoalEditDialog.h"
#include "GoalRow.h"
#include "HeadlessClient.h"

#include "HeadlessPage.h"
#include "Lightbox.h"
#include "ManagementPage.h"
#include "PendingBand.h"
#include "SessionRail.h"
#include "ShellChrome.h"
#include "ShellHeader.h"
#include "StatusStrip.h"
#include "TerminalPanel.h"
#include "TrajectoryPage.h"
#include "TranscriptView.h"
#include "WebPage.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

// The QML window's own numbers (qml/main.qml: width 940, height 720, minimum
// 560x400). Copied from the QML shell deliberately, so the two shells are
// comparable while both exist (plan §10a); the Widgets shell opens at these and
// restores a saved size over them.
constexpr int DefaultWidth = 940;
constexpr int DefaultHeight = 720;
constexpr int MinimumWidth = 560;
constexpr int MinimumHeight = 400;

// The rail's expanded width is `SessionRail::expandedWidth()` (240), and its
// collapsed width is `SessionRail::collapsedWidth()` (34): the numbers are the
// QML rail's own, and they live with the rail because the rail is what has to
// fit inside them. What the shell owns is the transition BETWEEN them and the
// transcript margin that follows it (Shell::toggleRail()).

/// The QML's `footer:` band: the composer, and the dock stack above it.
///
/// main.qml:944 is a `Rectangle` filled `Theme.panel` with a `Theme.border`
/// hairline along its top edge — the seam between the transcript and the composer,
/// and the reason the dock draws no rule of its own (main.qml:1083-1087: a second
/// one 10 px below read as the double border the owner saw). Its height is
/// arithmetic over its parts rather than a measurement of them (main.qml:1049),
/// which is why the shell pins this widget's height from the composer's own number
/// and lets the layout fill it, instead of letting the layout decide.
class FooterBar : public QWidget
{
public:
    explicit FooterBar(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("composerFooter"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->border());
    }
};

class DockPanel : public QWidget
{
public:
    explicit DockPanel(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("dockStack"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->border());
    }
};

/// The state machine between the header's three toggles, each page's two exits,
/// and the one Escape action.

} // namespace

QString Shell::iconDirectory()
{
    static const QString directory = []() {
        QStringList candidates;
        candidates << QCoreApplication::applicationDirPath()
                          + QStringLiteral("/assets/icons")
                   << QStringLiteral(DSH_NATIVE_DATA_SUBDIR "/icons")
                   << QCoreApplication::applicationDirPath()
                          + QStringLiteral("/../assets/icons");
        for (const QString &candidate : candidates) {
            if (QDir(candidate).exists()) {
                const QString resolved = QDir(candidate).absolutePath();
                qInfo().noquote() << "icons:" << resolved;
                return resolved;
            }
        }
        qWarning() << "icons: none of the candidate directories exists; the rail and"
                      " the transcript will draw without their marks";
        return QString();
    }();
    return directory;
}

Shell::Shell(ClientFacade *facade, const ShellPanes &panes, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
    , m_acpClient(panes.acpClient)
    , m_headlessClient(panes.headlessClient)
    , m_webUrl(panes.webUrl)
    , m_currentPage(QStringLiteral("chat"))
{
    setObjectName(QStringLiteral("widgetsShell"));
    setWindowTitle(QStringLiteral("ward"));
    resize(DefaultWidth, DefaultHeight);
    setMinimumSize(MinimumWidth, MinimumHeight);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The application's bar is above the stack, not inside a page: it is the same
    // whatever you are reading, so switching pages must not take it away.
    m_header = new ShellHeader(facade, this);
    outer->addWidget(m_header);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("pageStack"));
    outer->addWidget(m_pages, 1);

    m_pageByName.insert(QStringLiteral("chat"),
                        buildChatPage());
    // The three full-window pages are pages of this stack, each filling the
    // window below the header. They are built now and kept for the process's
    // life: rebuilding one on every open would throw away the state a toggle
    // exists to keep.
    buildFullWindowPages(panes);

    // One Escape action, application scope, because a page's own control may
    // hold focus and the way out must not depend on it. It is disabled on the
    // chat page, so Escape still reaches the transcript and composer when no
    // page is up.
    m_escape = new QAction(this);
    m_escape->setShortcut(QKeySequence(QStringLiteral("Escape")));
    m_escape->setShortcutContext(Qt::ApplicationShortcut);
    m_escape->setEnabled(false);
    addAction(m_escape);
    QObject::connect(m_escape, &QAction::triggered, this, &Shell::escapeCurrentPage);

    for (QWidget *page : m_pageByName)
        m_pages->addWidget(page);

    m_pages->setCurrentWidget(m_pageByName.value(QStringLiteral("chat")));
    m_header->setCurrentPage(m_currentPage);

    m_lightbox = new Lightbox(this);
    m_lightbox->setGeometry(rect());
    m_lightbox->hide();
    QObject::connect(m_conversation, &TranscriptView::openImage, this,
                     &Shell::openImageLightbox);
    QObject::connect(m_conversation, &TranscriptView::openFile, this,
                     [this](const QString &path) {
                         if (m_filesDock == nullptr)
                             return;
                         m_filesDock->setShown(true);
                         m_facade->readFile(path);
                     });
    QObject::connect(m_facade, &ClientFacade::sessionIdChanged, m_lightbox,
                     &Lightbox::closeLightbox);
    if (QObject *transport = m_facade->transport()) {
        QObject *transcript = transport->property("transcript").value<QObject *>();
        if (auto *model = qobject_cast<QAbstractItemModel *>(transcript))
            QObject::connect(model, &QAbstractItemModel::modelReset, m_lightbox,
                             &Lightbox::closeLightbox);
    }
    qInfo().nospace() << "image lightbox: built once, hidden until the conversation asks";

    // A press on a page's toggle: when the page is down it opens it, when it is
    // up it puts it away (minimise). The page's own close button is the other
    // exit and reaches closePage().
    QObject::connect(m_header, &ShellHeader::paneToggleRequested, this,
                     &Shell::togglePage);

    // The mapping the QML kept in six functions and four shortcuts, written down
    // once because the header's press is the only way into it with no pointer.
    qInfo().nospace() << "page toggles: web -> openTrapdoor()/minimiseTrapdoor()"
                         "/closeTrapdoor(); acp -> openAcpPane()/minimiseAcpPane()"
                         "/closeAcpPane(); headless -> openHeadlessPane()"
                         "/minimiseHeadlessPane()/closeHeadlessPane(); Escape is the"
                         " QML's own exit for the page that is up";

    // A probe, not a feature: an offscreen run has no pointer, and the header's
    // handlers are the only way through the state machine, so
    // `DSH_SHELL_PAGES_PROBE=1` presses them in turn — open, minimise, Escape,
    // the pane's own close (which is the exit that unloads the web page), and for
    // the ACP page the live-child and close cases too — then leaves on the chat
    // page. The management page has no header toggle at all, so its steps emit the
    // rail's own two signals by invoking them: that walks the real seam (rail
    // signal -> shell slot), which a direct call to the opener would skip. Its
    // delete is walked as far as the shell's confirmation, and cancelled there —
    // accepting it would mutate the host, and a probe is not allowed to do that.
    // Gated on the environment variable so a normal run never sees it, and one
    // timer so the sequence is the same every time.
    if (qEnvironmentVariableIsSet("DSH_SHELL_PAGES_PROBE")) {
        auto *probe = new QTimer(this);
        probe->setInterval(500);
        auto *step = new int(0);
        // `mutable` because the sequence clears `step` when it ends.
        QObject::connect(probe, &QTimer::timeout, this, [this, probe, step]() mutable {
            const bool acpHidden =
                m_facade->transportName() == QLatin1String("acp");
            switch ((*step)++) {
            case 0: qInfo() << "probe: web toggle (down)"; togglePage(QStringLiteral("web")); break;
            case 1: qInfo() << "probe: web toggle (up)"; togglePage(QStringLiteral("web")); break;
            case 2: qInfo() << "probe: headless toggle (down)"; togglePage(QStringLiteral("headless")); break;
            case 3: qInfo() << "probe: headless toggle (up)"; togglePage(QStringLiteral("headless")); break;
            case 4: qInfo() << "probe: web toggle (down again, for Escape)"; togglePage(QStringLiteral("web")); break;
            case 5: qInfo() << "probe: Escape on the web page"; escapeCurrentPage(); break;
            case 6: qInfo() << "probe: web toggle (down again, for the pane's close)"; togglePage(QStringLiteral("web")); break;
            case 7: qInfo() << "probe: web page close (the pane's own exit)"; closePage(QStringLiteral("web")); break;
            case 8:
                qInfo() << "probe: acp toggle" << (acpHidden ? "(hidden, skipped)" : "(down)");
                if (!acpHidden)
                    togglePage(QStringLiteral("acp"));
                break;
            case 9:
                qInfo() << "probe: acp toggle" << (acpHidden ? "(hidden, skipped)" : "(up)");
                if (!acpHidden)
                    togglePage(QStringLiteral("acp"));
                break;
            case 10:
                qInfo() << "probe: acp toggle" << (acpHidden ? "(hidden, skipped)" : "(down again, live child)");
                if (!acpHidden)
                    togglePage(QStringLiteral("acp"));
                break;
            case 11:
                qInfo() << "probe: Escape on the acp page" << (acpHidden ? "(skipped)" : "(close)");
                if (!acpHidden)
                    escapeCurrentPage();
                break;
            case 12:
                qInfo() << "probe: rail automationRequested() (the rail's own signal, not a"
                           " direct call)";
                if (!QMetaObject::invokeMethod(m_rail, "automationRequested"))
                    qWarning() << "probe: the rail has no automationRequested() signal";
                break;
            case 13:
                qInfo().nospace() << "probe: management up? page " << m_currentPage
                                  << ", tab \"" << m_managementTab << "\", pane visible "
                                  << m_managementPage->isVisible();
                qInfo() << "probe: Escape on the management page";
                escapeCurrentPage();
                break;
            case 14:
                qInfo().nospace() << "probe: after Escape the page is " << m_currentPage
                                  << " and the management tab is \"" << m_managementTab
                                  << "\"";
                qInfo() << "probe: rail pluginsRequested() (the second entry point)";
                if (!QMetaObject::invokeMethod(m_rail, "pluginsRequested"))
                    qWarning() << "probe: the rail has no pluginsRequested() signal";
                break;
            case 15:
                qInfo().nospace() << "probe: management up on the plugins tab? page "
                                  << m_currentPage << ", tab \"" << m_managementTab << "\"";
                qInfo() << "probe: header web toggle while management is up (the page yields)";
                togglePage(QStringLiteral("web"));
                break;
            case 16:
                qInfo().nospace() << "probe: after the web toggle the page is " << m_currentPage
                                  << ", management pane visible "
                                  << m_managementPage->isVisible();
                qInfo() << "probe: Escape on the web page";
                escapeCurrentPage();
                break;
            case 17:
                qInfo() << "probe: rail automationRequested() (down again, for the pane's own close)";
                QMetaObject::invokeMethod(m_rail, "automationRequested");
                break;
            case 18: {
                qInfo() << "probe: management page's own Close (the page's exit)";
                auto *close = m_managementPage->findChild<QWidget *>(
                    QStringLiteral("managementClose"));
                if (close != nullptr) {
                    const QPointF at(close->width() / 2.0, close->height() / 2.0);
                    QMouseEvent press(QEvent::MouseButtonPress, at, close->mapToGlobal(at),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(close, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease, at,
                                        close->mapToGlobal(at), Qt::LeftButton,
                                        Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(close, &release);
                } else {
                    qWarning() << "probe: no managementClose control";
                }
                break;
            }
            case 19: {
                // `window()->focusWidget()` rather than the application's: a
                // widget's focus is not conditional on the window being active,
                // and an offscreen run has no active window. The composer is a
                // container, so the test is whether the focused child is under it.
                QWidget *focus = window()->focusWidget();
                const bool inComposer = focus != nullptr && m_composer->isAncestorOf(focus);
                qInfo().nospace() << "probe: after the management close the page is "
                                  << m_currentPage << ", the tab is \"" << m_managementTab
                                  << "\", focus "
                                  << (focus != nullptr ? focus->objectName()
                                                       : QStringLiteral("none"))
                                  << (inComposer ? " (the composer)" : " (NOT the composer)");
                qInfo() << "probe: the delete's shell half (synthetic id, cancelled below;"
                           " a probe must not mutate the host)";
                deleteManagementTask(QStringLiteral("probe-task"),
                                     QStringLiteral("Probe task"));
                break;
            }
            case 20: {
                auto *dialog = findChild<QMessageBox *>(QStringLiteral("deleteTaskDialog"));
                if (dialog == nullptr) {
                    qWarning() << "probe: the delete confirmation is not on screen;"
                               << "deleteTaskRequested() would have deleted without asking";
                    break;
                }
                qInfo().nospace() << "probe: the shell's confirmation is up: \""
                                  << dialog->windowTitle() << "\" / \""
                                  << dialog->text() << "\"";
                dialog->reject();
                break;
            }
            default:
                probe->stop();
                delete step;
                step = nullptr;
                qInfo() << "probe: done";
                QCoreApplication::quit();
                break;
            }
        });
        probe->start();
    }

    // `DSH_RAIL_PROBE=<dir>`: the two stills the coordinator checks the rail's
    // two widths by, taken from the widget tree itself — no X server, no
    // screenshots of somebody's live window. It is gated, it waits for the
    // host's roster to arrive, and it puts the rail back to 240 px when done.
    // The rail's own fold gesture is used, so the stills are the real shape
    // rather than a forced width.
    if (qEnvironmentVariableIsSet("DSH_RAIL_PROBE")) {
        const QString dir = qEnvironmentVariable("DSH_RAIL_PROBE");
        const QString base = dir.isEmpty() ? QStringLiteral("/tmp") : dir;
        auto *probe = new QTimer(this);
        probe->setSingleShot(true);
        probe->setInterval(2500);
        QObject::connect(probe, &QTimer::timeout, this, [this, base]() {
            const QString expanded = base + QStringLiteral("/rail-240.png");
            if (grab().save(expanded)) {
                qInfo().nospace() << "rail probe: wrote " << expanded << " (rail "
                                  << m_rail->width() << " px, " << m_rail->rowCount()
                                  << " rows)";
            } else {
                qWarning().nospace() << "rail probe: could not write " << expanded;
            }
            toggleRail();
            QTimer::singleShot(250, this, [this, base]() {
                const QString collapsed = base + QStringLiteral("/rail-34.png");
                if (grab().save(collapsed)) {
                    qInfo().nospace() << "rail probe: wrote " << collapsed << " (rail "
                                      << m_rail->width() << " px)";
                } else {
                    qWarning().nospace() << "rail probe: could not write " << collapsed;
                }
                toggleRail();
                // A probe is a throwaway client: quitting beats being killed by
                // the caller's timeout and holding a second mux open meanwhile.
                QTimer::singleShot(200, this, []() { QCoreApplication::quit(); });
            });
        });
        probe->start();
    }

    // `DSH_RAIL_MENU_PROBE=<dir>`: the keyboard path and the row menu, walked
    // without a pointer or a screen. The menu is a `QMenu`, so what can be
    // checked from here is that it IS a top-level popup parented to the shell
    // (which is what makes it unclippable), where it was placed, and what it
    // paints. Whether it reads right over the rail is a person's check on a real
    // window.
    if (qEnvironmentVariableIsSet("DSH_RAIL_MENU_PROBE")) {
        const QString menuDir = qEnvironmentVariable("DSH_RAIL_MENU_PROBE");
        const QString menuBase = menuDir.isEmpty() ? QStringLiteral("/tmp") : menuDir;
        auto *menuProbe = new QTimer(this);
        menuProbe->setSingleShot(true);
        menuProbe->setInterval(2500);
        QObject::connect(menuProbe, &QTimer::timeout, this, [this, menuBase]() {
            m_rail->rowView()->setFocus(Qt::ShortcutFocusReason);
            const auto sendKey = [this](int key) {
                QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::sendEvent(m_rail->rowView(), &event);
            };
            // End first: the last row is the one that matters for placement, and
            // if it is a header the walk upwards finds the nearest session.
            sendKey(Qt::Key_End);
            for (int attempt = 0; attempt < 12 && m_openRailMenu == nullptr; ++attempt) {
                sendKey(Qt::Key_Menu);
                if (m_openRailMenu == nullptr)
                    sendKey(Qt::Key_Up);
            }
            if (m_openRailMenu != nullptr) {
                const QRect global = m_openRailMenu->geometry();
                const QPoint railRight = m_rail->mapToGlobal(QPoint(m_rail->width(), 0));
                qInfo().nospace()
                    << "rail menu probe: row menu is a "
                    << (m_openRailMenu->isWindow() ? "top-level window" : "CHILD widget")
                    << ", parent \"" << m_openRailMenu->parentWidget()->objectName()
                    << "\", geometry " << global.x() << "," << global.y() << " "
                    << global.width() << "x" << global.height()
                    << ", rail right edge " << railRight.x()
                    << " (a popup may cross it; it is not clipped)";
                const QString shot = menuBase + QStringLiteral("/rail-menu.png");
                if (m_openRailMenu->grab().save(shot))
                    qInfo().nospace() << "rail menu probe: wrote " << shot;
                else
                    qWarning().nospace() << "rail menu probe: could not write " << shot;
                m_openRailMenu->hide();
            } else {
                qWarning() << "rail menu probe: the context-menu key opened no row menu";
            }
            QTimer::singleShot(300, this, []() { QCoreApplication::quit(); });
        });
        menuProbe->start();
    }

    // The arithmetic this page is built on, written down once: the coordinator
    // checks it on screen, and a later slice that changes a band has this line to
    // change with it.
    const int docks = m_terminalDockSlot->dockHeight() + m_goalRow->dockHeight()
                      + m_pendingBandSlot->dockHeight() + m_filesDockSlot->height();
    qInfo().nospace() << "shell layout: header " << m_header->height() << "px, content, strip "
                      << StatusStrip::stripHeight() << "px, composer "
                      << m_composer->composerHeight() << "px + docks " << docks
                      << "px (footer " << (m_composer->composerHeight() + docks)
                      << "px); rail " << (m_rail->isHidden() ? 0 : m_rail->width())
                      << "px (QWidget, " << m_rail->rowCount() << " rows)";

    // The dock frame's own arithmetic, slot by slot: the terminal panel (stage 2,
    // live and closed at launch), and two reserved rectangles named for the slice
    // that fills each. Written down here because a zero cannot be seen on screen.
    qInfo().nospace() << "dock stack: " << m_filesDockSlot->objectName() << " "
                      << m_filesDockSlot->height() << "px (stage 4c), "
                      << m_terminalDockSlot->objectName() << " "
                      << m_terminalDockSlot->height() << "px (stage 2, "
                      << (m_terminalDockSlot->available() ? "terminal" : "no terminal")
                      << (m_terminalDockSlot->shown() ? ", open" : ", closed") << "), "
                      << m_goalRow->objectName() << " " << m_goalRow->dockHeight()
                      << "px (goal), " << m_pendingBandSlot->objectName() << " "
                      << m_pendingBandSlot->dockHeight() << "px (pending); inset "
                      << (m_rail->isHidden() ? 0 : m_rail->width())
                      << "px for the rail";

    restoreWindowGeometry();
    m_geometryRestored = true;
}

QWidget *Shell::buildChatPage()
{
    Theme *theme = Theme::instance();

    auto *chat = new QWidget(m_pages);
    chat->setObjectName(QStringLiteral("chatPage"));
    // The page is the transcript's surface, not the chrome's: `bg` is the darkest
    // band and every band above it is a step lighter (Theme.h's surface ladder).
    chat->setAutoFillBackground(true);
    QPalette chatPalette = chat->palette();
    chatPalette.setColor(QPalette::Window, theme->bg());
    chat->setPalette(chatPalette);

    auto *chatLayout = new QVBoxLayout(chat);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    // ---- the content row: rail | conversation ------------------------------
    auto *content = new QWidget(chat);
    content->setObjectName(QStringLiteral("chatContent"));
    auto *contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    // ---- the rail (stage 3b): a real QWidget over the C++ rows model ------
    //
    // The rail no longer reads `dshTransport` by name: every fact it draws is a
    // typed read of the facade, and every gesture comes back as a signal this
    // shell answers. The rectangle is still the shell's — the splitter and the
    // 140 ms transition live in toggleRail() — because the dock stack's inset
    // and the transcript's margin are this page's layout, not the rail's.
    //
    // The two marks the delegate draws are the icon set's own SVGs
    // (qml/icons/pin.svg and schedule.svg), so the rail is handed that
    // directory. It comes from the conversation pane's path: both panes are
    // installed into one directory, and this shell is given pane FILES rather
    // than the QML directory.
    m_rail = new SessionRail(m_facade, iconDirectory(), content);
    m_rail->setFixedWidth(SessionRail::expandedWidth());
    // ACP publishes no roster, so the QML rail hides itself there
    // (`visible: supported`); a 240 px empty column would be a bug the rail
    // cannot fix from inside a fixed rectangle, so the shell does not give it
    // one. The facade's capability flag is the same test, without the
    // typeof-on-a-compat-empty false positive.
    m_rail->setVisible(m_facade->hasSessionList());
    contentLayout->addWidget(m_rail);

    // The conversation column, to the right of the rail: the strip at its top,
    // the transcript under it. The composer is NOT in here - it spans the whole
    // window, under the rail as well, exactly as `main.qml` lays it out.
    auto *rightColumn = new QWidget(content);
    rightColumn->setObjectName(QStringLiteral("conversationColumn"));
    auto *rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_strip = new StatusStrip(m_facade, rightColumn);
    rightLayout->addWidget(m_strip);

    m_filesDock = new FilesDock(m_facade, content);
    m_filesDock->setObjectName(QStringLiteral("filesDock"));
    contentLayout->addWidget(rightColumn, 1);
    contentLayout->addWidget(m_filesDock);

    m_conversation = new TranscriptView(m_facade, rightColumn);
    m_conversation->setObjectName(QStringLiteral("transcriptView"));
    m_conversationStack = new QStackedWidget(rightColumn);
    m_conversationStack->setObjectName(QStringLiteral("conversationStack"));
    m_conversationStack->addWidget(m_conversation);
    m_trajectory = new TrajectoryPage(m_facade, rightColumn);
    m_trajectory->setObjectName(QStringLiteral("trajectoryPage"));
    m_conversationStack->addWidget(m_trajectory);
    m_conversationStack->setCurrentWidget(m_conversation);
    QObject::connect(m_facade, &ClientFacade::sessionIdChanged, this, [this]() {
        if (m_trajectory != nullptr)
            m_trajectory->setSession(m_facade->sessionId());
    });
    rightLayout->addWidget(m_conversationStack, 1);

    // ---- the conversation's bar --------------------------------------------
    //
    // Between the rail's edge and the transcript, at the TOP of the conversation
    // column - which is where `main.qml`'s `statusBar` sits, directly under the
    // header. It is the conversation's, not the composer's: the model, the
    // context count and the session id are facts about what is being read, and
    // they were never at the foot of the window.
    chatLayout->addWidget(content, 1);

    // ---- the footer: the dock stack, then the composer ----------------------
    //
    // `main.qml`'s `footer:` band, in its own order: the docks grow upward and the
    // composer keeps the bottom edge of the window. The composer is the full width
    // of the window, under the rail as well (main.qml:944 has no rail inset); the
    // docks are not — the pending band and the terminal are inset by the rail's
    // width (main.qml:4682, 4705), and in the QML by the files dock's width on the
    // right too. That asymmetry is the QML's, and it is kept so the slices that
    // fill these slots inherit the geometry they expect.
    auto *footer = new FooterBar(chat);
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(0);

    // TerminalPanel.qml:27 is `implicitHeight: shown ? expandedHeight : 0`;
    // PendingBand.qml:66 is `height: visible ? implicitHeight : 0`; the files dock
    // is a width (`WorkspaceFilesPanel.qml:28`), so its height is always zero.
    //
    // The order is main.qml's own, top to bottom: the terminal bottoms on the
    // pending band's top (`anchors.bottom: pendingBand.top`, main.qml:4715) and the
    // pending band bottoms on the content item (main.qml:4687), so the band is the
    // surface directly above the composer and the terminal is the dock above it.
    // Plan §2.4 states the same placement.
    auto *dockStack = new DockPanel(rightColumn);
    dockStack->setObjectName(QStringLiteral("dockStack"));
    auto *dockLayout = new QVBoxLayout(dockStack);
    // `isHidden()` rather than `isVisible()`: the page is not shown yet while it is
    // being built, so `isVisible()` is false for every child here and would give the
    // docks no inset even when the rail exists. The rail is hidden exactly when the
    // transport has no roster (`hasSessionList()`, AcpClient's empty compat list).
    // The margin is the rail's width, and toggleRail() animates it with the rail:
    // the QML insets the docks by the rail's width and lets the composer span the
    // window, so the two numbers move together or the dock slides under the rail.
    m_dockLayout = dockLayout;
    dockLayout->setContentsMargins(0, 0, 0, 0);
    dockLayout->setSpacing(0);

    // Stage 4c fills this one: the workspace files dock. It is a full-height
    // RIGHT-hand dock in the QML, so it has no height in this stack at all; the
    // slot records the name and the zero its slice starts from, and stage 4c takes
    // its width from the content row (making the right inset above its width).
    m_filesDockSlot = new QWidget(dockStack);
    m_filesDockSlot->setObjectName(QStringLiteral("filesDock"));
    m_filesDockSlot->setFixedHeight(0);
    dockLayout->addWidget(m_filesDockSlot);

    // Stage 2 fills this one: the terminal dock, `TerminalPanel.qml` ported to a
    // QWidget. It takes the transport the emulator binds (the facade's own
    // object), because a QWidget has no QML context to read `dshTransport` from.
    // The panel sizes itself from `shown ? expandedHeight : 0`; the shell's
    // footer reads that same contract rather than measuring the widget.
    m_terminalDockSlot = new TerminalPanel(m_facade->transport(), dockStack);
    m_terminalDockSlot->setObjectName(QStringLiteral("terminalDock"));
    dockLayout->addWidget(m_terminalDockSlot);

    m_goalRow = new GoalRow(m_facade, dockStack);
    m_goalRow->setObjectName(QStringLiteral("goalRow"));
    dockLayout->addWidget(m_goalRow);
    QObject::connect(m_goalRow, &GoalRow::editRequested, this, &Shell::showGoalEditDialog);

    m_pendingBandSlot = new PendingBand(dockStack);
    m_pendingBandSlot->setObjectName(QStringLiteral("pendingBand"));
    dockLayout->addWidget(m_pendingBandSlot);

    QObject::connect(m_facade, &ClientFacade::permissionRequested, m_pendingBandSlot,
                     &PendingBand::showApproval);
    QObject::connect(m_facade, &ClientFacade::questionsRequested, m_pendingBandSlot,
                     &PendingBand::showQuestions);
    QObject::connect(m_facade, &ClientFacade::permissionWithdrawn, m_pendingBandSlot,
                     [band = m_pendingBandSlot](const QString &requestId) {
                         band->dismissApproval(requestId);
                         band->dismissQuestions(requestId);
                     });
    QObject::connect(m_pendingBandSlot, &PendingBand::approvalAnswered, m_facade,
                     &ClientFacade::respondToPermission);
    QObject::connect(m_pendingBandSlot, &PendingBand::questionsAnswered, m_facade,
                     &ClientFacade::answerQuestions);

    rightLayout->addWidget(dockStack);

    // The composer itself: the field, its trailing controls and the rules that
    // turn a keystroke into a prompt, all in one widget (Composer.h).
    m_composer = new Composer(m_facade, footer);
    footerLayout->addWidget(m_composer);
    chatLayout->addWidget(footer);

    // The footer's height is arithmetic over its parts, the same choice
    // main.qml:1049 makes for the dock above its composer: composer + the docks.
    // The composer's height is its own contract (Composer::composerHeight), and
    // the terminal's is `shown ? expandedHeight : 0` (TerminalPanel::dockHeight)
    // — no child is measured in the direction the footer lays it out, so there is
    // no layout circle. Each dock changes its own fixed height and this line adds
    // it, so a dock row cannot slip under the composer.
    auto relayoutFooter = [this, footer]() {
        const int terminal = m_terminalDockSlot->dockHeight();
        const int goal = m_goalRow->dockHeight();
        const int pending = m_pendingBandSlot->dockHeight();
        const int files = m_filesDockSlot->height();
        const int docks = terminal + goal + pending + files;
        const int composer = m_composer->composerHeight();
        footer->setFixedHeight(composer);
        qInfo().nospace() << "shell footer: " << footer->height() << "px = composer "
                          << composer << "; docks " << docks << " in the column (terminal "
                          << terminal << (m_terminalDockSlot->shown() ? " open," : " closed,")
                          << " goal " << goal << ", pending " << pending << ", files " << files
                          << ")";
    };

    // The footer grows and shrinks with the field, and with the docks above it;
    // the page gives the transcript exactly the room that is left.
    QObject::connect(m_composer, &Composer::heightChanged, footer, relayoutFooter);
    QObject::connect(m_terminalDockSlot, &TerminalPanel::dockHeightChanged, footer,
                     relayoutFooter);
    QObject::connect(m_goalRow, &GoalRow::dockHeightChanged, footer, relayoutFooter);
    QObject::connect(m_pendingBandSlot, &PendingBand::dockHeightChanged, footer,
                     relayoutFooter);

    // The rail's terminal toggle is the one way in and out of this dock (the QML
    // has no second control either), and the overflow menu's Terminal entry is
    // the same verb: one member function, so the two entries cannot disagree
    // about what "open" means. The dock's own state is written back to the rail
    // so the button cannot keep a second copy of it.
    m_rail->setTerminalSupported(m_terminalDockSlot->available());
    QObject::connect(m_rail, &SessionRail::terminalToggleRequested, this,
                     &Shell::toggleTerminalDock);

    // ---- everything else the rail asks for ---------------------------------
    //
    // The rail names the gesture and the shell performs it, which is the same
    // seam the QML rail used (`sidebar.<signal>` -> main.qml's Connections).
    // The transport actions go by name rather than through the facade because
    // the facade deliberately carries only the chrome's subset (plan §3.3): the
    // rail's own verbs are pane-surface verbs, not chrome values.

    // The fold: the shell owns the splitter, the rail owns the shape it draws.
    QObject::connect(m_rail, &SessionRail::foldToggleRequested, this, &Shell::toggleRail);

    // The two management entries name a TAB, not a page: there is one page in
    // the stack, and the tab is its `page` input. Two entry points because the
    // rail has two menu entries; one opener because the page, its exclusivity
    // against the three panes and its exit are one piece of state.
    QObject::connect(m_rail, &SessionRail::automationRequested, this, [this]() {
        openManagementPage(QStringLiteral("automation"));
    });
    QObject::connect(m_rail, &SessionRail::pluginsRequested, this, [this]() {
        openManagementPage(QStringLiteral("plugins"));
    });

    QObject::connect(m_rail, &SessionRail::workspaceNewRequested, this,
                     [this]() { callRailTransport("createWorkspaceFromPicker"); });
    QObject::connect(m_rail, &SessionRail::sessionNewRequested, this, [this]() {
        callRailTransport("createSession");
        m_composer->focusEntry();
    });
    QObject::connect(m_rail, &SessionRail::sessionNewInWorkspaceRequested, this,
                     [this](const QString &workspaceId) {
                         callRailTransport("createSessionInWorkspace", workspaceId);
                         m_composer->focusEntry();
                     });

    // A row: the plain address for a top-level session, the tagged subagent
    // address for a child — the host refuses a bare child id with
    // `session/agent-busy`, which is why the parent id travels with it.
    QObject::connect(m_rail, &SessionRail::sessionActivated, this,
                     [this](const QString &sessionId, const QString &parentId,
                            bool child) {
                         if (child && !parentId.isEmpty()) {
                             qInfo().nospace() << "session rail: open subagent " << sessionId
                                               << " under " << parentId;
                             m_facade->followSubagent(parentId, sessionId);
                         } else {
                             qInfo().nospace() << "session rail: open session " << sessionId;
                             m_facade->followSession(sessionId);
                         }
                         m_composer->focusEntry();
                     });

    QObject::connect(m_rail, &SessionRail::renameRequested, this,
                     &Shell::renameRailSession);
    QObject::connect(m_rail, &SessionRail::copyIdRequested, this,
                     [this](const QString &sessionId) {
                         QGuiApplication::clipboard()->setText(sessionId);
                         qInfo().nospace() << "session rail: copied session id " << sessionId;
                     });
    QObject::connect(m_rail, &SessionRail::forkRequested, this,
                     [this](const QString &sessionId) {
                         callRailTransport("forkSession", sessionId);
                     });
    QObject::connect(m_rail, &SessionRail::pinRequested, this,
                     [this](const QString &sessionId, bool pinned) {
                         // `pinned` is the state the row was in, so the verb is
                         // the other one: the menu says "Unpin" on a pinned row.
                         callRailTransport(pinned ? "unpinSession" : "pinSession", sessionId);
                     });
    QObject::connect(m_rail, &SessionRail::archiveRequested, this,
                     [this](const QString &sessionId, bool archived) {
                         callRailTransport(archived ? "unarchiveSession" : "archiveSession",
                                           sessionId);
                     });
    QObject::connect(m_rail, &SessionRail::refreshRequested, this,
                     [this]() { callRailTransport("refreshSessions"); });
    QObject::connect(m_rail, &SessionRail::restartRequested, this,
                     [this]() { callRailTransport("restartSelf"); });
    QObject::connect(m_rail, &SessionRail::settingsRequested, this, [this]() {
        // The settings dialog is stage 5's; saying so is better than a menu entry
        // that opens nothing. The overflow menu draws the entry disabled.
        qInfo() << "session rail: settingsRequested(); the Widgets shell has no"
                   " settings dialog yet (stage 5)";
    });
    QObject::connect(m_rail, &SessionRail::overflowMenuRequested, this,
                     &Shell::openRailOverflowMenu);
    QObject::connect(m_rail, &SessionRail::rowMenuRequested, this,
                     &Shell::openRailRowMenu);
    QObject::connect(m_rail, &SessionRail::rowMenuDismissRequested, this, [this]() {
        if (m_openRailMenu != nullptr)
            m_openRailMenu->hide();
    });

    relayoutFooter();

    return chat;
}

void Shell::openImageLightbox(const QVariant &source, const QVariant &alt,
                              qreal fallbackWidth, qreal fallbackHeight)
{
    qInfo().nospace() << "shell: opening the image lightbox for " << source.toString()
                      << " alt=\"" << alt.toString() << "\" fallback="
                      << fallbackWidth << "x" << fallbackHeight;
    if (m_lightbox != nullptr)
        m_lightbox->showImage(source, alt, fallbackWidth, fallbackHeight);
}

void Shell::buildFullWindowPages(const ShellPanes &panes)
{
    // ---- the web trapdoor ---------------------------------------------------
    //
    // The pane is a QWidget now: Servo's own GL surface paints into the page's
    // frame, so there is no QQuickWidget and no `servoTarget` to parent a view
    // into. The page owns its own url/available/status and its own exits.
    m_webPage = new WebPage(m_pages);
    m_webPage->setObjectName(QStringLiteral("webPage"));
    m_webPage->setUrl(panes.webUrl);
    m_webPage->hide();
    m_pageByName.insert(QStringLiteral("web"), m_webPage);

    QObject::connect(m_webPage, &WebPage::closeRequested, this,
                     [this]() { closePage(QStringLiteral("web")); });
    QObject::connect(m_webPage, &WebPage::minimiseRequested, this,
                     [this]() { minimisePage(QStringLiteral("web")); });
    QObject::connect(m_webPage, &WebPage::browserRequested, this, [this]() {
        qInfo().nospace() << "web page: browserRequested() -> opening "
                          << m_webUrl.toString() << " in the system browser";
        QDesktopServices::openUrl(m_webUrl);
    });
    qInfo().nospace() << "web page: built (url " << panes.webUrl.toString() << ")";

    // ---- the ACP pane -------------------------------------------------------
    //
    // `AcpPane.qml` takes the pane client and its derived state as inputs rather
    // than reading the context property, because the QML shell binds them from
    // its own guarded reads. The same values are written here before setSource;
    // the client's seven change signals keep them current afterwards, because a
    // QML `var` cannot follow a C++ pointer it was not handed.
    AcpClient *acp = panes.acpClient;
    m_acpPage = new AcpPage(acp, iconDirectory(), m_pages);
    m_acpPage->setObjectName(QStringLiteral("acpPage"));
    m_acpPage->hide();
    m_pageByName.insert(QStringLiteral("acp"), m_acpPage);

    QObject::connect(m_acpPage, &AcpPage::closeRequested, this,
                     [this]() { closePage(QStringLiteral("acp")); });

    QObject::connect(m_acpPage, &AcpPage::acpSessionSelected, this,
                     [this](const QString &sessionId) {
        if (m_acpClient == nullptr || sessionId.isEmpty())
            return;
        m_acpClient->switchToSession(sessionId);
        m_acpPage->pinToTail();
        m_acpPage->focusPage();
    });
    QObject::connect(m_acpPage, &AcpPage::acpSessionCloseRequested, this,
                     [this](const QString &sessionId) {
        if (m_acpClient == nullptr || sessionId.isEmpty())
            return;
        m_acpClient->closeSession(sessionId);
        m_acpPage->focusPage();
    });
    QObject::connect(m_acpPage, &AcpPage::acpNewSessionRequested, this, [this]() {
        if (m_acpClient == nullptr || m_acpClient->state() != AcpClient::Ready)
            return;
        m_acpClient->createSession();
        m_acpPage->focusPage();
    });

    if (m_acpClient == nullptr)
        qWarning() << "acp page: no dshAcpPane client was handed to the shell;"
                   << "the pane will read as idle";

    m_headlessPage = new HeadlessPage(panes.headlessClient, iconDirectory(), m_pages);
    m_headlessPage->hide();
    m_pageByName.insert(QStringLiteral("headless"), m_headlessPage);

    QObject::connect(m_headlessPage, &HeadlessPage::dismissRequested, this, [this]() {
        minimisePage(QStringLiteral("headless"));
    });
    QObject::connect(m_headlessPage, &HeadlessPage::shutRequested, this, [this]() {
        closePage(QStringLiteral("headless"));
    });

    // ---- the management pages ----------------------------------------------
    //
    // One page in the stack and two tabs: the two rail entries are two entry points
    // into one page, not two pages, and the shell keeps one name ("management") for
    // what is on screen.
    m_managementPage = new ManagementPage(m_facade, m_pages);
    m_managementPage->setObjectName(QStringLiteral("managementPage"));
    m_managementPage->hide();
    m_pageByName.insert(QStringLiteral("management"), m_managementPage);

    QObject::connect(m_managementPage, &ManagementPage::closeRequested, this,
                     [this]() { closePage(QStringLiteral("management")); });

    qInfo().nospace() << "management page: built; the rail's automationRequested() and"
                         " pluginsRequested() open this one page on its two tabs, Escape and"
                         " its Close return to chat, and its Delete is a shell confirmation"
                         " followed by the transport's deleteScheduleTask";
}

void Shell::showPage(const QString &page)
{
    QWidget *target = m_pageByName.value(page);
    if (target == nullptr) {
        // An unknown page name is a request the shell cannot answer; falling back
        // to the chat page silently would hide a wiring mistake in the caller.
        qWarning() << "shell: no page named" << page;
        return;
    }
    if (target == m_pages->currentWidget())
        return;

    m_pages->setCurrentWidget(target);
    m_currentPage = page;
    m_header->setCurrentPage(page);

    // The QML's four Escape shortcuts are enabled one at a time, each guarded on
    // its own pane being open. One page up means one enable: a page is up.
    if (m_escape != nullptr)
        m_escape->setEnabled(page != QLatin1String("chat"));

    qInfo().nospace() << "shell page: " << page;
}

// ---- the full-window pages' state machine ----------------------------------

void Shell::togglePage(const QString &page)
{
    // The header's semantics, press for press: on the page that is up the toggle
    // PUTS IT AWAY (minimise), and on any other page it opens that page. The two
    // are deliberately different exits in every pane; the header is the first.
    if (page == m_currentPage) {
        qInfo().nospace() << "header toggle " << page << ": page is up -> minimise";
        minimisePage(page);
    } else {
        qInfo().nospace() << "header toggle " << page << ": page is down -> open";
        openPage(page);
    }
}

void Shell::openPage(const QString &page)
{
    // The ACP toggle is not a control at all when the process already speaks ACP
    // (main.qml:851: `visible: dshTransport.transportName !== "acp"`); the header
    // hides it, and this is the guard behind the hidden button.
    if (page == QLatin1String("acp") && m_facade->transportName() == QLatin1String("acp")) {
        qWarning() << "acp page: the ACP toggle is hidden when the transport is ACP;"
                   << "not opening";
        return;
    }

    const QString was = m_currentPage;

    // One page at a time, whatever is being opened. The management page is the
    // fourth page and has no header toggle of its own, so a header press is the
    // only way it can be replaced from here. It yields by its own exit — chat,
    // then this page — because main.qml merely COVERED it with the trapdoor and
    // this stack has no "covered": one page is on screen and the other is not.
    if (was == QLatin1String("management"))
        closePage(QStringLiteral("management"));

    if (page == QLatin1String("web")) {
        // main.qml's web toggle (main.qml:818): an ACP pane that is UP is closed
        // first, and its child stopped — "not now", not "put away"; openTrapdoor()
        // then minimises the headless pane. Only one page can be up in this stack,
        // so `was` names the one that has to yield.
        if (was == QLatin1String("acp"))
            closePage(QStringLiteral("acp"));
        else if (was == QLatin1String("headless"))
            minimisePage(QStringLiteral("headless"));
        showPage(QStringLiteral("web"));
        // The page is the one surface whose content is a native view rather than
        // a control that would take focus itself, so it takes the keyboard when
        // it opens.
        m_webPage->focusPage();
        qInfo().nospace() << "web page: openTrapdoor() -> up; the page is kept across a"
                             " minimise (its state and its Servo view are not torn down)";
    } else if (page == QLatin1String("acp")) {
        // openAcpPane(): the web page is minimised (not closed), the headless pane
        // is minimised, and the child is started only when it is not live. A live
        // child with no sessions gets one, because an empty bar is not a
        // conversation; a live child with sessions is resumed as it stands.
        if (was == QLatin1String("web"))
            minimisePage(QStringLiteral("web"));
        else if (was == QLatin1String("headless"))
            minimisePage(QStringLiteral("headless"));
        showPage(QStringLiteral("acp"));

        if (m_acpClient == nullptr) {
            qWarning() << "acp page: no client to start; the pane stays idle";
            return;
        }
        const AcpClient::State state = m_acpClient->state();
        const bool live = state == AcpClient::Starting || state == AcpClient::Ready;
        if (!live) {
            m_acpClient->restart();
            qInfo().nospace() << "acp page: openAcpPane() -> restart() (state " << int(state)
                              << "); a fresh child and a fresh conversation";
        } else if (state == AcpClient::Ready && m_acpClient->openSessions().isEmpty()) {
            m_acpClient->createSession();
            qInfo().nospace() << "acp page: openAcpPane() -> live child with no sessions;"
                                 " createSession()";
        } else {
            qInfo().nospace() << "acp page: openAcpPane() -> live child kept (state "
                              << int(state) << ", " << m_acpClient->openSessions().size()
                              << " sessions); reofferCurrentPermission()";
        }
        m_acpClient->reofferCurrentPermission();
        m_acpPage->pinToTail();
        m_acpPage->focusPage();
    } else if (page == QLatin1String("headless")) {
        // openHeadlessPane(): the trapdoor is MINIMISED (its page is the kind of
        // state a toggle preserves) and an up ACP pane is closed, its child
        // stopped. A management page was already closed above, which is what
        // main.qml's openHeadlessPane() does in its own third clause.
        if (was == QLatin1String("web"))
            minimisePage(QStringLiteral("web"));
        else if (was == QLatin1String("acp"))
            closePage(QStringLiteral("acp"));
        showPage(QStringLiteral("headless"));
        qInfo().nospace() << "headless page: openHeadlessPane() -> up; a task can be sent"
                             " (the child starts on the first send, as in the QML)";
    } else {
        qWarning() << "shell: no full-window page named" << page;
    }
}

void Shell::minimisePage(const QString &page)
{
    if (page != m_currentPage) {
        qWarning() << "shell: minimise requested for" << page << "but" << m_currentPage
                   << "is the page on screen";
        return;
    }

    // Hide first, then touch the child: the QML's minimise functions set their
    // open flag before they stop anything, so the pane has already stopped
    // painting by the time the process behind it is asked to go.
    showPage(QStringLiteral("chat"));

    if (page == QLatin1String("web")) {
        qInfo().nospace() << "web page: minimiseTrapdoor() -> chat; the page and its state"
                             " are kept, nothing is unloaded";
    } else if (page == QLatin1String("acp")) {
        // No permission dialog exists in this slice; when it does, it closes with
        // the pane (minimiseAcpPane), because a modal cannot outlive its surface.
        qInfo().nospace() << "acp page: minimiseAcpPane() -> chat; the child is kept"
                             " (state "
                          << (m_acpClient != nullptr ? int(m_acpClient->state()) : 0) << ", "
                          << (m_acpClient != nullptr ? m_acpClient->openSessions().size() : 0)
                          << " sessions) so the next opening returns to them";
    } else if (page == QLatin1String("headless")) {
        if (m_headlessClient != nullptr) {
            const QString session = m_headlessClient->sessionId();
            m_headlessClient->hibernate();
            qInfo().nospace() << "headless page: minimiseHeadlessPane() -> chat;"
                                 " hibernate() stopped the child and the conversation is"
                                 " kept (session \""
                              << session << "\")";
        } else {
            qWarning() << "headless page: no child to hibernate";
        }
    }

    focusChatComposer();
}

void Shell::closePage(const QString &page)
{
    if (page != m_currentPage) {
        qWarning() << "shell: close requested for" << page << "but" << m_currentPage
                   << "is the page on screen";
        return;
    }

    // Close hides first too: main.qml's closeTrapdoor() emits `shutRequested`
    // after the pane is hidden, so the item has stopped its pump before the
    // handle it points at goes away.
    showPage(QStringLiteral("chat"));

    if (page == QLatin1String("web")) {
        // closeTrapdoor()'s `webPane.shutRequested()` is main.cpp's connection to
        // ServoView::unload — the exit that frees the page. The emit is the whole
        // of the shell's part: the page is already hidden, so its pump has
        // stopped, and whether a native view is there to free is the seam's
        // question. A build without Servo leaves the signal unconnected, exactly
        // as the QML shell does, and that is why close and minimise are not the
        // same verb.
        // closeTrapdoor()'s `webPane.shutRequested()` is main.cpp's connection to
        // ServoView::unload — the exit that frees the page, its FBO and its GL
        // context. It is safe again because the shim holds ONE Servo per process
        // (`webview/src/lib.rs`'s `SERVO`): destroying a handle no longer drops
        // the engine, so a later open does not build a second one and abort on
        // `Opts: Already initialized`.
        m_webPage->unload();
        qInfo().nospace() << "web page: closeTrapdoor() -> chat; the page unloaded"
                             " (the engine itself is process-wide and outlives the handle)";
    } else if (page == QLatin1String("acp")) {
        if (m_acpPage != nullptr)
            m_acpPage->shut();
        if (m_acpClient != nullptr) {
            m_acpClient->shutdown();
            qInfo().nospace() << "acp page: closeAcpPane() -> chat; shutdown() stopped the"
                                 " child and forgot every session on it";
        }
    } else if (page == QLatin1String("headless")) {
        if (m_headlessClient != nullptr) {
            m_headlessClient->reset();
            qInfo().nospace() << "headless page: closeHeadlessPane() -> chat; reset()"
                                 " stopped the child and forgot the conversation";
        }
    } else if (page == QLatin1String("management")) {
        // The pane's Close and Escape share this verb, and it is the whole of
        // main.qml's closeManagementPage(): the tab input goes back to "nothing in
        // front" and the chat page comes back. Nothing is unloaded — the
        // catalogues are the transport's, and the Delete was already the shell's
        // confirmed action, not a child process to stop.
        m_managementPage->setTab(QString());
        m_managementTab.clear();
        qInfo().nospace() << "management page: closeManagementPage() -> chat; the tab input"
                             " is empty again and the composer has the keyboard";
    }

    focusChatComposer();
}

void Shell::showSettingsDialog()
{
    if (m_settingsDialog == nullptr) {
        m_settingsDialog = new SettingsDialog(m_facade, this);
        m_settingsDialog->setObjectName(QStringLiteral("settingsDialog"));
    }
    m_settingsDialog->showDialog();
}

void Shell::showChangesDialog()
{
    if (m_changesDialog == nullptr) {
        m_changesDialog = new ChangesDialog(m_facade, this);
        m_changesDialog->setObjectName(QStringLiteral("changesDialog"));
        QObject::connect(m_changesDialog, &ChangesDialog::fileRequested, this,
                         [this](const QString &path) {
                             if (m_filesDock == nullptr)
                                 return;
                             m_filesDock->setShown(true);
                             m_facade->readFile(path);
                         });
    }
    m_changesDialog->showDialog();
}

void Shell::showGoalEditDialog(const QString &objective, int maxGoalRounds)
{
    if (m_goalEditDialog == nullptr) {
        m_goalEditDialog = new GoalEditDialog(this);
        m_goalEditDialog->setObjectName(QStringLiteral("goalEditDialog"));
        QObject::connect(m_goalEditDialog, &GoalEditDialog::saveRequested, m_facade,
                         &ClientFacade::editGoal);
    }
    m_goalEditDialog->showDialog(objective, maxGoalRounds);
}

void Shell::openManagementPage(const QString &tab)
{
    const bool automation = tab == QLatin1String("automation");
    if (!automation && tab != QLatin1String("plugins")) {
        // The rail has two entries and names two tabs. An unknown name is a
        // wiring mistake, not a page, and opening "nothing" would show a page
        // whose bar and body disagree.
        qWarning() << "management page: no tab named" << tab;
        return;
    }
    if (m_managementPage == nullptr) {
        qWarning() << "management page: no pane to open; the shell was built without one";
        return;
    }

    // main.qml's openManagementPage(name) touches no other surface; the
    // exclusivity lives in the other openers there (openAcpPane and
    // openHeadlessPane close a management page). Here the page is opened over
    // whatever is up, so it yields by the verbs the other openers use against
    // each other, and each verb keeps what its own exit keeps: the web page is a
    // state worth returning to, the headless conversation is worth resuming, and
    // the ACP child is the explicit stop — openHeadlessPane()'s rule, not a
    // minimise, because a page that borrows the window does not need a hidden
    // child behind it.
    const QString was = m_currentPage;
    if (was == QLatin1String("web"))
        minimisePage(QStringLiteral("web"));
    else if (was == QLatin1String("acp"))
        closePage(QStringLiteral("acp"));
    else if (was == QLatin1String("headless"))
        minimisePage(QStringLiteral("headless"));

    const bool switching = m_currentPage == QLatin1String("management");
    // The tab first, the page second: the pane's two halves key off this input, so
    // setting it after the stack switch would paint the old tab for one frame.
    m_managementPage->setTab(tab);
    m_managementTab = tab;
    showPage(QStringLiteral("management"));

    // One refresh per opening, exactly as main.qml's opener does it once before it
    // sets its state. The host's forwarded events keep the catalogue current from
    // then on (`schedule/changed`, `plugin-manager/changed`).

    qInfo().nospace() << "management page: " << tab << " tab up (was "
                      << (switching ? QStringLiteral("management, tab switched") : was)
                      << "); Escape and the page's Close both return to chat";
}

void Shell::deleteManagementTask(const QString &taskId, const QString &taskTitle)
{
    // A single press on a list row must never delete: the confirmation is a
    // `QMessageBox` shown with `open()` rather than `exec()`, so it is
    // window-modal with no nested event loop, and the deletion is issued from its
    // `finished`.
    qInfo().nospace() << "management page: deleteTaskRequested(" << taskId << ", \""
                      << taskTitle << "\") -> the shell's confirmation";

    auto *dialog = new QMessageBox(this);
    dialog->setObjectName(QStringLiteral("deleteTaskDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setIcon(QMessageBox::Warning);
    dialog->setWindowTitle(QStringLiteral("Delete this task?"));
    // The QML shows the task's title as the body and falls back to "This task"
    // when the row had none.
    dialog->setText(taskTitle.isEmpty() ? QStringLiteral("This task") : taskTitle);
    dialog->setInformativeText(QStringLiteral(
        "The task and its saved delivery records are removed. Messages it already "
        "queued are left alone."));
    QAbstractButton *deleteButton =
        dialog->addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    QAbstractButton *cancelButton =
        dialog->addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    // Cancel is the default and the Escape answer: a QMessageBox would otherwise
    // make its first button the default, and Return on a confirmation must not be
    // the destructive answer. The QML's dialog has no default at all; this is the
    // same restraint, stated for a control that does have one.
    dialog->setDefaultButton(qobject_cast<QPushButton *>(cancelButton));
    dialog->setEscapeButton(cancelButton);

    QObject::connect(dialog, &QMessageBox::finished, this,
                     [this, dialog, deleteButton, taskId]() {
        // `finished` arrives before the delete-on-close, so the dialog and its
        // buttons are still readable here. Cancel is the only answer that leaves
        // the host untouched, and it must say so in the log: a refused deletion
        // and a completed one look the same from the catalogue a moment later.
        if (dialog->clickedButton() != deleteButton) {
            qInfo().nospace() << "management page: delete of " << taskId
                              << " cancelled; nothing was sent";
            return;
        }
        QObject *transport = m_facade->transport();
        if (transport == nullptr) {
            qWarning() << "management page: no transport to delete" << taskId << "through";
            return;
        }
        if (QMetaObject::invokeMethod(transport, "deleteScheduleTask", Q_ARG(QString, taskId)))
            qInfo().nospace() << "management page: deleteScheduleTask(" << taskId
                              << ") on transport " << m_facade->transportName();
        else
            qWarning().nospace() << "management page: transport " << m_facade->transportName()
                                 << " has no deleteScheduleTask(); " << taskId
                                 << " was not deleted";
    });

    dialog->open();
}

// ---- the rail's rectangle and its menus ------------------------------------

void Shell::toggleTerminalDock()
{
    if (!m_terminalDockSlot->available()) {
        // The QML's rail uses a typeof test that AcpCompat's no-op satisfies
        // (plan §3.3 records the false positive), so the button can exist on a
        // transport with no terminal. The dock does not: the request is refused
        // here, visibly, and the button stays unlit.
        qInfo().nospace() << "terminal dock: refused; transport "
                          << m_facade->transportName()
                          << " has no terminal surface (the dock stays closed)";
        return;
    }
    const bool open = !m_terminalDockSlot->shown();
    m_terminalDockSlot->setShown(open);
    m_rail->setTerminalActive(open);
    // The QML hands the keyboard back to the composer when the dock folds
    // (main.qml:2273-2275); the panel takes it when the dock unfolds.
    if (!open)
        m_composer->focusEntry();
    qInfo().nospace() << "terminal dock: " << (open ? "open" : "closed")
                      << " (shown " << m_terminalDockSlot->shown() << ", rail"
                      << " terminalActive " << open << ")";
}

void Shell::toggleRail()
{
    if (m_rail == nullptr || m_rail->isHidden())
        return;
    // A row menu is anchored to a rectangle that is about to disappear: the rows
    // go behind the collapsed strip, and the popup would hang over the
    // transcript. The popup is the shell's, so the shell closes it.
    if (m_openRailMenu != nullptr)
        m_openRailMenu->hide();

    m_railExpanded = !m_railExpanded;
    const int target = m_railExpanded ? SessionRail::expandedWidth()
                                      : SessionRail::collapsedWidth();
    // The rail is told the direction at once: it chooses its 240 px tree or its
    // 34 px strip from the WIDTH it is given, so the shape changes when the
    // animation crosses the midpoint rather than when the timeline ends.
    m_rail->setExpanded(m_railExpanded);

    if (m_railAnimation != nullptr) {
        m_railAnimation->stop();
        m_railAnimation->deleteLater();
        m_railAnimation = nullptr;
    }
    auto *animation = new QVariantAnimation(this);
    animation->setDuration(SessionRail::animationMs());
    animation->setStartValue(m_rail->width());
    animation->setEndValue(target);
    // The QML's `Behavior on width` uses a NumberAnimation's default easing,
    // which is linear; a curve here would be a second opinion about the same
    // 140 ms.
    animation->setEasingCurve(QEasingCurve::Linear);
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { applyRailWidth(value.toInt()); });
    connect(animation, &QVariantAnimation::finished, this, [this, target]() {
        applyRailWidth(target);
        m_railAnimation = nullptr;
    });
    m_railAnimation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    qInfo().nospace() << "session rail: " << (m_railExpanded ? "expand" : "collapse")
                      << " " << m_rail->width() << " -> " << target << " px over "
                      << SessionRail::animationMs()
                      << " ms (the shell owns the splitter; the rail owns its shape)";
}

void Shell::applyRailWidth(int width)
{
    if (m_rail == nullptr)
        return;
    m_rail->setFixedWidth(width);
}

void Shell::openRailOverflowMenu()
{
    if (m_rail == nullptr)
        return;
    if (m_openRailMenu != nullptr)
        m_openRailMenu->hide();

    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    m_openRailMenu = menu;
    m_rail->setOverflowMenuOpen(true);
    // The rail's ⋯ stays lit while its menu is open and cannot see a surface it
    // does not own: the state goes back to it when the popup hides, on every
    // close path.
    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        if (m_openRailMenu == menu)
            m_openRailMenu = nullptr;
        m_rail->setOverflowMenuOpen(false);
    });

    Chrome::addCaption(menu, QStringLiteral("Conversations"));
    // The header's + makes a Workspace; the action that used to sit there is
    // here, in words, and it works in every state including a rail with no
    // Workspace row to press (main.qml:2600).
    QAction *newSession = menu->addAction(Chrome::menuText(QStringLiteral("New session")));
    connect(newSession, &QAction::triggered, this,
            [this]() { callRailTransport("createSession"); });

    QAction *group = menu->addAction(Chrome::menuText(QStringLiteral("Group by workspace")));
    group->setCheckable(true);
    group->setChecked(m_rail->groupByWorkspace());
    connect(group, &QAction::triggered, this,
            [this](bool grouped) { m_rail->setGroupByWorkspace(grouped); });

    // The QML puts the current value in a right-hand column; a stock QMenu has no
    // such column (its second one is the shortcut, and a shortcut here really
    // would bind the key), so the value is stated in the label instead. The two
    // entries cycle, which is what the QML's rows do.
    const bool byUpdated = m_rail->sortOrder() == SessionRailModel::SortOrder::Updated;
    QAction *sort = menu->addAction(
        Chrome::menuText(byUpdated ? QStringLiteral("Sort by last updated")
                                   : QStringLiteral("Sort in manual order")));
    connect(sort, &QAction::triggered, this, [this]() { m_rail->cycleSortOrder(); });

    const SessionRailModel::ArchivedFilter archived = m_rail->archivedFilter();
    const QString archivedText =
        archived == SessionRailModel::ArchivedFilter::Default
            ? QStringLiteral("Archived sessions: hidden")
            : archived == SessionRailModel::ArchivedFilter::Show
                  ? QStringLiteral("Archived sessions: shown")
                  : QStringLiteral("Archived sessions: only");
    QAction *archivedAction = menu->addAction(Chrome::menuText(archivedText));
    connect(archivedAction, &QAction::triggered, this,
            [this]() { m_rail->cycleArchivedFilter(); });

    if (m_rail->terminalSupported()) {
        menu->addSeparator();
        Chrome::addCaption(menu, QStringLiteral("Panels"));
        QAction *terminal = menu->addAction(Chrome::menuText(QStringLiteral("Terminal")));
        terminal->setCheckable(true);
        terminal->setChecked(m_rail->terminalActive());
        connect(terminal, &QAction::triggered, this, &Shell::toggleTerminalDock);

        QAction *files = menu->addAction(Chrome::menuText(QStringLiteral("Files")));
        files->setCheckable(true);
        files->setChecked(m_filesDock != nullptr && m_filesDock->shown());
        files->setEnabled(m_filesDock != nullptr && m_filesDock->available());
        connect(files, &QAction::triggered, this, [this]() {
            if (m_filesDock != nullptr)
                m_filesDock->setShown(!m_filesDock->shown());
        });

        const bool ledger = m_conversationStack != nullptr
                            && m_conversationStack->currentWidget() == m_trajectory;
        QAction *trajectory = menu->addAction(Chrome::menuText(QStringLiteral("Trajectory")));
        trajectory->setCheckable(true);
        trajectory->setChecked(ledger);
        trajectory->setEnabled(m_facade->hasTrajectory());
        connect(trajectory, &QAction::triggered, this, [this]() {
            if (m_conversationStack == nullptr || m_trajectory == nullptr)
                return;
            const bool show = m_conversationStack->currentWidget() != m_trajectory;
            if (show)
                m_trajectory->setSession(m_facade->sessionId());
            m_conversationStack->setCurrentWidget(show ? static_cast<QWidget *>(m_trajectory)
                                                       : static_cast<QWidget *>(m_conversation));
        });
    }

    if (m_facade->transportName() == QLatin1String("dsh")) {
        menu->addSeparator();
        Chrome::addCaption(menu, QStringLiteral("Host pages"));
        QAction *automation =
            menu->addAction(Chrome::menuText(QStringLiteral("Automation tasks…")));
        automation->setEnabled(m_facade->state() == ClientFacade::Ready);
        connect(automation, &QAction::triggered, this,
                [this]() { openManagementPage(QStringLiteral("automation")); });
        QAction *plugins =
            menu->addAction(Chrome::menuText(QStringLiteral("Plugins…")));
        plugins->setEnabled(m_facade->state() == ClientFacade::Ready);
        connect(plugins, &QAction::triggered, this,
                [this]() { openManagementPage(QStringLiteral("plugins")); });
    }

    menu->addSeparator();
    Chrome::addCaption(menu, QStringLiteral("Client"));
    QAction *restart = menu->addAction(Chrome::menuText(QStringLiteral("Restart client")));
    connect(restart, &QAction::triggered, this,
            [this]() { callRailTransport("restartSelf"); });
    QAction *refresh = menu->addAction(Chrome::menuText(QStringLiteral("Refresh")));
    connect(refresh, &QAction::triggered, this, [this]() {
        callRailTransport("refreshSessions");
        m_facade->refreshPermissionPresets();
        m_facade->refreshModelCatalog();
    });

    QAction *settings = menu->addAction(Chrome::menuText(QStringLiteral("Settings…")));
    connect(settings, &QAction::triggered, this, &Shell::showSettingsDialog);

    QAction *changes = menu->addAction(Chrome::menuText(QStringLiteral("Changed files…")));
    changes->setEnabled(!m_facade->changedFiles().isEmpty());
    connect(changes, &QAction::triggered, this, &Shell::showChangesDialog);

    const QRect anchor(m_rail->overflowAnchor()->mapToGlobal(QPoint(0, 0)),
                       m_rail->overflowAnchor()->size());
    Chrome::popupMenuAt(anchor, menu, true);
    qInfo().nospace() << "session rail: overflow menu open (" << menu->actions().size()
                      << " entries)";
}

void Shell::openRailRowMenu(const QRect &globalRow, const QString &sessionId,
                            const QString &storedTitle, bool pinned, bool archived)
{
    if (m_rail == nullptr || !globalRow.isValid())
        return;
    if (m_openRailMenu != nullptr)
        m_openRailMenu->hide();

    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    m_openRailMenu = menu;
    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        if (m_openRailMenu == menu)
            m_openRailMenu = nullptr;
        m_rail->noteRowMenuClosed();
    });

    Chrome::addCaption(menu, QStringLiteral("This session"));

    QAction *rename =
        menu->addAction(Chrome::menuText(QStringLiteral("Rename…")));
    connect(rename, &QAction::triggered, this, [this, sessionId, storedTitle]() {
        renameRailSession(sessionId, storedTitle);
    });

    QAction *fork = menu->addAction(Chrome::menuText(QStringLiteral("Fork")));
    connect(fork, &QAction::triggered, this, [this, sessionId]() {
        callRailTransport("forkSession", sessionId);
    });

    QAction *pin = menu->addAction(
        Chrome::menuText(pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin")));
    pin->setCheckable(true);
    pin->setChecked(pinned);
    connect(pin, &QAction::triggered, this, [this, sessionId, pinned]() {
        callRailTransport(pinned ? "unpinSession" : "pinSession", sessionId);
    });

    QAction *archive = menu->addAction(
        Chrome::menuText(archived ? QStringLiteral("Unarchive") : QStringLiteral("Archive")));
    connect(archive, &QAction::triggered, this, [this, sessionId, archived]() {
        callRailTransport(archived ? "unarchiveSession" : "archiveSession", sessionId);
    });

    menu->addSeparator();
    QAction *copy = menu->addAction(Chrome::menuText(QStringLiteral("Copy session id")));
    connect(copy, &QAction::triggered, this, [this, sessionId]() {
        QGuiApplication::clipboard()->setText(sessionId);
        qInfo().nospace() << "session rail: copied session id " << sessionId;
    });

    // Under the row, aligned to the row's right edge — over the rail rather than
    // over the transcript the rail exists to give width back to.
    Chrome::popupMenuAt(globalRow, menu, true);
    qInfo().nospace() << "session rail: row menu open for " << sessionId;
}

void Shell::renameRailSession(const QString &sessionId, const QString &storedTitle)
{
    qInfo().nospace() << "session rail: renameRequested(" << sessionId << ", \""
                      << storedTitle << "\") -> the shell's rename dialog";
    auto *dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("renameSessionDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowTitle(QStringLiteral("Rename session"));
    dialog->setLabelText(QStringLiteral("Session title"));
    dialog->setTextValue(storedTitle);
    dialog->setOkButtonText(QStringLiteral("Rename"));
    dialog->setCancelButtonText(QStringLiteral("Cancel"));
    // `open()` rather than `exec()`: window-modal in the same way, but with no
    // nested event loop, so the shell keeps processing while the dialog is up.
    connect(dialog, &QInputDialog::textValueSelected, this,
            [this, sessionId](const QString &title) {
                const QString trimmed = title.trimmed();
                if (trimmed.isEmpty()) {
                    qInfo() << "session rail: rename cancelled; the title is empty";
                    return;
                }
                QObject *transport = m_facade->transport();
                if (transport == nullptr)
                    return;
                if (QMetaObject::invokeMethod(transport, "renameSession",
                                              Q_ARG(QString, trimmed),
                                              Q_ARG(QString, sessionId))) {
                    qInfo().nospace() << "session rail: renameSession(\"" << trimmed
                                      << "\", " << sessionId << ") on transport "
                                      << m_facade->transportName();
                } else {
                    qWarning().nospace() << "session rail: transport "
                                         << m_facade->transportName()
                                         << " has no renameSession(); the title did not change";
                }
            });
    dialog->open();
}

void Shell::callRailTransport(const char *method, const QString &argument)
{
    QObject *transport = m_facade->transport();
    if (transport == nullptr) {
        qWarning().nospace() << "session rail: no transport to call " << method << "() on";
        return;
    }
    // A null QString is "no argument": the rail's verbs are one no-argument call
    // (createSession, restartSelf) or one string (pin, archive, create in a
    // Workspace), and passing an empty string to the first kind would look for a
    // signature that does not exist.
    const bool delivered =
        argument.isNull()
            ? QMetaObject::invokeMethod(transport, method)
            : QMetaObject::invokeMethod(transport, method, Q_ARG(QString, argument));
    if (delivered) {
        qInfo().nospace() << "session rail: " << method << "("
                          << (argument.isNull() ? QString() : argument)
                          << ") on transport " << m_facade->transportName();
    } else {
        qWarning().nospace() << "session rail: transport " << m_facade->transportName()
                             << " has no " << method << "("
                             << (argument.isNull() ? QString() : argument)
                             << "); nothing was sent";
    }
}

void Shell::escapeCurrentPage()
{
    // The QML's four guarded shortcuts, one page at a time:
    //   web         Escape -> minimiseTrapdoor()  (a page is worth returning to)
    //   headless    Escape -> minimiseHeadlessPane() (hibernate; keep the rows)
    //   acp         Escape -> closeAcpPane()      (the explicit stop, unlike its toggle)
    //   management  Escape -> closeManagementPage() (back to chat, nothing unloaded)
    // The action is disabled on the chat page, so Escape still reaches the
    // transcript and the composer there.
    if (m_currentPage == QLatin1String("web")) {
        qInfo() << "escape: web page up -> minimiseTrapdoor()";
        minimisePage(QStringLiteral("web"));
    } else if (m_currentPage == QLatin1String("acp")) {
        qInfo() << "escape: acp page up -> closeAcpPane() (the QML's Escape stops the child)";
        closePage(QStringLiteral("acp"));
    } else if (m_currentPage == QLatin1String("headless")) {
        qInfo() << "escape: headless page up -> minimiseHeadlessPane()";
        minimisePage(QStringLiteral("headless"));
    } else if (m_currentPage == QLatin1String("management")) {
        qInfo() << "escape: management page up -> closeManagementPage()";
        closePage(QStringLiteral("management"));
    }
}

void Shell::focusChatComposer()
{
    // Every QML minimise and close ends with `input.forceActiveFocus()`: the
    // reader was typing in the chat before the pane took the window and must be
    // put back where they were.
    if (m_composer != nullptr)
        m_composer->focusEntry();
}

// ---- window geometry -------------------------------------------------------

void Shell::restoreWindowGeometry()
{
    // The same keys and rules as the QML shell (main.cpp's `window/width` block):
    // a tiny saved size is a corrupt value, not a window, and a remembered
    // position is honoured only when it still lands on a screen — a monitor that
    // went away must not restore the window where nothing can reach it.
    QSettings prefs;
    const QVariant savedWidth = prefs.value(QStringLiteral("window/width"));
    const QVariant savedHeight = prefs.value(QStringLiteral("window/height"));
    if (savedWidth.toInt() > 200 && savedHeight.toInt() > 200)
        resize(savedWidth.toInt(), savedHeight.toInt());

    const QVariant savedX = prefs.value(QStringLiteral("window/x"));
    const QVariant savedY = prefs.value(QStringLiteral("window/y"));
    if (savedX.isValid() && savedY.isValid()) {
        const QRect remembered(savedX.toInt(), savedY.toInt(), width(), height());
        for (const QScreen *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(remembered)) {
                move(savedX.toInt(), savedY.toInt());
                break;
            }
        }
    }
}

void Shell::saveWindowGeometry(bool position, bool size)
{
    QSettings prefs;
    if (size) {
        prefs.setValue(QStringLiteral("window/width"), width());
        prefs.setValue(QStringLiteral("window/height"), height());
    }
    if (position) {
        prefs.setValue(QStringLiteral("window/x"), x());
        prefs.setValue(QStringLiteral("window/y"), y());
    }
}

void Shell::closeEvent(QCloseEvent *event)
{
    saveWindowGeometry(true, true);
    QWidget::closeEvent(event);
}

void Shell::moveEvent(QMoveEvent *event)
{
    if (m_geometryRestored)
        saveWindowGeometry(true, false);
    QWidget::moveEvent(event);
}

void Shell::resizeEvent(QResizeEvent *event)
{
    if (m_geometryRestored)
        saveWindowGeometry(false, true);
    if (m_lightbox != nullptr)
        m_lightbox->setGeometry(rect());
    QWidget::resizeEvent(event);
}
