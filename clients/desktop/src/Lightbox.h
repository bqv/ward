#pragma once

#include <QImage>
#include <QRectF>
#include <QSizeF>
#include <QVariant>
#include <QWidget>

class QShortcut;

class Lightbox : public QWidget
{
    Q_OBJECT

public:
    explicit Lightbox(QWidget *parent = nullptr);

    bool showImage(const QVariant &source, const QVariant &alt,
                   qreal fallbackWidth, qreal fallbackHeight);
    bool isOpen() const { return isVisible(); }
    QString altText() const { return m_alt; }

public slots:
    void closeLightbox();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QRectF frameRect() const;

    QString m_alt;
    QSizeF m_fallbackSize;
    QSizeF m_naturalSize;
    QImage m_image;
    quint64 m_request = 0;
    QShortcut *m_escape = nullptr;
};
