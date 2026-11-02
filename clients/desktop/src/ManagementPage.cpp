#include "ManagementPage.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QColor>
#include <QDebug>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLinearGradient>
#include <QList>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

#include <functional>

namespace {

QLabel *proseLabel(QWidget *parent, const QFont &font, const QColor &colour)
{
    auto *label = new QLabel(parent);
    label->setFont(font);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
    return label;
}

void setProseColour(QLabel *label, const QColor &colour)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
}

void applyFieldPalette(QWidget *field)
{
    Theme *theme = Theme::instance();
    QPalette palette = field->palette();
    palette.setColor(QPalette::Base, theme->fieldFill());
    palette.setColor(QPalette::Text, theme->fg());
    palette.setColor(QPalette::Highlight, theme->accent());
    palette.setColor(QPalette::HighlightedText, theme->accentFg());
    palette.setColor(QPalette::PlaceholderText, theme->dim());
    field->setPalette(palette);
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

class Line : public QWidget
{
public:
    Line(const QFont &font, const QColor &colour, QWidget *parent = nullptr,
         Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter)
        : QWidget(parent)
        , m_font(font)
        , m_colour(colour)
        , m_alignment(alignment)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(QFontMetrics(font).height());
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setColour(const QColor &colour)
    {
        if (m_colour == colour)
            return;
        m_colour = colour;
        update();
    }

    void setHoverColour(const QColor &colour)
    {
        m_hoverColour = colour;
        setAttribute(Qt::WA_Hover, true);
    }

    void setClick(std::function<void()> handler)
    {
        m_click = std::move(handler);
        setCursor(m_click ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (m_click)
            setAttribute(Qt::WA_Hover, true);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(m_font);
        return QSize(metrics.horizontalAdvance(m_text) + 2 * Theme::instance()->space1(),
                     metrics.height());
    }

    QSize minimumSizeHint() const override { return QSize(0, QFontMetrics(m_font).height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QColor colour =
            m_click && underMouse() && m_hoverColour.isValid() ? m_hoverColour : m_colour;
        Chrome::paintLine(painter, rect(), m_font, colour, m_text, m_alignment);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_click
            && rect().contains(event->position().toPoint())) {
            m_click();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEnterEvent *) override
    {
        if (m_click)
            update();
    }

    void leaveEvent(QEvent *) override
    {
        if (m_click)
            update();
    }

private:
    QFont m_font;
    QColor m_colour;
    QColor m_hoverColour;
    QString m_text;
    Qt::Alignment m_alignment;
    std::function<void()> m_click;
};

class Rule : public QWidget
{
public:
    explicit Rule(const QColor &colour, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_colour(colour)
    {
        setFixedHeight(Theme::instance()->hairline());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), m_colour);
    }

private:
    QColor m_colour;
};

class Panel : public QWidget
{
public:
    explicit Panel(QWidget *parent = nullptr) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal inset = theme->hairline() / 2.0;
        const QRectF body = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
        painter.setBrush(theme->panel());
        painter.setPen(QPen(theme->border(), theme->hairline()));
        const int radius = theme->radius();
        painter.drawRoundedRect(body, radius, radius);
    }
};

class PushButton : public QWidget
{
public:
    explicit PushButton(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_text(text)
    {
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setHandler(std::function<void()> handler) { m_click = std::move(handler); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
        return QSize(qMax(8 * theme->space4(), metrics.horizontalAdvance(m_text) + theme->space6()),
                     theme->controlHeight());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            update();
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (!isEnabled())
            painter.setOpacity(0.55);

        const bool down = m_pressed && isEnabled();
        const bool hot = underMouse() && isEnabled();
        const qreal inset = theme->hairline() / 2.0;
        const QRectF body = QRectF(rect()).adjusted(inset, inset, -inset, -inset);

        QLinearGradient face(body.topLeft(), body.bottomLeft());
        if (down) {
            face.setColorAt(0.0, theme->buttonPressed());
            face.setColorAt(1.0, theme->buttonPressed());
        } else if (hot) {
            face.setColorAt(0.0, theme->buttonHover());
            face.setColorAt(1.0, theme->buttonGradientTop());
        } else {
            face.setColorAt(0.0, theme->buttonGradientTop());
            face.setColorAt(1.0, theme->buttonGradientBottom());
        }

        const int radius = theme->radius();
        painter.setBrush(face);
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, radius, radius);

        painter.setPen(QPen(theme->buttonBorderBottom(), theme->hairline()));
        painter.drawLine(QPointF(body.left() + radius, body.bottom()),
                         QPointF(body.right() - radius, body.bottom()));
        if (!down) {
            painter.setPen(QPen(theme->insetHighlight(), theme->hairline()));
            painter.drawLine(QPointF(body.left() + radius, body.top() + theme->hairline()),
                             QPointF(body.right() - radius, body.top() + theme->hairline()));
        }

        const QFont font = Chrome::uiFont(theme->fontUI());
        const QRectF label = body.adjusted(theme->space2(), 0, -theme->space2(), 0)
                                 .translated(0, down ? theme->hairline() : 0);
        if (isEnabled()) {
            painter.setFont(font);
            painter.setPen(theme->textShadow());
            painter.drawText(label.translated(0, -theme->hairline()), Qt::AlignCenter, m_text);
        }
        painter.setFont(font);
        painter.setPen(isEnabled() ? theme->fg() : theme->faint());
        painter.drawText(label, Qt::AlignCenter, m_text);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            m_pressed = true;
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool wasPressed = m_pressed;
        m_pressed = false;
        update();
        if (wasPressed && event->button() == Qt::LeftButton
            && rect().contains(event->position().toPoint()) && m_click) {
            m_click();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QString m_text;
    bool m_pressed = false;
    std::function<void()> m_click;
};

class Switch : public QWidget
{
public:
    Switch(bool on, bool switchable, std::function<void()> onToggle, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_on(on)
        , m_switchable(switchable)
        , m_toggle(std::move(onToggle))
    {
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(m_switchable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        return QSize(5 * theme->space4(), 2 * theme->space5() + theme->space1());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (!m_switchable)
            painter.setOpacity(0.45);

        const qreal inset = theme->hairline() / 2.0;
        const QRectF track = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
        painter.setPen(QPen(theme->borderStrong(), theme->hairline()));
        painter.setBrush(m_on ? theme->accent() : theme->fieldFill());
        painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

        const int knob = 2 * theme->space4();
        const int margin = (height() - knob) / 2;
        const QRectF knobRect(m_on ? width() - knob - margin : margin, margin, knob, knob);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_on ? theme->accentFg() : theme->faint());
        painter.drawEllipse(knobRect);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_switchable
            && rect().contains(event->position().toPoint())) {
            m_toggle();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    bool m_on;
    bool m_switchable;
    std::function<void()> m_toggle;
};

class TaskRow : public QWidget
{
public:
    TaskRow(const QVariantMap &task, std::function<void()> onClick, QWidget *parent)
        : QWidget(parent)
        , m_task(task)
        , m_click(std::move(onClick))
    {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(2 * Theme::instance()->rowHeight());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QString id() const { return m_task.value(QStringLiteral("id")).toString(); }

    void setSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (m_selected || m_hover) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme->raised());
            const int radius = theme->radius();
            painter.drawRoundedRect(QRectF(rect()), radius, radius);
        }

        const QFont titleFont = Chrome::uiFont(theme->fontUI());
        const QFont detailFont = Chrome::uiFont(theme->fontData());
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics detailMetrics(detailFont);
        const int inset = theme->space3();
        const int block = titleMetrics.height() + theme->space1() + detailMetrics.height();
        int top = (height() - block) / 2;
        const QRect titleRect(inset, top, width() - 2 * inset, titleMetrics.height());
        top += titleMetrics.height() + theme->space1();
        const QRect detailRect(inset, top, width() - 2 * inset, detailMetrics.height());

        const QString status = m_task.value(QStringLiteral("status")).toString();
        Chrome::paintLine(painter, titleRect, titleFont, theme->fg(),
                          m_task.value(QStringLiteral("title")).toString());
        Chrome::paintLine(painter, detailRect, detailFont,
                          status == QLatin1String("active") ? theme->dim() : theme->caption(),
                          detailText());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())
            && m_click) {
            m_click();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEnterEvent *) override
    {
        m_hover = true;
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }

private:
    QString detailText() const
    {
        const QString status = m_task.value(QStringLiteral("status")).toString();
        const QString rule = m_task.value(QStringLiteral("rule")).toString();
        if (status != QLatin1String("active"))
            return QStringLiteral("inactive \u00b7 ") + rule;
        const QString when =
            m_task.value(QStringLiteral("due")).toBool()
                ? QStringLiteral("due now")
                : QStringLiteral("next ") + m_task.value(QStringLiteral("nextRun")).toString();
        return when + QStringLiteral(" \u00b7 ") + rule;
    }

    QVariantMap m_task;
    bool m_selected = false;
    bool m_hover = false;
    std::function<void()> m_click;
};

}

class ManagementPage::Bar : public QWidget
{
public:
    explicit Bar(QWidget *parent)
        : QWidget(parent)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("managementBar"));
        setFixedHeight(theme->headerHeight());

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(theme->space4(), 0, theme->space4(), 0);
        layout->setSpacing(theme->space3());

        m_title = new Line(Chrome::uiFont(theme->fontProse(), true), theme->fg(), this);
        m_title->setObjectName(QStringLiteral("managementTitle"));
        m_subtitle = new Line(Chrome::uiFont(theme->fontData()), theme->dim(), this);
        m_refresh = new PushButton(QStringLiteral("Refresh"), this);
        m_refresh->setObjectName(QStringLiteral("managementRefresh"));
        m_close = new PushButton(QStringLiteral("Close"), this);
        m_close->setObjectName(QStringLiteral("managementClose"));

        layout->addWidget(m_title);
        layout->addWidget(m_subtitle, 1);
        layout->addWidget(m_refresh);
        layout->addWidget(m_close);
    }

    void setTitle(const QString &title) { m_title->setText(title); }
    void setSubtitle(const QString &subtitle) { m_subtitle->setText(subtitle); }
    void setRefreshHandler(std::function<void()> handler)
    {
        m_refresh->setHandler(std::move(handler));
    }
    void setCloseHandler(std::function<void()> handler) { m_close->setHandler(std::move(handler)); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        painter.fillRect(QRect(0, height() - theme->hairline(), width(), theme->hairline()),
                         theme->border());
    }

private:
    Line *m_title;
    Line *m_subtitle;
    PushButton *m_refresh;
    PushButton *m_close;
};

