#pragma once

// TerminalWidget — the terminal screen, as a QWidget.
//
// The same split as TerminalView, on the other side of the dual-shell period
// (plan §10a): TerminalEmulator owns the state and both renderers paint it with
// the one painter in TerminalPaint. This file is the Widgets half of the seam —
// the QWidget equivalents of the Quick item's events — plus the two things a
// desktop terminal needs and a QML item cannot offer: real text selection with a
// clipboard copy, and an input method.
//
// WHAT IS DIFFERENT FROM THE QUICK ITEM, and why the difference is not drift:
//
//  * selection is new. It is a renderer affordance (a mouse drag on the grid),
//    not emulator state: the QML item has no selection because nothing in the
//    QML panel asks for one, and adding it there is not this slice's business.
//    It is drawn by the same painter from a viewport-space band, so it cannot
//    change a cell, a colour or the cursor.
//  * Tab reaches the PTY instead of moving focus. A terminal that swallowed Tab
//    would be unable to run completion, so event() intercepts Key_Tab/Key_Backtab
//    before QWidget's focus navigation sees them — the same bytes the QML item
//    sends for those keys.
//  * a lost mouse grab releases the button it told the PTY about. QQuickItem has
//    mouseUngrabEvent for exactly this; a QWidget has QEvent::UngrabMouse and
//    leaveEvent, and a PTY left believing a button is down turns every later
//    move into a drag.
//
// The focus RING is the panel's, not this widget's (stage 2b): the QML panel
// draws it on the Rectangle around the grid, and a widget that drew its own ring
// would double it inside that panel. The cursor already distinguishes focused
// from unfocused here, as it does in QML.

#include <QPoint>
#include <QString>
#include <QWidget>

#include "TerminalPaint.h"

class QEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class TerminalEmulator;

class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    /// The transport is injected, exactly as it is into the emulator directly:
    /// there is no QML engine here to look a context property up in.
    explicit TerminalWidget(QObject *transport, QWidget *parent = nullptr);
    ~TerminalWidget() override;

    TerminalEmulator *emulator() const { return m_emulator; }
    void setTransport(QObject *transport);

    // The facts a panel binds to. They are forwarded rather than duplicated so
    // there is one owner of each: the emulator.
    bool supported() const;
    bool attached() const;
    QString hostState() const;
    int exitCode() const;
    int columns() const;
    int rows() const;
    int scrollbackLines() const;
    int scrollOffset() const;
    bool atBottom() const;

    void openTerminal();
    void closeTerminal();
    void scrollBy(int lines);
    void scrollToBottom();

    // --- selection -------------------------------------------------------------

    bool hasSelection() const { return m_selection.active; }
    /// The selected cells as text, one line per viewport row, trailing blanks
    /// trimmed. Empty when there is no selection.
    QString selectedText() const;
    /// Put selectedText() on the clipboard as ordinary text. A no-op when there
    /// is nothing selected; Ctrl+Shift+C is the keyboard route to it.
    void copySelection();
    void clearSelection();

signals:
    /// The history length or the scroll position changed, so a scrollback
    /// indicator can follow.
    void scrollbackChanged();
    /// Forwarded from the emulator so a panel can update its status line and
    /// its chrome without reaching through emulator() for the four facts it
    /// draws.
    void attachedChanged();
    void hostStateChanged();
    void gridChanged();
    /// The selection appeared, moved, or went away.
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    /// QWidget::event() turns Tab into focus navigation before keyPressEvent()
    /// is ever called, and carries the grab loss that QQuickItem calls
    /// mouseUngrabEvent. Both are handled here.
    bool event(QEvent *event) override;

private:
    /// Viewport position -> viewport cell, clamped to the grid that is drawn.
    /// The PTY is told a cell, not a pixel; selection is kept in the same space
    /// so the band a drag paints and the text a copy reads out are the same
    /// cells.
    QPoint cellAt(const QPointF &position) const;

    /// The cursor's cell rectangle in widget coordinates, for an input method's
    /// candidate window. Empty when there is no visible cursor.
    QRect cursorRect() const;

    TerminalEmulator *m_emulator = nullptr;
    TerminalPaint::Selection m_selection;
    bool m_selecting = false;
};
