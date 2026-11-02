#pragma once

#include <QDialog>

class ClientFacade;
class QLabel;
class QListWidget;
class QPushButton;
class QSpacerItem;

class ChangesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChangesDialog(ClientFacade *facade, QWidget *parent = nullptr);

    void showDialog();

signals:
    void fileRequested(const QString &path);

public slots:
    void reject() override;

private slots:
    void reload();

private:
    void setConfirming(bool confirming);
    void confirmClear();
    void openCurrent();

    ClientFacade *m_facade = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_empty = nullptr;
    QListWidget *m_list = nullptr;
    QWidget *m_actions = nullptr;
    QWidget *m_confirm = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_keep = nullptr;
    QSpacerItem *m_bottomSpacer = nullptr;
};
