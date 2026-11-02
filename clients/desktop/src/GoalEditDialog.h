#pragma once

#include <QDialog>

class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class GoalEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GoalEditDialog(QWidget *parent = nullptr);

    void showDialog(const QString &objective, int maxGoalRounds);

signals:
    void saveRequested(const QString &objective, int maxGoalRounds);

private:
    void save();

    QPlainTextEdit *m_objective = nullptr;
    QSpinBox *m_rounds = nullptr;
    QPushButton *m_save = nullptr;
};
