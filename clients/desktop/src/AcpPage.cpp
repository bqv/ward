#include "AcpPage.h"

#include "AcpClient.h"
#include "Markdown.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractItemModel>
#include <QAbstractTextDocumentLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <QTextOption>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr int ProseLineHeight = 135;
constexpr int MaxFieldHeight = 120;
constexpr int ProcessTextLines = 4;
constexpr int PayloadLines = 12;
constexpr int MinButtonWidth = 64;
constexpr qreal BubbleWidthRatio = 0.7;
constexpr qreal DisabledOpacity = 0.55;

void applyProseRhythm(QTextDocument *document)
{
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat format = cursor.blockFormat();
        format.setLineHeight(ProseLineHeight, QTextBlockFormat::ProportionalHeight);
        cursor.setBlockFormat(format);
    }
}

void configureProse(QTextEdit *edit, const QFont &font, const QColor &text)
{
    Theme *theme = Theme::instance();
    edit->setReadOnly(true);
    edit->setFrameShape(QFrame::NoFrame);
    edit->setContextMenuPolicy(Qt::NoContextMenu);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setLineWrapMode(QTextEdit::WidgetWidth);
    edit->setAcceptDrops(false);
    edit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    edit->document()->setDocumentMargin(0);
    edit->setFont(font);
    edit->setAutoFillBackground(false);
    edit->viewport()->setAutoFillBackground(false);

    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Window, Qt::transparent);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Highlight, theme->accent());
    palette.setColor(QPalette::HighlightedText, theme->accentFg());
    edit->setPalette(palette);
    edit->viewport()->setPalette(palette);
}

QTextOption wrappedOption()
{
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    return option;
}

int wrappedHeight(const QString &text, const QFont &font, int width, int maxLines)
{
    if (text.isEmpty() || width <= 0 || maxLines <= 0)
        return 0;

    QTextLayout layout(text, font);
    layout.setTextOption(wrappedOption());
    layout.beginLayout();
    int height = 0;
    int lines = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid() || lines >= maxLines)
            break;
        line.setLineWidth(width);
        height += qCeil(line.height());
        ++lines;
    }
    layout.endLayout();
    return height;
}

void drawWrapped(QPainter *painter, const QRect &rect, const QString &text, const QFont &font,
                 const QColor &colour, int maxLines)
{
    if (text.isEmpty() || rect.width() <= 0 || maxLines <= 0)
        return;

    QTextLayout layout(text, font);
    layout.setTextOption(wrappedOption());
    layout.beginLayout();

    painter->save();
    painter->setPen(colour);
    int y = 0;
    int lines = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(rect.width());
        if (lines + 1 >= maxLines && line.textStart() + line.textLength() < text.size()) {
            const QString rest = text.mid(line.textStart());
            const QString elided =
                QFontMetrics(font).elidedText(rest, Qt::ElideRight, rect.width());
            painter->drawText(QPointF(rect.left(), rect.top() + y + line.ascent()), elided);
            y += qCeil(line.height());
            ++lines;
            break;
        }
        line.draw(painter, QPointF(rect.left(), rect.top() + y));
        y += qCeil(line.height());
        ++lines;
        if (lines >= maxLines)
            break;
    }
    layout.endLayout();
    painter->restore();
}

int proseHeight(const QString &markup, const QFont &font, int width)
{
    QTextDocument document;
    document.setDocumentMargin(0);
    document.setDefaultFont(font);
    document.setHtml(markup);
    applyProseRhythm(&document);
    document.setTextWidth(width);
    return qCeil(document.size().height());
}

int plainHeight(const QString &text, const QFont &font, int width)
{
    QTextDocument document;
    document.setDocumentMargin(0);
    document.setDefaultFont(font);
    document.setPlainText(text);
    document.setTextWidth(width);
    return qCeil(document.size().height());
}

}

class AcpPage::Band : public QWidget
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

class AcpPage::TextLine : public QWidget
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

    int lineHeight() const { return QFontMetrics(m_font).height(); }

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

class AcpPage::Button : public QWidget
{
public:
    enum Kind { Chip, Push };

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

    void setPixelSize(int pixelSize)
    {
        if (m_pixelSize == pixelSize)
            return;
        m_pixelSize = pixelSize;
        resizeToText();
        update();
    }

    void setHighlighted(bool highlighted)
    {
        if (m_highlighted == highlighted)
            return;
        m_highlighted = highlighted;
        update();
    }