class ManagementPage::AutomationPage : public QWidget
{
public:
    AutomationPage(ClientFacade *facade, std::function<void()> onClose, QWidget *parent)
        : QWidget(parent)
        , m_facade(facade)
        , m_close(std::move(onClose))
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("automationPage"));

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                  theme->space4());
        outer->setSpacing(theme->space3());

        m_notice = proseLabel(this, Chrome::uiFont(theme->fontData()), theme->dim());
        outer->addWidget(m_notice);

        auto *body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(theme->space4());
        outer->addLayout(body, 1);

        m_listPanel = new Panel(this);
        auto *listLayout = new QVBoxLayout(m_listPanel);
        listLayout->setContentsMargins(theme->space3(), theme->space3(), theme->space3(),
                                       theme->space3());
        listLayout->setSpacing(theme->space2());

        m_search = new QLineEdit(m_listPanel);
        m_search->setObjectName(QStringLiteral("automationTaskSearch"));
        m_search->setPlaceholderText(QStringLiteral("Search tasks"));
        m_search->setFont(Chrome::uiFont(theme->fontUI()));
        m_search->setFixedHeight(theme->fieldHeight());
        applyFieldPalette(m_search);
        listLayout->addWidget(m_search);

        m_listScroll = new QScrollArea(m_listPanel);
    Chrome::styleScrollBar(m_listScroll->verticalScrollBar());
        m_listScroll->setObjectName(QStringLiteral("automationTaskList"));
        m_listScroll->setFrameShape(QFrame::NoFrame);
        m_listScroll->setWidgetResizable(true);
        m_listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_rowsHost = new QWidget;
        m_rowsLayout = new QVBoxLayout(m_rowsHost);
        m_rowsLayout->setContentsMargins(0, 0, 0, 0);
        m_rowsLayout->setSpacing(theme->space1());
        m_rowsLayout->addStretch(1);
        m_listScroll->setWidget(m_rowsHost);
        listLayout->addWidget(m_listScroll, 1);

        m_listEmpty = proseLabel(m_listPanel, Chrome::uiFont(theme->fontData()), theme->dim());
        listLayout->addWidget(m_listEmpty);

        body->addWidget(m_listPanel);

        m_detailPanel = new Panel(this);
        auto *detailLayout = new QVBoxLayout(m_detailPanel);
        detailLayout->setContentsMargins(0, 0, 0, 0);
        detailLayout->setSpacing(0);

        m_detailStack = new QStackedWidget(m_detailPanel);
        detailLayout->addWidget(m_detailStack);

        auto *emptyPage = new QWidget;
        auto *emptyLayout = new QVBoxLayout(emptyPage);
        emptyLayout->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                        theme->space4());
        emptyLayout->setSpacing(0);
        m_detailEmpty = proseLabel(emptyPage, Chrome::uiFont(theme->fontData()), theme->dim());
        m_detailEmpty->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        emptyLayout->addStretch(1);
        emptyLayout->addWidget(m_detailEmpty);
        emptyLayout->addStretch(1);

        m_detailScroll = new QScrollArea;
        m_detailScroll->setFrameShape(QFrame::NoFrame);
        m_detailScroll->setWidgetResizable(true);
        m_detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        Chrome::styleScrollBar(m_detailScroll->verticalScrollBar());
        m_detailHost = new QWidget;
        m_detailColumn = new QVBoxLayout(m_detailHost);
        m_detailColumn->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                           theme->space4());
        m_detailColumn->setSpacing(theme->space3());
        m_detailScroll->setWidget(m_detailHost);

        m_detailStack->addWidget(emptyPage);
        m_detailStack->addWidget(m_detailScroll);

        auto *statusRow = new QHBoxLayout;
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(theme->space2());
        m_status = new Line(Chrome::uiFont(theme->fontData()), theme->caption(), m_detailHost);
        m_ruleLine = new Line(Chrome::uiFont(theme->fontData()), theme->dim(), m_detailHost);
        statusRow->addWidget(m_status);
        statusRow->addWidget(m_ruleLine, 1);
        m_detailColumn->addLayout(statusRow);

        m_nextRun = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()), theme->dim());
        m_detailColumn->addWidget(m_nextRun);

        m_sessionLink = new Line(Chrome::uiFont(theme->fontData()), theme->accent(), m_detailHost);
        m_sessionLink->setHoverColour(theme->fg());
        m_sessionLink->setClick([this]() {
            const QVariantMap task = selectedTask();
            if (task.isEmpty())
                return;
            const QString sessionId = task.value(QStringLiteral("sessionId")).toString();
            if (sessionId.isEmpty())
                return;
            m_close();
            m_facade->followSession(sessionId);
        });
        m_detailColumn->addWidget(m_sessionLink);

        m_detailColumn->addWidget(new Rule(theme->borderL1(), m_detailHost));

        auto *nameCaption =
            new Line(Chrome::uiFont(theme->fontData()), theme->dim(), m_detailHost);
        nameCaption->setText(QStringLiteral("Name"));
        m_detailColumn->addWidget(nameCaption);

        m_title = new QLineEdit(m_detailHost);
        m_title->setObjectName(QStringLiteral("automationTaskName"));
        m_title->setFont(Chrome::uiFont(theme->fontUI()));
        m_title->setFixedHeight(theme->fieldHeight());
        applyFieldPalette(m_title);
        m_detailColumn->addWidget(m_title);

        auto *instructionCaption =
            new Line(Chrome::uiFont(theme->fontData()), theme->dim(), m_detailHost);
        instructionCaption->setText(QStringLiteral("Instruction"));
        m_detailColumn->addWidget(instructionCaption);

        m_prompt = new QTextEdit(m_detailHost);
        m_prompt->setObjectName(QStringLiteral("automationTaskInstruction"));
        m_prompt->setAcceptRichText(false);
        m_prompt->setTabChangesFocus(true);
        m_prompt->setFont(Chrome::uiFont(theme->fontUI()));
        m_prompt->setFixedHeight(4 * theme->fieldHeight());
        m_prompt->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_prompt->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        applyFieldPalette(m_prompt);
        m_detailColumn->addWidget(m_prompt);

        m_inactiveNote = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()),
                                    theme->caption());
        m_inactiveNote->setText(QStringLiteral(
            "Inactive tasks are read-only. Their timing cannot be edited."));
        m_detailColumn->addWidget(m_inactiveNote);

        m_scheduleNote = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()),
                                    theme->caption());
        m_scheduleNote->setText(QStringLiteral(
            "The rule is shown as the host stores it. This page edits the name and the "
            "instruction; timing is changed in the web UI's task page."));
        m_detailColumn->addWidget(m_scheduleNote);

        auto *actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(theme->space2());
        m_save = new PushButton(QStringLiteral("Save changes"), m_detailHost);
        m_save->setObjectName(QStringLiteral("automationTaskSave"));
        m_save->setHandler([this]() {
            const QVariantMap task = selectedTask();
            if (task.isEmpty())
                return;
            m_facade->saveScheduleTask(task.value(QStringLiteral("id")).toString(),
                                       m_title->text(), m_prompt->toPlainText());
        });
        m_delete = new PushButton(QStringLiteral("Delete"), m_detailHost);
        m_delete->setObjectName(QStringLiteral("automationTaskDelete"));
        m_delete->setHandler([this]() { confirmDelete(); });
        actions->addWidget(m_save);
        actions->addWidget(m_delete);
        actions->addStretch(1);
        m_detailColumn->addLayout(actions);

        m_detailColumn->addWidget(new Rule(theme->borderL1(), m_detailHost));

        auto *historyRow = new QHBoxLayout;
        historyRow->setContentsMargins(0, 0, 0, 0);
        historyRow->setSpacing(theme->space2());
        auto *historyCaption =
            new Line(Chrome::uiFont(theme->fontUI()), theme->fg(), m_detailHost);
        historyCaption->setText(QStringLiteral("Delivery records"));
        m_historyReload = new PushButton(QStringLiteral("Reload"), m_detailHost);
        m_historyReload->setObjectName(QStringLiteral("automationTaskHistory"));
        m_historyReload->setHandler([this]() {
            const QVariantMap task = selectedTask();
            if (!task.isEmpty())
                m_facade->loadScheduleHistory(task.value(QStringLiteral("id")).toString());
        });
        historyRow->addWidget(historyCaption);
        historyRow->addWidget(m_historyReload);
        historyRow->addStretch(1);
        m_detailColumn->addLayout(historyRow);

        m_historyEmpty = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()), theme->dim());
        m_historyEmpty->setText(QStringLiteral("No delivery record available."));
        m_detailColumn->addWidget(m_historyEmpty);

        m_historyLayout = new QVBoxLayout;
        m_historyLayout->setContentsMargins(0, 0, 0, 0);
        m_historyLayout->setSpacing(theme->hairline());
        m_detailColumn->addLayout(m_historyLayout);

        m_detailColumn->addStretch(1);

        connect(m_search, &QLineEdit::textChanged, this, [this]() { rebuildList(); });
        connect(m_title, &QLineEdit::textChanged, this, [this]() { updateActions(); });

        reload();
    }

    void reload()
    {
        Theme *theme = Theme::instance();
        m_notice->setText(m_facade->scheduleNotice());
        setProseColour(m_notice,
                       m_facade->scheduleNoticeError() ? theme->danger() : theme->dim());
        m_notice->setVisible(!m_notice->text().isEmpty());
        rebuildList();
        syncDetail();
        syncHistory();
    }

    void reloadHistory()
    {
        syncDetail();
        syncHistory();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        Theme *theme = Theme::instance();
        const int preferred = qRound(width() * 0.36);
        m_listPanel->setFixedWidth(
            qBound(24 * theme->space4(), preferred, 40 * theme->space4()));
    }

