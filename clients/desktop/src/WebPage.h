#pragma once

#include <QString>
#include <QUrl>
#include <QWidget>

#ifdef DSH_SERVO
class ServoWidget;
#endif

class WebPage : public QWidget
{
    Q_OBJECT

public:
    explicit WebPage(QWidget *parent = nullptr);

    QUrl url() const;
    void setUrl(const QUrl &url);

    QString status() const;
    void setStatus(const QString &status);

    bool available() const;

    void focusPage();

public slots:
    void unload();

signals:
    void closeRequested();
    void minimiseRequested();
    void browserRequested();
    void statusChanged(const QString &status);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    class Bar;
    class Message;
    class StatusLine;

    void applyStatusGeometry();

    Bar *m_bar = nullptr;
    Message *m_message = nullptr;
    StatusLine *m_statusLine = nullptr;
#ifdef DSH_SERVO
    ServoWidget *m_servo = nullptr;
#endif
    QUrl m_url;
    QString m_status;
};
