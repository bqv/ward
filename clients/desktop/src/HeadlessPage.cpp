#include "HeadlessPage.h"

#include "AcpClient.h"
#include "HeadlessClient.h"
#include "Markdown.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractItemModel>
#include <QAbstractTextDocumentLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr int MarkSize = 16;
constexpr int LeadCap = 400;
constexpr int ProseLineHeight = 170;
constexpr qreal BubbleWidthRatio = 0.82;
constexpr qreal IconOpacity = 0.85;
constexpr qreal DisabledOpacity = 0.55;
constexpr int MinButtonWidth = 64;
constexpr int WrapProbeHeight = 10000;
constexpr int MaxFieldHeight = 120;

QString firstLine(const QString &text)
{
    const int cut = text.indexOf(QLatin1Char('\n'));
    return cut >= 0 ? text.left(cut) : text;
}

void applyProseRhythm(QTextDocument *document)
{
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat format = cursor.blockFormat();
        format.setLineHeight(ProseLineHeight, QTextBlockFormat::ProportionalHeight);
        cursor.setBlockFormat(format);
    }
}

void applyWrapAnywhere(QTextDocument *document)
{
    QTextOption option = document->defaultTextOption();
    option.setWrapMode(QTextOption::WrapAnywhere);
    document->setDefaultTextOption(option);
}

void configureReadOnly(QTextEdit *edit, const QFont &font, const QColor &base,
                       const QColor &text)
{
    Theme *theme = Theme::instance();
    edit->setReadOnly(true);
    edit->setFrameShape(QFrame::NoFrame);
    edit->setContextMenuPolicy(Qt::NoContextMenu);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setLineWrapMode(QTextEdit::WidgetWidth);
    edit->setAcceptDrops(false);
    edit->document()->setDocumentMargin(0);
    edit->setFont(font);

    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Highlight, theme->accent());
    palette.setColor(QPalette::HighlightedText, theme->accentFg());
    edit->setPalette(palette);
    edit->viewport()->setPalette(palette);
}

}

class HeadlessPage::Band : public QWidget
{
public:
    Band(bool topRule, bool bottomRule, QWidget *parent)
        : QWidget(parent)
        , m_topRule(topRule)
        , m_bottomRule(bottomRule)
    {
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        if (m_topRule)
            painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->border());
        if (m_bottomRule) {
            painter.fillRect(QRect(0, height() - theme->hairline(), width(),
                                   theme->hairline()),
                             theme->border());
        }
    }

private:
    bool m_topRule;
    bool m_bottomRule;
};

class HeadlessPage::TextLine : public QWidget
{
public:
    explicit TextLine(QWidget *parent)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFont(Chrome::uiFont(Theme::instance()->fontData()));
        m_colour = Theme::instance()->dim();
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setLineFont(const QFont &font)
    {
        if (m_font == font)
            return;
        m_font = font;
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

    void setElideMiddle(bool elideMiddle) { m_elideMiddle = elideMiddle; }

    void setAlignment(Qt::Alignment alignment) { m_alignment = alignment; }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(m_font);
        return QSize(metrics.horizontalAdvance(m_text), metrics.height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, QFontMetrics(m_font).height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        Chrome::paintLine(painter, rect(), m_font, m_colour, m_text, m_alignment,
                          m_elideMiddle);
    }

private:
    QString m_text;
    QFont m_font;
    QColor m_colour;
    Qt::Alignment m_alignment = Qt::AlignLeft | Qt::AlignVCenter;
    bool m_elideMiddle = false;
};

class HeadlessPage::Button : public QWidget
{
public:
    enum Kind { Push, Chip };

    Button(const QString &text, Kind kind, QWidget *parent)
        : QWidget(parent)
        , m_kind(kind)
        , m_text(text)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        resizeToText();
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        resizeToText();
        update();
    }

