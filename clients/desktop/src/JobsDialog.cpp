/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "JobsDialog.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

JobsDialog::JobsDialog(ClientFacade *facade, QWidget *parent)
    : QDialog(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setWindowTitle(QStringLiteral("Jobs"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(720, 480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(), theme->space6(), theme->space6());
    layout->setSpacing(theme->space4());

    layout->addWidget(new QLabel(QStringLiteral("Jobs"), this));
    m_note = new QLabel(QStringLiteral("A job is a long-running turn. Output is the daemon's retained ring."), this);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    auto *body = new QHBoxLayout();
    body->setSpacing(theme->space4());
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(220);
    body->addWidget(m_list, 1);
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(Chrome::monoFont(theme->fontMono()));
    body->addWidget(m_output, 3);
    layout->addLayout(body, 1);

    auto *actions = new QHBoxLayout();
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), this);
    m_kill = new QPushButton(QStringLiteral("Kill"), this);
    auto *close = new QPushButton(QStringLiteral("Close"), this);
    m_kill->setEnabled(false);
    actions->addWidget(refresh);
    actions->addWidget(m_kill);
    actions->addStretch(1);
    actions->addWidget(close);
    layout->addLayout(actions);

    connect(refresh, &QPushButton::clicked, this, &JobsDialog::reload);
    connect(m_kill, &QPushButton::clicked, this, &JobsDialog::killSelected);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { followSelected(); });

    if (m_facade != nullptr) {
        connect(m_facade, &ClientFacade::jobsChanged, this, &JobsDialog::reload);
        if (QObject *transport = m_facade->transport()) {
            if (transport->metaObject()->indexOfSignal("jobOutput(QString,QString,qlonglong,bool)") >= 0) {
                connect(transport, SIGNAL(jobOutput(QString,QString,qlonglong,bool)),
                        this, SLOT(onJobOutput(QString,QString,qlonglong,bool)));
            }
            QMetaObject::invokeMethod(transport, "refreshJobs");
        }
    }
    reload();
}

void JobsDialog::reload()
{
    if (m_facade == nullptr)
        return;
    const QString keep = m_selected;
    m_list->blockSignals(true);
    m_list->clear();
    for (const QVariant &value : m_facade->jobs()) {
        const QVariantMap job = value.toMap();
        const QString id = job.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QString status = job.value(QStringLiteral("status")).toString();
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(id, status), m_list);
        item->setData(Qt::UserRole, id);
        if (id == keep)
            m_list->setCurrentItem(item);
    }
    m_list->blockSignals(false);
    if (m_list->count() == 0)
        m_note->setText(QStringLiteral("No jobs on this session."));
    else
        m_note->setText(QStringLiteral("A job is a long-running turn. Output is the daemon's retained ring."));
}

void JobsDialog::followSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    m_selected = item != nullptr ? item->data(Qt::UserRole).toString() : QString();
    m_kill->setEnabled(!m_selected.isEmpty());
    m_output->clear();
    if (m_selected.isEmpty() || m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    QMetaObject::invokeMethod(m_facade->transport(), "followJob",
                              Q_ARG(QString, m_selected), Q_ARG(qlonglong, 0));
}

void JobsDialog::killSelected()
{
    if (m_selected.isEmpty() || m_facade == nullptr)
        return;
    m_facade->killJob(m_selected);
}

void JobsDialog::onJobOutput(const QString &jobId, const QString &text, qlonglong next,
                             bool dropped)
{
    Q_UNUSED(next);
    if (jobId != m_selected)
        return;
    if (dropped)
        m_output->appendPlainText(QStringLiteral("[output truncated at the ring start]"));
    if (!text.isEmpty()) {
        m_output->moveCursor(QTextCursor::End);
        m_output->insertPlainText(text);
    }
}