private:
    QVariantMap selectedTask() const
    {
        if (m_selectedId.isEmpty())
            return QVariantMap();
        const QVariantList all = m_facade->scheduleTasks();
        for (const QVariant &entry : all) {
            const QVariantMap task = entry.toMap();
            if (task.value(QStringLiteral("id")).toString() == m_selectedId)
                return task;
        }
        return QVariantMap();
    }

    void rebuildList()
    {
        const QString query = m_search->text().trimmed().toLower();
        const QVariantList all = m_facade->scheduleTasks();
        QVariantList shown;
        for (const QVariant &entry : all) {
            const QVariantMap task = entry.toMap();
            if (!query.isEmpty()) {
                const QString hay = (task.value(QStringLiteral("title")).toString()
                                     + QLatin1Char(' ')
                                     + task.value(QStringLiteral("prompt")).toString()
                                     + QLatin1Char(' ')
                                     + task.value(QStringLiteral("sessionTitle")).toString())
                                        .toLower();
                if (!hay.contains(query))
                    continue;
            }
            shown.append(task);
        }

        clearLayout(m_rowsLayout);
        m_rows.clear();
        for (const QVariant &entry : shown) {
            const QVariantMap task = entry.toMap();
            auto *row = new TaskRow(task, [this, task]() { selectTask(task); }, m_rowsHost);
            row->setSelected(row->id() == m_selectedId);
            m_rowsLayout->addWidget(row);
            m_rows.append(row);
        }
        m_rowsLayout->addStretch(1);

        QString empty;
        if (m_facade->scheduleLoading()) {
            empty = QStringLiteral("Loading tasks\u2026");
        } else if (m_facade->transportName() != QLatin1String("dsh")) {
            empty = QStringLiteral(
                "This transport cannot reach the host\u2019s task catalog.");
        } else if (!m_search->text().trimmed().isEmpty()) {
            empty = QStringLiteral("No matching tasks.");
        } else {
            empty = QStringLiteral(
                "No tasks yet. Tasks created in your sessions appear here.");
        }
        m_listEmpty->setText(empty);
        m_listEmpty->setVisible(m_rows.isEmpty());
    }

    void syncDetail()
    {
        const QVariantMap task = selectedTask();
        if (task.isEmpty()) {
            m_selectedId.clear();
            m_loadedId.clear();
            m_detailEmpty->setText(
                m_facade->scheduleLoading()
                    ? QStringLiteral("Reading the task catalog\u2026")
                    : QStringLiteral("Select a task to see its rule, its instruction and its "
                                     "delivery records."));
            m_detailStack->setCurrentIndex(0);
            return;
        }

        m_detailStack->setCurrentIndex(1);
        const QString id = task.value(QStringLiteral("id")).toString();
        if (m_loadedId != id) {
            m_loadedId = id;
            m_title->setText(task.value(QStringLiteral("title")).toString());
            m_prompt->setPlainText(task.value(QStringLiteral("prompt")).toString());
        }
        applyDetail(task);
    }

    void applyDetail(const QVariantMap &task)
    {
        Theme *theme = Theme::instance();
        const bool active = task.value(QStringLiteral("status")).toString()
                            == QLatin1String("active");
        const bool due = task.value(QStringLiteral("due")).toBool();

        m_status->setText(active ? (due ? QStringLiteral("Active \u00b7 due now")
                                        : QStringLiteral("Active"))
                                 : QStringLiteral("Inactive"));
        m_status->setColour(active ? theme->running() : theme->caption());
        m_ruleLine->setText(task.value(QStringLiteral("rule")).toString());

        const QString nextRun = task.value(QStringLiteral("nextRun")).toString();
        const QString lastDelivered =
            task.value(QStringLiteral("lastDeliveredAt")).toString();
        m_nextRun->setText(QStringLiteral("Next run ") + nextRun
                           + (lastDelivered.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" \u00b7 last delivered ") + lastDelivered));

        const QString sessionId = task.value(QStringLiteral("sessionId")).toString();
        const QString sessionTitle = task.value(QStringLiteral("sessionTitle")).toString();
        m_sessionLink->setText(QStringLiteral("From ")
                               + (sessionTitle.isEmpty() ? sessionId : sessionTitle));

        m_inactiveNote->setVisible(!active);
        updateActions();
    }

    void updateActions()
    {
        const QVariantMap task = selectedTask();
        const bool active = !task.isEmpty()
                            && task.value(QStringLiteral("status")).toString()
                                   == QLatin1String("active");
        const bool saving = m_facade->scheduleSaving();
        const bool loading = m_facade->scheduleHistoryLoading();

        m_title->setEnabled(active);
        m_prompt->setEnabled(active);
        m_save->setText(saving ? QStringLiteral("Saving\u2026")
                               : QStringLiteral("Save changes"));
        m_save->setEnabled(!saving && active && !m_title->text().trimmed().isEmpty());
        m_delete->setEnabled(!saving && !task.isEmpty());
        m_historyReload->setText(loading ? QStringLiteral("Loading\u2026")
                                         : QStringLiteral("Reload"));
        m_historyReload->setEnabled(!loading && !task.isEmpty());
    }

    void syncHistory()
    {
        clearLayout(m_historyLayout);
        const QVariantMap task = selectedTask();
        const QString id = task.value(QStringLiteral("id")).toString();
        const bool forSelected =
            !id.isEmpty() && m_facade->scheduleHistoryFor() == id;
        const QVariantList records = forSelected ? m_facade->scheduleHistory()
                                                 : QVariantList();
        const Theme *theme = Theme::instance();

        for (const QVariant &entry : records) {
            const QVariantMap record = entry.toMap();
            auto *delivered = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()),
                                         theme->dim());
            delivered->setText(QStringLiteral("Delivered ")
                               + record.value(QStringLiteral("deliveredLabel")).toString()
                               + QStringLiteral(" (scheduled ")
                               + record.value(QStringLiteral("scheduledLabel")).toString()
                               + QLatin1Char(')'));
            m_historyLayout->addWidget(delivered);

            const QString prompt = record.value(QStringLiteral("prompt")).toString();
            if (!prompt.isEmpty()) {
                auto *line = proseLabel(m_detailHost, Chrome::uiFont(theme->fontData()),
                                        theme->caption());
                line->setText(prompt);
                m_historyLayout->addWidget(line);
            }
        }

        m_historyEmpty->setVisible(!id.isEmpty() && forSelected && records.isEmpty()
                                   && !m_facade->scheduleHistoryLoading());
    }

    void selectTask(const QVariantMap &task)
    {
        const QString id = task.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            return;
        m_selectedId = id;
        m_loadedId.clear();
        for (TaskRow *row : m_rows)
            row->setSelected(row->id() == id);
        syncDetail();
        m_facade->loadScheduleHistory(id);
    }

    void confirmDelete()
    {
        const QVariantMap task = selectedTask();
        if (task.isEmpty())
            return;
        const QString id = task.value(QStringLiteral("id")).toString();
        const QString title = task.value(QStringLiteral("title")).toString();

        auto *dialog = new QMessageBox(window());
        dialog->setObjectName(QStringLiteral("deleteTaskDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setIcon(QMessageBox::Warning);
        dialog->setWindowTitle(QStringLiteral("Delete this task?"));
        dialog->setText(title.isEmpty() ? QStringLiteral("This task") : title);
        dialog->setInformativeText(QStringLiteral(
            "The task and its saved delivery records are removed. Messages it already "
            "queued are left alone."));
        QAbstractButton *deleteButton =
            dialog->addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
        QAbstractButton *cancelButton =
            dialog->addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        dialog->setDefaultButton(qobject_cast<QPushButton *>(cancelButton));
        dialog->setEscapeButton(cancelButton);

        connect(dialog, &QMessageBox::finished, this,
                [this, dialog, deleteButton, id]() {
                    if (dialog->clickedButton() == deleteButton)
                        m_facade->deleteScheduleTask(id);
                });
        dialog->open();
    }

    ClientFacade *m_facade;
    std::function<void()> m_close;

    QLabel *m_notice = nullptr;
    Panel *m_listPanel = nullptr;
    QLineEdit *m_search = nullptr;
    QScrollArea *m_listScroll = nullptr;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_listEmpty = nullptr;
    QList<TaskRow *> m_rows;

    Panel *m_detailPanel = nullptr;
    QStackedWidget *m_detailStack = nullptr;
    QLabel *m_detailEmpty = nullptr;
    QScrollArea *m_detailScroll = nullptr;
    QWidget *m_detailHost = nullptr;
    QVBoxLayout *m_detailColumn = nullptr;
    Line *m_status = nullptr;
    Line *m_ruleLine = nullptr;
    QLabel *m_nextRun = nullptr;
    Line *m_sessionLink = nullptr;
    QLineEdit *m_title = nullptr;
    QTextEdit *m_prompt = nullptr;
    QLabel *m_inactiveNote = nullptr;
    QLabel *m_scheduleNote = nullptr;
    PushButton *m_save = nullptr;
    PushButton *m_delete = nullptr;
    PushButton *m_historyReload = nullptr;
    QLabel *m_historyEmpty = nullptr;
    QVBoxLayout *m_historyLayout = nullptr;

    QString m_selectedId;
    QString m_loadedId;
};

class ManagementPage::PluginsPage : public QWidget
{
public:
    PluginsPage(ClientFacade *facade, QWidget *parent)
        : QWidget(parent)
        , m_facade(facade)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("pluginsPage"));

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                  theme->space4());
        outer->setSpacing(theme->space3());

        auto *top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(theme->space3());
        m_search = new QLineEdit(this);
        m_search->setObjectName(QStringLiteral("pluginSearch"));
        m_search->setPlaceholderText(QStringLiteral("Search plugins"));
        m_search->setFont(Chrome::uiFont(theme->fontUI()));
        m_search->setFixedHeight(theme->fieldHeight());
        applyFieldPalette(m_search);
        m_count = new Line(Chrome::uiFont(theme->fontData()), theme->dim(), this);
        m_count->setObjectName(QStringLiteral("pluginCount"));
        top->addWidget(m_search, 1);
        top->addWidget(m_count);
        outer->addLayout(top);

        m_notice = proseLabel(this, Chrome::uiFont(theme->fontData()), theme->dim());
        outer->addWidget(m_notice);

        m_scroll = new QScrollArea(this);
    Chrome::styleScrollBar(m_scroll->verticalScrollBar());
        m_scroll->setObjectName(QStringLiteral("pluginList"));
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setWidgetResizable(true);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        m_host = new QWidget;
        auto *hostLayout = new QVBoxLayout(m_host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(theme->space4());

        m_column = new QVBoxLayout;
        m_column->setContentsMargins(0, 0, 0, 0);
        m_column->setSpacing(theme->space4());
        hostLayout->addLayout(m_column);

        m_empty = proseLabel(m_host, Chrome::uiFont(theme->fontData()), theme->dim());
        hostLayout->addWidget(m_empty);

        m_footer = proseLabel(m_host, Chrome::uiFont(theme->fontData()), theme->caption());
        m_footer->setText(QStringLiteral(
            "A switch selects or drops that bundle\u2019s layer in the host\u2019s profile, "
            "and the list is re-read once the host answers. Installing and removing bundles "
            "are not offered here."));
        hostLayout->addWidget(m_footer);
        hostLayout->addStretch(1);

        m_scroll->setWidget(m_host);
        outer->addWidget(m_scroll, 1);

        connect(m_search, &QLineEdit::textChanged, this, [this]() { rebuild(); });
        reload();
    }

    void reload()
    {
        Theme *theme = Theme::instance();
        m_notice->setText(m_facade->pluginsNotice());
        setProseColour(m_notice,
                       m_facade->pluginsNoticeError() ? theme->danger() : theme->dim());
        m_notice->setVisible(!m_notice->text().isEmpty());
        rebuild();
    }