    void setHandler(std::function<void()> handler) { m_handler = std::move(handler); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (m_kind == Chip) {
            const bool hot = underMouse() && isEnabled();
            Chrome::paintChip(painter, rect(), hot, false);
            Chrome::paintLine(painter, rect(), Chrome::uiFont(theme->fontData()),
                              hot ? theme->fg() : theme->dim(), m_text,
                              Qt::AlignCenter);
            return;
        }

        if (!isEnabled())
            painter.setOpacity(DisabledOpacity);

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

        painter.setBrush(face);
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, theme->radius(), theme->radius());

        const int radius = theme->radius();
        painter.setPen(QPen(theme->buttonBorderBottom(), theme->hairline()));
        painter.drawLine(QPointF(body.left() + radius, body.bottom()),
                         QPointF(body.right() - radius, body.bottom()));
        if (!down) {
            painter.setPen(QPen(theme->insetHighlight(), theme->hairline()));
            painter.drawLine(QPointF(body.left() + radius, body.top() + 1),
                             QPointF(body.right() - radius, body.top() + 1));
        }

        const QFont font = Chrome::uiFont(theme->fontUI());
        const QRectF label = body.adjusted(theme->space2(), 0, -theme->space2(), 0)
                                 .translated(0, down ? 1 : 0);
        if (isEnabled()) {
            painter.setFont(font);
            painter.setPen(theme->textShadow());
            painter.drawText(label.translated(0, -1), int(Qt::AlignCenter), m_text);
        }
        painter.setPen(isEnabled() ? theme->fg() : theme->faint());
        painter.drawText(label, int(Qt::AlignCenter), m_text);
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
        if (event->button() == Qt::LeftButton && wasPressed && isEnabled()
            && rect().contains(event->position().toPoint()) && m_handler) {
            m_handler();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
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

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            update();
    }

private:
    void resizeToText()
    {
        Theme *theme = Theme::instance();
        const QFont font =
            Chrome::uiFont(m_kind == Chip ? theme->fontData() : theme->fontUI());
        const int textWidth = QFontMetrics(font).horizontalAdvance(m_text);
        if (m_kind == Chip) {
            setFixedSize(textWidth + 2 * theme->space3(),
                         theme->fontData() + 2 * theme->space2());
        } else {
            setFixedSize(std::max(MinButtonWidth, textWidth + theme->space6()),
                         theme->controlHeight());
        }
    }

    Kind m_kind;
    QString m_text;
    bool m_pressed = false;
    std::function<void()> m_handler;
};

class HeadlessPage::Entry : public QPlainTextEdit
{
public:
    Entry(HeadlessPage *owner, QWidget *parent)
        : QPlainTextEdit(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("headlessInput"));
        setPlaceholderText(QStringLiteral("Give the headless child one task…"));
        setFont(Chrome::uiFont(theme->fontUI()));
        setFrameShape(QFrame::NoFrame);
        setContextMenuPolicy(Qt::NoContextMenu);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setTabChangesFocus(false);
        setAcceptDrops(false);
        document()->setDocumentMargin(theme->space4());

        QPalette palette = this->palette();
        palette.setColor(QPalette::Base, theme->fieldFill());
        palette.setColor(QPalette::Text, theme->fg());
        palette.setColor(QPalette::PlaceholderText, theme->dim());
        palette.setColor(QPalette::Highlight, theme->accent());
        palette.setColor(QPalette::HighlightedText, theme->accentFg());
        setPalette(palette);
        viewport()->setPalette(palette);
    }

    int preferredHeight() const
    {
        Theme *theme = Theme::instance();
        const int margin = qCeil(document()->documentMargin());
        const int text = qCeil(document()->size().height()) - 2 * margin;
        return std::min(MaxFieldHeight,
                        std::max(theme->fieldHeight(), text + theme->space6()));
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !(event->modifiers() & Qt::ShiftModifier)) {
            event->accept();
            m_owner->submit();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

private:
    HeadlessPage *m_owner;
};

