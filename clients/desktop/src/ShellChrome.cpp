#include "ShellChrome.h"

#include "Theme.h"

#include <QAction>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QWidgetAction>
#include <QtMath>

namespace Chrome {

QFont uiFont(int pixelSize, bool bold)
{
    QFont font(Theme::instance()->ui());
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    return font;
}

QFont monoFont(int pixelSize, bool bold)
{
    QFont font(Theme::instance()->mono());
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    return font;
}

void paintChip(QPainter &painter, const QRect &rect, bool hovered, bool active)
{
    Theme *theme = Theme::instance();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The half-pixel inset is what keeps a 1 px pen inside the widget's bounds
    // rather than straddling the edge, which is how QML's `border.width: 1`
    // draws: inside the rectangle, not around it.
    const QRectF body = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);

    painter.setBrush(hovered ? QBrush(theme->buttonHover()) : QBrush(Qt::NoBrush));
    painter.setPen(QPen(active ? theme->accent() : theme->borderL2(), theme->hairline()));
    const int radius = theme->radius();
    painter.drawRoundedRect(body, radius, radius);
    painter.restore();
}

void paintLine(QPainter &painter, const QRect &rect, const QFont &font,
               const QColor &colour, const QString &text,
               Qt::Alignment alignment, bool elideMiddle)
{
    if (text.isEmpty() || rect.width() <= 0 || rect.height() <= 0)
        return;

    painter.save();
    painter.setFont(font);
    painter.setPen(colour);

    const QFontMetrics metrics(font);
    const Qt::TextElideMode mode = elideMiddle ? Qt::ElideMiddle : Qt::ElideRight;
    const QString shown = metrics.horizontalAdvance(text) <= rect.width()
                              ? text
                              : metrics.elidedText(text, mode, rect.width());

    // The elided string is drawn into the full rectangle, so the alignment the
    // caller asked for still holds: an elided chip label stays centred rather than
    // hugging the left edge it was trimmed towards.
    painter.drawText(rect, int(alignment), shown);
    painter.restore();
}

void styleScrollBar(QScrollBar *bar)
{
    if (bar == nullptr)
        return;
    Theme *theme = Theme::instance();
    const QString sheet = QStringLiteral(
        "QScrollBar:vertical { width: 10px; margin: 0 2px 0 0; background: transparent;"
        " border: none; }"
        "QScrollBar::handle:vertical { background: %1; border-radius: 5px; min-height: 32px; }"
        "QScrollBar::handle:vertical:hover { background: %2; }"
        "QScrollBar:horizontal { height: 10px; margin: 0 0 2px 0; background: transparent;"
        " border: none; }"
        "QScrollBar::handle:horizontal { background: %1; border-radius: 5px; min-width: 32px; }"
        "QScrollBar::handle:horizontal:hover { background: %2; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
        .arg(theme->border().name(), theme->borderStrong().name());
    bar->setStyleSheet(sheet);
}

QMenu *shellMenu(QWidget *parent)
{
    Theme *theme = Theme::instance();
    auto *menu = new QMenu(parent);
    menu->setFont(uiFont(theme->fontUI()));
    // Off by default on a QMenu; the catalog's descriptions are the only place
    // some of these choices are explained, and the QML shows them the same way.
    menu->setToolTipsVisible(true);

    QPalette palette = Theme::palette();
    palette.setColor(QPalette::Base, theme->panel());
    palette.setColor(QPalette::Highlight, theme->raised());
    palette.setColor(QPalette::HighlightedText, theme->fg());
    menu->setPalette(palette);
    return menu;
}

void deleteOnHide(QMenu *menu)
{
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
}

QString menuText(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QLatin1String("&&"));
    return escaped;
}

QLabel *captionLabel(QWidget *parent, const QString &text)
{
    Theme *theme = Theme::instance();
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setFont(uiFont(theme->fontData()));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, theme->caption());
    label->setPalette(palette);
    label->setContentsMargins(theme->space3(), theme->space2(), theme->space3(),
                              theme->space2());
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

void addCaption(QMenu *menu, const QString &text)
{
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(captionLabel(menu, text));
    action->setEnabled(false);
    menu->addAction(action);
}

void addEmptyRow(QMenu *menu, const QString &text)
{
    QAction *action = menu->addAction(menuText(text));
    action->setEnabled(false);
}

void popupMenuAt(const QRect &globalAnchor, QMenu *menu, bool alignRightEdge)
{
    Theme *theme = Theme::instance();
    const QSize hint = menu->sizeHint();

    QPoint position = globalAnchor.bottomLeft() + QPoint(0, theme->space1());
    if (alignRightEdge)
        position.rx() = globalAnchor.right() + 1 - hint.width();

    // Ask the screen the anchor is on, not the already-shifted point: a menu
    // wider than the strip can put the point on the neighbouring screen.
    QScreen *screen = QGuiApplication::screenAt(globalAnchor.topLeft());
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    if (screen != nullptr) {
        const QRect available = screen->availableGeometry();
        if (position.x() + hint.width() - 1 > available.right())
            position.setX(available.right() - hint.width() + 1);
        if (position.x() < available.left())
            position.setX(available.left());
        // A QMenu's own flip anchors on the POINT it was given, so a menu at
        // the bottom edge would otherwise put its bottom at the anchor's bottom,
        // over the trigger. Flip above the anchor rectangle instead.
        if (position.y() + hint.height() - 1 > available.bottom())
            position.setY(globalAnchor.top() - hint.height() - theme->space1());
        if (position.y() < available.top())
            position.setY(available.top());
    }

    menu->popup(position);
}

QString compactCount(int value)
{
    const qint64 n = value;
    const qint64 magnitude = n < 0 ? -n : n;
    const QString sign = n < 0 ? QStringLiteral("-") : QString();

    auto decimal = [](qreal scaled) {
        // One decimal only below ten, so "1M" rather than "1.0M" but "1.5M".
        QString text = QString::number(scaled, 'f', 1);
        if (text.endsWith(QLatin1String(".0")))
            text.chop(2);
        return text;
    };

    if (magnitude >= 1000000) {
        const qreal millions = qreal(magnitude) / 1000000.0;
        return sign + (millions >= 10.0 ? QString::number(qRound(millions))
                                        : decimal(millions))
               + QStringLiteral("M");
    }
    if (magnitude >= 1000) {
        const qreal thousands = qreal(magnitude) / 1000.0;
        return sign + (thousands >= 100.0 ? QString::number(qRound(thousands))
                                          : decimal(thousands))
               + QStringLiteral("k");
    }
    return sign + QString::number(magnitude);
}

} // namespace Chrome
