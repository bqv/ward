#include "SettingsDialog.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QProcess>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QLabel>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

namespace {

QLabel *textLabel(QWidget *parent, const QString &text, const QColor &colour,
                  const QFont &font)
{
    auto *label = new QLabel(text, parent);
    label->setFont(font);
    label->setWordWrap(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
    return label;
}

QPushButton *pushButton(QWidget *parent, const QString &text)
{
    auto *control = new QPushButton(text, parent);
    control->setFont(Chrome::uiFont(Theme::instance()->fontUI()));
    control->setAutoDefault(false);
    control->setDefault(false);
    return control;
}

QString rowTitle(const QVariantMap &session)
{
    const QString title = session.value(QStringLiteral("title")).toString().trimmed();
    return title.isEmpty() ? QStringLiteral("Untitled") : title;
}

}

SettingsDialog::SettingsDialog(ClientFacade *facade, QWidget *parent)
    : QDialog(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("Settings"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(560, 460);
    setMinimumSize(480, 360);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space5());

    layout->addWidget(textLabel(this, QStringLiteral("Settings"), theme->fg(),
                                Chrome::uiFont(theme->fontProse() + 2, true)));
    layout->addWidget(textLabel(this,
                                QStringLiteral("Preferences this client keeps between runs."),
                                theme->dim(), Chrome::uiFont(theme->fontData())));

    layout->addSpacing(theme->space2());
    layout->addWidget(textLabel(this, QStringLiteral("Composer"), theme->accent(),
                                Chrome::monoFont(theme->fontData())));
    layout->addWidget(textLabel(this, QStringLiteral("Enter while a turn is running"),
                                theme->fg(), Chrome::uiFont(theme->fontUI())));

    m_queueMode = new QRadioButton(QStringLiteral("Queue the message"), this);
    m_steerMode = new QRadioButton(QStringLiteral("Steer the running turn"), this);
    for (QRadioButton *radio : {m_queueMode, m_steerMode}) {
        radio->setFont(Chrome::uiFont(theme->fontUI()));
        layout->addWidget(radio);
    }
    layout->addWidget(textLabel(this,
                                QStringLiteral("A queued message waits for the next turn; "
                                               "a steered message joins the turn already running."),
                                theme->dim(), Chrome::uiFont(theme->fontData())));

    if (m_facade != nullptr && m_facade->transportName() == QLatin1String("ward")) {
        layout->addSpacing(theme->space3());
        layout->addWidget(textLabel(this, QStringLiteral("Daemon"), theme->accent(),
                                    Chrome::monoFont(theme->fontData())));
        layout->addWidget(textLabel(this, QStringLiteral("Concurrent turns"), theme->fg(),
                                    Chrome::uiFont(theme->fontUI())));
        m_maxRuns = new QSpinBox(this);
        m_maxRuns->setRange(1, 64);
        m_maxRuns->setFont(Chrome::uiFont(theme->fontUI()));
        layout->addWidget(m_maxRuns, 0, Qt::AlignLeft);
        m_debug = new QCheckBox(QStringLiteral("Debug logging"), this);
        m_debug->setFont(Chrome::uiFont(theme->fontUI()));
        layout->addWidget(m_debug);
        m_daemonNote = textLabel(this, QStringLiteral("Stored beside models.toml; applies immediately."),
                                 theme->dim(), Chrome::uiFont(theme->fontData()));
        layout->addWidget(m_daemonNote);
        m_hasSettings = true;
    }

    if (m_facade != nullptr && m_facade->transportName() == QLatin1String("ward")) {
        layout->addSpacing(theme->space3());
        layout->addWidget(textLabel(this, QStringLiteral("Modules"), theme->accent(),
                                    Chrome::monoFont(theme->fontData())));
        m_modulesNote = textLabel(this,
                                  QStringLiteral("Disabling applies to turns started afterwards; "
                                                 "a running turn keeps its instance."),
                                  theme->dim(), Chrome::uiFont(theme->fontData()));
        layout->addWidget(m_modulesNote);
        m_modules = new QListWidget(this);
        m_modules->setFont(Chrome::uiFont(theme->fontUI()));
        m_modules->setFrameShape(QFrame::NoFrame);
        m_modules->setMaximumHeight(150);
        layout->addWidget(m_modules);
        m_hasModules = true;
    }

    layout->addSpacing(theme->space3());
    layout->addWidget(textLabel(this, QStringLiteral("QUIC device"), theme->accent(),
                                Chrome::monoFont(theme->fontData())));
    m_pairNote = textLabel(this,
                           QStringLiteral("Pairing mints a token over the local socket and "
                                          "enrols a new certificate over QUIC."),
                           theme->dim(), Chrome::uiFont(theme->fontData()));
    layout->addWidget(m_pairNote);
    m_pair = pushButton(this, QStringLiteral("Pair a device"));
    layout->addWidget(m_pair, 0, Qt::AlignLeft);

    layout->addSpacing(theme->space3());
    m_archivedTitle = textLabel(this, QStringLiteral("Archived sessions"), theme->accent(),
                                Chrome::monoFont(theme->fontData()));
    m_archivedNote = textLabel(this,
                               QStringLiteral("Archiving files a session away; restoring "
                                              "puts it back in the rail."),
                               theme->dim(), Chrome::uiFont(theme->fontData()));
    m_archivedEmpty = textLabel(this, QStringLiteral("No archived sessions."),
                                theme->caption(), Chrome::uiFont(theme->fontData()));
    layout->addWidget(m_archivedTitle);
    layout->addWidget(m_archivedNote);
    layout->addWidget(m_archivedEmpty);

    m_archivedList = new QListWidget(this);
    Chrome::styleScrollBar(m_archivedList->verticalScrollBar());
    m_archivedList->setFont(Chrome::uiFont(theme->fontUI()));
    m_archivedList->setFrameShape(QFrame::NoFrame);
    m_archivedList->setUniformItemSizes(true);
    m_archivedList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_archivedList->setTextElideMode(Qt::ElideRight);
    m_archivedList->setMinimumHeight(120);
    QPalette listPalette = m_archivedList->palette();
    listPalette.setColor(QPalette::Base, theme->panel());
    listPalette.setColor(QPalette::Text, theme->fg());
    listPalette.setColor(QPalette::Highlight, theme->accent());
    listPalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_archivedList->setPalette(listPalette);
    layout->addWidget(m_archivedList, 1);

    m_unarchive = pushButton(this, QStringLiteral("Unarchive"));
    layout->addWidget(m_unarchive, 0, Qt::AlignLeft);

    layout->addStretch(1);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(theme->space3());
    actions->addStretch(1);
    auto *cancel = pushButton(this, QStringLiteral("Cancel"));
    auto *ok = pushButton(this, QStringLiteral("OK"));
    ok->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(ok);
    layout->addLayout(actions);

    connect(m_unarchive, &QPushButton::clicked, this, &SettingsDialog::unarchiveSelected);
    connect(m_pair, &QPushButton::clicked, this, &SettingsDialog::pairDevice);
    if (m_modules != nullptr) {
        connect(m_modules, &QListWidget::itemChanged, this,
                [this](QListWidgetItem *item) {
            if (m_reloadingModules || item == nullptr || m_facade == nullptr
                    || m_facade->transport() == nullptr)
                return;
            const QString name = item->data(Qt::UserRole).toString();
            QMetaObject::invokeMethod(m_facade->transport(), "setModuleEnabled",
                                      Q_ARG(QString, name),
                                      Q_ARG(bool, item->checkState() == Qt::Checked));
        });
    }
    connect(m_archivedList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_unarchive->setEnabled(m_archivedList->currentItem() != nullptr);
    });
    connect(ok, &QPushButton::clicked, this, [this]() {
        apply();
        accept();
    });
    connect(cancel, &QPushButton::clicked, this, &SettingsDialog::reject);

