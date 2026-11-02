#include "PendingBand.h"

#include "ShellChrome.h"
#include "Theme.h"

#include <QColor>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLayoutItem>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QList>
#include <QPalette>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariantMap>

#include <functional>

namespace {

constexpr int OptionColumns = 3;

class PendingText : public QWidget
{
public:
    PendingText(const QFont &font, const QColor &colour, QWidget *parent)
        : QWidget(parent)
        , m_font(font)
        , m_colour(colour)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(Theme::instance()->rowHeight());
    }

    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(QFontMetrics(m_font).horizontalAdvance(m_text), height());
    }

    QSize minimumSizeHint() const override { return QSize(0, height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        Chrome::paintLine(painter, rect(), m_font, m_colour, m_text);
    }

private:
    QFont m_font;
    QColor m_colour;
    QString m_text;
};

class PendingHeading : public QWidget
{
public:
    PendingHeading(const QString &text, QWidget *parent)
        : QWidget(parent)
        , m_text(text)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(Theme::instance()->rowHeight());
    }

    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        update();
    }

    QSize sizeHint() const override { return QSize(0, height()); }
    QSize minimumSizeHint() const override { return QSize(0, height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int dot = theme->space3();
        const QRectF mark(0, (height() - dot) / 2.0, dot, dot);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme->warn());
        painter.drawEllipse(mark);

        QRect textRect = rect();
        textRect.setLeft(dot + theme->space2());
        Chrome::paintLine(painter, textRect, Chrome::uiFont(theme->fontUI(), true), theme->fg(),
                          m_text);
    }

private:
    QString m_text;
};

class PendingButton : public QWidget
{
public:
    PendingButton(const QString &text, QWidget *parent)
        : QWidget(parent)
        , m_text(text)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(Theme::instance()->rowHeight());
    }

    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setHighlighted(bool highlighted)
    {
        if (highlighted == m_highlighted)
            return;
        m_highlighted = highlighted;
        update();
    }

    void setHandler(std::function<void()> handler) { m_click = std::move(handler); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
        return QSize(metrics.horizontalAdvance(m_text) + 2 * theme->space5(),
                     theme->rowHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, Theme::instance()->rowHeight()); }

protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            update();
    }

    void focusInEvent(QFocusEvent *event) override
    {
        QWidget::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !isEnabled()) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = true;
        update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const bool fire = m_pressed && isEnabled() && rect().contains(event->position().toPoint());
        m_pressed = false;
        update();
        if (fire)
            activate();
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Space) {
            activate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
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
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

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
            painter.drawLine(QPointF(body.left() + radius, body.top() + 1),
                             QPointF(body.right() - radius, body.top() + 1));
        }

        QRectF ring = body.adjusted(theme->hairline(), theme->hairline(),
                                    -theme->hairline(), -theme->hairline());
        if (m_highlighted && isEnabled()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            painter.drawRoundedRect(ring, radius, radius);
            ring = ring.adjusted(theme->hairline(), theme->hairline(),
                                 -theme->hairline(), -theme->hairline());
        }
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            painter.drawRoundedRect(ring, radius, radius);
        }

        Chrome::paintLine(painter, rect(), Chrome::uiFont(theme->fontUI()), theme->fg(), m_text,
                          Qt::AlignCenter);
    }

private:
    void activate()
    {
        if (m_click)
            m_click();
    }

    QString m_text;
    bool m_highlighted = false;
    bool m_pressed = false;
    std::function<void()> m_click;
};

QString bodyBlock(const QString &text, int pixelSize, const QColor &colour)
{
    Theme *theme = Theme::instance();
    QString escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral("<div style=\"font-family:%1;font-size:%2px;color:%3;\">%4</div>")
        .arg(theme->ui())
        .arg(pixelSize)
        .arg(colour.name())
        .arg(escaped);
}

} // namespace

class PendingBand::ApprovalCard : public QWidget
{
public:
    ApprovalCard(PendingBand *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                   theme->space4());
        layout->setSpacing(theme->space2());

        m_heading = new PendingHeading(QStringLiteral("Waiting for approval"), this);
        m_tool = new PendingText(Chrome::monoFont(theme->fontData()), theme->fg(), this);

        m_detail = new QPlainTextEdit(this);
        m_detail->setObjectName(QStringLiteral("approvalDetail"));
        m_detail->setReadOnly(true);
        m_detail->setFrameShape(QFrame::NoFrame);
        m_detail->setContextMenuPolicy(Qt::NoContextMenu);
        m_detail->setTabChangesFocus(true);
        m_detail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_detail->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_detail->setFont(Chrome::uiFont(theme->fontUI()));
        m_detail->document()->setDocumentMargin(theme->space2());
        m_detail->setFixedHeight(2 * theme->rowHeight());
        QPalette detailPalette = m_detail->palette();
        detailPalette.setColor(QPalette::Base, theme->fieldFill());
        detailPalette.setColor(QPalette::Text, theme->dim());
        m_detail->setPalette(detailPalette);
        m_detail->viewport()->setPalette(detailPalette);