private:
    QVariantList shownSections() const
    {
        const QString query = m_search->text().trimmed().toLower();
        const QVariantList all = m_facade->pluginInventory();
        QVariantList shown;
        for (const QVariant &entry : all) {
            const QVariantMap section = entry.toMap();
            QVariantList rows;
            const QVariantList source = section.value(QStringLiteral("rows")).toList();
            for (const QVariant &rowEntry : source) {
                const QVariantMap row = rowEntry.toMap();
                if (!query.isEmpty()) {
                    const QString hay = (row.value(QStringLiteral("title")).toString()
                                         + QLatin1Char(' ')
                                         + row.value(QStringLiteral("name")).toString()
                                         + QLatin1Char(' ')
                                         + row.value(QStringLiteral("description")).toString())
                                            .toLower();
                    if (!hay.contains(query))
                        continue;
                }
                rows.append(row);
            }
            if (rows.isEmpty())
                continue;
            QVariantMap kept = section;
            kept.insert(QStringLiteral("rows"), rows);
            shown.append(kept);
        }
        return shown;
    }

    int shownCount() const
    {
        int total = 0;
        for (const QVariant &entry : shownSections())
            total += entry.toMap().value(QStringLiteral("rows")).toList().size();
        return total;
    }

    QString countText() const
    {
        if (m_facade->pluginsLoading())
            return QStringLiteral("Reading plugins\u2026");
        const int count = shownCount();
        return count == 1 ? QStringLiteral("1 bundle")
                          : QString::number(count) + QStringLiteral(" bundles");
    }

    QString emptyText() const
    {
        if (m_facade->pluginsLoading())
            return QStringLiteral("Reading plugins\u2026");
        if (m_facade->transportName() != QLatin1String("dsh"))
            return QStringLiteral(
                "This transport cannot reach the host\u2019s plugin bundles.");
        if (!m_search->text().trimmed().isEmpty())
            return QStringLiteral("No matching plugin bundles.");
        return QStringLiteral(
            "No plugin bundles are installed in this deployment.");
    }

    QWidget *buildRow(const QVariantMap &row, QWidget *parent)
    {
        Theme *theme = Theme::instance();
        auto *container = new QWidget(parent);
        auto *outer = new QVBoxLayout(container);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        auto *body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(theme->space3());

        auto *left = new QVBoxLayout;
        left->setContentsMargins(0, 0, 0, 0);
        left->setSpacing(theme->hairline());

        const QString title = row.value(QStringLiteral("title")).toString();
        const QString name = row.value(QStringLiteral("name")).toString();
        const QString version = row.value(QStringLiteral("version")).toString();
        const QString description = row.value(QStringLiteral("description")).toString();
        const QString status = row.value(QStringLiteral("status")).toString();
        const QString errorSummary = row.value(QStringLiteral("errorSummary")).toString();
        const QString readOnlyText = row.value(QStringLiteral("readOnlyText")).toString();
        const bool changing = row.value(QStringLiteral("changing")).toBool();
        const bool hasError = row.value(QStringLiteral("hasError")).toBool();
        const bool enabled = row.value(QStringLiteral("enabled")).toBool();
        const bool switchable = row.value(QStringLiteral("switchable")).toBool();
        const bool readOnly = row.value(QStringLiteral("readOnly")).toBool();

        auto *titleLine = new Line(Chrome::uiFont(theme->fontUI()), theme->fg(), container);
        titleLine->setText(title);
        left->addWidget(titleLine);

        QString meta = name == title ? QString() : name;
        if (!version.isEmpty())
            meta = meta.isEmpty() ? version : meta + QStringLiteral("  ") + version;
        if (!meta.isEmpty()) {
            auto *metaLine =
                new Line(Chrome::monoFont(theme->fontData()), theme->caption(), container);
            metaLine->setText(meta);
            left->addWidget(metaLine);
        }

        if (!description.isEmpty()) {
            auto *descriptionLabel =
                proseLabel(container, Chrome::uiFont(theme->fontData()), theme->caption());
            descriptionLabel->setText(description);
            left->addWidget(descriptionLabel);
        }

        auto *statusLine = new Line(Chrome::uiFont(theme->fontData()),
                                    hasError ? theme->danger()
                                             : (enabled ? theme->ok() : theme->caption()),
                                    container);
        statusLine->setText(changing ? QStringLiteral("Changing\u2026") : status);
        left->addWidget(statusLine);

        if (hasError && !errorSummary.isEmpty()) {
            auto *errorLabel =
                proseLabel(container, Chrome::uiFont(theme->fontData()), theme->danger());
            errorLabel->setText(errorSummary);
            left->addWidget(errorLabel);
        }

        if (readOnly) {
            auto *readOnlyLabel =
                proseLabel(container, Chrome::uiFont(theme->fontData()), theme->caption());
            readOnlyLabel->setText(readOnlyText);
            left->addWidget(readOnlyLabel);
        }

        body->addLayout(left, 1);

        auto *toggle = new Switch(enabled, switchable,
                                  [this, name, enabled]() {
                                      m_facade->setBundleEnabled(name, !enabled);
                                  },
                                  container);
        toggle->setObjectName(QStringLiteral("pluginSwitch"));
        toggle->setFixedSize(toggle->sizeHint());
        body->addWidget(toggle, 0, Qt::AlignVCenter);

        outer->addLayout(body);
        outer->addWidget(new Rule(theme->borderL1(), container));
        return container;
    }

    void rebuild()
    {
        Theme *theme = Theme::instance();
        const QVariantList sections = shownSections();

        clearLayout(m_column);
        for (const QVariant &entry : sections) {
            const QVariantMap section = entry.toMap();
            auto *sectionWidget = new QWidget(m_host);
            auto *sectionLayout = new QVBoxLayout(sectionWidget);
            sectionLayout->setContentsMargins(0, 0, 0, 0);
            sectionLayout->setSpacing(theme->space2());

            auto *title =
                new Line(Chrome::uiFont(theme->fontProse(), true), theme->fg(), sectionWidget);
            title->setText(section.value(QStringLiteral("title")).toString());
            sectionLayout->addWidget(title);

            auto *note = proseLabel(sectionWidget, Chrome::uiFont(theme->fontData()),
                                    theme->caption());
            note->setText(section.value(QStringLiteral("note")).toString());
            sectionLayout->addWidget(note);

            sectionLayout->addWidget(new Rule(theme->borderL1(), sectionWidget));

            const QVariantList rows = section.value(QStringLiteral("rows")).toList();
            for (const QVariant &rowEntry : rows)
                sectionLayout->addWidget(buildRow(rowEntry.toMap(), sectionWidget));

            m_column->addWidget(sectionWidget);
        }

        m_empty->setText(emptyText());
        m_empty->setVisible(sections.isEmpty());
        m_count->setText(countText());
    }

    ClientFacade *m_facade;
    QLineEdit *m_search = nullptr;
    Line *m_count = nullptr;
    QLabel *m_notice = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_host = nullptr;
    QVBoxLayout *m_column = nullptr;
    QLabel *m_empty = nullptr;
    QLabel *m_footer = nullptr;
};

