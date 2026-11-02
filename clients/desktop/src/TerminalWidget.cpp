// TerminalWidget — see TerminalWidget.h for what it is and what it deliberately
// leaves to the panel.
//
// Every terminal decision — the bytes, the grid, the palette, the cursor, the
// scrollback — is the emulator's. This file is only the widget's side of the
// seam: events in, a paint call out, and the selection that a desktop terminal
// needs and a QML item never had.

#include "TerminalWidget.h"

#include "TerminalEmulator.h"
#include "Theme.h"

#include <QClipboard>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>

TerminalWidget::TerminalWidget(QObject *transport, QWidget *parent)
    : QWidget(parent)
    , m_emulator(new TerminalEmulator(this))
{
    setObjectName(QStringLiteral("terminalWidget"));

    // A terminal is a text surface: it wants the keyboard, and it is reachable
    // by Tab like any other field. Tab reaching the emulator rather than moving
    // focus is event()'s business, below.
    setFocusPolicy(Qt::StrongFocus);
    // Hover reporting (VTERM_PROP_MOUSE_MOVE) only exists if bare moves arrive.
    setMouseTracking(true);

    // An input method needs to know it may compose here, and a terminal is not a
    // text field: autocapitalisation and predictive text would rewrite what the
    // shell is being told.
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);

    // The widget fills its whole rectangle in paintEvent, so Qt need not clear
    // behind it — the same opaque painting the Quick item does.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The palette comes from the token table and nowhere else: the QML panel
    // passes the same values as properties, so the two shells draw one terminal.
    const Theme *theme = Theme::instance();
    m_emulator->setForeground(theme->terminalFg());
    m_emulator->setBackground(theme->terminalBg());
    m_emulator->setCursorColor(theme->terminalCursor());
    m_emulator->setAnsiPalette(theme->ansi());
    m_emulator->setFontFamily(theme->mono());
    m_emulator->setFontPixelSize(theme->fontMono());

    connect(m_emulator, &TerminalEmulator::damaged, this, [this]() { update(); });
    connect(m_emulator, &TerminalEmulator::attachedChanged, this,
            &TerminalWidget::attachedChanged);
    connect(m_emulator, &TerminalEmulator::hostStateChanged, this,
            &TerminalWidget::hostStateChanged);
    connect(m_emulator, &TerminalEmulator::gridChanged, this, &TerminalWidget::gridChanged);

    // New output scrolls the viewport whenever the emulator is at the bottom, and
    // that changes which line every viewport row shows. A selection is kept in
    // viewport cells (TerminalPaint::Selection), so it cannot survive that: it is
    // dropped rather than left claiming cells that now hold other text.
    connect(m_emulator, &TerminalEmulator::scrollbackChanged, this, [this]() {
        clearSelection();
        emit scrollbackChanged();
    });

    if (transport)
        setTransport(transport);
}

TerminalWidget::~TerminalWidget() = default;

void TerminalWidget::setTransport(QObject *transport)
{
    m_emulator->setTransport(transport);
}

// --- forwarded facts ----------------------------------------------------------

bool TerminalWidget::supported() const { return m_emulator->supported(); }
bool TerminalWidget::attached() const { return m_emulator->attached(); }
QString TerminalWidget::hostState() const { return m_emulator->hostState(); }
int TerminalWidget::exitCode() const { return m_emulator->exitCode(); }
int TerminalWidget::columns() const { return m_emulator->columns(); }
int TerminalWidget::rows() const { return m_emulator->rows(); }
int TerminalWidget::scrollbackLines() const { return m_emulator->scrollbackLines(); }
int TerminalWidget::scrollOffset() const { return m_emulator->scrollOffset(); }
bool TerminalWidget::atBottom() const { return m_emulator->atBottom(); }

void TerminalWidget::openTerminal() { m_emulator->openTerminal(); }
void TerminalWidget::closeTerminal() { m_emulator->closeTerminal(); }
void TerminalWidget::scrollBy(int lines) { m_emulator->scrollBy(lines); }
void TerminalWidget::scrollToBottom() { m_emulator->scrollToBottom(); }