    if (m_facade != nullptr) {
        connect(m_facade, &ClientFacade::busyEnterChanged, this, &SettingsDialog::reload);
        if (QObject *transport = m_facade->transport()) {
            if (transport->metaObject()->indexOfSignal("settingsChanged()") >= 0)
                connect(transport, SIGNAL(settingsChanged()), this,
                        SLOT(reloadSettings()));
            if (transport->metaObject()->indexOfSignal("modulesChanged()") >= 0)
                connect(transport, SIGNAL(modulesChanged()), this,
                        SLOT(reloadModules()));
        }
        connect(m_facade, &ClientFacade::sessionsChanged,
                this, &SettingsDialog::reloadArchivedSessions);
    }

    reload();
    reloadArchivedSessions();
}

void SettingsDialog::showDialog()
{
    reload();
    reloadArchivedSessions();
    if (m_hasSettings && m_facade && m_facade->transport())
        QMetaObject::invokeMethod(m_facade->transport(), "refreshSettings");
    if (m_hasModules && m_facade && m_facade->transport())
        QMetaObject::invokeMethod(m_facade->transport(), "refreshModules");
    reloadSettings();
    reloadModules();
    open();
    raise();
    activateWindow();
}

void SettingsDialog::reload()
{
    if (m_facade == nullptr)
        return;
    const bool steer = m_facade->busyEnter() == QLatin1String("steer");
    m_steerMode->setChecked(steer);
    m_queueMode->setChecked(!steer);
}