        auto *answers = new QWidget(this);
        answers->setFixedHeight(theme->rowHeight());
        auto *answerLayout = new QHBoxLayout(answers);
        answerLayout->setContentsMargins(0, 0, 0, 0);
        answerLayout->setSpacing(theme->space2());
        m_reject = new PendingButton(QStringLiteral("Reject"), answers);
        m_allow = new PendingButton(QStringLiteral("Allow once"), answers);
        m_allow->setHighlighted(true);
        answerLayout->addStretch(1);
        answerLayout->addWidget(m_reject);
        answerLayout->addWidget(m_allow);

        layout->addWidget(m_heading);
        layout->addWidget(m_tool);
        layout->addWidget(m_detail);
        layout->addWidget(answers);

        m_reject->setHandler([this]() { answer(false); });
        m_allow->setHandler([this]() { answer(true); });

        setFixedHeight(cardHeight());
        hide();
    }

    bool shown() const { return !m_requestId.isEmpty(); }

    int cardHeight() const
    {
        Theme *theme = Theme::instance();
        return 2 * theme->space4() + 5 * theme->rowHeight() + 3 * theme->space2();
    }

    void show(const QString &requestId, const QString &toolName, const QString &detail)
    {
        if (requestId.isEmpty()) {
            dismiss(m_requestId);
            return;
        }
        m_requestId = requestId;
        m_tool->setText(toolName.isEmpty() ? QStringLiteral("a tool") : toolName);
        m_detail->setPlainText(detail.isEmpty()
                                   ? QStringLiteral("The agent is asking to run the tool above.")
                                   : detail);
        m_detail->moveCursor(QTextCursor::Start);
        setFixedHeight(cardHeight());
        setVisible(true);
        m_owner->syncDockHeight();
    }

    void dismiss(const QString &requestId)
    {
        if (requestId != m_requestId)
            return;
        m_requestId.clear();
        m_tool->setText(QString());
        m_detail->clear();
        hide();
        m_owner->syncDockHeight();
    }

private:
    void answer(bool allow)
    {
        if (m_requestId.isEmpty())
            return;
        const QString requestId = m_requestId;
        emit m_owner->approvalAnswered(requestId, allow);
        dismiss(requestId);
    }

    PendingBand *m_owner;
    PendingHeading *m_heading = nullptr;
    PendingText *m_tool = nullptr;
    QPlainTextEdit *m_detail = nullptr;
    PendingButton *m_reject = nullptr;
    PendingButton *m_allow = nullptr;
    QString m_requestId;
};

class PendingBand::QuestionCard : public QWidget
{
public:
    QuestionCard(PendingBand *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                   theme->space4());
        layout->setSpacing(theme->space2());

        m_heading = new PendingHeading(QStringLiteral("Your agent is asking"), this);

        m_body = new QTextEdit(this);
        m_body->setObjectName(QStringLiteral("questionBody"));
        m_body->setReadOnly(true);
        m_body->setFrameShape(QFrame::NoFrame);
        m_body->setContextMenuPolicy(Qt::NoContextMenu);
        m_body->setTabChangesFocus(true);
        m_body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_body->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_body->setFont(Chrome::uiFont(theme->fontUI()));
        m_body->setFixedHeight(2 * theme->rowHeight());
        QPalette bodyPalette = m_body->palette();
        bodyPalette.setColor(QPalette::Base, theme->fieldFill());
        bodyPalette.setColor(QPalette::Text, theme->fg());
        m_body->setPalette(bodyPalette);
        m_body->viewport()->setPalette(bodyPalette);

        m_options = new QWidget(this);
        m_optionLayout = new QGridLayout(m_options);
        m_optionLayout->setContentsMargins(0, 0, 0, 0);
        m_optionLayout->setHorizontalSpacing(theme->space1());
        m_optionLayout->setVerticalSpacing(0);
        for (int column = 0; column < OptionColumns; ++column)
            m_optionLayout->setColumnStretch(column, 1);

        auto *actions = new QWidget(this);
        actions->setFixedHeight(theme->rowHeight());
        auto *actionLayout = new QHBoxLayout(actions);
        actionLayout->setContentsMargins(0, 0, 0, 0);
        actionLayout->setSpacing(theme->space2());
        m_skip = new PendingButton(QStringLiteral("Skip all"), actions);
        m_next = new PendingButton(QStringLiteral("Answer"), actions);
        m_next->setHighlighted(true);
        actionLayout->addStretch(1);
        actionLayout->addWidget(m_skip);
        actionLayout->addWidget(m_next);

