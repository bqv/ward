#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class ClientFacade;
class QLabel;

class TrajectoryPage : public QWidget
{
    Q_OBJECT

public:
    explicit TrajectoryPage(ClientFacade *facade, QWidget *parent = nullptr);

    bool supported() const;
    bool shown() const { return m_active; }
    void setShown(bool shown);
    void setSession(const QString &sessionId);
    QString sessionId() const { return m_sessionId; }
    int rowCount() const;
    int expandedCount() const { return int(m_expandedCells.size()); }

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Row;
    class Rows;
    class Delegate;
    class List;
    class LaneStrip;
    class ChipButton;
    class FilterField;

    void onFacadeChanged();
    void refresh();
    void rebuildRows();
    void updateFoldButton();
    void positionEmptyState();
    void onScrolled();
    void activateRow(int index, int offset);
    bool rowBodyContains(int index, int offset) const;
    void toggleTurn(int turn);
    void toggleCell(const QString &id);
    void toggleAllFolds();
    void setLaneFilter(int lane);
    void setNeedle(const QString &needle);
    void openLaneMenu();
    void jumpToCell(const QString &id);
    bool rowActivatable(int index) const;

    bool isTurnCollapsed(int turn) const;
    bool isCellExpanded(const QString &id) const;
    bool cellExpandable(const QVariantMap &cell) const;
    QVariantList filteredCells() const;
    QVector<int> laneCounts() const;
    QString summaryText(const QVariantMap &cell) const;
    QString resultText(const QVariantMap &cell) const;
    QColor laneColour(int lane) const;

    int lanePitch() const;
    int captionWidth() const;
    int kindSlot() const;
    int seqColumn() const;
    int contentLeft() const;
    int stripHeight() const;
    int contentRight(const QVariantMap &cell, int width) const;
    int bodyHeight(const QVariantMap &cell, int width) const;
    int detailHeight(const QVariantMap &cell, int width) const;
    int rowHeight(int index, int width) const;
    int wrappedHeight(const QString &text, const QFont &font, int width, int maxLines) const;
    int maxTextLines(bool expanded) const;

    ClientFacade *m_facade = nullptr;
    QString m_sessionId;
    bool m_active = false;
    bool m_relayouting = false;
    bool m_follow = true;
    int m_laneFilter = -1;
    QString m_needle;
    QVariantList m_cells;
    QHash<int, bool> m_collapsedTurns;
    QSet<QString> m_expandedCells;
    int m_hoveredRow = -1;

    QWidget *m_header = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_count = nullptr;
    ChipButton *m_lane = nullptr;
    FilterField *m_filter = nullptr;
    ChipButton *m_fold = nullptr;
    LaneStrip *m_strip = nullptr;
    List *m_list = nullptr;
    Rows *m_rows = nullptr;
    Delegate *m_delegate = nullptr;
    QLabel *m_empty = nullptr;
};