void SettingsDialog::pairDevice()
{
    if (m_facade == nullptr || m_facade->transportName() != QLatin1String("ward")) {
        m_pairNote->setText(QStringLiteral("Pairing is only available on the ward transport."));
        return;
    }
    QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/ward-pair");
    if (!QFileInfo::exists(program))
        program = QStandardPaths::findExecutable(QStringLiteral("ward-pair"));
    if (program.isEmpty() || !QFileInfo::exists(program)) {
        m_pairNote->setText(QStringLiteral("ward-pair is not built; configure with WARD_QUIC=ON."));
        return;
    }
    bool ok = false;
    const QString scope = QInputDialog::getItem(
        this, QStringLiteral("Pair a device"), QStringLiteral("Scope"),
        {QStringLiteral("desktop"), QStringLiteral("phone"), QStringLiteral("readonly")},
        0, false, &ok);
    if (!ok)
        return;
    m_pair->setEnabled(false);
    m_pairNote->setText(QStringLiteral("Pairing…"));
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, {QStringLiteral("--scope"), scope});
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        m_pairNote->setText(QStringLiteral("Pairing failed: %1").arg(process.errorString()));
        m_pair->setEnabled(true);
        return;
    }
    const QString output = QString::fromUtf8(process.readAll()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_pairNote->setText(QStringLiteral("Pairing refused: %1").arg(output));
        m_pair->setEnabled(true);
        return;
    }
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                       + QStringLiteral("/ward-gui/quic");
    QSettings settings;
    settings.setValue(QStringLiteral("quic/cert"), root + QStringLiteral("/device.crt"));
    settings.setValue(QStringLiteral("quic/key"), root + QStringLiteral("/device.key"));
    settings.setValue(QStringLiteral("quic/ca"), root + QStringLiteral("/ca.crt"));
    m_pairNote->setText(QStringLiteral("Paired. Restart ward-gui with --socket quic://… "
                                       "to use the new device.\n%1").arg(output));
    m_pair->setEnabled(true);
}