class HeadlessPage::Field : public QWidget
{
public:
    Field(HeadlessPage *owner, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("headlessField"));
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_entry = new Entry(owner, this);
        m_entry->installEventFilter(this);
        setFixedHeight(m_entry->preferredHeight());
        fitEntry();
    }

    Entry *entry() const { return m_entry; }

    void adoptHeight(int height)
    {
        if (height != minimumHeight()) {
            setFixedHeight(height);
            updateGeometry();
        }
        fitEntry();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setBrush(m_entry->isEnabled() ? theme->fieldFill() : theme->disabledFill());
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, theme->radius(), theme->radius());

        if (m_entry->hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            const QRectF ring = body.adjusted(1, 1, -1, -1);
            painter.drawRoundedRect(ring, theme->radius() - 1, theme->radius() - 1);
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        fitEntry();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_entry
            && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
            update();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void fitEntry()
    {
        const int hairline = Theme::instance()->hairline();
        m_entry->setGeometry(rect().adjusted(hairline, hairline, -hairline, -hairline));
    }

    Entry *m_entry = nullptr;
};

class HeadlessPage::Column : public QWidget
{
public:
    Column(HeadlessPage *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        setObjectName(QStringLiteral("headlessRows"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    }

    QSize sizeHint() const override { return QSize(0, minimumHeight()); }

    QSize minimumSizeHint() const override { return QSize(0, minimumHeight()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Theme::instance()->bg());
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_owner->relayoutRows();
    }

private:
    HeadlessPage *m_owner;
};

class HeadlessPage::Row : public QWidget
{
public:
    Row(HeadlessPage *owner, int index, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
        , m_index(index)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("headlessRow"));
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_user = new QTextEdit(this);
        configureReadOnly(m_user, Chrome::uiFont(theme->fontProse() + 1), theme->bubble(),
                          theme->fg());
        m_user->hide();

        m_agent = new QTextEdit(this);
        configureReadOnly(m_agent, Chrome::uiFont(theme->fontProse() + 1), theme->bg(),
                          theme->fg());
        m_agent->hide();

        m_payload = new QTextEdit(this);
        configureReadOnly(m_payload, Chrome::monoFont(theme->fontMono()),
                          theme->codeBlock(), theme->fg());
        m_payload->hide();

        m_user->installEventFilter(this);
        m_agent->installEventFilter(this);
        m_payload->installEventFilter(this);
    }

    QString rowId() const { return m_rowId; }

    void refresh()
    {
        AcpTranscript *model = m_owner->transcript();
        if (model == nullptr || m_index < 0 || m_index >= model->rowCount())
            return;

        const QModelIndex index = model->index(m_index, 0);
        m_kind = model->data(index, AcpTranscript::KindRole).toString();
        m_body = model->data(index, AcpTranscript::TextRole).toString();
        m_title = model->data(index, AcpTranscript::TitleRole).toString();
        m_status = model->data(index, AcpTranscript::StatusRole).toString();
        m_detail = model->data(index, AcpTranscript::DetailRole).toString();
        m_request = model->data(index, AcpTranscript::RequestRole).toString();
        m_key = model->data(index, AcpTranscript::KeyRole).toString();
        m_rowId = m_key.isEmpty() ? QStringLiteral("i:") + QString::number(m_index)
                                  : QStringLiteral("k:") + m_key;
        m_expanded = m_owner->isOpen(m_rowId);
        m_measuredWidth = -1;

        if (isUser()) {
            m_user->setPlainText(m_body);
        } else if (isAgent()) {
            m_agent->setHtml(Markdown::instance()->toHtml(m_body));
            applyProseRhythm(m_agent->document());
        } else if (isProcess()) {
            m_payload->setPlainText(payloadText());
            applyWrapAnywhere(m_payload->document());
        }

        m_user->setVisible(isUser());
        m_agent->setVisible(isAgent());
        m_payload->setVisible(isProcess() && m_expanded && !payloadText().isEmpty());
        setCursor(canOpen() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    int rowHeightFor(int width) const
    {
        if (width <= 0)
            return 0;
        if (width == m_measuredWidth)
            return m_measuredHeight;

        Theme *theme = Theme::instance();
        int height = 0;

        if (isUser()) {
            const QFont font = Chrome::uiFont(theme->fontProse() + 1);
            const QFontMetrics metrics(font);
            int longest = 0;
            const QStringList lines = m_body.split(QLatin1Char('\n'));
            for (const QString &line : lines)
                longest = std::max(longest, metrics.horizontalAdvance(line));

            int bubble = std::min(int(width * BubbleWidthRatio),
                                  longest + 2 * theme->space5());
            bubble = std::max(bubble, 2 * theme->space5());
            const int inner = std::max(1, bubble - 2 * theme->space5());

            QTextDocument document;
            document.setDocumentMargin(0);
            document.setDefaultFont(font);
            document.setPlainText(m_body);
            document.setTextWidth(inner);

            m_bubbleWidth = bubble;
            m_bubbleHeight = qCeil(document.size().height()) + 2 * theme->space4();
            height = m_bubbleHeight;
        } else if (isAgent()) {
            QTextDocument document;
            document.setDocumentMargin(0);
            document.setDefaultFont(Chrome::uiFont(theme->fontProse() + 1));
            document.setHtml(Markdown::instance()->toHtml(m_body));
            applyProseRhythm(&document);
            document.setTextWidth(width);
            height = m_body.isEmpty() ? 0 : qCeil(document.size().height());
        } else if (isProcess()) {
            height = theme->rowHeight();
            if (m_expanded && !payloadText().isEmpty()) {
                m_payloadHeight = payloadHeightFor(std::max(1, width - 2 * theme->space4()));
                height += m_payloadHeight + 2 * theme->space4();
            } else {
                m_payloadHeight = 0;
            }
        }

        m_measuredWidth = width;
        m_measuredHeight = height;
        return height;
    }

    void layoutChildren(int width)
    {
        Theme *theme = Theme::instance();
        if (isUser()) {
            const int x = width - m_bubbleWidth;
            m_user->setGeometry(x + theme->space5(), theme->space4(),
                                std::max(0, m_bubbleWidth - 2 * theme->space5()),
                                std::max(0, m_bubbleHeight - 2 * theme->space4()));
        }
        if (isAgent())
            m_agent->setGeometry(0, 0, width, std::max(0, m_measuredHeight));
        if (m_payload->isVisible()) {
            m_payload->setGeometry(theme->space4(), theme->rowHeight() + theme->space4(),
                                   std::max(0, width - 2 * theme->space4()),
                                   m_payloadHeight);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);

        if (isUser() && m_bubbleWidth > 0) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme->bubble());
            painter.drawRoundedRect(QRectF(width() - m_bubbleWidth, 0, m_bubbleWidth,
                                           m_bubbleHeight),
                                    theme->radius(), theme->radius());
            return;
        }

        if (!isProcess())
            return;

        const QString iconName = markName();
        if (iconName != m_iconName) {
            m_iconName = iconName;
            QPixmap pixmap(m_owner->iconDir() + QLatin1Char('/') + iconName
                           + QStringLiteral(".svg"));
            m_icon = pixmap.isNull()
                         ? pixmap
                         : pixmap.scaled(MarkSize, MarkSize, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
        }
        if (!m_icon.isNull()) {
            painter.setOpacity(IconOpacity);
            painter.drawPixmap(QRect(0, qRound((theme->rowHeight() - MarkSize) / 2.0),
                                     MarkSize, MarkSize),
                               m_icon);
            painter.setOpacity(1.0);
        }

        const QFont font = Chrome::uiFont(theme->fontData());
        const QString status = processStatus();
        const int statusWidth =
            status.isEmpty() ? 0 : QFontMetrics(font).horizontalAdvance(status);
        const int left = MarkSize + theme->space3();
        const int right = width() - (status.isEmpty() ? 0 : statusWidth + theme->space3());
        Chrome::paintLine(painter,
                          QRect(left, 0, std::max(0, right - left), theme->rowHeight()),
                          font, isError() ? theme->danger() : theme->dim(),
                          processLabel());
        if (!status.isEmpty()) {
            Chrome::paintLine(painter,
                              QRect(width() - statusWidth, 0, statusWidth,
                                    theme->rowHeight()),
                              font, statusColour(), status,
                              Qt::AlignRight | Qt::AlignVCenter);
        }

        if (m_expanded && m_payloadHeight > 0) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme->codeBlock());
            painter.drawRoundedRect(QRectF(0, theme->rowHeight(), width(),
                                           m_payloadHeight + 2 * theme->space4()),
                                    theme->radius(), theme->radius());
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && canOpen()
            && event->position().y() < Theme::instance()->rowHeight()) {
            m_owner->toggleOpen(m_rowId);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
                key->accept();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    bool isUser() const { return m_kind == QLatin1String("user"); }
    bool isTool() const { return m_kind == QLatin1String("tool"); }
    bool isThought() const { return m_kind == QLatin1String("thought"); }
    bool isNotice() const { return m_kind == QLatin1String("notice"); }
    bool isError() const { return m_kind == QLatin1String("error"); }
    bool isAgent() const
    {
        return !isUser() && !isTool() && !isThought() && !isNotice() && !isError();
    }
    bool isProcess() const
    {
        return isTool() || isNotice() || isError() || (isThought() && !m_body.isEmpty());
    }

    QString processLabel() const
    {
        if (isTool())
            return m_title;
        const QString line = firstLine(m_body);
        return line.size() > LeadCap ? line.left(LeadCap) : line;
    }

    QString processStatus() const { return isTool() ? m_status : QString(); }

    QColor statusColour() const
    {
        Theme *theme = Theme::instance();
        if (isError() || m_status == QLatin1String("failed"))
            return theme->danger();
        if (m_status == QLatin1String("completed"))
            return theme->ok();
        return theme->warn();
    }

    QString markName() const
    {
        if (isTool())
            return QStringLiteral("tool");
        if (isThought())
            return QStringLiteral("think");
        if (isError())
            return QStringLiteral("error");
        return QStringLiteral("notice");
    }

    QString payloadText() const
    {
        if (!isTool())
            return m_body;
        QStringList parts;
        if (!m_request.isEmpty())
            parts.append(QStringLiteral("in\n") + m_request);
        if (!m_detail.isEmpty())
            parts.append(QStringLiteral("out\n") + m_detail);
        return parts.join(QStringLiteral("\n\n"));
    }

    bool canOpen() const
    {
        if (isTool())
            return !m_request.isEmpty() || !m_detail.isEmpty();
        if (isThought() || isError() || isNotice()) {
            const QString line = firstLine(m_body);
            const QString lead = line.size() > LeadCap ? line.left(LeadCap) : line;
            return m_body.size() > lead.size();
        }
        return false;
    }

    int payloadHeightFor(int width) const
    {
        Theme *theme = Theme::instance();
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setDefaultFont(Chrome::monoFont(theme->fontMono()));
        applyWrapAnywhere(&document);
        document.setPlainText(payloadText());
        document.setTextWidth(width);
        return qCeil(document.size().height());
    }

    HeadlessPage *m_owner;
    int m_index;
    QString m_kind;
    QString m_body;
    QString m_title;
    QString m_status;
    QString m_detail;
    QString m_request;
    QString m_key;
    QString m_rowId;
    bool m_expanded = false;
    QTextEdit *m_user = nullptr;
    QTextEdit *m_agent = nullptr;
    QTextEdit *m_payload = nullptr;
    QPixmap m_icon;
    QString m_iconName;
    mutable int m_measuredWidth = -1;
    mutable int m_measuredHeight = 0;
    mutable int m_bubbleWidth = 0;
    mutable int m_bubbleHeight = 0;
    mutable int m_payloadHeight = 0;
};

HeadlessPage::HeadlessPage(HeadlessClient *client, const QString &iconDir, QWidget *parent)
    : QWidget(parent)
    , m_client(client)
    , m_iconDir(iconDir)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("headlessPage"));
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_bar = new Band(false, true, this);
    m_bar->setObjectName(QStringLiteral("headlessBar"));
    m_bar->setFixedHeight(theme->headerHeight());
    m_bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *barRow = new QHBoxLayout(m_bar);
    barRow->setContentsMargins(theme->space5(), 0, theme->space5(), 0);
    barRow->setSpacing(theme->space4());

