#pragma once

#include <QList>
#include <QMetaObject>
#include <QString>
#include <QWidget>

class AcpClient;
class AcpTranscript;
class QLabel;
class QScrollArea;

class AcpPage : public QWidget
{
    Q_OBJECT

public:
    AcpPage(AcpClient *client, const QString &iconDir, QWidget *parent = nullptr);

    void focusPage();
    void pinToTail();
    void shut();

    AcpClient *client() const { return m_client; }
    QString iconDir() const { return m_iconDir; }

signals:
    void closeRequested();
    void acpSessionSelected(const QString &sessionId);
    void acpSessionCloseRequested(const QString &sessionId);
    void acpNewSessionRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    class Band;
    class Button;
    class Chip;
    class Column;
    class Entry;
    class Field;
    class PermissionBand;
    class Row;
    class SessionStrip;
    class TextLine;

    void sendDraft();
    void stopOrSend();
    void refreshBar();
    void refreshComposer();
    void refreshSessions();
    void refreshEmpty();
    void refreshPermission();
    void showPermission(const QString &requestId, const QString &toolName,
                        const QString &detail);
    void clearPermission();
    void withdrawPermission(const QString &requestId);
    void answerPermission(bool allow);
    void bindTranscript();
    void rebuildRows();
    void relayoutRows();
    void followTail();
    AcpTranscript *transcript() const;
    int clientState() const;

    AcpClient *m_client = nullptr;
    QString m_iconDir;

    Band *m_bar = nullptr;
    TextLine *m_title = nullptr;
    TextLine *m_status = nullptr;
    Button *m_close = nullptr;
    SessionStrip *m_strip = nullptr;

    QScrollArea *m_scroll = nullptr;
    Column *m_column = nullptr;
    QLabel *m_empty = nullptr;
    QList<Row *> m_rows;

    PermissionBand *m_permission = nullptr;
    QString m_permissionRequestId;

    Band *m_composer = nullptr;
    Field *m_field = nullptr;
    Entry *m_entry = nullptr;
    Button *m_send = nullptr;

    QList<QMetaObject::Connection> m_modelConnections;
    bool m_follow = true;
    bool m_scrollSettling = false;
    bool m_relayingOut = false;
};
