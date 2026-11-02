#include "WebPage.h"

#include "ShellChrome.h"
#include "Theme.h"

#ifdef DSH_SERVO
#include "ServoWidget.h"
#endif

#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

constexpr int WrapProbe = 10000;

class Chip : public QWidget
{
public:
    explicit Chip(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_text(text)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setHandler(std::function<void()> handler) { m_handler = std::move(handler); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        return QSize(metrics.horizontalAdvance(m_text) + 2 * theme->space3(),
                     metrics.height() + 2 * theme->space2());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        Chrome::paintChip(painter, rect(), m_hover, false);
        Chrome::paintLine(painter, rect(), Chrome::uiFont(theme->fontData()),
                          m_hover ? theme->fg() : theme->dim(), m_text, Qt::AlignCenter);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        m_hover = true;
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        m_hover = false;
        m_pressed = false;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
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
        const bool inside = m_pressed && rect().contains(event->position().toPoint());
        m_pressed = false;
        update();
        if (inside && m_handler)
            m_handler();
        event->accept();
    }

private:
    QString m_text;
    std::function<void()> m_handler;
    bool m_hover = false;
    bool m_pressed = false;
};

class Line : public QWidget
{
public:
    Line(const QFont &font, const QColor &colour, QWidget *parent)
        : QWidget(parent)
        , m_font(font)
        , m_colour(colour)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setExpanding(bool expanding)
    {
        setSizePolicy(expanding ? QSizePolicy::Expanding : QSizePolicy::Fixed,
                      QSizePolicy::Fixed);
    }

    void setElideMiddle(bool elideMiddle) { m_elideMiddle = elideMiddle; }

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
        Chrome::paintLine(painter, rect(), m_font, m_colour, m_text,
                          Qt::AlignLeft | Qt::AlignVCenter, m_elideMiddle);
    }

private:
    QString m_text;
    QFont m_font;
    QColor m_colour;
    bool m_elideMiddle = false;
};

}

class WebPage::Bar : public QWidget
{
public:
    explicit Bar(QWidget *parent)
        : QWidget(parent)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("webBar"));
        setFixedHeight(theme->headerHeight());
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(theme->space5(), 0, theme->space5(), 0);
        layout->setSpacing(theme->space4());

        m_title = new Line(Chrome::uiFont(theme->fontData()), theme->fg(), this);
        m_title->setText(QStringLiteral("web UI"));

        m_url = new Line(Chrome::monoFont(theme->fontData()), theme->dim(), this);
        m_url->setObjectName(QStringLiteral("webTrapdoorUrl"));
        m_url->setElideMiddle(true);
        m_url->setExpanding(true);
        m_url->setMinimumWidth(0);

        m_ime = new Line(Chrome::uiFont(theme->fontData()), theme->warn(), this);
        m_ime->setText(QStringLiteral("plain keys only — no IME"));

        m_browser = new Chip(QStringLiteral("open in browser"), this);
        m_browser->setObjectName(QStringLiteral("webBrowserButton"));

        m_close = new Chip(QStringLiteral("close"), this);
        m_close->setObjectName(QStringLiteral("webCloseButton"));
        m_close->setToolTip(
            QStringLiteral("Close the page and free it — the web toggle only puts it away"));

        layout->addWidget(m_title, 0, Qt::AlignVCenter);
        layout->addWidget(m_url, 1, Qt::AlignVCenter);
        layout->addWidget(m_ime, 0, Qt::AlignVCenter);
        layout->addWidget(m_browser, 0, Qt::AlignVCenter);
        layout->addWidget(m_close, 0, Qt::AlignVCenter);
    }

    void setUrl(const QUrl &url) { m_url->setText(url.toString()); }
    void setAvailable(bool available) { m_ime->setVisible(available); }
    void setBrowserHandler(std::function<void()> handler) { m_browser->setHandler(std::move(handler)); }
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
    Line *m_url;
    Line *m_ime;
    Chip *m_browser;
    Chip *m_close;
};