    m_title = new TextLine(m_bar);
    m_title->setText(QStringLiteral("headless"));
    m_title->setLineFont(Chrome::uiFont(theme->fontData()));
    m_title->setColour(theme->fg());
    barRow->addWidget(m_title, 0, Qt::AlignVCenter);

    m_session = new TextLine(m_bar);
    m_session->setObjectName(QStringLiteral("headlessSessionId"));
    m_session->setLineFont(Chrome::uiFont(theme->fontData()));
    m_session->setColour(theme->dim());
    m_session->setElideMiddle(true);
    m_session->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    barRow->addWidget(m_session, 1, Qt::AlignVCenter);

    m_status = new TextLine(m_bar);
    m_status->setObjectName(QStringLiteral("headlessStatus"));
    m_status->setLineFont(Chrome::uiFont(theme->fontData()));
    m_status->setColour(theme->dim());
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setMaximumWidth(16 * theme->space6());
    barRow->addWidget(m_status, 0, Qt::AlignVCenter);

    m_close = new Button(QStringLiteral("close"), Button::Chip, m_bar);
    m_close->setObjectName(QStringLiteral("headlessClose"));
    m_close->setToolTip(QStringLiteral(
        "Close the pane and forget this conversation — the headless toggle only puts it away"));
    m_close->setHandler([this]() { emit shutRequested(); });
    barRow->addWidget(m_close, 0, Qt::AlignVCenter);

