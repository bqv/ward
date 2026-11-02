#include "ServoWidget.h"

#include "Theme.h"

#include <QClipboard>
#include <QDebug>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMutexLocker>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyleHints>
#include <QWheelEvent>
#include <QWindow>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

#include <cstdint>
#include <cstring>

extern "C" {
struct ServoWebView;
ServoWebView *servo_webview_new(void *(*get_proc)(const char *), void (*wake)(),
                                unsigned width, unsigned height, float scale);
void servo_webview_load(ServoWebView *, const char *url);
void servo_webview_resize(ServoWebView *, unsigned width, unsigned height);
void servo_webview_spin(ServoWebView *);
void servo_webview_paint(ServoWebView *);
void servo_webview_destroy(ServoWebView *);
void servo_webview_mouse_button(ServoWebView *, uint8_t action, uint8_t button,
                                float x, float y);
void servo_webview_mouse_move(ServoWebView *, float x, float y);
void servo_webview_wheel(ServoWebView *, float dx, float dy, float x, float y);
void servo_webview_key(ServoWebView *, bool down, const char *key, const char *code,
                       uint32_t modifiers, bool repeat);
void servo_webview_focus(ServoWebView *, bool focused);

typedef size_t (*ClipboardGetText)(size_t user, char *buf, size_t cap);
typedef void (*ClipboardSetText)(size_t user, const char *text);
void servo_webview_set_clipboard(ClipboardGetText get_text,
                                 ClipboardSetText set_text, size_t user);
}

static bool inputTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("DSH_SERVO_INPUT_DEBUG");
    return enabled;
}

static void traceShim(const char *what)
{
    if (inputTraceEnabled())
        qInfo().noquote() << "[servo-shim]" << what;
}

static size_t clipboardGetText(size_t, char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0)
        return 0;
    const QString text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    if (text.isEmpty())
        return 0;

    const QByteArray utf8 = text.toUtf8();
    if (size_t(utf8.size()) > cap)
        return 0;
    std::memcpy(buf, utf8.constData(), size_t(utf8.size()));
    return size_t(utf8.size());
}

static void clipboardSetText(size_t, const char *text)
{
    if (text == nullptr)
        return;
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text),
                                         QClipboard::Clipboard);
}

namespace {

std::atomic<bool> g_wake{false};

void servoWake()
{
    g_wake.store(true, std::memory_order_relaxed);
}

void *servoGetProc(const char *name)
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr)
        return nullptr;
    return reinterpret_cast<void *>(context->getProcAddress(name));
}

const char *namedKeyFor(int key)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return "Enter";
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return "Tab";
    case Qt::Key_Backspace:
        return "Backspace";
    case Qt::Key_Delete:
        return "Delete";
    case Qt::Key_Escape:
        return "Escape";
    case Qt::Key_Insert:
        return "Insert";
    case Qt::Key_Home:
        return "Home";
    case Qt::Key_End:
        return "End";
    case Qt::Key_PageUp:
        return "PageUp";
    case Qt::Key_PageDown:
        return "PageDown";
    case Qt::Key_Left:
        return "ArrowLeft";
    case Qt::Key_Right:
        return "ArrowRight";
    case Qt::Key_Up:
        return "ArrowUp";
    case Qt::Key_Down:
        return "ArrowDown";
    default:
        return nullptr;
    }
}

const char *codeFor(int key)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return "Enter";
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return "Tab";
    case Qt::Key_Backspace:
        return "Backspace";
    case Qt::Key_Delete:
        return "Delete";
    case Qt::Key_Escape:
        return "Escape";
    case Qt::Key_Insert:
        return "Insert";
    case Qt::Key_Home:
        return "Home";
    case Qt::Key_End:
        return "End";
    case Qt::Key_PageUp:
        return "PageUp";
    case Qt::Key_PageDown:
        return "PageDown";
    case Qt::Key_Left:
        return "ArrowLeft";
    case Qt::Key_Right:
        return "ArrowRight";
    case Qt::Key_Up:
        return "ArrowUp";
    case Qt::Key_Down:
        return "ArrowDown";
    case Qt::Key_Space:
        return "Space";
    default:
        break;
    }

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        static const char *const letters[] = {
            "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH",
            "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP",
            "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX",
            "KeyY", "KeyZ",
        };
        return letters[key - Qt::Key_A];
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        static const char *const digits[] = {
            "Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
            "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
        };
        return digits[key - Qt::Key_0];
    }

    switch (key) {
    case Qt::Key_Exclam:
        return "Digit1";
    case Qt::Key_At:
        return "Digit2";
    case Qt::Key_NumberSign:
        return "Digit3";
    case Qt::Key_Dollar:
        return "Digit4";
    case Qt::Key_Percent:
        return "Digit5";
    case Qt::Key_AsciiCircum:
        return "Digit6";
    case Qt::Key_Ampersand:
        return "Digit7";
    case Qt::Key_Asterisk:
        return "Digit8";
    case Qt::Key_ParenLeft:
        return "Digit9";
    case Qt::Key_ParenRight:
        return "Digit0";
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        return "Minus";
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        return "Equal";
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft:
        return "BracketLeft";
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        return "BracketRight";
    case Qt::Key_Backslash:
    case Qt::Key_Bar:
        return "Backslash";
    case Qt::Key_Semicolon:
    case Qt::Key_Colon:
        return "Semicolon";
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl:
        return "Quote";
    case Qt::Key_Comma:
    case Qt::Key_Less:
        return "Comma";
    case Qt::Key_Period:
    case Qt::Key_Greater:
        return "Period";
    case Qt::Key_Slash:
    case Qt::Key_Question:
        return "Slash";
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:
        return "Backquote";
    default:
        return nullptr;
    }
}

