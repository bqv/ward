#pragma once

#include <QRect>
#include <QVariantMap>
#include <QWidget>

class ClientFacade;
class QTimer;

class GoalRow : public QWidget
{
    Q_OBJECT

public:
    explicit GoalRow(ClientFacade *facade, QWidget *parent = nullptr);

    bool shown() const;
    int dockHeight() const;

signals:
    void dockHeightChanged();
    void editRequested(const QString &objective, int maxRounds);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void refresh();

    ClientFacade *m_facade;
    QVariantMap m_goal;
    QRect m_pauseRect;
    QRect m_editRect;
    QRect m_clearRect;
    bool m_hoverPause = false;
    bool m_hoverEdit = false;
    bool m_hoverClear = false;
    bool m_confirmClear = false;
    QTimer *m_clearReset = nullptr;
};
