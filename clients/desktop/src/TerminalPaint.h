#pragma once

// TerminalPaint — the one painter both terminal renderers call.
//
// WHY IT IS SHARED, rather than a paintEvent in each renderer: the QML shell and
// the Widgets shell show the same emulator, and the acceptance test for stage 2a
// is that a Widgets render matches the QML baseline — the cursor, the ANSI
// palette, the wide-glyph continuation and the bold-bright rule. Two paint loops
// over one emulator would still be two places for a glyph coordinate or a colour
// decision to drift, and the difference would be found on the shell nobody is
// looking at. The emulator owns the state; this file owns the only way that
// state is turned into pixels.
//
// It is not part of the emulator on purpose: an emulator that can paint is an
// emulator that cannot be used without a painter, and the logic tests in
// ~/tmp/stage2a-test bind it to no widget and no window at all.

#include <QPoint>
#include <QSizeF>

class QPainter;
class TerminalEmulator;

namespace TerminalPaint {

/// A text selection in VIEWPORT cells — what the user drew on screen, not buffer
/// lines. `anchor` is where the drag began and `extent` where it is now; both are
/// (column, row) and either may be the earlier one. The painter normalises them,
/// so a selection dragged up and to the left is the same band as one dragged
/// down and to the right.
///
/// It lives in the viewport because that is the space a mouse lands in. The
/// widget clears it whenever the viewport scrolls, so it can never claim cells
/// that now show different lines.
struct Selection {
    bool active = false;
    QPoint anchor;
    QPoint extent;
};

/// Paint the emulator's current view into `size` pixels. `focused` decides only
/// how the cursor is drawn — solid and inverting when focused, a hollow outline
/// otherwise — which is the behaviour the QML item has always had.
void paint(QPainter *painter, const TerminalEmulator &terminal, const QSizeF &size,
           bool focused, const Selection *selection = nullptr);

}   // namespace TerminalPaint
