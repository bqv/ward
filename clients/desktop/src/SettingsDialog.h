#pragma once

#include <QDialog>

class ClientFacade;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(ClientFacade *facade, QWidget *parent = nullptr);

    void showDialog();

private slots:
    void reload();
    void reloadArchivedSessions();
    void unarchiveSelected();
    void pairDevice();

private:
    void apply();

    ClientFacade *m_facade = nullptr;
    QRadioButton *m_queueMode = nullptr;
    QRadioButton *m_steerMode = nullptr;
    QLabel *m_archivedTitle = nullptr;
    QLabel *m_archivedNote = nullptr;
    QLabel *m_archivedEmpty = nullptr;
    QListWidget *m_archivedList = nullptr;
    QPushButton *m_unarchive = nullptr;
    QPushButton *m_pair = nullptr;
    QLabel *m_pairNote = nullptr;
};
