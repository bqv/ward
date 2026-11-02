#include "MentionPopup.h"

#include "ShellChrome.h"
#include "Theme.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QScreen>
#include <QVariantMap>

namespace {

constexpr int MaxVisibleRows = 7;

}

MentionPopup::MentionPopup(QWidget *parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("mentionPopup"));
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void MentionPopup::setCandidates(const QVariantList &candidates)
{
    if (candidates == m_candidates)
        return;
    m_candidates = candidates;
    m_selected = 0;
    m_top = 0;
    if (m_candidates.isEmpty())
        dismiss();
    update();
}

bool MentionPopup::isOpen() const
{
    return isVisible() && !m_candidates.isEmpty();
}

void MentionPopup::setSelectedIndex(int index)
{
    if (m_candidates.isEmpty())
        return;
    const int wanted = qBound(0, index, int(m_candidates.size()) - 1);
    if (wanted == m_selected)
        return;
    m_selected = wanted;
    ensureVisible(m_selected);
    update();
}

void MentionPopup::moveSelection(int step)
{
    const int total = int(m_candidates.size());
    if (total == 0)
        return;
    m_selected = (m_selected + step + total) % total;
    ensureVisible(m_selected);
    update();
}

QString MentionPopup::selectedPath() const
{
    if (m_selected < 0 || m_selected >= int(m_candidates.size()))
        return QString();
    return m_candidates.at(m_selected).toMap().value(QStringLiteral("path")).toString();
}

void MentionPopup::showAt(const QRect &caretGlobal, const QRect &fieldGlobal)
{
    if (m_candidates.isEmpty()) {
        dismiss();
        return;
    }

    Theme *theme = Theme::instance();
    const int width = 26 * theme->space6();
    const int rows = visibleRows();
    const int height = rows * rowHeight() + 2 * theme->space2();

    const QScreen *onScreen = QGuiApplication::screenAt(caretGlobal.center());
    if (onScreen == nullptr)
        onScreen = screen() != nullptr ? screen() : QGuiApplication::primaryScreen();
    const QRect available = onScreen != nullptr ? onScreen->availableGeometry() : QRect();
    const int margin = theme->space2();

    int x = fieldGlobal.left();
    if (!available.isNull()) {
        x = qMin(x, available.right() - margin - width);
        x = qMax(x, available.left() + margin);
    }

    int y = caretGlobal.top() - theme->space2() - height;
    if (!available.isNull() && y < available.top() + margin) {
        y = caretGlobal.bottom() + theme->space2();
        y = qMin(y, available.bottom() - margin - height);
        y = qMax(y, available.top() + margin);
    }

    ensureVisible(m_selected);
    setGeometry(QRect(x, y, width, height));
    update();
    if (!isVisible())
        show();
    raise();
}

void MentionPopup::dismiss()
{
    if (isVisible())
        hide();
}

int MentionPopup::visibleRows() const
{
    return qMin(int(m_candidates.size()), MaxVisibleRows);
}

int MentionPopup::rowHeight() const
{
    return Theme::instance()->rowHeight();
}

QRect MentionPopup::rowRect(int row) const
{
    const int pad = Theme::instance()->space2();
    return QRect(pad, pad + row * rowHeight(), width() - 2 * pad, rowHeight());
}

int MentionPopup::rowAt(const QPoint &position) const
{
    const int pad = Theme::instance()->space2();
    if (position.y() < pad)
        return -1;
    const int row = (position.y() - pad) / rowHeight();
    if (row < 0 || row >= visibleRows())
        return -1;
    const int index = m_top + row;
    return index < int(m_candidates.size()) ? index : -1;
}

void MentionPopup::ensureVisible(int index)
{
    const int rows = visibleRows();
    if (rows <= 0)
        return;
    if (index < m_top)
        m_top = index;
    if (index >= m_top + rows)
        m_top = index - rows + 1;
    m_top = qBound(0, m_top, qMax(0, int(m_candidates.size()) - rows));
}

void MentionPopup::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setBrush(theme->panel());
    painter.setPen(QPen(theme->border(), theme->hairline()));
    painter.drawRoundedRect(body, theme->radius(), theme->radius());

    QPainterPath clip;
    clip.addRoundedRect(body.adjusted(theme->hairline(), theme->hairline(),
                                      -theme->hairline(), -theme->hairline()),
                        qMax(0, theme->radius() - theme->hairline()),
                        qMax(0, theme->radius() - theme->hairline()));
    painter.save();
    painter.setClipPath(clip);

    const QFont font = Chrome::monoFont(theme->fontData());
    const QFontMetrics metrics(font);
    painter.setFont(font);

    for (int row = 0; row < visibleRows(); ++row) {
        const int index = m_top + row;
        if (index < 0 || index >= int(m_candidates.size()))
            break;
        const QRect area = rowRect(row);
        if (index == m_selected)
            painter.fillRect(area, theme->raised());

        const QVariantMap entry = m_candidates.at(index).toMap();
        const QString path = entry.value(QStringLiteral("path")).toString();
        const bool directory =
            entry.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
        const QRect label = area.adjusted(theme->space2(), 0, -theme->space2(), 0);
        if (path.isEmpty() || label.width() <= 0)
            continue;
        const QString shown = metrics.horizontalAdvance(path) <= label.width()
                                  ? path
                                  : metrics.elidedText(path, Qt::ElideLeft, label.width());
        painter.setPen(directory ? theme->dim() : theme->fg());
        painter.drawText(label, int(Qt::AlignLeft | Qt::AlignVCenter), shown);
    }

    painter.restore();
}

void MentionPopup::mouseMoveEvent(QMouseEvent *event)
{
    const int index = rowAt(event->position().toPoint());
    if (index >= 0)
        setSelectedIndex(index);
    QWidget::mouseMoveEvent(event);
}

void MentionPopup::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int index = rowAt(event->position().toPoint());
        if (index >= 0) {
            setSelectedIndex(index);
            const QString path = selectedPath();
            if (!path.isEmpty()) {
                emit chosen(path);
                event->accept();
                return;
            }
        }
    }
    QWidget::mouseReleaseEvent(event);
}