    void setHandler(std::function<void()> handler) { m_handler = std::move(handler); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int pixelSize = m_pixelSize > 0
                                  ? m_pixelSize
                                  : (m_kind == Chip ? theme->fontData() : theme->fontUI());

        if (m_kind == Chip) {
            if (!isEnabled())
                painter.setOpacity(DisabledOpacity);
            const bool hot = underMouse() && isEnabled();
            Chrome::paintChip(painter, rect(), hot, false);
            Chrome::paintLine(painter, rect(), Chrome::uiFont(pixelSize),
                              hot ? theme->fg() : theme->dim(), m_text, Qt::AlignCenter);
            return;
        }

        if (!isEnabled())
            painter.setOpacity(DisabledOpacity);

        const bool down = m_pressed && isEnabled();
        const bool hot = underMouse() && isEnabled();
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QColor faceTop = theme->buttonGradientTop();
        QColor faceBottom = theme->buttonGradientBottom();
        if (down) {
            faceTop = theme->buttonPressed();
            faceBottom = theme->buttonPressed();
        } else if (hot) {
            faceTop = theme->buttonHover();
            faceBottom = theme->buttonGradientTop();
        }

        QLinearGradient face(body.topLeft(), body.bottomLeft());
        face.setColorAt(0.0, faceTop);
        face.setColorAt(1.0, faceBottom);
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

        const int ringInset = theme->hairline() + 1;
        if (m_highlighted && isEnabled()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            painter.drawRoundedRect(body.adjusted(ringInset, ringInset, -ringInset, -ringInset),
                                    theme->radius() - 1, theme->radius() - 1);
        }

        const QFont font = Chrome::uiFont(pixelSize);
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
        const int pixelSize = m_pixelSize > 0
                                  ? m_pixelSize
                                  : (m_kind == Chip ? theme->fontData() : theme->fontUI());
        const int textWidth = QFontMetrics(Chrome::uiFont(pixelSize)).horizontalAdvance(m_text);
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
    int m_pixelSize = 0;
    bool m_highlighted = false;
    bool m_pressed = false;
    std::function<void()> m_handler;
};

class AcpPage::Chip : public QWidget
{
public:
    Chip(const QVariantMap &session, bool current, QWidget *parent)
        : QWidget(parent)
        , m_current(current)
    {
        Theme *theme = Theme::instance();
        m_id = session.value(QStringLiteral("id")).toString();
        m_title = session.value(QStringLiteral("title")).toString();
        m_cwd = session.value(QStringLiteral("cwd")).toString();
        m_cwdName = session.value(QStringLiteral("cwdName")).toString();
        m_busy = session.value(QStringLiteral("busy")).toBool();
        m_awaiting = session.value(QStringLiteral("awaitingPermission")).toBool();

        setObjectName(QStringLiteral("acpSessionChip"));
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);

        const QFont ui = Chrome::uiFont(theme->fontData());
        const QFont mono = Chrome::monoFont(theme->fontData());
        m_titleWidth = std::min(QFontMetrics(ui).horizontalAdvance(m_title),
                                theme->space6() * 10);
        m_cwdWidth = m_cwdName.isEmpty()
                         ? 0
                         : std::min(QFontMetrics(mono).horizontalAdvance(m_cwdName),
                                    theme->space6() * 6);
        m_closeSize = theme->fontData() + theme->space3();

        int width = theme->space3() + theme->space3() + theme->space2() + m_titleWidth;
        if (m_cwdWidth > 0)
            width += theme->space2() + m_cwdWidth;
        width += theme->space2() + m_closeSize + theme->space3();
        setFixedSize(width, theme->fontData() + 2 * theme->space2());

        QString state = QStringLiteral("idle");
        if (m_awaiting)
            state = QStringLiteral("waiting for permission");
        else if (m_busy)
            state = QStringLiteral("running");
        setToolTip(m_cwd + QLatin1Char('\n')
                   + QStringLiteral("session ") + m_id.left(8) + QStringLiteral(" \u2014 ")
                   + state);
    }