class WebPage::Message : public QWidget
{
public:
    explicit Message(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    void setUrl(const QUrl &url)
    {
        m_url = url;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->bg());

        const QRect body = rect().adjusted(theme->space6(), theme->space6(),
                                           -theme->space6(), -theme->space6());
        if (body.width() <= 0 || body.height() <= 0)
            return;

        painter.setFont(Chrome::uiFont(theme->fontProse()));
        painter.setPen(theme->dim());
        painter.drawText(body, Qt::AlignCenter | Qt::TextWordWrap, m_url.toString().isEmpty()
                             ? QStringLiteral("This build has no Servo web engine.")
                             : QStringLiteral("This build has no Servo web engine.\n\n"
                                              "Open %1 in your browser, or rebuild"
                                              " with -DDSH_SERVO=ON.")
                                   .arg(m_url.toString()));
    }

private:
    QUrl m_url;
};

class WebPage::StatusLine : public QWidget
{
public:
    explicit StatusLine(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
    }

    void setText(const QString &text)
    {
        if (m_text == text)
            return;
        m_text = text;
        update();
    }

    int wrappedHeight(int width) const
    {
        if (m_text.isEmpty() || width <= 0)
            return 0;
        const QFontMetrics metrics(Chrome::uiFont(Theme::instance()->fontData()));
        return metrics.boundingRect(QRect(0, 0, width, WrapProbe), Qt::TextWordWrap, m_text)
            .height();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setFont(Chrome::uiFont(theme->fontData()));
        painter.setPen(theme->caption());
        painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_text);
    }

private:
    QString m_text;
};

WebPage::WebPage(QWidget *parent)
    : QWidget(parent)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("webPage"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);

    QPalette colours = palette();
    colours.setColor(QPalette::Window, theme->bg());
    setPalette(colours);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_bar = new Bar(this);
    layout->addWidget(m_bar);

#ifdef DSH_SERVO
    m_servo = new ServoWidget(this);
    m_servo->setObjectName(QStringLiteral("servoWidget"));
    layout->addWidget(m_servo, 1);
    connect(m_servo, &ServoWidget::statusChanged, this, &WebPage::setStatus);
#else
    m_message = new Message(this);
    layout->addWidget(m_message, 1);
#endif

    m_statusLine = new StatusLine(this);
    m_statusLine->hide();

    m_bar->setAvailable(available());
    m_bar->setBrowserHandler([this]() { emit browserRequested(); });
    m_bar->setCloseHandler([this]() { emit closeRequested(); });
}

QUrl WebPage::url() const
{
    return m_url;
}

void WebPage::setUrl(const QUrl &url)
{
    if (m_url == url)
        return;
    m_url = url;
    m_bar->setUrl(url);
#ifdef DSH_SERVO
    if (m_servo != nullptr)
        m_servo->setUrl(url);
#else
    if (m_message != nullptr)
        m_message->setUrl(url);
#endif
    update();
}

QString WebPage::status() const
{
    return m_status;
}

void WebPage::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    m_statusLine->setText(status);
    applyStatusGeometry();
    emit statusChanged(status);
}

bool WebPage::available() const
{
#ifdef DSH_SERVO
    return m_servo != nullptr;
#else
    return false;
#endif
}

void WebPage::focusPage()
{
#ifdef DSH_SERVO
    if (m_servo != nullptr) {
        m_servo->setFocus(Qt::PopupFocusReason);
        return;
    }
#endif
    setFocus(Qt::PopupFocusReason);
}

void WebPage::unload()
{
#ifdef DSH_SERVO
    if (m_servo != nullptr)
        m_servo->unload();
#endif
    setStatus(QString());
}

void WebPage::applyStatusGeometry()
{
    Theme *theme = Theme::instance();
    const int inset = theme->space6();
    const int lineWidth = width() - 2 * inset;
    const int lineHeight = lineWidth > 0 ? m_statusLine->wrappedHeight(lineWidth) : 0;

    if (m_status.isEmpty() || lineWidth <= 0 || lineHeight <= 0) {
        m_statusLine->hide();
        return;
    }

    m_statusLine->setGeometry(inset, height() - theme->space3() - lineHeight,
                              lineWidth, lineHeight);
    m_statusLine->show();
    m_statusLine->raise();
}

void WebPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applyStatusGeometry();
    if (!available())
        setFocus(Qt::PopupFocusReason);
}

void WebPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyStatusGeometry();
}

void WebPage::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        emit minimiseRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool WebPage::focusNextPrevChild(bool)
{
    return true;
}
