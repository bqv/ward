#include "Lightbox.h"

#include "Theme.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QKeySequence>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QRunnable>
#include <QShortcut>
#include <QThreadPool>
#include <QUrl>
#include <QWheelEvent>

#include <functional>
#include <utility>

namespace {

QImage decodeImageSource(const QVariant &source)
{
    const QString text = source.toString();
    if (text.isEmpty())
        return QImage();
    if (text.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
        const int comma = text.indexOf(QLatin1Char(','));
        if (comma < 0)
            return QImage();
        const QString header = text.mid(5, comma - 5);
        QByteArray payload = text.mid(comma + 1).toLatin1();
        if (header.contains(QStringLiteral(";base64"), Qt::CaseInsensitive))
            payload = QByteArray::fromBase64(payload);
        else
            payload = QByteArray::fromPercentEncoding(payload);
        return QImage::fromData(payload);
    }
    const QUrl url(text);
    if (url.isLocalFile())
        return QImage(url.toLocalFile());
    if (url.scheme() == QLatin1String("qrc"))
        return QImage(QStringLiteral(":") + url.path());
    return QImage(text);
}

class ImageDecodeTask : public QRunnable
{
public:
    ImageDecodeTask(QObject *context, std::function<QImage()> decode,
                    std::function<void(const QImage &)> deliver)
        : m_context(context)
        , m_decode(std::move(decode))
        , m_deliver(std::move(deliver))
    {
    }

    void run() override
    {
        const QImage image = m_decode();
        QMetaObject::invokeMethod(
            m_context,
            [deliver = std::move(m_deliver), image]() { deliver(image); },
            Qt::QueuedConnection);
    }

private:
    QObject *m_context;
    std::function<QImage()> m_decode;
    std::function<void(const QImage &)> m_deliver;
};

}

Lightbox::Lightbox(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("imageLightbox"));
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setCursor(Qt::PointingHandCursor);

    m_escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_escape->setContext(Qt::ApplicationShortcut);
    m_escape->setEnabled(false);
    QObject::connect(m_escape, &QShortcut::activated, this, &Lightbox::closeLightbox);

    hide();
}

bool Lightbox::showImage(const QVariant &source, const QVariant &alt,
                         qreal fallbackWidth, qreal fallbackHeight)
{
    if (source.toString().isEmpty())
        return false;

    ++m_request;
    m_alt = alt.toString();
    m_fallbackSize = QSizeF(qMax(0.0, fallbackWidth), qMax(0.0, fallbackHeight));
    m_naturalSize = QSizeF();
    m_image = QImage();

    setAccessibleName(m_alt);
    if (QWidget *owner = parentWidget())
        setGeometry(owner->rect());
    show();
    raise();
    m_escape->setEnabled(true);
    update();

    const quint64 request = m_request;
    auto decode = [source]() { return decodeImageSource(source); };
    const QPointer<Lightbox> guard(this);
    auto deliver = [guard, request](const QImage &image) {
        if (guard.isNull() || request != guard->m_request)
            return;
        guard->m_image = image;
        guard->m_naturalSize = image.isNull()
            ? QSizeF()
            : QSizeF(image.width(), image.height());
        guard->update();
    };
    if (QCoreApplication *application = QCoreApplication::instance()) {
        QThreadPool::globalInstance()->start(
            new ImageDecodeTask(application, std::move(decode), std::move(deliver)));
    }

    return true;
}

void Lightbox::closeLightbox()
{
    if (!isVisible())
        return;
    ++m_request;
    m_image = QImage();
    m_naturalSize = QSizeF();
    m_fallbackSize = QSizeF();
    m_alt.clear();
    m_escape->setEnabled(false);
    setAccessibleName(QString());
    hide();
}

QRectF Lightbox::frameRect() const
{
    Theme *theme = Theme::instance();
    const qreal maxWidth = qMax(0.0, qreal(width()) - 4.0 * theme->space6());
    const qreal maxHeight = qMax(0.0, qreal(height()) - 4.0 * theme->space6());
    if (maxWidth <= 0.0 || maxHeight <= 0.0)
        return QRectF();

    qreal naturalWidth = m_naturalSize.width() > 0.0 ? m_naturalSize.width()
                                                     : m_fallbackSize.width();
    qreal naturalHeight = m_naturalSize.height() > 0.0 ? m_naturalSize.height()
                                                       : m_fallbackSize.height();

    if (m_image.isNull() && !m_alt.isEmpty()
        && (naturalWidth <= 0.0 || naturalHeight <= 0.0)) {
        const QFontMetrics metrics(font());
        const qreal textWidth = qMax(1.0, maxWidth - 2.0 * theme->space4());
        const QRect bounds =
            metrics.boundingRect(QRect(0, 0, int(textWidth), 100000),
                                 Qt::AlignLeft | Qt::TextWordWrap, m_alt);
        naturalWidth = qMin(maxWidth, qMax(120.0, bounds.width() + 2.0 * theme->space4()));
        naturalHeight = qMin(maxHeight, qMax(48.0, bounds.height() + 2.0 * theme->space4()));
    }

    if (naturalWidth <= 0.0 || naturalHeight <= 0.0)
        return QRectF();

    const qreal fit =
        qMin(1.0, qMin(maxWidth / naturalWidth, maxHeight / naturalHeight));
    const QSizeF size(naturalWidth * fit, naturalHeight * fit);
    return QRectF(QPointF((width() - size.width()) / 2.0,
                          (height() - size.height()) / 2.0),
                  size);
}

void Lightbox::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), theme->scrim());

    const QRectF frame = frameRect();
    if (frame.width() < 1.0 || frame.height() < 1.0)
        return;

    const qreal radius = theme->radius();
    const qreal hairline = theme->hairline();
    QPainterPath path;
    path.addRoundedRect(frame, radius, radius);
    painter.fillPath(path, theme->raised());

    if (hairline > 0) {
        QPen pen(theme->border());
        pen.setWidthF(hairline);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame.adjusted(hairline / 2.0, hairline / 2.0,
                                               -hairline / 2.0, -hairline / 2.0),
                                radius, radius);
    }

    const QRectF inner =
        frame.adjusted(hairline, hairline, -hairline, -hairline);
    painter.save();
    painter.setClipPath(path);
    if (!m_image.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(inner, m_image);
    } else if (!m_alt.isEmpty()) {
        painter.setPen(theme->fg());
        painter.drawText(inner.adjusted(theme->space4(), theme->space4(),
                                        -theme->space4(), -theme->space4()),
                         Qt::AlignCenter | Qt::TextWordWrap, m_alt);
    }
    painter.restore();
}

void Lightbox::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void Lightbox::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
    closeLightbox();
}

void Lightbox::wheelEvent(QWheelEvent *event)
{
    event->accept();
}