        layout->addWidget(m_heading);
        layout->addWidget(m_body);
        layout->addWidget(m_options);
        m_note = new QLineEdit(this);
        m_note->setObjectName(QStringLiteral("questionNote"));
        m_note->setPlaceholderText(QStringLiteral("Add a note (optional)"));
        m_note->setFrame(false);
        m_note->setFont(Chrome::uiFont(theme->fontUI()));
        m_note->setFixedHeight(theme->rowHeight());
        QPalette notePalette = m_note->palette();
        notePalette.setColor(QPalette::Base, theme->fieldFill());
        notePalette.setColor(QPalette::Text, theme->fg());
        m_note->setPalette(notePalette);
        QObject::connect(m_note, &QLineEdit::textChanged, this, [this]() { refreshNext(); });
        layout->addWidget(m_note);
        layout->addWidget(actions);

        m_skip->setHandler([this]() { skipAll(); });
        m_next->setHandler([this]() { advance(false); });

        setFixedHeight(cardHeight());
        hide();
    }

    bool shown() const { return m_shown; }

    int cardHeight() const
    {
        Theme *theme = Theme::instance();
        const int items = optionRows() > 0 ? 5 : 4;
        const int rows = 5 + optionRows();
        return 2 * theme->space4() + rows * theme->rowHeight()
               + (items - 1) * theme->space2();
    }

    void openFor(const QString &eventId, const QVariantList &questions)
    {
        if (eventId.isEmpty())
            return;
        if (questions.isEmpty()) {
            emit m_owner->questionsAnswered(eventId, QVariantList());
            return;
        }
        if (eventId != m_eventId) {
            m_eventId = eventId;
            m_questions = questions;
            m_index = 0;
            m_answers.clear();
            m_chosen.clear();
        }
        m_shown = true;
        renderCurrent();
    }

    void dismiss(const QString &eventId)
    {
        if (eventId != m_eventId)
            return;
        m_shown = false;
        hide();
        m_owner->syncDockHeight();
    }