    std::function<void()> onSelect;
    std::function<void()> onClose;

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool hot = underMouse();
        QColor fill = Qt::transparent;
        if (m_current)
            fill = theme->raised();
        else if (hot)
            fill = theme->buttonHover();
        painter.setBrush(fill);
        painter.setPen(QPen(m_current ? theme->accent() : theme->borderL2(),
                            theme->hairline()));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                theme->radius(), theme->radius());

        int x = theme->space3();
        const int dot = theme->space3();
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_awaiting ? theme->warn()
                                   : m_busy ? theme->running() : theme->faint());
        painter.drawEllipse(QRectF(x, (height() - dot) / 2.0, dot, dot));
        x += dot + theme->space2();

        const QFont ui = Chrome::uiFont(theme->fontData());
        Chrome::paintLine(painter, QRect(x, 0, m_titleWidth, height()), ui,
                          m_current ? theme->fg() : theme->dim(), m_title,
                          Qt::AlignLeft | Qt::AlignVCenter);
        x += m_titleWidth + theme->space2();

        if (m_cwdWidth > 0) {
            Chrome::paintLine(painter, QRect(x, 0, m_cwdWidth, height()),
                              Chrome::monoFont(theme->fontData()), theme->faint(), m_cwdName,
                              Qt::AlignLeft | Qt::AlignVCenter, true);
            x += m_cwdWidth + theme->space2();
        }

        const QRect close(x, (height() - m_closeSize) / 2, m_closeSize, m_closeSize);
        if (m_hoverClose) {
            painter.setBrush(theme->buttonHover());
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(close), theme->radius(), theme->radius());
        }
        Chrome::paintLine(painter, close, ui,
                          m_hoverClose ? theme->fg() : theme->caption(),
                          QStringLiteral("\u00d7"), Qt::AlignCenter);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressed = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool wasPressed = m_pressed;
        m_pressed = false;
        if (event->button() == Qt::LeftButton && wasPressed
            && rect().contains(event->position().toPoint())) {
            if (overClose(event->position().toPoint())) {
                if (onClose)
                    onClose();
            } else if (!m_current) {
                if (onSelect)
                    onSelect();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool hoverClose = overClose(event->position().toPoint());
        if (hoverClose != m_hoverClose) {
            m_hoverClose = hoverClose;
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        m_hoverClose = false;
        QWidget::leaveEvent(event);
        update();
    }

private:
    bool overClose(const QPoint &pos) const
    {
        const int right = width() - Theme::instance()->space3();
        return pos.x() >= right - m_closeSize && pos.x() < right;
    }

    QString m_id;
    QString m_title;
    QString m_cwd;
    QString m_cwdName;
    bool m_current;
    bool m_busy = false;
    bool m_awaiting = false;
    int m_titleWidth = 0;
    int m_cwdWidth = 0;
    int m_closeSize = 0;
    bool m_hoverClose = false;
    bool m_pressed = false;
};

class AcpPage::SessionStrip : public QWidget
{
public:
    SessionStrip(AcpPage *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("acpSessionBar"));

        auto *outer = new QHBoxLayout(this);
        outer->setContentsMargins(theme->space5(), theme->hairline(), theme->space5(), 0);
        outer->setSpacing(theme->space2());

        m_scroll = new QScrollArea(this);
        m_scroll->setObjectName(QStringLiteral("acpSessionList"));
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setWidgetResizable(true);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setFocusPolicy(Qt::NoFocus);

        QPalette scrollPalette = m_scroll->palette();
        scrollPalette.setColor(QPalette::Base, theme->panel());
        scrollPalette.setColor(QPalette::Window, theme->panel());
        m_scroll->setPalette(scrollPalette);
        m_scroll->viewport()->setPalette(scrollPalette);
        m_scroll->viewport()->setAutoFillBackground(false);

        m_row = new QWidget(m_scroll);
        m_row->setAutoFillBackground(false);
        m_rowLayout = new QHBoxLayout(m_row);
        m_rowLayout->setContentsMargins(0, 0, 0, 0);
        m_rowLayout->setSpacing(theme->space2());
        m_rowLayout->addStretch(1);
        m_scroll->setWidget(m_row);

        outer->addWidget(m_scroll, 1);

        m_new = new Button(QStringLiteral("+"), Button::Chip, this);
        m_new->setObjectName(QStringLiteral("acpSessionNew"));
        m_new->setPixelSize(theme->fontData() + theme->space1());
        m_new->setToolTip(QStringLiteral("New session on the same ACP child"));
        m_new->setHandler([this]() { emit m_owner->acpNewSessionRequested(); });
        outer->addWidget(m_new, 0, Qt::AlignVCenter);

        m_empty = new TextLine(this);
        m_empty->setObjectName(QStringLiteral("acpSessionEmpty"));
        m_empty->setLineFont(Chrome::uiFont(theme->fontData()));
        m_empty->setColour(theme->dim());
        m_empty->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_empty->raise();
    }

    void refresh()
    {
        const AcpClient *client = m_owner->client();
        const int state = m_owner->clientState();
        const QString current = client != nullptr ? client->sessionId() : QString();
        const QVariantList sessions =
            client != nullptr ? client->openSessions() : QVariantList();

        QString signature = QString::number(state) + QLatin1Char('|') + current;
        for (const QVariant &entry : sessions) {
            const QVariantMap session = entry.toMap();
            signature += QLatin1Char('|')
                         + session.value(QStringLiteral("id")).toString()
                         + QLatin1Char(':')
                         + session.value(QStringLiteral("title")).toString()
                         + QLatin1Char(':')
                         + session.value(QStringLiteral("cwdName")).toString()
                         + QLatin1Char(':')
                         + QString::number(session.value(QStringLiteral("busy")).toBool())
                         + QLatin1Char(':')
                         + QString::number(
                               session.value(QStringLiteral("awaitingPermission")).toBool());
        }

        m_new->setEnabled(client != nullptr && state == int(AcpClient::Ready));

        if (signature == m_signature) {
            placeEmpty();
            return;
        }
        m_signature = signature;

        for (Chip *chip : m_chips) {
            m_rowLayout->removeWidget(chip);
            delete chip;
        }
        m_chips.clear();

        for (const QVariant &entry : sessions) {
            const QVariantMap session = entry.toMap();
            const QString id = session.value(QStringLiteral("id")).toString();
            auto *chip = new Chip(session, id == current, m_row);
            chip->onSelect = [this, id]() { emit m_owner->acpSessionSelected(id); };
            chip->onClose = [this, id]() { emit m_owner->acpSessionCloseRequested(id); };
            m_rowLayout->insertWidget(std::max(0, m_rowLayout->count() - 1), chip);
            m_chips.append(chip);
        }

        if (sessions.isEmpty()) {
            if (state == int(AcpClient::Failed))
                m_empty->setText(QStringLiteral("the ACP server is not running"));
            else if (state == int(AcpClient::Ready))
                m_empty->setText(QStringLiteral("no sessions \u2014 + starts one on this "
                                                "connection"));
            else
                m_empty->setText(QStringLiteral("waiting for the ACP server\u2026"));
        }
        placeEmpty();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(QRect(0, 0, width(), Theme::instance()->hairline()),
                         Theme::instance()->border());
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeEmpty();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        QScrollBar *bar = m_scroll->horizontalScrollBar();
        const QPoint delta = event->angleDelta();
        if (bar->maximum() > 0) {
            bar->setValue(bar->value() - delta.y() - delta.x());
            event->accept();
            return;
        }
        QWidget::wheelEvent(event);
    }

private:
    void placeEmpty()
    {
        Theme *theme = Theme::instance();
        m_empty->setVisible(m_chips.isEmpty());
        if (m_chips.isEmpty()) {
            const int room = std::max(0, width() - 2 * theme->space5() - theme->space6() * 3);
            const int lineHeight = theme->fontData() + 2 * theme->space2();
            m_empty->setGeometry(theme->space5(), (height() - lineHeight) / 2, room,
                                 lineHeight);
        }
    }

    AcpPage *m_owner;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_row = nullptr;
    QHBoxLayout *m_rowLayout = nullptr;
    Button *m_new = nullptr;
    TextLine *m_empty = nullptr;
    QList<Chip *> m_chips;
    QString m_signature;
};

