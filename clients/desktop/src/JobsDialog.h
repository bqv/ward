/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QDialog>

class ClientFacade;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class JobsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit JobsDialog(ClientFacade *facade, QWidget *parent = nullptr);

private slots:
    void reload();
    void followSelected();
    void killSelected();
    void onJobOutput(const QString &jobId, const QString &text, qlonglong next,
                     bool dropped);

private:
    ClientFacade *m_facade = nullptr;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QPushButton *m_kill = nullptr;
    QLabel *m_note = nullptr;
    QString m_selected;
};
