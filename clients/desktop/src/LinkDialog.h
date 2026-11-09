#pragma once

#include <QDialog>
#include <QString>
#include <QUrl>

class QLabel;
class QPushButton;

class LinkDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LinkDialog(QWidget *parent = nullptr);

    void showLink(const QString &href);

signals:
    void browserRequested(const QUrl &url);
    void inWindowRequested(const QUrl &url);

private:
    void copy();

    QLabel *m_url = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_browser = nullptr;
    QPushButton *m_inWindow = nullptr;
    QPushButton *m_copy = nullptr;
    QString m_href;
};
