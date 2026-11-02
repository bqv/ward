#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <Qt>

class QLabel;
class QMenu;
class QScrollBar;
class QPainter;
class QRect;
class QWidget;

/// The paint the Widgets chrome shares between its two bars.
///
/// This is a helper, not a style. `qml/main.qml` draws the header's toggles and
/// the status strip's chips as plain `Rectangle`s with a radius, a hairline border
/// and a text label; a Fusion `QToolButton` is a bevelled gradient slab, and the
/// design does not ship stock-looking controls (docs/ui-traditions.md: a themed
/// app that happens to sit on Fusion). So the two bars paint their chips from the
/// token table with `QWidget::paintEvent`, and the one piece of arithmetic they
/// share — the chip and the elided line — is here rather than copied into both.
///
/// Everything a colour is made of comes from `Theme`; there is no literal in this
/// file or in either bar.
namespace Chrome {

/// The UI face at a token size, in pixels. QML states `font.pixelSize` and a
/// family name, so the counterpart sets the same two things and leaves the point
/// size alone: mixing the two would make the two shells' type scales differ by
/// the screen's DPI.
QFont uiFont(int pixelSize, bool bold = false);

/// The fixed-pitch face, for machine text — a session id.
QFont monoFont(int pixelSize, bool bold = false);

/// Paint one chip: the radius, the transparent fill, the hairline border (accent
/// when `active`), and the hover fill. `active` is the QML's "open or lit" state,
/// which for this slice is a page toggle's checked state.
void paintChip(QPainter &painter, const QRect &rect, bool hovered, bool active);

/// Paint one line of text clipped and elided to the rectangle. `elideMiddle` is
/// the session id's rule — both ends of a uuid are fixed and the middle is what
/// the eye can lose — and the default is QML's `Text.ElideRight`.
void paintLine(QPainter &painter, const QRect &rect, const QFont &font,
               const QColor &colour, const QString &text,
               Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter,
               bool elideMiddle = false);

// ---- the shell's menus -----------------------------------------------------
//
// A `QMenu` is a `Qt::Popup` window, so it is a SHELL surface in the plan's
// §4.4 sense: not clipped by a pane, grabbing the keyboard while it is open, and
// owned by no pane. The status strip's three chips and the session rail's two
// menus therefore share one construction (`shellMenu`), one lifetime rule
// (`deleteOnHide`), one caption/empty/escaping vocabulary and one placement.
// Reusing them is the point: a second copy would drift the first time either
// menu grew a column.

/// A menu with the shell's palette and the UI face.
QMenu *shellMenu(QWidget *parent);

/// Destroy a menu built for one opening when it hides. Not `WA_DeleteOnClose`:
/// a dismissed QMenu is hidden, not closed, so every path emits `aboutToHide`
/// and that is where the deferred delete belongs.
void deleteOnHide(QMenu *menu);

/// One ampersand, printed as one: a catalog name is not a mnemonic.
QString menuText(const QString &text);

QLabel *captionLabel(QWidget *parent, const QString &text);

/// A non-interactive heading in the `caption` tier.
void addCaption(QMenu *menu, const QString &text);

/// A disabled row saying the list is empty.
void addEmptyRow(QMenu *menu, const QString &text);

/// Place a menu under an anchor rectangle: its left edge (or its right, for a
/// menu that reads back into a left-hand rail), clamped to the screen and
/// flipped above the anchor when it would not fit below. `globalAnchor` is in
/// global coordinates because a list row is not a widget.
void popupMenuAt(const QRect &globalAnchor, QMenu *menu, bool alignRightEdge);

/// main.qml's `compactCount()`: 1 000 becomes "1k", 1 500 "1.5k", 1 000 000
/// "1M", and anything under a thousand is itself. Numbers are grouped by hand
/// because QML's own `toLocaleString` was unreliable and this shell has to print
/// the same words the other one does while both exist (plan §10a).
QString compactCount(int value);

/// One scrollbar style for every scrollable surface: a 10 px bar with a rounded
/// handle, no arrows and a transparent track, brightening under the pointer.
void styleScrollBar(QScrollBar *bar);

} // namespace Chrome
