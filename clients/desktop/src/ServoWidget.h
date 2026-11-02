#pragma once

#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <atomic>

class QEnterEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
struct ServoWebView;

class ServoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ServoWidget(QWidget *parent = nullptr);
    ~ServoWidget() override;

    QUrl url() const;
    void setUrl(const QUrl &url);

    void setActive(bool active);

public slots:
    void unload();

signals:
    void failed(const QString &message);
    void statusChanged(const QString &status);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;

private:
    void ensureStarted();
    void scheduleStart();
    void shutdown();
    void tick();
    void applyGeometry();
    void resizeSurface(int width, int height);
    void report(const QString &status);
    void fail(const QString &message);

    bool forwardsInput() const;
    QPointF toCss(const QPointF &point) const;

    void traceInput(const char *what, const QPointF &point = QPointF(),
                    const QKeyEvent *key = nullptr) const;
    void sendMouseMove(const QPointF &point);
    void sendMouseButton(Qt::MouseButton button, bool down, const QPointF &point);
    void sendKey(bool down, const QKeyEvent *event);
    void releaseForwardedButtons();

    QUrl m_url;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_startScheduled{false};
    QTimer m_timer;

    QMutex m_imageMutex;
    QImage m_image;

    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QOpenGLFramebufferObject *m_fbo = nullptr;
    ServoWebView *m_view = nullptr;
    int m_fboWidth = 0;
    int m_fboHeight = 0;
    float m_scale = 1.0f;

    Qt::MouseButtons m_forwardedButtons;
    QPointF m_lastCss;
};