    outer->addWidget(m_bar);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("headlessTranscript"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);

    QPalette scrollPalette = m_scroll->palette();
    scrollPalette.setColor(QPalette::Base, theme->bg());
    scrollPalette.setColor(QPalette::Window, theme->bg());
    m_scroll->setPalette(scrollPalette);
    m_scroll->setBackgroundRole(QPalette::Base);
    m_scroll->viewport()->setAutoFillBackground(true);
    m_scroll->viewport()->setBackgroundRole(QPalette::Base);
    m_scroll->viewport()->setPalette(scrollPalette);

    m_column = new Column(this, m_scroll);

    m_empty = new QLabel(m_column);
    m_empty->setObjectName(QStringLiteral("headlessEmpty"));
    m_empty->setTextFormat(Qt::PlainText);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_empty->setFont(Chrome::uiFont(theme->fontProse()));
    m_empty->setText(QStringLiteral(
        "One task per child. Type below and the client runs it; a second task continues "
        "the same session, so the conversation grows in this pane."));
    QPalette emptyPalette = m_empty->palette();
    emptyPalette.setColor(QPalette::WindowText, theme->dim());
    m_empty->setPalette(emptyPalette);
    m_empty->hide();

    m_scroll->setWidget(m_column);

    outer->addWidget(m_scroll, 1);

