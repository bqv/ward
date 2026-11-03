/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "SearchDialog.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

SearchDialog::SearchDialog(ClientFacade *facade, QWidget *parent)
    : QDialog(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setWindowTitle(QStringLiteral("Search messages"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(720, 520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(), theme->space6(), theme->space6());
    layout->setSpacing(theme->space4());

    auto *top = new QHBoxLayout();
    m_query = new QLineEdit(this);
    m_query->setObjectName(QStringLiteral("searchQuery"));
    m_query->setPlaceholderText(QStringLiteral("Search user and assistant text"));
    m_query->setClearButtonEnabled(true);
    m_query->setFont(Chrome::uiFont(theme->fontUI()));
    top->addWidget(m_query, 1);
    auto *button = new QPushButton(QStringLiteral("Search"), this);
    top->addWidget(button);
    layout->addLayout(top);

    m_results = new QListWidget(this);
    m_results->setObjectName(QStringLiteral("searchResults"));
    m_results->setFont(Chrome::uiFont(theme->fontUI()));
    layout->addWidget(m_results, 1);

    m_status = new QLabel(QStringLiteral("Two characters start a search."), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *actions = new QHBoxLayout();
    m_more = new QPushButton(QStringLiteral("More"), this);
    auto *close = new QPushButton(QStringLiteral("Close"), this);
    m_more->setEnabled(false);
    actions->addWidget(m_more);
    actions->addStretch(1);
    actions->addWidget(close);
    layout->addLayout(actions);

    connect(button, &QPushButton::clicked, this, &SearchDialog::search);
    connect(m_query, &QLineEdit::returnPressed, this, &SearchDialog::search);
    connect(m_more, &QPushButton::clicked, this, &SearchDialog::more);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_results, &QListWidget::itemDoubleClicked, this, &SearchDialog::openSelected);

    if (m_facade != nullptr && m_facade->transport() != nullptr
            && m_facade->transport()->metaObject()->indexOfSignal("searchChanged()") >= 0) {
        connect(m_facade->transport(), SIGNAL(searchChanged()), this, SLOT(refresh()));
    }
}

void SearchDialog::search()
{
    if (m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    QMetaObject::invokeMethod(m_facade->transport(), "searchSessions",
                              Q_ARG(QString, m_query->text()));
}

void SearchDialog::more()
{
    if (m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    QMetaObject::invokeMethod(m_facade->transport(), "searchMore");
}

void SearchDialog::openSelected()
{
    QListWidgetItem *item = m_results->currentItem();
    if (item == nullptr || m_facade == nullptr)
        return;
    const QString session = item->data(Qt::UserRole).toString();
    if (!session.isEmpty()) {
        m_facade->followSession(session);
        accept();
    }
}

void SearchDialog::refresh()
{
    if (m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    const bool loading = m_facade->transport()->property("searchLoading").toBool();
    const QString query = m_facade->transport()->property("searchQuery").toString();
    const QVariantList hits = m_facade->transport()->property("searchResults").toList();
    m_results->clear();
    for (const QVariant &value : hits) {
        const QVariantMap hit = value.toMap();
        const QString session = hit.value(QStringLiteral("session")).toString();
        const QString snippet = hit.value(QStringLiteral("snippet")).toString();
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  [%2]  %3")
                .arg(session, hit.value(QStringLiteral("role")).toString(),
                     snippet.isEmpty() ? hit.value(QStringLiteral("text")).toString()
                                       : snippet),
            m_results);
        item->setData(Qt::UserRole, session);
    }
    m_more->setEnabled(m_facade->transport()->property("searchHasMore").toBool());
    if (loading)
        m_status->setText(QStringLiteral("Searching…"));
    else if (query.size() < 2)
        m_status->setText(QStringLiteral("Two characters start a search."));
    else
        m_status->setText(QStringLiteral("%1 match%2 for “%3”")
                              .arg(hits.size())
                              .arg(hits.size() == 1 ? QString() : QStringLiteral("es"))
                              .arg(query));
}
