#include "GoalRow.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QColor>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QStringList>
#include <QTimer>
#include <QVariant>

GoalRow::GoalRow(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    setObjectName(QStringLiteral("goalRow"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(0);
    setVisible(false);
    setMouseTracking(true);
    m_clearReset = new QTimer(this);
    m_clearReset->setSingleShot(true);
    m_clearReset->setInterval(4000);
    connect(m_clearReset, &QTimer::timeout, this, [this]() {
        if (!m_confirmClear)
            return;
        m_confirmClear = false;
        update();
    });
    if (m_facade)
        connect(m_facade, &ClientFacade::goalChanged, this, &GoalRow::refresh);
    refresh();
}

bool GoalRow::shown() const
{
    const QVariant phase = m_goal.value(QStringLiteral("phase"));
    return phase.isValid() && !phase.isNull() && !phase.toString().isEmpty();
}

int GoalRow::dockHeight() const
{
    return shown() ? Theme::instance()->rowHeight() : 0;
}

void GoalRow::refresh()
{
    const QVariantMap goal = m_facade ? m_facade->goal() : QVariantMap();
    if (goal == m_goal)
        return;
    m_goal = goal;
    setFixedHeight(dockHeight());
    setVisible(dockHeight() > 0);
    update();
    emit dockHeightChanged();
}

void GoalRow::paintEvent(QPaintEvent *)
{
    if (!shown())
        return;

    Theme *theme = Theme::instance();
    QPainter painter(this);
    const QFont font = Chrome::uiFont(theme->fontData());
    const QFontMetrics metrics(font);
    painter.setFont(font);

    const QString phase = m_goal.value(QStringLiteral("phase")).toString();
    const bool blocked = phase == QLatin1String("blocked");
    const QString phaseText = phase == QLatin1String("paused")
                                  ? QStringLiteral("\u25c6  Paused Goal")
                              : blocked
                                  ? QStringLiteral("\u25c6  Blocked Goal")
                              : phase == QLatin1String("complete")
                                  ? QStringLiteral("\u25c6  Complete Goal")
                                  : QStringLiteral("\u25c6  Ongoing Goal");

    const bool resumable = phase == QLatin1String("paused") || blocked;
    const bool toggleable = resumable || phase == QLatin1String("active");
    const bool editable = m_facade != nullptr && m_facade->hasGoalEditing();
    const QString pauseLabel = resumable ? QStringLiteral("Resume") : QStringLiteral("Pause");
    const QString editLabel = QStringLiteral("Edit");
    const QString clearLabel = m_confirmClear ? QStringLiteral("Clear goal?")
                                              : QStringLiteral("Clear");
    const int padding = theme->space2();
    const int pauseWidth = metrics.horizontalAdvance(pauseLabel) + 2 * padding;
    const int editWidth = metrics.horizontalAdvance(editLabel) + 2 * padding;
    const int clearWidth = metrics.horizontalAdvance(clearLabel) + 2 * padding;
    m_clearRect = QRect(width() - theme->space3() - clearWidth, 0, clearWidth, height());
    m_editRect = editable
        ? QRect(m_clearRect.left() - theme->space1() - editWidth, 0, editWidth, height())
        : QRect();
    m_pauseRect = toggleable
        ? QRect((editable ? m_editRect.left() : m_clearRect.left()) - theme->space1() - pauseWidth,
                0, pauseWidth, height())
        : QRect();

    const int top = 0;
    const int firstAction = toggleable ? m_pauseRect.left()
                            : editable ? m_editRect.left() : m_clearRect.left();
    const int right = firstAction - theme->space2();
    int x = theme->space3();

    const QColor phaseColour = blocked ? theme->danger()
                                : phase == QLatin1String("paused") ? theme->warn()
                                : phase == QLatin1String("complete") ? theme->ok()
                                : theme->running();
    painter.setPen(phaseColour);
    const int phaseWidth = metrics.horizontalAdvance(phaseText);
    painter.drawText(QRect(x, top, phaseWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, phaseText);
    x += phaseWidth + theme->space2();

    const QVariant rounds = m_goal.value(QStringLiteral("roundsStarted"));
    if (rounds.isValid() && !rounds.isNull()) {
        const QVariant maxRounds = m_goal.value(QStringLiteral("maxRounds"));
        const QString progress = maxRounds.isValid() && maxRounds.toInt() > 0
                                     ? rounds.toString() + QLatin1Char('/') + maxRounds.toString()
                                     : rounds.toString();
        painter.setPen(theme->faint());
        const int progressWidth = metrics.horizontalAdvance(progress);
        painter.drawText(QRect(x, top, progressWidth, height()),
                         Qt::AlignLeft | Qt::AlignVCenter, progress);
        x += progressWidth + theme->space2();
    }

    const QVariant objective = m_goal.value(QStringLiteral("objective"));
    if (objective.isValid() && !objective.isNull() && x < right) {
        const QStringList lines = objective.toString().split(QLatin1Char('\n'));
        QString summary;
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                summary = trimmed;
                break;
            }
        }
        if (!summary.isEmpty()) {
            painter.setPen(theme->fg());
            const QString elided = metrics.elidedText(summary, Qt::ElideRight, right - x);
            painter.drawText(QRect(x, top, right - x, height()),
                             Qt::AlignLeft | Qt::AlignVCenter, elided);
        }
    }

    painter.setPen(m_hoverPause ? theme->fg() : theme->caption());
    painter.drawText(m_pauseRect, Qt::AlignCenter, pauseLabel);
    if (editable) {
        painter.setPen(m_hoverEdit ? theme->fg() : theme->caption());
        painter.drawText(m_editRect, Qt::AlignCenter, editLabel);
    }
    painter.setPen(m_confirmClear ? theme->danger()
                  : (m_hoverClear ? theme->fg() : theme->caption()));
    painter.drawText(m_clearRect, Qt::AlignCenter, clearLabel);
}

void GoalRow::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const bool pause = m_pauseRect.contains(pos);
    const bool edit = m_editRect.contains(pos);
    const bool clear = m_clearRect.contains(pos);
    if (pause == m_hoverPause && edit == m_hoverEdit && clear == m_hoverClear)
        return;
    m_hoverPause = pause;
    m_hoverEdit = edit;
    m_hoverClear = clear;
    update();
}

void GoalRow::leaveEvent(QEvent *)
{
    if (!m_hoverPause && !m_hoverEdit && !m_hoverClear)
        return;
    m_hoverPause = false;
    m_hoverEdit = false;
    m_hoverClear = false;
    update();
}

void GoalRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !shown()) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const QString phase = m_goal.value(QStringLiteral("phase")).toString();
    if (m_pauseRect.contains(pos)) {
        m_confirmClear = false;
        m_facade->goalAction(phase == QLatin1String("paused")
                                     || phase == QLatin1String("blocked")
                                 ? QStringLiteral("resume")
                                 : QStringLiteral("pause"));
        return;
    }
    if (m_editRect.contains(pos)) {
        m_confirmClear = false;
        emit editRequested(m_goal.value(QStringLiteral("objective")).toString(),
                           m_goal.value(QStringLiteral("maxRounds")).toInt());
        return;
    }
    if (!m_clearRect.contains(pos)) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (!m_confirmClear) {
        m_confirmClear = true;
        update();
        m_clearReset->start();
        return;
    }
    m_confirmClear = false;
    m_clearReset->stop();
    m_facade->goalAction(QStringLiteral("clear"));
}