    m_composerBand = new Band(true, false, this);
    m_composerBand->setObjectName(QStringLiteral("headlessComposer"));
    m_composerBand->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *composerColumn = new QVBoxLayout(m_composerBand);
    composerColumn->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                       theme->space4());
    composerColumn->setSpacing(theme->space2());

    m_field = new Field(this, m_composerBand);
    m_entry = m_field->entry();
    composerColumn->addWidget(m_field);

    auto *controls = new QHBoxLayout();
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(theme->space3());

    m_hint = new TextLine(m_composerBand);
    m_hint->setLineFont(Chrome::uiFont(theme->fontData()));
    m_hint->setColour(theme->faint());
    controls->addWidget(m_hint, 0, Qt::AlignVCenter);
    controls->addStretch(1);

    m_send = new Button(QStringLiteral("Send"), Button::Push, m_composerBand);
    m_send->setObjectName(QStringLiteral("headlessSend"));
    m_send->setHandler([this]() { triggerButton(); });
    controls->addWidget(m_send, 0, Qt::AlignVCenter);
    composerColumn->addLayout(controls);

    outer->addWidget(m_composerBand);

    QObject::connect(m_entry, &QPlainTextEdit::textChanged, this,
                     [this]() { refreshComposer(); });
    QObject::connect(m_entry->document()->documentLayout(),
                     &QAbstractTextDocumentLayout::documentSizeChanged, this,
                     [this]() { refreshComposer(); });

    if (m_client != nullptr) {
        QObject::connect(m_client, &HeadlessClient::busyChanged, this, [this]() {
            refreshBar();
            refreshComposer();
        });
        QObject::connect(m_client, &HeadlessClient::sessionChanged, this,
                         [this]() { refreshBar(); });
        QObject::connect(m_client, &HeadlessClient::statusChanged, this,
                         [this]() { refreshBar(); });
        QObject::connect(m_client, &HeadlessClient::lastErrorChanged, this,
                         [this]() { refreshBar(); });

        AcpTranscript *model = m_client->transcript();
        QObject::connect(model, &QAbstractItemModel::rowsInserted, this,
                         [this](const QModelIndex &parent, int first, int last) {
            if (parent.isValid())
                return;
            if (first != m_rows.size()) {
                rebuildRows();
                return;
            }
            for (int index = first; index <= last; ++index) {
                auto *row = new Row(this, index, m_column);
                row->refresh();
                m_rows.append(row);
            }
            relayoutRows();
        });
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, this,
                         [this](const QModelIndex &parent) {
            if (parent.isValid())
                return;
            rebuildRows();
        });
        QObject::connect(model, &QAbstractItemModel::dataChanged, this,
                         [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
            if (topLeft.parent().isValid())
                return;
            for (int index = topLeft.row();
                 index <= bottomRight.row() && index < m_rows.size(); ++index) {
                m_rows[index]->refresh();
            }
            relayoutRows();
        });
        QObject::connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            m_openRows.clear();
            rebuildRows();
        });
        QObject::connect(model, &QAbstractItemModel::layoutChanged, this,
                         [this]() { rebuildRows(); });
    } else {
        qWarning() << "headless page: no child client was handed to the page";
    }

    QScrollBar *bar = m_scroll->verticalScrollBar();
    QObject::connect(bar, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_scrollSettling)
            return;
        m_follow = value >= m_scroll->verticalScrollBar()->maximum();
    });
    QObject::connect(bar, &QScrollBar::rangeChanged, this, [this](int, int maximum) {
        if (!m_follow)
            return;
        m_scrollSettling = true;
        m_scroll->verticalScrollBar()->setValue(maximum);
        m_scrollSettling = false;
    });

    refreshBar();
    refreshComposer();
    rebuildRows();
}

void HeadlessPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    focusComposer();
}

void HeadlessPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Theme::instance()->bg());
}

void HeadlessPage::dragEnterEvent(QDragEnterEvent *event)
{
    event->ignore();
}

void HeadlessPage::dragMoveEvent(QDragMoveEvent *event)
{
    event->ignore();
}

void HeadlessPage::dropEvent(QDropEvent *event)
{
    event->ignore();
}

void HeadlessPage::focusComposer()
{
    if (m_entry != nullptr)
        m_entry->setFocus(Qt::OtherFocusReason);
}

void HeadlessPage::submit()
{
    if (m_client == nullptr || m_client->busy())
        return;
    if (m_entry->toPlainText().trimmed().isEmpty())
        return;
    if (m_client->send(m_entry->toPlainText()))
        m_entry->clear();
}

void HeadlessPage::triggerButton()
{
    if (m_client == nullptr)
        return;
    if (m_client->busy())
        m_client->stop();
    else
        submit();
}

void HeadlessPage::refreshBar()
{
    const QString sessionId = m_client != nullptr ? m_client->sessionId() : QString();
    const QString cwd = m_client != nullptr ? m_client->cwd() : QString();
    const QString status = m_client != nullptr ? m_client->status() : QString();
    const bool busy = m_client != nullptr && m_client->busy();
    const bool failed = m_client != nullptr && !m_client->lastError().isEmpty();

    Theme *theme = Theme::instance();
    m_session->setText(sessionId.isEmpty()
                           ? QStringLiteral("a new session starts with the first task")
                           : sessionId);
    m_session->setLineFont(sessionId.isEmpty() ? Chrome::uiFont(theme->fontData())
                                               : Chrome::monoFont(theme->fontData()));
    m_session->setToolTip(sessionId.isEmpty()
                              ? QStringLiteral("The child reports its session id with the first task")
                              : QStringLiteral("Every task in this pane continues this session, in ")
                                    + cwd);
    m_status->setText(status);
    m_status->setColour(busy ? theme->running()
                             : failed ? theme->danger() : theme->dim());
}