QString keyTextFor(const QKeyEvent *event, const char *named)
{
    if (named != nullptr)
        return QString::fromLatin1(named);

    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isPrint())
        return text;

    const int key = event->key();
    if (key == Qt::Key_Space)
        return QStringLiteral(" ");

    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        const QChar letter(ushort('a' + (key - Qt::Key_A)));
        return shift ? QString(letter.toUpper()) : QString(letter);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        static const char shiftedDigits[] = ")!@#$%^&*(";
        if (shift)
            return QString(QChar(shiftedDigits[key - Qt::Key_0]));
        return QString(QChar(ushort('0' + (key - Qt::Key_0))));
    }
    if (key > 0x20 && key < 0x7f)
        return QString(QChar(ushort(key)));
    return QString();
}

uint32_t modifiersFor(Qt::KeyboardModifiers modifiers)
{
    uint32_t bits = 0;
    if (modifiers.testFlag(Qt::ControlModifier))
        bits |= 0x1;
    if (modifiers.testFlag(Qt::ShiftModifier))
        bits |= 0x2;
    if (modifiers.testFlag(Qt::AltModifier))
        bits |= 0x4;
    if (modifiers.testFlag(Qt::MetaModifier))
        bits |= 0x8;
    return bits;
}

}

ServoWidget::ServoWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("servoWidget"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    setAcceptDrops(false);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_timer.setInterval(33);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ServoWidget::tick);
}

ServoWidget::~ServoWidget()
{
    m_timer.stop();
    shutdown();
}

QUrl ServoWidget::url() const
{
    return m_url;
}

void ServoWidget::setUrl(const QUrl &url)
{
    if (m_url == url)
        return;
    m_url = url;
    if (m_view == nullptr)
        return;

    if (m_context == nullptr || m_surface == nullptr)
        return;
    if (!m_context->makeCurrent(m_surface)) {
        fail(QStringLiteral("could not make the web view's offscreen surface current to load the page"));
        return;
    }
    const QByteArray spec = m_url.toEncoded();
    servo_webview_load(m_view, spec.constData());
    m_context->doneCurrent();
    report(QStringLiteral("loading %1").arg(m_url.toString()));
    update();
}

void ServoWidget::setActive(bool active)
{
    if (active == m_active.load())
        return;
    m_active.store(active);
    if (active) {
        update();
        setFocus(Qt::PopupFocusReason);
        traceShim("focus(true) from setActive");
        if (m_view != nullptr)
            servo_webview_focus(m_view, true);
        if (m_started)
            m_timer.start();
    } else {
        m_timer.stop();
        releaseForwardedButtons();
        traceShim("focus(false) from setActive(false)");
        if (m_view != nullptr)
            servo_webview_focus(m_view, false);
    }
}

void ServoWidget::unload()
{
    m_timer.stop();
    m_active.store(false);
    shutdown();

    {
        QMutexLocker locker(&m_imageMutex);
        m_image = QImage();
    }

    update();
    qInfo() << "web trapdoor: unloaded; the Servo view, its FBO and its GL context are freed";
}

void ServoWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Theme::instance()->bg());

    if (m_active && !m_started && width() > 0 && height() > 0)
        scheduleStart();

    QImage frame;
    {
        QMutexLocker locker(&m_imageMutex);
        frame = m_image;
    }
    if (frame.isNull())
        return;

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), frame);
}

void ServoWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    if (m_started)
        applyGeometry();
    else if (m_active && width() > 0 && height() > 0)
        update();
}

void ServoWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    setActive(true);
}

void ServoWidget::hideEvent(QHideEvent *event)
{
    setActive(false);
    QWidget::hideEvent(event);
}

bool ServoWidget::forwardsInput() const
{
    return m_active.load() && m_started.load() && m_view != nullptr && isVisible();
}

QPointF ServoWidget::toCss(const QPointF &point) const
{
    const qreal scale = m_scale > 0.0f ? qreal(m_scale) : 1.0;
    return QPointF(point.x() / scale, point.y() / scale);
}

void ServoWidget::traceInput(const char *what, const QPointF &point,
                             const QKeyEvent *key) const
{
    if (!inputTraceEnabled())
        return;

    QString where = QString::fromUtf8(what);
    if (key == nullptr) {
        where += QStringLiteral(" at %1,%2").arg(point.x()).arg(point.y());
    } else {
        where += QStringLiteral(" key=%1 text=\"%2\" repeat=%3")
                     .arg(key->key())
                     .arg(key->text())
                     .arg(key->isAutoRepeat() ? QStringLiteral("yes")
                                              : QStringLiteral("no"));
    }

    const QWindow *handle = window() != nullptr ? window()->windowHandle() : nullptr;
    const QWindow *keyWindow = QGuiApplication::focusWindow();

    qInfo().nospace()
        << "[trapdoor] " << where
        << " forwards=" << forwardsInput()
        << " active=" << m_active.load()
        << " started=" << m_started.load()
        << " handle=" << (m_view != nullptr)
        << " visible=" << isVisible()
        << " enabled=" << isEnabled()
        << " hasFocus=" << hasFocus()
        << " windowActive=" << (handle != nullptr && handle->isActive())
        << " keyWindow=" << (keyWindow == handle ? QStringLiteral("this")
                                                 : QStringLiteral("other"));
}

void ServoWidget::sendMouseMove(const QPointF &point)
{
    if (m_view == nullptr)
        return;
    servo_webview_mouse_move(m_view, float(point.x()), float(point.y()));
}

void ServoWidget::sendMouseButton(Qt::MouseButton button, bool down, const QPointF &point)
{
    int index = -1;
    if (button == Qt::LeftButton)
        index = 0;
    else if (button == Qt::RightButton)
        index = 1;
    if (index < 0 || m_view == nullptr)
        return;

    servo_webview_mouse_button(m_view, down ? uint8_t(0) : uint8_t(1),
                               uint8_t(index), float(point.x()), float(point.y()));
    m_forwardedButtons.setFlag(button, down);
}

void ServoWidget::releaseForwardedButtons()
{
    if (m_forwardedButtons == Qt::NoButton)
        return;
    if (m_view != nullptr) {
        if (m_forwardedButtons.testFlag(Qt::LeftButton)) {
            servo_webview_mouse_button(m_view, 1, 0, float(m_lastCss.x()),
                                       float(m_lastCss.y()));
        }
        if (m_forwardedButtons.testFlag(Qt::RightButton)) {
            servo_webview_mouse_button(m_view, 1, 1, float(m_lastCss.x()),
                                       float(m_lastCss.y()));
        }
    }
    m_forwardedButtons = Qt::NoButton;
}

void ServoWidget::sendKey(bool down, const QKeyEvent *event)
{
    if (m_view == nullptr)
        return;

    const char *named = namedKeyFor(event->key());
    const QString key = keyTextFor(event, named);
    if (key.isEmpty())
        return;

    const char *code = codeFor(event->key());
    const QByteArray keyBytes = key.toUtf8();
    const QByteArray codeBytes = code != nullptr ? QByteArray(code) : QByteArray();
    traceShim(qPrintable(QStringLiteral("key %1 %2 mods=%3 repeat=%4")
        .arg(down ? "down" : "up", QString::fromUtf8(keyBytes))
        .arg(modifiersFor(event->modifiers()))
        .arg(event->isAutoRepeat() ? 1 : 0)));
    servo_webview_key(m_view, down, keyBytes.constData(),
                      code != nullptr ? codeBytes.constData() : nullptr,
                      modifiersFor(event->modifiers()), event->isAutoRepeat());
}

void ServoWidget::mousePressEvent(QMouseEvent *event)
{
    const QPointF point = toCss(event->position());
    traceInput("press", point);

    if (!forwardsInput()) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_lastCss = point;
    setFocus(Qt::MouseFocusReason);
    sendMouseMove(point);
    servo_webview_focus(m_view, true);
    sendMouseButton(event->button(), true, point);
    event->accept();
}