class AcpPage::Row : public QWidget
{
public:
    Row(AcpPage *owner, int index, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
        , m_index(index)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("acpRow"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_user = new QTextEdit(this);
        configureProse(m_user, Chrome::uiFont(theme->fontProse()), theme->fg());
        m_user->hide();

        m_agent = new QTextEdit(this);
        configureProse(m_agent, Chrome::uiFont(theme->fontProse()), theme->fg());
        m_agent->hide();

        m_user->installEventFilter(this);
        m_agent->installEventFilter(this);
    }

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
        m_measuredWidth = -1;

        if (isUser()) {
            m_user->setPlainText(m_body);
        } else if (isAgent()) {
            Theme *theme = Theme::instance();
            m_agent->setFont(Chrome::uiFont(theme->fontProse()));
            m_agent->document()->setDefaultFont(Chrome::uiFont(theme->fontProse()));
            m_agent->setHtml(Markdown::instance()->toHtml(m_body));
            m_agent->document()->setDocumentMargin(0);
            applyProseRhythm(m_agent->document());
        }

        m_user->setVisible(isUser());
        m_agent->setVisible(isAgent() && !m_body.isEmpty());
        update();
    }

    int heightFor(int width)
    {
        if (width <= 0)
            return 0;
        if (width == m_measuredWidth)
            return m_measuredHeight;

        Theme *theme = Theme::instance();
        int height = 0;

        if (isUser()) {
            if (m_body.isEmpty()) {
                m_bubbleWidth = 0;
                m_bubbleHeight = 0;
                m_measuredWidth = width;
                m_measuredHeight = 0;
                return 0;
            }
            const QFont font = Chrome::uiFont(theme->fontProse());
            const QFontMetrics metrics(font);
            int longest = 0;
            const QStringList lines = m_body.split(QLatin1Char('\n'));
            for (const QString &line : lines)
                longest = std::max(longest, metrics.horizontalAdvance(line));

            const int textWidth = std::max(0, std::min(longest, int(width * BubbleWidthRatio)));

            m_bubbleWidth = textWidth + 2 * theme->space3();
            m_bubbleHeight = plainHeight(m_body, font, std::max(1, textWidth))
                             + 2 * theme->space3();
            height = m_bubbleHeight;
        } else if (isAgent()) {
            height = m_body.isEmpty()
                         ? 0
                         : proseHeight(Markdown::instance()->toHtml(m_body),
                                       Chrome::uiFont(theme->fontProse()), width);
        } else {
            const QList<Block> blocks = processBlocks();
            for (int index = 0; index < blocks.size(); ++index) {
                if (index > 0)
                    height += theme->space1();
                height += blockHeight(blocks.at(index), width);
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
            m_user->setGeometry(x + theme->space3(), theme->space3(),
                                std::max(0, m_bubbleWidth - 2 * theme->space3()),
                                std::max(0, m_bubbleHeight - 2 * theme->space3()));
        }
        if (isAgent())
            m_agent->setGeometry(0, 0, width, std::max(0, m_measuredHeight));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);

        if (isUser()) {
            if (m_bubbleWidth <= 0)
                return;
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

        const QList<Block> blocks = processBlocks();
        int y = 0;
        for (int index = 0; index < blocks.size(); ++index) {
            const Block &block = blocks.at(index);
            if (index > 0)
                y += theme->space1();
            const int height = blockHeight(block, width());
            if (block.single) {
                Chrome::paintLine(painter, QRect(0, y, width(), height), block.font,
                                  block.colour, block.text);
            } else {
                drawWrapped(&painter, QRect(0, y, width(), height), block.text, block.font,
                            block.colour, block.maxLines);
            }
            y += height;
        }
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
    struct Block
    {
        QString text;
        QFont font;
        QColor colour;
        int maxLines = 1;
        bool single = false;
    };

    bool isUser() const { return m_kind == QLatin1String("user"); }
    bool isAgent() const { return m_kind == QLatin1String("agent"); }
    bool isProcess() const { return !isUser() && !isAgent(); }

    QColor rowColour() const
    {
        Theme *theme = Theme::instance();
        if (m_kind == QLatin1String("error"))
            return theme->danger();
        if (m_kind == QLatin1String("tool"))
            return theme->caption();
        if (m_kind == QLatin1String("thought"))
            return theme->faint();
        return theme->dim();
    }

    QList<Block> processBlocks() const
    {
        Theme *theme = Theme::instance();
        QList<Block> blocks;

        if (!m_title.isEmpty()) {
            Block block;
            block.text = m_title;
            if (!m_status.isEmpty())
                block.text += QStringLiteral("  \u00b7  ") + m_status;
            block.font = Chrome::uiFont(theme->fontData());
            block.colour = rowColour();
            block.maxLines = 1;
            block.single = true;
            blocks.append(block);
        }
        if (!m_body.isEmpty()) {
            Block block;
            block.text = m_body;
            block.font = Chrome::uiFont(theme->fontData());
            block.colour = rowColour();
            block.maxLines = ProcessTextLines;
            blocks.append(block);
        }
        if (!m_request.isEmpty()) {
            Block block;
            block.text = m_request;
            block.font = Chrome::monoFont(theme->fontData());
            block.colour = theme->caption();
            block.maxLines = PayloadLines;
            blocks.append(block);
        }
        if (!m_detail.isEmpty()) {
            Block block;
            block.text = m_detail;
            block.font = Chrome::monoFont(theme->fontData());
            block.colour = theme->dim();
            block.maxLines = PayloadLines;
            blocks.append(block);
        }
        return blocks;
    }

    int blockHeight(const Block &block, int width) const
    {
        if (block.single)
            return QFontMetrics(block.font).height();
        return wrappedHeight(block.text, block.font, width, block.maxLines);
    }

    AcpPage *m_owner;
    int m_index;
    QString m_kind;
    QString m_body;
    QString m_title;
    QString m_status;
    QString m_detail;
    QString m_request;
    QTextEdit *m_user = nullptr;
    QTextEdit *m_agent = nullptr;
    int m_measuredWidth = -1;
    int m_measuredHeight = 0;
    int m_bubbleWidth = 0;
    int m_bubbleHeight = 0;
};

class AcpPage::Column : public QWidget
{
public:
    Column(AcpPage *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        setObjectName(QStringLiteral("acpRows"));
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
    AcpPage *m_owner;
};

class AcpPage::Entry : public QPlainTextEdit
{
public:
    Entry(AcpPage *owner, QWidget *parent)
        : QPlainTextEdit(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("acpPageInput"));
        setFont(Chrome::uiFont(theme->fontUI()));
        setFrameShape(QFrame::NoFrame);
        setContextMenuPolicy(Qt::NoContextMenu);
        setBackgroundVisible(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setTabChangesFocus(false);
        setAcceptDrops(false);
        document()->setDocumentMargin(theme->space4());

        QPalette palette = this->palette();
        palette.setColor(QPalette::Base, theme->fieldFill());
        palette.setColor(QPalette::Window, theme->fieldFill());
        palette.setColor(QPalette::Text, theme->fg());
        palette.setColor(QPalette::WindowText, theme->fg());
        palette.setColor(QPalette::PlaceholderText, theme->dim());
        palette.setColor(QPalette::Highlight, theme->accent());
        palette.setColor(QPalette::HighlightedText, theme->accentFg());
        palette.setColor(QPalette::Disabled, QPalette::Base, theme->disabledFill());
        palette.setColor(QPalette::Disabled, QPalette::Text, theme->faint());
        palette.setColor(QPalette::Disabled, QPalette::WindowText, theme->faint());
        palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, theme->faint());
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
            m_owner->sendDraft();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

private:
    AcpPage *m_owner;
};

class AcpPage::Field : public QWidget
{
public:
    Field(AcpPage *owner, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("acpPageField"));
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

    QSize sizeHint() const override
    {
        return QSize(200, m_entry->preferredHeight());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, Theme::instance()->fieldHeight());
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

class AcpPage::PermissionBand : public Band
{
public:
    PermissionBand(AcpPage *owner, QWidget *parent)
        : Band(true, false, parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("acpPermission"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        m_heading = new TextLine(this);
        m_heading->setText(QStringLiteral("Permission requested"));
        m_heading->setLineFont(Chrome::uiFont(theme->fontProse() + 2, true));
        m_heading->setColour(theme->fg());

        m_tool = new TextLine(this);
        m_tool->setLineFont(Chrome::monoFont(theme->fontProse() + 2));
        m_tool->setColour(theme->fg());

        m_reason = new QLabel(this);
        m_reason->setTextFormat(Qt::PlainText);
        m_reason->setWordWrap(true);
        m_reason->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_reason->setFont(Chrome::uiFont(theme->fontUI()));
        QPalette palette = m_reason->palette();
        palette.setColor(QPalette::WindowText, theme->dim());
        palette.setColor(QPalette::Text, theme->dim());
        m_reason->setPalette(palette);

        m_reject = new Button(QStringLiteral("Reject"), Button::Push, this);
        m_reject->setObjectName(QStringLiteral("acpPermissionReject"));
        m_reject->setHandler([this]() { m_owner->answerPermission(false); });

        m_allow = new Button(QStringLiteral("Allow once"), Button::Push, this);
        m_allow->setObjectName(QStringLiteral("acpPermissionAllow"));
        m_allow->setHighlighted(true);
        m_allow->setHandler([this]() { m_owner->answerPermission(true); });
    }

    void setPrompt(const QString &toolName, const QString &detail)
    {
        m_tool->setText(toolName);
        m_reason->setText(detail.isEmpty()
                              ? QStringLiteral("The ACP agent is asking to run the tool "
                                               "above.")
                              : detail);
        relayout();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        Band::resizeEvent(event);
        relayout();
    }

private:
    void relayout()
    {
        Theme *theme = Theme::instance();
        const int pad = theme->space6() + theme->space1();
        const int available = std::max(1, width() - 2 * pad);
        const int headingHeight = m_heading->lineHeight();
        const int toolHeight = m_tool->lineHeight();

        m_reason->setFixedWidth(available);
        int reasonHeight = m_reason->heightForWidth(available);
        if (reasonHeight <= 0)
            reasonHeight = QFontMetrics(m_reason->font()).height();

        const int buttonHeight = m_allow->height();
        const int total = pad + headingHeight + theme->space4() + toolHeight + theme->space4()
                          + reasonHeight + theme->space4() + buttonHeight + pad;
        if (height() != total)
            setFixedHeight(total);

        int y = pad;
        m_heading->setGeometry(pad, y, available, headingHeight);
        y += headingHeight + theme->space4();
        m_tool->setGeometry(pad, y, available, toolHeight);
        y += toolHeight + theme->space4();
        m_reason->setGeometry(pad, y, available, reasonHeight);
        y += reasonHeight + theme->space4();

        const int allowWidth = m_allow->width();
        const int rejectWidth = m_reject->width();
        m_allow->setGeometry(pad + available - allowWidth, y, allowWidth, buttonHeight);
        m_reject->setGeometry(pad + available - allowWidth - theme->space3() - rejectWidth, y,
                              rejectWidth, buttonHeight);
    }

    AcpPage *m_owner;
    TextLine *m_heading = nullptr;
    TextLine *m_tool = nullptr;
    QLabel *m_reason = nullptr;
    Button *m_reject = nullptr;
    Button *m_allow = nullptr;
};

AcpPage::AcpPage(AcpClient *client, const QString &iconDir, QWidget *parent)
    : QWidget(parent)
    , m_client(client)
    , m_iconDir(iconDir)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("acpPage"));
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    const int stripHeight = theme->fontData() + 4 * theme->space2();

    m_bar = new Band(false, true, this);
    m_bar->setObjectName(QStringLiteral("acpBar"));
    m_bar->setFixedHeight(theme->headerHeight() + stripHeight);
    m_bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *barColumn = new QVBoxLayout(m_bar);
    barColumn->setContentsMargins(0, 0, 0, 0);
    barColumn->setSpacing(0);

    auto *barTop = new QWidget(m_bar);
    barTop->setFixedHeight(theme->headerHeight());
    auto *barRow = new QHBoxLayout(barTop);
    barRow->setContentsMargins(theme->space5(), 0, theme->space5(), 0);
    barRow->setSpacing(theme->space4());

    m_title = new TextLine(barTop);
    m_title->setText(QStringLiteral("ACP"));
    m_title->setLineFont(Chrome::uiFont(theme->fontData()));
    m_title->setColour(theme->fg());
    barRow->addWidget(m_title, 0, Qt::AlignVCenter);

    m_status = new TextLine(barTop);
    m_status->setObjectName(QStringLiteral("acpPageStatus"));
    m_status->setLineFont(Chrome::monoFont(theme->fontData()));
    m_status->setColour(theme->dim());
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    barRow->addWidget(m_status, 1, Qt::AlignVCenter);

    m_close = new Button(QStringLiteral("close"), Button::Chip, barTop);
    m_close->setObjectName(QStringLiteral("acpClose"));
    m_close->setToolTip(QStringLiteral("Close the pane and stop the ACP child"));
    m_close->setHandler([this]() { emit closeRequested(); });
    barRow->addWidget(m_close, 0, Qt::AlignVCenter);

    barColumn->addWidget(barTop);

    m_strip = new SessionStrip(this, m_bar);
    m_strip->setFixedHeight(stripHeight);
    barColumn->addWidget(m_strip);

    outer->addWidget(m_bar);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("acpTranscript"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    Chrome::styleScrollBar(m_scroll->verticalScrollBar());

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
    m_empty->setObjectName(QStringLiteral("acpEmpty"));
    m_empty->setTextFormat(Qt::PlainText);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_empty->setFont(Chrome::uiFont(theme->fontProse()));
    QPalette emptyPalette = m_empty->palette();
    emptyPalette.setColor(QPalette::WindowText, theme->dim());
    m_empty->setPalette(emptyPalette);
    m_empty->hide();

    m_scroll->setWidget(m_column);

    outer->addWidget(m_scroll, 1);

    m_permission = new PermissionBand(this, this);
    m_permission->hide();
    outer->addWidget(m_permission);

    m_composer = new Band(true, false, this);
    m_composer->setObjectName(QStringLiteral("acpComposer"));
    m_composer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *composerRow = new QHBoxLayout(m_composer);
    composerRow->setContentsMargins(theme->space3(), theme->space3(), theme->space3(),
                                    theme->space3());
    composerRow->setSpacing(theme->space3());

    m_field = new Field(this, m_composer);
    m_entry = m_field->entry();
    composerRow->addWidget(m_field, 1);

    m_send = new Button(QStringLiteral("send"), Button::Push, m_composer);
    m_send->setObjectName(QStringLiteral("acpPaneSend"));
    m_send->setHandler([this]() { stopOrSend(); });
    composerRow->addWidget(m_send, 0, Qt::AlignVCenter);

    outer->addWidget(m_composer);

    QObject::connect(m_entry, &QPlainTextEdit::textChanged, this,
                     [this]() { refreshComposer(); });
    QObject::connect(m_entry->document()->documentLayout(),
                     &QAbstractTextDocumentLayout::documentSizeChanged, this,
                     [this]() { refreshComposer(); });

    if (m_client != nullptr) {
        QObject::connect(m_client, &AcpClient::stateChanged, this, [this]() {
            if (clientState() == AcpClient::Failed)
                clearPermission();
            refreshBar();
            refreshComposer();
            refreshSessions();
            refreshEmpty();
        });
        QObject::connect(m_client, &AcpClient::statusChanged, this,
                         [this]() { refreshBar(); });
        QObject::connect(m_client, &AcpClient::lastErrorChanged, this, [this]() {
            refreshBar();
            refreshEmpty();
        });
        QObject::connect(m_client, &AcpClient::busyChanged, this,
                         [this]() { refreshComposer(); });
        QObject::connect(m_client, &AcpClient::sessionChanged, this, [this]() {
            clearPermission();
            m_follow = true;
            refreshBar();
            refreshComposer();
            refreshSessions();
            refreshEmpty();
            followTail();
        });
        QObject::connect(m_client, &AcpClient::transcriptChanged, this, [this]() {
            bindTranscript();
            refreshEmpty();
        });
        QObject::connect(m_client, &AcpClient::openSessionsChanged, this, [this]() {
            refreshSessions();
            refreshEmpty();
        });
        QObject::connect(m_client, &AcpClient::permissionRequested, this,
                         [this](const QString &requestId, const QString &toolName,
                                const QString &detail) {
            if (!isVisible())
                return;
            showPermission(requestId, toolName, detail);
        });
        QObject::connect(m_client, &AcpClient::permissionWithdrawn, this,
                         [this](const QString &requestId) { withdrawPermission(requestId); });
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

    bindTranscript();
    refreshBar();
    refreshComposer();
    refreshSessions();
    refreshEmpty();
}

void AcpPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    focusPage();
}

void AcpPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Theme::instance()->bg());
}

void AcpPage::dragEnterEvent(QDragEnterEvent *event)
{
    event->ignore();
}

void AcpPage::dragMoveEvent(QDragMoveEvent *event)
{
    event->ignore();
}

void AcpPage::dropEvent(QDropEvent *event)
{
    event->ignore();
}

void AcpPage::focusPage()
{
    pinToTail();
    if (m_entry != nullptr)
        m_entry->setFocus(Qt::OtherFocusReason);
}

void AcpPage::pinToTail()
{
    m_follow = true;
    followTail();
}

void AcpPage::shut()
{
    clearPermission();
    if (m_entry != nullptr)
        m_entry->clear();
    m_follow = true;
}

void AcpPage::sendDraft()
{
    if (m_client == nullptr || clientState() != AcpClient::Ready
        || m_client->sessionId().isEmpty())
        return;
    const QString text = m_entry->toPlainText().trimmed();
    if (text.isEmpty())
        return;

    const bool command = !text.contains(QLatin1Char('\n'))
                         && text.startsWith(QLatin1Char('/'));
    if (command)
        m_client->runCommand(text);
    else
        m_client->sendPrompt(text);
    if (!command || m_client->slashCommands().contains(text.split(QLatin1Char(' ')).first()))
        m_entry->clear();
    m_entry->setFocus(Qt::OtherFocusReason);
    refreshComposer();
}

void AcpPage::stopOrSend()
{
    if (m_client == nullptr)
        return;
    if (m_client->busy())
        m_client->cancel();
    else
        sendDraft();
}

void AcpPage::refreshBar()
{
    Theme *theme = Theme::instance();
    const int state = clientState();
    const QString error = m_client != nullptr ? m_client->lastError() : QString();
    const QString status = m_client != nullptr ? m_client->status() : QString();

    QString line;
    if (state == AcpClient::Failed)
        line = error.isEmpty() ? QStringLiteral("the ACP server is not running") : error;
    else if (!status.isEmpty())
        line = status;
    else
        line = state == AcpClient::Idle ? QStringLiteral("not started")
                                        : QStringLiteral("starting\u2026");

    m_status->setText(line);
    m_status->setColour(state == AcpClient::Failed ? theme->danger() : theme->dim());
}

void AcpPage::refreshComposer()
{
    const bool ready = clientState() == AcpClient::Ready;
    const QString sessionId = m_client != nullptr ? m_client->sessionId() : QString();
    const bool hasSession = !sessionId.isEmpty();
    const bool busy = m_client != nullptr && m_client->busy();

    QString placeholder;
    if (ready)
        placeholder = hasSession ? QStringLiteral("Ask the ACP agent\u2026")
                                 : QStringLiteral("no session \u2014 + starts one");
    else if (clientState() == AcpClient::Failed)
        placeholder = QStringLiteral("the ACP server is not running");
    else
        placeholder = QStringLiteral("waiting for the ACP server\u2026");

    m_entry->setPlaceholderText(placeholder);
    m_entry->setEnabled(m_client != nullptr && ready && hasSession);
    m_send->setText(busy ? QStringLiteral("stop") : QStringLiteral("send"));
    m_send->setEnabled(m_client != nullptr
                       && (busy
                           || (ready && hasSession
                               && !m_entry->toPlainText().trimmed().isEmpty())));
    m_field->adoptHeight(m_entry->preferredHeight());
}

void AcpPage::refreshSessions()
{
    if (m_strip != nullptr)
        m_strip->refresh();
}

void AcpPage::refreshEmpty()
{
    if (m_empty == nullptr)
        return;

    const int state = clientState();
    const QString error = m_client != nullptr ? m_client->lastError() : QString();
    QString text;
    if (state == AcpClient::Failed)
        text = error.isEmpty() ? QStringLiteral("the ACP server is not available") : error;
    else if (state == AcpClient::Idle || state == AcpClient::Starting)
        text = QStringLiteral("starting the ACP server\u2026\n\nthis takes a few seconds");
    else if (m_client == nullptr || m_client->openSessions().isEmpty())
        text = QStringLiteral("no sessions yet\n\n+ starts one on this connection");
    else
        text = QStringLiteral("ready \u2014 ask something");

    m_empty->setText(text);
    relayoutRows();
}

void AcpPage::refreshPermission()
{
    if (m_permission == nullptr)
        return;
    m_permission->setVisible(!m_permissionRequestId.isEmpty());
}

void AcpPage::showPermission(const QString &requestId, const QString &toolName,
                             const QString &detail)
{
    m_permissionRequestId = requestId;
    if (m_permission == nullptr)
        return;
    m_permission->setPrompt(toolName, detail);
    refreshPermission();
}

void AcpPage::clearPermission()
{
    m_permissionRequestId.clear();
    refreshPermission();
}

void AcpPage::withdrawPermission(const QString &requestId)
{
    if (m_permissionRequestId != requestId)
        return;
    clearPermission();
}

void AcpPage::answerPermission(bool allow)
{
    if (m_permissionRequestId.isEmpty())
        return;
    const QString requestId = m_permissionRequestId;
    clearPermission();
    if (m_client != nullptr)
        m_client->respondToPermission(requestId, allow);
    if (m_entry != nullptr)
        m_entry->setFocus(Qt::OtherFocusReason);
}

AcpTranscript *AcpPage::transcript() const
{
    return m_client != nullptr ? m_client->transcript() : nullptr;
}

int AcpPage::clientState() const
{
    return m_client != nullptr ? int(m_client->state()) : int(AcpClient::Idle);
}

void AcpPage::bindTranscript()
{
    for (const QMetaObject::Connection &connection : m_modelConnections)
        QObject::disconnect(connection);
    m_modelConnections.clear();

    AcpTranscript *model = transcript();
    if (model != nullptr) {
        m_modelConnections.append(QObject::connect(
            model, &QAbstractItemModel::rowsInserted, this,
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
            }));
        m_modelConnections.append(QObject::connect(
            model, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &parent) {
                if (parent.isValid())
                    return;
                rebuildRows();
            }));
        m_modelConnections.append(QObject::connect(
            model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                if (topLeft.parent().isValid())
                    return;
                for (int index = topLeft.row();
                     index <= bottomRight.row() && index < m_rows.size(); ++index) {
                    m_rows[index]->refresh();
                }
                relayoutRows();
            }));
        m_modelConnections.append(QObject::connect(
            model, &QAbstractItemModel::modelReset, this, [this]() { rebuildRows(); }));
        m_modelConnections.append(QObject::connect(
            model, &QAbstractItemModel::layoutChanged, this, [this]() { rebuildRows(); }));
    }

    rebuildRows();
}