void HeadlessPage::refreshComposer()
{
    const bool busy = m_client != nullptr && m_client->busy();
    const bool hasText = !m_entry->toPlainText().trimmed().isEmpty();

    m_send->setText(busy ? QStringLiteral("Stop") : QStringLiteral("Send"));
    m_send->setEnabled(m_client != nullptr && (busy || hasText));
    m_hint->setText(busy ? QStringLiteral("Stop ends this task; the session is kept.")
                         : QStringLiteral("Enter sends · Shift+Enter for a new line"));
    m_entry->setEnabled(m_client != nullptr);
    m_field->adoptHeight(m_entry->preferredHeight());
}

void HeadlessPage::rebuildRows()
{
    for (Row *row : m_rows)
        delete row;
    m_rows.clear();

    const int count = m_client != nullptr ? m_client->transcript()->rowCount() : 0;
    m_rows.reserve(count);
    for (int index = 0; index < count; ++index) {
        auto *row = new Row(this, index, m_column);
        row->refresh();
        m_rows.append(row);
    }
    relayoutRows();
}

void HeadlessPage::relayoutRows()
{
    if (m_relayingOut || m_column == nullptr || m_scroll == nullptr
        || m_empty == nullptr)
        return;
    m_relayingOut = true;

    Theme *theme = Theme::instance();
    const int width = m_column->width();
    const int rowWidth = std::max(0, width - 2 * theme->space6());
    int y = theme->space4();
    for (Row *row : m_rows) {
        const int height = row->rowHeightFor(rowWidth);
        row->setGeometry(theme->space6(), y, rowWidth, height);
        row->layoutChildren(rowWidth);
        y += height + theme->space4();
    }

    const int viewportHeight = m_scroll->viewport()->height();
    const int total = std::max(y, viewportHeight);
    if (m_column->minimumHeight() != total)
        m_column->setMinimumHeight(total);

    if (m_rows.isEmpty() && m_client != nullptr) {
        const int labelWidth = std::max(0, std::min(width - 2 * theme->space6(),
                                                    32 * theme->space6()));
        const QFont font = Chrome::uiFont(theme->fontProse());
        const QFontMetrics metrics(font);
        const QRect bounds = metrics.boundingRect(
            QRect(0, 0, std::max(1, labelWidth), WrapProbeHeight),
            Qt::TextWordWrap | Qt::AlignHCenter, m_empty->text());
        const int labelHeight = std::max(metrics.height(), bounds.height());
        m_empty->setGeometry(std::max(0, (width - labelWidth) / 2),
                             std::max(theme->space4(), (total - labelHeight) / 2),
                             labelWidth, labelHeight);
        m_empty->show();
    } else {
        m_empty->hide();
    }

    m_relayingOut = false;
}

void HeadlessPage::toggleOpen(const QString &rowId)
{
    if (rowId.isEmpty())
        return;
    if (m_openRows.contains(rowId))
        m_openRows.remove(rowId);
    else
        m_openRows.insert(rowId);
    for (Row *row : m_rows) {
        if (row->rowId() == rowId)
            row->refresh();
    }
    relayoutRows();
}

bool HeadlessPage::isOpen(const QString &rowId) const
{
    return m_openRows.contains(rowId);
}

AcpTranscript *HeadlessPage::transcript() const
{
    return m_client != nullptr ? m_client->transcript() : nullptr;
}
