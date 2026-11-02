#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

class AcpTranscript;
class HeadlessClient;
class QLabel;
class QScrollArea;

class HeadlessPage : public QWidget
{
    Q_OBJECT

public:
    HeadlessPage(HeadlessClient *client, const QString &iconDir, QWidget *parent = nullptr);

    void focusComposer();
    HeadlessClient *client() const { return m_client; }
    QString iconDir() const { return m_iconDir; }

signals:
    void dismissRequested();
    void shutRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    class Band;
    class Button;
    class Column;
    class Entry;
    class Field;
    class Row;
    class TextLine;

    void submit();
    void triggerButton();
    void refreshBar();
    void refreshComposer();
    void rebuildRows();
    void relayoutRows();
    void toggleOpen(const QString &rowId);
    bool isOpen(const QString &rowId) const;
    AcpTranscript *transcript() const;

    HeadlessClient *m_client = nullptr;
    QString m_iconDir;

    Band *m_bar = nullptr;
    TextLine *m_title = nullptr;
    TextLine *m_session = nullptr;
    TextLine *m_status = nullptr;
    Button *m_close = nullptr;

    QScrollArea *m_scroll = nullptr;
    Column *m_column = nullptr;
    QLabel *m_empty = nullptr;
    QList<Row *> m_rows;

    Band *m_composerBand = nullptr;
    Field *m_field = nullptr;
    Entry *m_entry = nullptr;
    TextLine *m_hint = nullptr;
    Button *m_send = nullptr;

    QSet<QString> m_openRows;
    bool m_follow = true;
    bool m_scrollSettling = false;
    bool m_relayingOut = false;
};