void AcpPage::rebuildRows()
{
    for (Row *row : m_rows)
        delete row;
    m_rows.clear();

    AcpTranscript *model = transcript();
    const int count = model != nullptr ? model->rowCount() : 0;
    m_rows.reserve(count);
    for (int index = 0; index < count; ++index) {
        auto *row = new Row(this, index, m_column);
        row->refresh();
        m_rows.append(row);
    }
    relayoutRows();
    if (m_follow)
        followTail();
}

void AcpPage::relayoutRows()
{
    if (m_relayingOut || m_column == nullptr || m_scroll == nullptr || m_empty == nullptr)
        return;
    m_relayingOut = true;

    Theme *theme = Theme::instance();
    const int width = m_column->width();
    const int rowWidth = std::max(0, width - 2 * theme->space5());
    int y = theme->space4();
    for (int index = 0; index < m_rows.size(); ++index) {
        if (index > 0)
            y += theme->space3();
        Row *row = m_rows.at(index);
        const int height = row->heightFor(rowWidth);
        row->setGeometry(theme->space5(), y, rowWidth, height);
        row->layoutChildren(rowWidth);
        y += height;
    }

    const int viewportHeight = m_scroll->viewport()->height();
    const int total = std::max(y + theme->space4(), viewportHeight);
    if (m_column->minimumHeight() != total)
        m_column->setMinimumHeight(total);

    if (m_rows.isEmpty()) {
        const int labelWidth =
            std::max(0, std::min(width - 2 * theme->space6(), 32 * theme->space6()));
        const QFont font = Chrome::uiFont(theme->fontProse());
        const QFontMetrics metrics(font);
        const QRect bounds = metrics.boundingRect(
            QRect(0, 0, std::max(1, labelWidth), 10000),
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

void AcpPage::followTail()
{
    if (m_scroll == nullptr)
        return;
    m_scrollSettling = true;
    m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    m_scrollSettling = false;
}