void ServoWidget::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF point = toCss(event->position());
    traceInput("release", point);

    if (!forwardsInput()) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_lastCss = point;
    sendMouseButton(event->button(), false, point);
    event->accept();
}

void ServoWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toCss(event->position());
    traceInput("move", point);

    if (!forwardsInput()) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    m_lastCss = point;
    sendMouseMove(point);
    event->accept();
}

void ServoWidget::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    if (!forwardsInput())
        return;

    const QPointF point = toCss(event->position());
    m_lastCss = point;
    traceInput("enter", point);
    sendMouseMove(point);
}

void ServoWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (!forwardsInput())
        return;

    traceInput("leave", toCss(QPointF()));
    if (m_view != nullptr)
        servo_webview_mouse_move(m_view, -1.0f, -1.0f);
}

void ServoWidget::wheelEvent(QWheelEvent *event)
{
    if (!forwardsInput()) {
        QWidget::wheelEvent(event);
        return;
    }

    QPointF delta;
    const QPointF pixelDelta = event->pixelDelta();
    if (!pixelDelta.isNull()) {
        delta = pixelDelta;
    } else {
        const qreal step = QGuiApplication::styleHints()->wheelScrollLines() * 24.0;
        delta = QPointF(event->angleDelta()) / 120.0 * step;
    }

    const QPointF point = toCss(event->position());
    m_lastCss = point;
    servo_webview_wheel(m_view, float(delta.x()), float(delta.y()),
                        float(point.x()), float(point.y()));
    event->accept();
}

void ServoWidget::keyPressEvent(QKeyEvent *event)
{
    traceInput("key-down", QPointF(), event);

    if (!forwardsInput()) {
        QWidget::keyPressEvent(event);
        return;
    }

    sendKey(true, event);

    if (event->key() == Qt::Key_Escape)
        event->ignore();
    else
        event->accept();
}

void ServoWidget::keyReleaseEvent(QKeyEvent *event)
{
    traceInput("key-up", QPointF(), event);

    if (!forwardsInput()) {
        QWidget::keyReleaseEvent(event);
        return;
    }

    sendKey(false, event);
    event->accept();
}

void ServoWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    traceShim("focus(true) from focusInEvent");
    if (m_view != nullptr)
        servo_webview_focus(m_view, m_active.load());
}

void ServoWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    traceShim("focus(false) from focusOutEvent");
    if (m_view != nullptr)
        servo_webview_focus(m_view, false);
}

bool ServoWidget::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::UngrabMouse:
        releaseForwardedButtons();
        break;
    case QEvent::ShortcutOverride: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape)
            key->accept();
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            if (event->type() == QEvent::KeyPress)
                keyPressEvent(key);
            else
                keyReleaseEvent(key);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::event(event);
}

void ServoWidget::ensureStarted()
{
    if (m_started)
        return;
    if (!isVisible() || width() <= 0 || height() <= 0)
        return;

    m_scale = float(devicePixelRatioF());
    if (!(m_scale >= 0.5f && m_scale <= 4.0f))
        m_scale = 1.0f;

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);

    m_context = new QOpenGLContext;
    m_context->setFormat(format);
    if (!m_context->create()) {
        fail(QStringLiteral("could not create an OpenGL context for the web view"));
        shutdown();
        return;
    }

    m_surface = new QOffscreenSurface;
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!m_surface->isValid() || !m_context->makeCurrent(m_surface)) {
        fail(QStringLiteral("could not make the web view's offscreen surface current"));
        shutdown();
        return;
    }

    resizeSurface(qMax(1, qRound(width() * m_scale)),
                  qMax(1, qRound(height() * m_scale)));
    m_fbo->bind();

    servo_webview_set_clipboard(&clipboardGetText, &clipboardSetText, 0);

    m_view = servo_webview_new(&servoGetProc, &servoWake,
                               unsigned(m_fboWidth), unsigned(m_fboHeight), m_scale);
    if (m_view == nullptr) {
        fail(QStringLiteral("servo_webview_new failed"));
        m_context->doneCurrent();
        shutdown();
        return;
    }

    const QByteArray spec = m_url.toEncoded();
    servo_webview_load(m_view, spec.constData());

    traceShim(hasFocus() && m_active ? "focus(true) from ensureStarted"
                                     : "focus(false) from ensureStarted");
    servo_webview_focus(m_view, hasFocus() && m_active.load());
    m_context->doneCurrent();

    m_started = true;
    if (m_active)
        m_timer.start();
    report(QStringLiteral("loading %1").arg(m_url.toString()));
    qInfo() << "web trapdoor: Servo view created" << m_fboWidth << "x" << m_fboHeight
            << "scale" << m_scale << "url" << m_url.toString();
    update();
}

