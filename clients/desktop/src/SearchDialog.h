/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QDialog>

class ClientFacade;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class SearchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SearchDialog(ClientFacade *facade, QWidget *parent = nullptr);

private slots:
    void search();
    void more();
    void openSelected();
    void refresh();

private:
    ClientFacade *m_facade = nullptr;
    QLineEdit *m_query = nullptr;
    QListWidget *m_results = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_more = nullptr;
};
