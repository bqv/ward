#pragma once

// TerminalPanel — the terminal dock, as a QWidget.
//
// This is qml/TerminalPanel.qml's port (stage 2b, plan §5 stage 2): the same
// header row, the same terminal body, the same focus ring and scrollback
// indicator, and the same two contracts the shell's arithmetic depends on:
//
//   shown / expandedHeight   The panel is `shown ? expandedHeight : 0` tall, and
//                            `dockHeight()` is that number. The shell's footer
//                            adds exactly it to the composer's height, so folding
//                            the dock is one value changing, not a measurement of
//                            children the footer itself lays out (main.qml:1049's
//                            rule, quoted in Shell.cpp's FooterBar comment).
//
//   lazy open                The terminal is opened on the first show, never at
//                            construction. A terminal cannot be opened before a
//                            session exists, so the panel also waits for the
//                            transport to be Ready and for a session id — the two
//                            halves of the QML's `openIfNeeded()`.
//
// THE SETTLED-GRID TIMER IS NOT HERE. It belongs to TerminalEmulator, which took
// it over in stage 2a when the emulator absorbed TerminalView. The panel asks the
// emulator to open when the dock is first shown; the emulator holds that request
// until its 120 ms grid timer fires, so the host is told the grid that is actually
// drawn (docs/terminal-contract.md: `terminal/create` takes the grid and
// `terminal/resize` changes it, so opening at a transient layout size would build
// the PTY there and resize it twice more). The panel computes no grid at all — it
// gives the terminal a rectangle and the emulator measures cells from it, clamped
// by the host's `terminal/environment` ceiling.
//
// THE TRANSPORT IS INJECTED, the same QObject* the emulator binds by contract
// name: a QWidget has no QML context to read `dshTransport` from. A transport with
// no terminal surface (the ACP stdio client: `openTerminal` is a no-op, there is
// no `writeTerminal`, and none of the terminal signals exist) makes the panel
// ABSENT rather than present and dead. The QML's `typeof openTerminal ===
// "function"` test is a false positive on ACP — plan §3.3 records exactly that and
// says the port must not copy it — so `available()` asks the emulator, whose
// `supported()` is a real capability answer, not a name test.
//
// The focus RING is this panel's, not the widget's (stage 2a left it here): the
// QML draws it on the Rectangle around the grid, and TerminalWidget already
// documents why it must not draw a second one.

#include <QString>
#include <QWidget>

class QEvent;
class QPaintEvent;
class QObject;
class TerminalWidget;

class TerminalPanel : public QWidget
{
    Q_OBJECT

public:
    /// `transport` is the object `main.cpp` selected (DshClient or AcpClient),
    /// borrowed and outliving the panel. Null is legal and makes the panel
    /// unavailable rather than crashing.
    explicit TerminalPanel(QObject *transport, QWidget *parent = nullptr);
    ~TerminalPanel() override;

    // ---- the two QML inputs ------------------------------------------------

    /// Whether the dock is folded out. The panel resizes itself to `dockHeight()`
    /// on every change, so the shell never measures it.
    bool shown() const { return m_shown; }
    void setShown(bool shown);

    /// The height the dock takes when shown. 260 is the QML panel's own default.
    int expandedHeight() const { return m_expandedHeight; }
    void setExpandedHeight(int height);

    /// What the dock contributes to the footer: `shown ? expandedHeight : 0`, and
    /// zero whenever the transport cannot have a terminal at all.
    int dockHeight() const;

    /// Whether this transport has a terminal surface. False on ACP, where the
    /// dock is hidden and the rail's request is refused with a log line.
    bool available() const;

    /// The QML's focus gesture at the end of `onShownChanged`: give the keyboard
    /// to the grid. A widget cannot focus a disabled child, so when the terminal
    /// has not attached yet the focus is taken at the moment it does — the same
    /// intent, without the QML item's ability to hold focus while disabled.
    void focusTerminal();

    TerminalWidget *terminal() const { return m_terminal; }

signals:
    void shownChanged();

    /// The height contract changed (`shown` or `expandedHeight`), so the shell's
    /// footer must redo its arithmetic. Named for the quantity the shell reads,
    /// not for the widget event that caused it.
    void dockHeightChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

    /// Focus in/out on the grid is what the body's accent border follows, and a
    /// QWidget focus change is an event rather than a signal. TerminalWidget's
    /// header leaves the ring to this panel; this is the other half of that.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /// The QML's `openIfNeeded()`: open on the first show, and retry when the
    /// transport becomes ready or the session arrives, because a terminal cannot
    /// be opened before there is a session to open it in.
    void openIfNeeded();

private:
    // The internals are this file's business; the header only needs their
    // pointers. Each is a small paint-only widget, the counterpart of the QML
    // Text/MouseArea pairs in the panel's header and empty state.
    class Label;
    class Button;
    class Body;
    class EmptyState;

    void updateChrome();
    bool canOpen() const;
    bool dead() const;
    QString statusLine() const;
    void connectTransportSignals();

    QObject *m_transport = nullptr;
    TerminalWidget *m_terminal = nullptr;
    QWidget *m_header = nullptr;
    Body *m_body = nullptr;
    EmptyState *m_empty = nullptr;
    Label *m_state = nullptr;
    Label *m_scroll = nullptr;
    Button *m_close = nullptr;
    Button *m_open = nullptr;

    bool m_shown = false;
    int m_expandedHeight = 260;

    /// Set when the dock is folded out before the terminal attached, cleared when
    /// the focus is handed over. This is the widget counterpart of the QML's
    /// `forceActiveFocus()` on an item that is not enabled yet.
    bool m_focusOnAttach = false;
};