void ServoWidget::shutdown()
{
    if (m_context != nullptr && m_surface != nullptr) {
        if (m_context->makeCurrent(m_surface)) {
            if (m_view != nullptr) {
                servo_webview_destroy(m_view);
                m_view = nullptr;
            }
            delete m_fbo;
            m_fbo = nullptr;
            m_context->doneCurrent();
        } else {
            m_fbo = nullptr;
            m_view = nullptr;
        }
    }
    delete m_surface;
    m_surface = nullptr;
    delete m_context;
    m_context = nullptr;
    m_started = false;
    m_fboWidth = 0;
    m_fboHeight = 0;
    m_forwardedButtons = Qt::NoButton;
    m_lastCss = QPointF();
}

void ServoWidget::resizeSurface(int width, int height)
{
    delete m_fbo;
    m_fbo = new QOpenGLFramebufferObject(width, height,
                                         QOpenGLFramebufferObject::CombinedDepthStencil);
    m_fboWidth = width;
    m_fboHeight = height;
    if (m_view != nullptr)
        servo_webview_resize(m_view, unsigned(width), unsigned(height));
}

void ServoWidget::applyGeometry()
{
    if (!m_started || m_context == nullptr || m_surface == nullptr)
        return;
    const int width = qMax(1, qRound(this->width() * m_scale));
    const int height = qMax(1, qRound(this->height() * m_scale));
    if (m_fbo != nullptr && width == m_fboWidth && height == m_fboHeight)
        return;

    const bool current = QOpenGLContext::currentContext() == m_context;
    if (!current && !m_context->makeCurrent(m_surface)) {
        fail(QStringLiteral("could not make the web view's offscreen surface current to resize it"));
        return;
    }
    resizeSurface(width, height);
    if (!current)
        m_context->doneCurrent();
    update();
}

void ServoWidget::tick()
{
    if (!m_started || m_view == nullptr || m_context == nullptr || m_surface == nullptr)
        return;
    if (!m_context->makeCurrent(m_surface)) {
        fail(QStringLiteral("the web view's OpenGL context was lost"));
        m_timer.stop();
        return;
    }

    applyGeometry();
    if (m_fbo == nullptr) {
        m_context->doneCurrent();
        return;
    }

    g_wake.exchange(false);

    static const bool timing = qEnvironmentVariableIsSet("DSH_SERVO_TIMING");
    QElapsedTimer clock;
    qint64 spinMs = 0, paintMs = 0, readMs = 0;
    if (timing)
        clock.start();

    servo_webview_spin(m_view);
    if (timing)
        spinMs = clock.restart();

    m_fbo->bind();
    servo_webview_paint(m_view);
    if (timing)
        paintMs = clock.restart();

    const QImage frame = m_fbo->toImage();
    if (timing) {
        readMs = clock.restart();
        static qint64 ticks = 0, totalSpin = 0, totalPaint = 0, totalRead = 0, worst = 0;
        ++ticks;
        totalSpin += spinMs;
        totalPaint += paintMs;
        totalRead += readMs;
        worst = qMax(worst, spinMs + paintMs + readMs);
        if (spinMs + paintMs + readMs >= 100)
            qInfo().nospace() << "[servo-timing] hitch: spin " << spinMs << " ms, paint " << paintMs
                              << " ms, read-back " << readMs << " ms";
        if (ticks % 60 == 0) {
            qInfo().nospace() << "[servo-timing] " << ticks << " frames: mean spin "
                              << (totalSpin / 60) << " ms, paint " << (totalPaint / 60)
                              << " ms, read-back " << (totalRead / 60) << " ms, worst " << worst
                              << " ms  (" << (1000.0 * 60.0 / qMax<qint64>(1, totalSpin + totalPaint + totalRead))
                              << " fps ceiling)";
            totalSpin = totalPaint = totalRead = 0;
            worst = 0;
        }
    }
    if (!frame.isNull()) {
        {
            QMutexLocker locker(&m_imageMutex);
            m_image = frame;
        }
        update();
    }
    m_context->doneCurrent();
}

void ServoWidget::scheduleStart()
{
    if (m_startScheduled.exchange(true))
        return;
    QMetaObject::invokeMethod(this, [this]() {
        m_startScheduled.store(false);
        if (m_active)
            ensureStarted();
    }, Qt::QueuedConnection);
}

void ServoWidget::report(const QString &status)
{
    emit statusChanged(status);
}

void ServoWidget::fail(const QString &message)
{
    report(message);
    qWarning() << "web trapdoor:" << message;
    emit failed(message);
}