void SettingsDialog::reloadSettings()
{
    if (!m_hasSettings || m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    const QVariantMap settings =
        m_facade->transport()->property("settings").toMap();
    if (settings.isEmpty())
        return;
    const int maxRuns = settings.value(QStringLiteral("max_runs"), 8).toInt();
    m_maxRuns->blockSignals(true);
    m_maxRuns->setValue(qBound(1, maxRuns, 64));
    m_maxRuns->blockSignals(false);
    m_debug->blockSignals(true);
    m_debug->setChecked(settings.value(QStringLiteral("debug")).toBool());
    m_debug->blockSignals(false);
}

void SettingsDialog::reloadModules()
{
    if (!m_hasModules || m_facade == nullptr || m_facade->transport() == nullptr)
        return;
    m_reloadingModules = true;
    m_modules->clear();
    const QVariantList modules = m_facade->transport()->property("modules").toList();
    for (const QVariant &value : modules) {
        const QVariantMap module = value.toMap();
        const QString name = module.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(name, module.value(QStringLiteral("version")).toString()),
            m_modules);
        item->setData(Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(module.value(QStringLiteral("enabled")).toBool()
                                ? Qt::Checked : Qt::Unchecked);
    }
    m_reloadingModules = false;
    if (m_modules->count() == 0)
        m_modulesNote->setText(QStringLiteral("No modules reported."));
}

void SettingsDialog::reloadArchivedSessions()
{
    struct ArchivedRow
    {
        QString title;
        QString id;
    };

    QList<ArchivedRow> rows;
    const bool listed = m_facade != nullptr && m_facade->hasSessionList();
    if (listed) {
        for (const QVariant &entry : m_facade->sessions()) {
            const QVariantMap session = entry.toMap();
            if (!session.value(QStringLiteral("archived")).toBool())
                continue;
            const QString id = session.value(QStringLiteral("sessionId")).toString();
            if (id.isEmpty())
                continue;
            rows.append({rowTitle(session), id});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ArchivedRow &a, const ArchivedRow &b) {
        const int byTitle = QString::compare(a.title, b.title, Qt::CaseInsensitive);
        if (byTitle != 0)
            return byTitle < 0;
        return QString::compare(a.id, b.id, Qt::CaseSensitive) < 0;
    });

    m_archivedTitle->setVisible(listed);
    m_archivedNote->setVisible(listed);
    m_archivedEmpty->setVisible(listed && rows.isEmpty());
    m_archivedList->setVisible(listed && !rows.isEmpty());
    m_unarchive->setVisible(listed);
    m_unarchive->setEnabled(false);
    m_archivedList->clear();
    for (const ArchivedRow &row : rows) {
        auto *item = new QListWidgetItem(row.title, m_archivedList);
        item->setData(Qt::UserRole, row.id);
        item->setToolTip(row.id);
    }
}

void SettingsDialog::unarchiveSelected()
{
    QListWidgetItem *item = m_archivedList->currentItem();
    if (m_facade == nullptr || item == nullptr)
        return;
    const QString sessionId = item->data(Qt::UserRole).toString();
    if (sessionId.isEmpty())
        return;
    m_unarchive->setEnabled(false);
    m_facade->unarchiveSession(sessionId);
    qInfo().nospace() << "settings dialog: workspace/unarchiveSession " << sessionId;
}

void SettingsDialog::apply()
{
    if (m_facade == nullptr)
        return;
    if (m_hasSettings && m_facade->transport() != nullptr) {
        const QVariantMap current =
            m_facade->transport()->property("settings").toMap();
        if (m_maxRuns->value() != current.value(QStringLiteral("max_runs")).toInt())
            QMetaObject::invokeMethod(m_facade->transport(), "setSetting",
                                      Q_ARG(QString, QStringLiteral("max_runs")),
                                      Q_ARG(QVariant, m_maxRuns->value()));
        if (m_debug->isChecked() != current.value(QStringLiteral("debug")).toBool())
            QMetaObject::invokeMethod(m_facade->transport(), "setSetting",
                                      Q_ARG(QString, QStringLiteral("debug")),
                                      Q_ARG(QVariant, m_debug->isChecked()));
    }
    const QString mode = m_steerMode->isChecked() ? QStringLiteral("steer")
                                                  : QStringLiteral("queue");
    if (mode == m_facade->busyEnter())
        return;
    m_facade->setBusyEnter(mode);
    qInfo().nospace() << "settings dialog: composer/busyEnter -> " << mode;
}
