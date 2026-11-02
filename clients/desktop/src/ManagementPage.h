#pragma once

#include <QString>
#include <QWidget>

class ClientFacade;
class QStackedWidget;

class ManagementPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManagementPage(ClientFacade *facade, QWidget *parent = nullptr);

    QString tab() const { return m_tab; }
    void setTab(const QString &tab);

signals:
    void closeRequested();

private:
    class Bar;
    class AutomationPage;
    class PluginsPage;

    void applyTab();
    void refreshCurrent();

    ClientFacade *m_facade;
    QString m_tab;
    Bar *m_bar;
    QStackedWidget *m_stack;
    AutomationPage *m_automation;
    PluginsPage *m_plugins;
};