// --- events -------------------------------------------------------------------

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    // The one painter both shells use. The selection is the only thing this
    // renderer adds to the picture, and it is drawn from the same cells.
    TerminalPaint::paint(&painter, *m_emulator, QSizeF(size()), hasFocus(),
                         hasSelection() ? &m_selection : nullptr);
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    // A resize IS the grid request: the emulator derives columns and rows from
    // the viewport and tells the host once the size settles.
    m_emulator->setViewport(QSizeF(event->size()));
    QWidget::resizeEvent(event);
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (!event)
        return;

    const Qt::KeyboardModifiers modifiers = event->modifiers();

    // Ctrl+Shift+C copies, and only while something is selected. Without a
    // selection the same chord must stay Ctrl+C — SIGINT — which is what the key
    // table sends for it, so the intercept has to be conditional.
    if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier)
        && event->key() == Qt::Key_C && hasSelection()) {
        copySelection();
        event->accept();
        return;
    }

    m_emulator->sendKey(event->key(), event->text(), modifiers);
    event->accept();
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    // Three lines a notch unless a program asked for the mouse, which the
    // emulator decides; the event is ours either way.
    const int steps = event->angleDelta().y() / 120;
    m_emulator->wheel(steps, event->modifiers());
    event->accept();
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    // Clicking the grid gives it the keyboard, which is the only way back to the
    // PTY after another control has taken focus.
    setFocus(Qt::MouseFocusReason);

    if (m_emulator->mouseReporting()) {
        const QPoint cell = cellAt(event->position());
        m_emulator->mousePress(event->button(), cell.y(), cell.x(), event->modifiers());
        update();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        // A program that has not asked for the mouse has no use for a click, so
        // the press starts a selection instead. A single click with no drag is
        // dropped on release, so it does not leave a one-cell band behind.
        m_selecting = true;
        m_selection.active = true;
        m_selection.anchor = cellAt(event->position());
        m_selection.extent = m_selection.anchor;
        emit selectionChanged();
        update();
    }
    event->accept();
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_selecting) {
        const QPoint cell = cellAt(event->position());
        if (cell != m_selection.extent) {
            m_selection.extent = cell;
            emit selectionChanged();
            update();
        }
        event->accept();
        return;
    }

    const QPoint cell = cellAt(event->position());
    m_emulator->mouseMove(cell.y(), cell.x(), event->modifiers());
    event->accept();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_selecting && event->button() == Qt::LeftButton) {
        m_selecting = false;
        m_selection.extent = cellAt(event->position());
        // A click that never moved is not a selection: a one-cell band would
        // copy one character the user did not mean to pick.
        if (m_selection.extent == m_selection.anchor)
            m_selection.active = false;
        emit selectionChanged();
        update();
        event->accept();
        return;
    }

    m_emulator->mouseRelease(event->button(), event->modifiers());
    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    // The cursor is solid when focused and hollow otherwise, so a focus change is
    // itself a repaint.
    update();
    QWidget::focusInEvent(event);
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    update();
    QWidget::focusOutEvent(event);
}

void TerminalWidget::leaveEvent(QEvent *event)
{
    // With no button down, the pointer leaving is nothing to the PTY: hover
    // reporting is about cells inside the grid. If a button IS held, no release
    // may ever arrive (a grab taken away without an UngrabMouse), and a PTY left
    // believing a button is down turns every later move into a drag.
    if (m_emulator->buttonHeld()) {
        m_emulator->releaseHeldButton();
        update();
    }
    QWidget::leaveEvent(event);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    // Only the committed string is forwarded. A pre-edit is drawn by the input
    // method's own window over the grid; a terminal has no document to hold it,
    // and a shell told about half-composed text would receive bytes it cannot
    // undo.
    const QString commit = event->commitString();
    if (!commit.isEmpty())
        m_emulator->sendText(commit);
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    // Where the candidate window should sit: the cell the cursor is in. The
    // rest of the standard answers come from QWidget, which knows about
    // WA_InputMethodEnabled and the hints set in the constructor.
    if (query == Qt::ImCursorRectangle) {
        const QRect rectangle = cursorRect();
        if (rectangle.isValid())
            return rectangle;
    }
    return QWidget::inputMethodQuery(query);
}

