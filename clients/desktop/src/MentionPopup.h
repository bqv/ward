#pragma once

#include <QRect>
#include <QString>
#include <QVariantList>
#include <QWidget>

class MentionPopup : public QWidget
{
    Q_OBJECT

public:
    explicit MentionPopup(QWidget *parent = nullptr);

    void setCandidates(const QVariantList &candidates);
    QVariantList candidates() const { return m_candidates; }

    bool isOpen() const;
    int count() const { return int(m_candidates.size()); }

    int selectedIndex() const { return m_selected; }
    void setSelectedIndex(int index);
    void moveSelection(int step);
    QString selectedPath() const;

    void showAt(const QRect &caretGlobal, const QRect &fieldGlobal);
    void dismiss();

signals:
    void chosen(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    int visibleRows() const;
    int rowHeight() const;
    QRect rowRect(int row) const;
    int rowAt(const QPoint &position) const;
    void ensureVisible(int index);

    QVariantList m_candidates;
    int m_selected = 0;
    int m_top = 0;
};
