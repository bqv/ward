// TerminalPaint — see TerminalPaint.h for why the two renderers share it.
//
// This is the paint loop that used to live in TerminalView::paint(), moved
// rather than rewritten: every coordinate, every condition and every colour
// decision is the one the QML shell has been drawing, so the Widgets render is
// the same render and not a second interpretation of it.

#include "TerminalPaint.h"

#include "TerminalEmulator.h"
#include "Theme.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QString>

#include <cmath>
#include <utility>

namespace TerminalPaint {
namespace {

/// Whether (column, row) is inside the selection, with `from` the earlier cell
/// and `to` the later one. Comparison is row-major, which is what makes a
/// multi-row selection a rectangle with ragged ends rather than a diagonal.
bool selected(const Selection &selection, int column, int row, const QPoint &from,
              const QPoint &to)
{
    if (!selection.active || row < from.y() || row > to.y())
        return false;
    if (from.y() == to.y())
        return column >= from.x() && column <= to.x();
    if (row == from.y())
        return column >= from.x();
    if (row == to.y())
        return column <= to.x();
    return true;
}

}   // namespace

void paint(QPainter *painter, const TerminalEmulator &terminal, const QSizeF &size,
           bool focused, const Selection *selection)
{
    painter->fillRect(QRectF(0, 0, size.width(), size.height()), terminal.background());
    if (terminal.cellWidth() <= 0 || terminal.cellHeight() <= 0)
        return;

    // The cursor is the live screen's, so scrolled back it would sit over a line
    // that has nothing to do with it. The grid is drawn first and the cursor
    // suppressed by the offset check further down.
    //
    // Cell text is drawn on a fixed grid, so antialiasing only blurs glyph edges
    // across neighbouring cells.
    painter->setRenderHint(QPainter::TextAntialiasing, false);
    painter->setRenderHint(QPainter::Antialiasing, false);

    const qreal cellWidth = terminal.cellWidth();
    const qreal cellHeight = terminal.cellHeight();
    const int columns = terminal.viewportColumns(size.width());
    const int rows = terminal.viewportRows(size.height());
    const int firstLine = terminal.firstViewportLine(size.height());

    // A selection is drawn as a wash over the theme's accent, not as a solid
    // fill: the glyph's own colour has to stay readable underneath, which a solid
    // band would not allow.
    QColor wash = Theme::instance()->accent();
    wash.setAlpha(150);

    QPoint from, to;
    if (selection && selection->active) {
        from = selection->anchor;
        to = selection->extent;
        if (to.y() < from.y() || (to.y() == from.y() && to.x() < from.x()))
            std::swap(from, to);
    }

    bool fontSet = false;
    bool boldFont = false, italicFont = false, underlineFont = false, strikeFont = false;

    for (int row = 0; row < rows; ++row) {
        const int line = firstLine + row;

        for (int column = 0; column < columns; ++column) {
            VTermScreenCell cell;
            if (!terminal.bufferCell(line, column, &cell))
                continue;
            if (TerminalEmulator::isContinuation(cell))
                continue;   // the right half of a wide glyph: the left half drew it

            const int span = cell.width > 1 ? 2 : 1;
            const QRectF cellRect(column * cellWidth, row * cellHeight, cellWidth * span,
                                  cellHeight);

            QColor fg = terminal.resolveColour(cell.fg, true, cell.attrs.bold != 0);
            QColor bg = terminal.resolveColour(cell.bg, false, false);
            if (cell.attrs.reverse) {
                const QColor swap = fg;
                fg = bg;
                bg = swap;
            }

            if (bg != terminal.background())
                painter->fillRect(cellRect, bg);

            // The span, not one cell: a wide glyph is one letter and highlights
            // as one, and its right half is skipped below as a continuation.
            if (selection && selected(*selection, column, row, from, to))
                painter->fillRect(cellRect, wash);

            if (!fontSet || boldFont != (cell.attrs.bold != 0) || italicFont != (cell.attrs.italic != 0)
                || underlineFont != (cell.attrs.underline != VTERM_UNDERLINE_OFF)
                || strikeFont != (cell.attrs.strike != 0)) {
                QFont font = terminal.font();
                boldFont = cell.attrs.bold != 0;
                italicFont = cell.attrs.italic != 0;
                underlineFont = cell.attrs.underline != VTERM_UNDERLINE_OFF;
                strikeFont = cell.attrs.strike != 0;
                font.setBold(boldFont);
                font.setItalic(italicFont);
                font.setUnderline(underlineFont);
                font.setStrikeOut(strikeFont);
                painter->setFont(font);
                fontSet = true;
            }

            if (cell.chars[0] != 0 && !cell.attrs.conceal) {
                painter->setPen(fg);
                painter->drawText(QPointF(column * cellWidth, row * cellHeight + terminal.ascent()),
                                  TerminalEmulator::cellText(cell));
            }
        }
    }

    const VTermPos cursor = terminal.cursorPos();
    // The host's word decides whether there is a cursor at all: an exited shell has
    // no prompt waiting for input, and drawing one would say otherwise. An empty
    // state means the host has not spoken yet, so the cursor is drawn.
    // And not while scrolled back: the cursor belongs to the live screen, where it
    // is the only row the user is not looking at.
    const QString hostState = terminal.hostState();
    const int historyCount = terminal.scrollbackLines();
    const bool cursorShown = terminal.scrollOffset() == 0 && terminal.cursorVisible()
                             && (hostState.isEmpty() || hostState == QLatin1String("running"));
    if (cursorShown && cursor.row >= 0 && cursor.row < rows && cursor.col >= 0
        && cursor.col < columns) {
        // At offset 0 the live screen starts `historyCount` lines into the buffer,
        // so the cursor's grid row is that much further down the viewport.
        const qreal cursorViewRow = (cursor.row + historyCount) - firstLine;
        const QRectF cursorRect(cursor.col * cellWidth, cursorViewRow * cellHeight, cellWidth,
                                cellHeight);
        if (focused) {
            painter->fillRect(cursorRect, terminal.cursorColor());
            // The glyph under a solid cursor has to be redrawn in the background
            // colour, or the cursor hides the character it is sitting on.
            VTermScreenCell cell;
            if (terminal.screenCell(cursor.row, cursor.col, &cell) && cell.chars[0] != 0
                && !TerminalEmulator::isContinuation(cell) && !cell.attrs.conceal) {
                painter->setPen(terminal.background());
                painter->drawText(
                    QPointF(cursor.col * cellWidth, cursor.row * cellHeight + terminal.ascent()),
                    TerminalEmulator::cellText(cell));
            }
        } else {
            // An unfocused terminal keeps a hollow cursor: the panel is still
            // showing where input would land, without claiming the keyboard.
            painter->setPen(QPen(terminal.cursorColor(), 1));
            painter->drawRect(cursorRect.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
}

}   // namespace TerminalPaint
