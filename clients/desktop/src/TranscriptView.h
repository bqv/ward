#pragma once

#include <QList>
#include <QSet>
#include <QVariant>
#include <QWidget>

class AttachmentStore;
class ClientFacade;
class QAbstractItemModel;
class QLabel;
class QModelIndex;
class QPropertyAnimation;
class QScrollArea;
class QVBoxLayout;
class QWheelEvent;
class TranscriptPagingHeader;
class TranscriptRow;

class TranscriptView : public QWidget
{
    Q_OBJECT

public:
    explicit TranscriptView(ClientFacade *facade, QWidget *parent = nullptr);
    ~TranscriptView() override;

    int rowCount() const;
    bool followTail() const { return m_followTail; }
    QWidget *rowAt(int index) const;

    int contentY() const;
    int contentHeight() const;
    int viewportHeight() const;

    void scrollToEnd();

signals:
    void openImage(const QVariant &source, const QVariant &alt,
                   double fallbackWidth, double fallbackHeight);
    void openFile(const QString &path);
    void pageEarlier();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onAttachmentsChanged();

private:
    void setModel(QAbstractItemModel *model);
    void bindRow(TranscriptRow *row);
    void rebuild();
    void insertRows(const QModelIndex &parent, int first, int last);
    void removeRows(const QModelIndex &parent, int first, int last);
    void updateRows(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                    const QList<int> &roles);
    void rebuildRows();
    void buildSlice();
    void scheduleBuild();
    bool buildPending() const;
    void beginBuildWave(bool rebuild);
    void logBuildComplete();
    void settleBuildSlice(int beforeValue, int grownAbove);
    int layoutIndexFor(int index) const;
    void buildRowsNow(int first, int last);
    void refreshRow(int index);
    void toggleExpanded(TranscriptRow *row);
    int columnWidth() const;
    int chromeHeight() const;
    void syncColumnHeight();
    void scheduleColumnHeight();
    void refreshPagingHeader();
    void refreshEmptyState();
    void layoutEmptyState();
    void applyFollow();
    void beginEarlierPage();
    void holdEarlierPlace();
    void maybeLoadEarlier();
    void onScrollValueChanged(int value);
    void onScrollRangeChanged(int minimum, int maximum);
    bool handleWheel(QWheelEvent *wheel);
    bool atContentEnd() const;
    bool atContentStart() const;

    ClientFacade *m_facade = nullptr;
    QAbstractItemModel *m_model = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    AttachmentStore *m_attachments = nullptr;
    QPropertyAnimation *m_glide = nullptr;
    double m_glideTarget = 0.0;
    QVBoxLayout *m_column = nullptr;
    TranscriptPagingHeader *m_paging = nullptr;
    QLabel *m_empty = nullptr;
    QList<TranscriptRow *> m_rows;
    QList<int> m_rowHeights;
    int m_measuredWidth = -1;
    bool m_heightQueued = false;
    QSet<QString> m_expandedRows;
    bool m_followTail = true;
    bool m_pinning = false;
    int m_lastValue = 0;
    bool m_earlierArmed = true;
    bool m_earlierBrace = false;
    bool m_earlierPageArrived = false;
    int m_braceValue = 0;
    int m_braceHeight = 0;
    bool m_pagedEarlier = false;
    bool m_historyStart = false;
    int m_builtStart = 0;
    int m_tailStart = 0;
    int m_tailNext = -1;
    int m_buildGeneration = 0;
    bool m_buildQueued = false;
    bool m_waveActive = false;
    bool m_waveRebuild = false;
    int m_waveBuilt = 0;
    qint64 m_buildStartedMs = 0;
};
