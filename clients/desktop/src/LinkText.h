#pragma once

#include <QString>
#include <QTextEdit>

class QEvent;
class QMouseEvent;

class LinkText : public QTextEdit
{
    Q_OBJECT

public:
    explicit LinkText(QWidget *parent = nullptr);

signals:
    void linkActivated(const QString &href);

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString m_pressedHref;
};
