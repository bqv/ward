#include "LinkText.h"

#include "Theme.h"

#include <QEvent>
#include <QFrame>
#include <QMouseEvent>
#include <QPalette>
#include <QTextCursor>
#include <QTextDocument>

LinkText::LinkText(QWidget *parent)
    : QTextEdit(parent)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("linkText"));
    setReadOnly(true);
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setAcceptDrops(false);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setContentsMargins(0, 0, 0, 0);
    document()->setDocumentMargin(0);
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);

    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);

    QPalette colours = palette();
    colours.setColor(QPalette::Base, Qt::transparent);
    colours.setColor(QPalette::Window, Qt::transparent);
    colours.setColor(QPalette::Highlight, theme->accent());
    colours.setColor(QPalette::HighlightedText, theme->accentFg());
    colours.setColor(QPalette::Text, theme->fg());
    setPalette(colours);
}

void LinkText::mouseMoveEvent(QMouseEvent *event)
{
    QTextEdit::mouseMoveEvent(event);
    const QPoint pos = event->position().toPoint();
    viewport()->setCursor(anchorAt(pos).isEmpty() ? Qt::IBeamCursor
                                                  : Qt::PointingHandCursor);
}

void LinkText::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QString href = anchorAt(event->position().toPoint());
        if (!href.isEmpty()) {
            m_pressedHref = href;
            QTextCursor cursor = textCursor();
            cursor.clearSelection();
            setTextCursor(cursor);
            event->accept();
            return;
        }
    }

    m_pressedHref.clear();
    QTextEdit::mousePressEvent(event);
}

void LinkText::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_pressedHref.isEmpty()) {
        const QString pressed = m_pressedHref;
        m_pressedHref.clear();
        const QString href = anchorAt(event->position().toPoint());
        event->accept();
        if (href == pressed)
            emit linkActivated(href);
        return;
    }

    QTextEdit::mouseReleaseEvent(event);
}

bool LinkText::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event->type() == QEvent::Leave)
        viewport()->setCursor(Qt::IBeamCursor);
    return QTextEdit::eventFilter(watched, event);
}