private:
    QVariantMap current() const
    {
        if (m_index < 0 || m_index >= m_questions.size())
            return QVariantMap();
        return m_questions.at(m_index).toMap();
    }

    int optionRows() const
    {
        const QVariantMap item = current();
        const QVariantList options = item.value(QStringLiteral("options")).toList();
        if (options.isEmpty())
            return 0;
        return (options.size() + OptionColumns - 1) / OptionColumns;
    }

    void clearOptions()
    {
        while (QLayoutItem *entry = m_optionLayout->takeAt(0)) {
            if (QWidget *widget = entry->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete entry;
        }
        m_optionButtons.clear();
        m_optionLabels.clear();
    }

    void renderCurrent()
    {
        Theme *theme = Theme::instance();
        const QVariantMap item = current();
        if (item.isEmpty()) {
            dismiss(m_eventId);
            return;
        }

        m_heading->setText(m_questions.size() > 1
                               ? QStringLiteral("Your agent is asking (%1 of %2)")
                                     .arg(m_index + 1)
                                     .arg(m_questions.size())
                               : QStringLiteral("Your agent is asking"));

        QString html;
        const QString header = item.value(QStringLiteral("header")).toString();
        if (!header.isEmpty())
            html += bodyBlock(header, theme->fontData(), theme->dim());
        html += bodyBlock(item.value(QStringLiteral("question")).toString(), theme->fontProse(),
                          theme->fg());
        const QString detail = item.value(QStringLiteral("detail")).toString();
        if (!detail.isEmpty())
            html += bodyBlock(detail, theme->fontData(), theme->dim());
        m_body->setHtml(html);
        m_body->document()->setDocumentMargin(theme->space2());
        m_body->moveCursor(QTextCursor::Start);

        clearOptions();
        const QVariantList options = item.value(QStringLiteral("options")).toList();
        const int rows = optionRows();
        for (int i = 0; i < options.size(); ++i) {
            const QVariantMap option = options.at(i).toMap();
            const QString label = option.value(QStringLiteral("label")).toString();
            auto *button = new PendingButton(label, m_options);
            button->setHighlighted(m_chosen.contains(label));
            button->setHandler([this, label]() { toggle(label); });
            m_optionLayout->addWidget(button, i / OptionColumns, i % OptionColumns);
            m_optionButtons.append(button);
            m_optionLabels.append(label);
        }
        if (rows > 0) {
            m_options->setFixedHeight(rows * theme->rowHeight());
            m_options->setVisible(true);
        } else {
            m_options->setFixedHeight(0);
            m_options->setVisible(false);
        }

        m_skip->setText(QStringLiteral("Skip all"));
        m_next->setText(m_index + 1 < m_questions.size() ? QStringLiteral("Next")
                                                         : QStringLiteral("Answer"));
        m_note->clear();
        m_next->setEnabled(!m_chosen.isEmpty());

        setFixedHeight(cardHeight());
        setVisible(true);
        m_owner->syncDockHeight();
    }

    void refreshNext()
    {
        m_next->setEnabled(!m_chosen.isEmpty()
                           || !m_note->text().trimmed().isEmpty());
    }

    void toggle(const QString &label)
    {
        const QVariantMap item = current();
        if (item.value(QStringLiteral("multiSelect")).toBool()) {
            if (m_chosen.contains(label))
                m_chosen.removeAll(label);
            else
                m_chosen.append(label);
        } else {
            m_chosen = QStringList{label};
        }
        for (int i = 0; i < m_optionButtons.size(); ++i)
            m_optionButtons.at(i)->setHighlighted(m_chosen.contains(m_optionLabels.at(i)));
        refreshNext();
    }

    void advance(bool skip)
    {
        const QVariantMap item = current();
        if (item.isEmpty())
            return;

        QVariantList selected;
        if (!skip) {
            for (const QString &label : m_chosen)
                selected.append(label);
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), item.value(QStringLiteral("id")));
        entry.insert(QStringLiteral("selected"), selected);
        const QString note = skip ? QString() : m_note->text().trimmed();
        if (!note.isEmpty())
            entry.insert(QStringLiteral("custom"), note);
        m_answers.append(entry);
        m_chosen.clear();
        m_note->clear();

        if (m_index + 1 < m_questions.size()) {
            ++m_index;
            renderCurrent();
            return;
        }

        const QString eventId = m_eventId;
        const QVariantList answers = m_answers;
        m_shown = false;
        hide();
        m_owner->syncDockHeight();
        emit m_owner->questionsAnswered(eventId, answers);
    }

    void skipAll()
    {
        if (m_questions.isEmpty())
            return;
        for (int i = m_index; i < m_questions.size(); ++i) {
            QVariantMap entry;
            entry.insert(QStringLiteral("id"),
                         m_questions.at(i).toMap().value(QStringLiteral("id")));
            entry.insert(QStringLiteral("selected"), QVariantList());
            m_answers.append(entry);
        }
        const QString eventId = m_eventId;
        const QVariantList answers = m_answers;
        m_shown = false;
        hide();
        m_owner->syncDockHeight();
        emit m_owner->questionsAnswered(eventId, answers);
    }

    PendingBand *m_owner;
    PendingHeading *m_heading = nullptr;
    QTextEdit *m_body = nullptr;
    QLineEdit *m_note = nullptr;
    QWidget *m_options = nullptr;
    QGridLayout *m_optionLayout = nullptr;
    QList<PendingButton *> m_optionButtons;
    QStringList m_optionLabels;
    PendingButton *m_skip = nullptr;
    PendingButton *m_next = nullptr;
    QString m_eventId;
    QVariantList m_questions;
    QVariantList m_answers;
    QStringList m_chosen;
    int m_index = 0;
    bool m_shown = false;
};

PendingBand::PendingBand(QWidget *parent)
    : QWidget(parent)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("pendingBand"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, theme->space3(), 0, theme->space3());
    layout->setSpacing(theme->space3());

    m_approval = new ApprovalCard(this, this);
    m_question = new QuestionCard(this, this);
    layout->addWidget(m_approval);
    layout->addWidget(m_question);

    syncDockHeight();
}

bool PendingBand::approvalShown() const
{
    return m_approval != nullptr && m_approval->shown();
}

bool PendingBand::questionShown() const
{
    return m_question != nullptr && m_question->shown();
}

int PendingBand::dockHeight() const
{
    Theme *theme = Theme::instance();
    int height = 0;
    int cards = 0;
    if (approvalShown()) {
        height += m_approval->cardHeight();
        ++cards;
    }
    if (questionShown()) {
        height += m_question->cardHeight();
        ++cards;
    }
    if (cards == 0)
        return 0;
    return height + 2 * theme->space3() + (cards - 1) * theme->space3();
}

void PendingBand::showApproval(const QString &requestId, const QString &toolName,
                               const QString &detail)
{
    m_approval->show(requestId, toolName, detail);
}

void PendingBand::dismissApproval(const QString &requestId)
{
    m_approval->dismiss(requestId);
}

void PendingBand::showQuestions(const QString &eventId, const QVariantList &questions)
{
    m_question->openFor(eventId, questions);
}

void PendingBand::dismissQuestions(const QString &eventId)
{
    m_question->dismiss(eventId);
}

void PendingBand::syncDockHeight()
{
    const int height = dockHeight();
    setFixedHeight(height);
    setVisible(height > 0);
    emit dockHeightChanged();
}

void PendingBand::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->border());
}