ManagementPage::ManagementPage(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("managementPage"));
    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, theme->bg());
    setPalette(palette);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bar = new Bar(this);
    m_stack = new QStackedWidget(this);
    m_automation = new AutomationPage(facade, [this]() { emit closeRequested(); }, m_stack);
    m_plugins = new PluginsPage(facade, m_stack);
    m_stack->addWidget(m_automation);
    m_stack->addWidget(m_plugins);

    layout->addWidget(m_bar);
    layout->addWidget(m_stack, 1);

    m_bar->setRefreshHandler([this]() { refreshCurrent(); });
    m_bar->setCloseHandler([this]() { emit closeRequested(); });

    connect(m_facade, &ClientFacade::scheduleChanged, this,
            [this]() { m_automation->reload(); });
    connect(m_facade, &ClientFacade::scheduleHistoryChanged, this,
            [this]() { m_automation->reloadHistory(); });
    connect(m_facade, &ClientFacade::pluginsChanged, this,
            [this]() { m_plugins->reload(); });
    connect(m_facade, &ClientFacade::availableChanged, this, [this]() {
        m_automation->reload();
        m_plugins->reload();
    });

    applyTab();
}

void ManagementPage::setTab(const QString &tab)
{
    const bool known = tab.isEmpty() || tab == QLatin1String("automation")
                       || tab == QLatin1String("plugins");
    if (!known)
        qWarning() << "ManagementPage: unknown tab" << tab;
    m_tab = known ? tab : QString();
    applyTab();
    refreshCurrent();
}

void ManagementPage::applyTab()
{
    const bool automation = m_tab == QLatin1String("automation");
    m_bar->setTitle(automation ? QStringLiteral("Automation tasks")
                               : QStringLiteral("Plugins"));
    m_bar->setSubtitle(
        automation
            ? QStringLiteral("Reminders the host delivers to the session that created them.")
            : QStringLiteral("The plugin bundles this deployment installs, and the switch "
                             "for each."));

    if (automation)
        m_stack->setCurrentWidget(m_automation);
    else if (m_tab == QLatin1String("plugins"))
        m_stack->setCurrentWidget(m_plugins);
    else
        m_stack->setCurrentIndex(-1);
}

void ManagementPage::refreshCurrent()
{
    if (m_tab == QLatin1String("automation"))
        m_facade->refreshScheduleTasks();
    else if (m_tab == QLatin1String("plugins"))
        m_facade->refreshPluginInventory();
}