bool TerminalWidget::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        // QWidget::event() handles Tab and Backtab itself, as focus navigation,
        // before keyPressEvent() is ever called. A terminal cannot lose Tab —
        // shell completion is Tab — so the two keys are sent to the PTY here,
        // exactly as the QML item's keyPressEvent sends them.
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
        break;
    }
    case QEvent::UngrabMouse:
        // QQuickItem calls this mouseUngrabEvent. There will be no release event
        // for a button the PTY was told about, so the emulator is asked to end
        // it.
        m_emulator->releaseHeldButton();
        update();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

// --- selection ----------------------------------------------------------------

QPoint TerminalWidget::cellAt(const QPointF &position) const
{
    const qreal cellWidth = m_emulator->cellWidth();
    const qreal cellHeight = m_emulator->cellHeight();
    const int columns = qMax(1, m_emulator->viewportColumns(width()));
    const int rows = qMax(1, m_emulator->viewportRows(height()));
    const int column = cellWidth > 0 ? qBound(0, int(position.x() / cellWidth), columns - 1) : 0;
    const int row = cellHeight > 0 ? qBound(0, int(position.y() / cellHeight), rows - 1) : 0;
    return QPoint(column, row);
}

QRect TerminalWidget::cursorRect() const
{
    if (!m_emulator->cursorVisible() || m_emulator->scrollOffset() != 0)
        return {};

    const VTermPos cursor = m_emulator->cursorPos();
    const qreal cellWidth = m_emulator->cellWidth();
    const qreal cellHeight = m_emulator->cellHeight();
    if (cellWidth <= 0 || cellHeight <= 0)
        return {};

    // The cursor is a live-screen cell; the viewport row it is drawn on is that
    // row shifted by the history the viewport is also showing.
    const qreal row = cursor.row + m_emulator->scrollbackLines()
                    - m_emulator->firstViewportLine(height());
    return QRect(int(cursor.col * cellWidth), int(row * cellHeight), int(cellWidth),
                 int(cellHeight));
}

QString TerminalWidget::selectedText() const
{
    if (!m_selection.active)
        return {};

    QPoint from = m_selection.anchor;
    QPoint to = m_selection.extent;
    if (to.y() < from.y() || (to.y() == from.y() && to.x() < from.x()))
        std::swap(from, to);

    const int rows = m_emulator->viewportRows(height());
    const int columns = m_emulator->viewportColumns(width());
    const int firstLine = m_emulator->firstViewportLine(height());

    QStringList lines;
    for (int row = from.y(); row <= to.y() && row < rows; ++row) {
        if (row < 0)
            continue;
        const int line = firstLine + row;
        const int firstColumn = row == from.y() ? from.x() : 0;
        const int lastColumn = row == to.y() ? to.x() : columns - 1;

        QString text;
        for (int column = firstColumn; column <= lastColumn; ++column) {
            VTermScreenCell cell;
            if (!m_emulator->bufferCell(line, column, &cell)) {
                text += QLatin1Char(' ');
                continue;
            }
            if (TerminalEmulator::isContinuation(cell))
                continue;
            if (cell.chars[0] == 0) {
                text += QLatin1Char(' ');
                continue;
            }
            text += TerminalEmulator::cellText(cell);
        }
        // Trailing blanks are not part of what the user drew a box around, and
        // copying them turns every line of a selection into a ragged, padded
        // message.
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        lines += text;
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection()
{
    const QString text = selectedText();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
}

void TerminalWidget::clearSelection()
{
    if (!m_selection.active && !m_selecting)
        return;
    m_selection.active = false;
    m_selecting = false;
    m_selection.anchor = QPoint();
    m_selection.extent = QPoint();
    emit selectionChanged();
    update();
}
