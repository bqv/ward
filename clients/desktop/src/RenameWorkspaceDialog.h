#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;

class RenameWorkspaceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RenameWorkspaceDialog(QWidget *parent = nullptr);

    void showDialog(const QString &title);

signals:
    void saveRequested(const QString &title);

private:
    void save();

    QLineEdit *m_title = nullptr;
    QPushButton *m_save = nullptr;
};
