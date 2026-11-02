#include "ClientFacade.h"

// The binder: one table, one reader, one place per transport difference.
//
// Every row is a transport property the shell reads and the facade signal (or
// recomputing slot) its NOTIFY re-emits. Rows are listed in the order the header
// documents them so the two files read as one list; a row whose transport notify
// does not exist is reported in missingProperties() rather than silently empty.

#include <QDebug>
#include <QMetaProperty>
#include <QVariant>

#include "AcpClient.h"
#include "HeadlessClient.h"

namespace {

struct Binding
{
    /// The property's name on the transport. Both transports publish it — the
    /// static check in the verification harness asserts exactly that against
    /// `DshClient::staticMetaObject` and `AcpClient::staticMetaObject`.
    const char *property;
    /// The facade signal re-emitted from the property's NOTIFY, or null when a
    /// slot has work to do first (a count, the parent seat, the state word).
    const char *signal;
    const char *slot;
};

const Binding kBindings[] = {
    // ---- connection, and the header's dot ---------------------------------
    {"state", nullptr, "onStateNotify"},
    {"status", "statusChanged", nullptr},
    {"lastError", "lastErrorChanged", nullptr},

    // ---- the conversation in front ----------------------------------------
    {"sessionId", nullptr, "onSessionIdNotify"},
    {"busy", "busyChanged", nullptr},
    {"turnOpen", "turnOpenChanged", nullptr},
    {"pendingSession", "pendingSessionChanged", nullptr},
    {"busyEnter", "busyEnterChanged", nullptr},

    // ---- the rail: rows, pins, marks and workspaces -----------------------
    {"sessions", nullptr, "onSessionsNotify"},
    {"pendingInteractionSessions", nullptr, "onSessionMarksNotify"},
    {"unreadSessionIds", nullptr, "onSessionMarksNotify"},
    {"workspaces", nullptr, "onWorkspacesNotify"},
    {"sessionsLoading", "sessionsLoadingChanged", nullptr},
    {"openingSession", "openingSessionChanged", nullptr},

    // ---- the status strip -------------------------------------------------
    {"permissionPreset", "permissionPresetChanged", nullptr},
    {"permissionPresets", "permissionPresetsChanged", nullptr},
    {"accessNotice", "accessNoticeChanged", nullptr},
    {"configOptions", "configOptionsChanged", nullptr},
    {"modelNotice", "modelNoticeChanged", nullptr},
    {"contextUsed", "contextChanged", nullptr},
    {"contextSize", "contextSizeChanged", nullptr},
    {"jobs", nullptr, "onJobsNotify"},

    // ---- the composer's dock ----------------------------------------------
    {"todos", "todosChanged", nullptr},
    {"goal", "goalChanged", nullptr},
    {"changedFiles", "changedFilesChanged", nullptr},
    {"queue", nullptr, "onQueueNotify"},
    {"pendingAttachments", nullptr, "onAttachmentsNotify"},
    {"fileCandidates", "fileCandidatesChanged", nullptr},

    {"fileEntries", "fileEntriesChanged", nullptr},
    {"filePath", "filePathChanged", nullptr},
    {"fileText", "fileTextChanged", nullptr},
    {"fileSelectedPath", "fileSelectedPathChanged", nullptr},
    {"fileBytes", "fileBytesChanged", nullptr},
    {"fileTruncated", "fileTruncatedChanged", nullptr},
    {"fileError", "fileErrorChanged", nullptr},
    {"scheduleTasks", "scheduleChanged", nullptr},
    {"scheduleLoading", "scheduleChanged", nullptr},
    {"scheduleNotice", "scheduleChanged", nullptr},
    {"scheduleNoticeError", "scheduleChanged", nullptr},
    {"scheduleSaving", "scheduleChanged", nullptr},
    {"scheduleHistory", "scheduleHistoryChanged", nullptr},
    {"scheduleHistoryFor", "scheduleHistoryChanged", nullptr},
    {"scheduleHistoryLoading", "scheduleHistoryChanged", nullptr},
    {"pluginInventory", "pluginsChanged", nullptr},
    {"pluginsLoading", "pluginsChanged", nullptr},
    {"pluginsNotice", "pluginsChanged", nullptr},
    {"pluginsNoticeError", "pluginsChanged", nullptr},
    {"trajectory", "trajectoryChanged", nullptr},
    {"transcript", "transcriptChanged", nullptr},
    {"hasEarlier", "hasEarlierChanged", nullptr},
    {"loadingEarlier", "hasEarlierChanged", nullptr},
};

/// A session id in the bare form the host uses on both sides of a comparison.
/// `session-<uuid>` is the roster's punctuation and not part of the identity;
/// this mirrors main.qml's `normaliseSessionId`.
QString bareSessionId(const QString &id)
{
    return id.startsWith(QLatin1String("session-")) ? id.mid(8) : id;
}

/// Argument forms for the one call helper, built by hand rather than with Q_ARG.
///
/// Qt 6.11's Q_ARG macro produces the newer QMetaMethodArgument, while the
/// argument-passing overload of QMetaMethod::invoke this class uses takes
/// QGenericArgument — the one present in every Qt 6. Naming the type here keeps
/// the call sites readable and keeps the version difference in one place. Both
/// helpers take a reference, so the value they point at is the caller's and lives
/// through the call.
QGenericArgument stringArg(const QString &value)
{
    return QGenericArgument("QString", &value);
}

QGenericArgument urlArg(const QUrl &value)
{
    return QGenericArgument("QUrl", &value);
}

QGenericArgument boolArg(const bool &value)
{
    return QGenericArgument("bool", &value);
}

QGenericArgument listArg(const QVariantList &value)
{
    return QGenericArgument("QVariantList", &value);
}

QGenericArgument intArg(const int &value)
{
    return QGenericArgument("int", &value);
}

}   // namespace

ClientFacade::ClientFacade(QObject *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
{
    if (!transport) {
        // No transport at all — a stub build or a test. That is not an error and
        // not 28 missing properties: `available` is the fact that says so, and
        // the state word must not claim "idle" for a transport that is not there.
        m_stateText = QStringLiteral("unavailable");
        return;
    }

    // Cached before anything binds: the name is CONSTANT on both transports, and
    // the capability flags below are read from it.
    m_transportName = readString(QStringLiteral("transportName"));

    // One capability rule, keyed on the transport's name. ACP publishes the
    // rail's roster, the Workspace registry, the permission catalog, the job list
    // and the attachment surface as compat empties precisely so a QML binding
    // cannot throw — an empty container is not a capability. The plan (§3.3) names
    // `transportName` as the test to use instead of `typeof openTerminal`, which is
    // true on ACP and false as evidence. A name this class does not know is a stub,
    // and a stub having nothing is the honest reading rather than optimistic.
    const bool roster = m_transportName == QLatin1String("dsh")
                     || m_transportName == QLatin1String("ward");
    m_hasSessionList = roster;
    m_hasWorkspaces = m_transportName == QLatin1String("dsh")
                   || m_transportName == QLatin1String("ward");
    m_hasPermissionCatalog = m_transportName == QLatin1String("dsh");
    m_hasAttachmentStaging = m_transportName == QLatin1String("dsh");
    m_hasFileSurface = m_transportName == QLatin1String("dsh");
    m_hasTrajectory = roster;
    m_hasGoalEditing = m_transportName == QLatin1String("dsh");

    // The window outlives a transport that is destroyed under it (a failed child,
    // a test tearing its stub down). QPointer clears itself first, so the flag is
    // emitted once and every later read is the empty value.
    connect(transport, &QObject::destroyed, this, [this]() {
        m_transport = nullptr;
        emit availableChanged();
    });

    bindAll();

    const QMetaObject *transportMeta = transport->metaObject();
    if (transportMeta->indexOfSignal("permissionRequested(QString,QString,QString)") >= 0)
        QObject::connect(transport, SIGNAL(permissionRequested(QString,QString,QString)), this,
                         SIGNAL(permissionRequested(QString,QString,QString)));
    if (transportMeta->indexOfSignal("permissionWithdrawn(QString)") >= 0)
        QObject::connect(transport, SIGNAL(permissionWithdrawn(QString)), this,
                         SIGNAL(permissionWithdrawn(QString)));
    if (transportMeta->indexOfSignal("questionsRequested(QString,QVariantList)") >= 0)
        QObject::connect(transport, SIGNAL(questionsRequested(QString,QVariantList)), this,
                         SIGNAL(questionsRequested(QString,QVariantList)));

    // Seed the derived values before anything can read them. Every recompute is
    // the same code the notify handlers run, so there is no second spelling of a
    // count that could disagree with the first.
    recountSessions();
    recountSessionMarks();
    recountWorkspaces();
    recountJobs();
    recountQueue();
    recountAttachments();
    refreshStateText();
    refreshParentSeat();
}

ClientFacade::~ClientFacade() = default;

QStringList ClientFacade::requiredProperties()
{
    QStringList names;
    const int count = int(sizeof(kBindings) / sizeof(kBindings[0]));
    names.reserve(count);
    for (const Binding &binding : kBindings)
        names.append(QString::fromLatin1(binding.property));
    return names;
}

// ---- the binder -----------------------------------------------------------

void ClientFacade::bindAll()
{
    for (const Binding &binding : kBindings)
        bind(binding.property, binding.signal, binding.slot);

    if (!m_missing.isEmpty()) {
        qWarning().noquote()
            << "ClientFacade: transport" << m_transportName << "does not publish"
            << m_missing.size() << "property(ies) the shell reads:" << m_missing.join(QLatin1String(", "))
            << "- each of those reads as the empty value. Either the shell is asking for something"
               " this transport cannot answer, or a capability flag above is missing a case.";
    }
}

void ClientFacade::bind(const char *property, const char *facadeSignal, const char *facadeSlot)
{
    const QMetaObject *meta = m_transport->metaObject();
    const int index = meta->indexOfProperty(property);
    if (index < 0) {
        reportMissing(QString::fromLatin1(property));
        return;
    }

    const QMetaProperty metaProperty = meta->property(index);
    if (!metaProperty.hasNotifySignal()) {
        // A constant transport property has nothing to forward: its value is the
        // same for the life of the object. `transportName` is the only one, and it
        // is cached at construction, so reaching here means a transport made one
        // of the shell's properties constant — worth knowing, not an error.
        qInfo().noquote() << "ClientFacade: transport" << m_transportName
                          << "publishes" << property << "as a constant; it is read once";
        return;
    }

    // The classic signature-based connect is deliberate: the receiver is either a
    // signal or a slot, both resolved in this class's own meta-object, and the two
    // transports' notify names differ (permissionPresets is `presetsChanged` on
    // DSH and `permissionPresetsChanged` on ACP) — so the signature is taken from
    // the property, never spelled by hand.
    const QByteArray notify = QByteArrayLiteral("2") + metaProperty.notifySignal().methodSignature();
    const QByteArray receiver = facadeSignal
        ? QByteArrayLiteral("2") + facadeSignal + "()"
        : QByteArrayLiteral("1") + facadeSlot + "()";

    // Two properties may ride one notify (the rail's two marks share
    // `sessionMarksChanged`), and one slot serves both. Connecting twice would
    // recompute and emit twice for one change, so an identical pair is skipped.
    const QString key = QString::fromLatin1(notify) + QLatin1String("->") + QString::fromLatin1(receiver);
    if (m_bound.contains(key))
        return;
    m_bound.insert(key);

    if (!QObject::connect(m_transport, notify.constData(), this, receiver.constData(), Qt::AutoConnection))
        qWarning().noquote() << "ClientFacade: cannot connect" << notify << "to" << receiver;
}

QMetaMethod ClientFacade::transportMethod(const char *signature) const
{
    if (!m_transport)
        return QMetaMethod();
    const QMetaObject *meta = m_transport->metaObject();
    const int index = meta->indexOfMethod(signature);
    return index < 0 ? QMetaMethod() : meta->method(index);
}

bool ClientFacade::call(const char *signature, QGenericReturnArgument result,
                        QGenericArgument a0, QGenericArgument a1, QGenericArgument a2) const
{
    if (!m_transport)
        return false;

    const QMetaMethod method = transportMethod(signature);
    if (!method.isValid()) {
        // No silent no-op at this level: the facade asked the transport for a
        // method by exact signature and it is not there. Either the shell should
        // have checked a capability flag, or a transport changed its surface.
        qWarning().noquote() << "ClientFacade: transport" << m_transportName
                             << "has no method" << signature;
        return false;
    }

    if (!method.invoke(m_transport, Qt::DirectConnection, result, a0, a1, a2)) {
        qWarning().noquote() << "ClientFacade:" << signature << "failed on transport" << m_transportName;
        return false;
    }
    return true;
}

void ClientFacade::reportMissing(const QString &name)
{
    if (!m_missing.contains(name))
        m_missing.append(name);
}

// ---- the readers ----------------------------------------------------------

QVariant ClientFacade::read(const QString &name) const
{
    if (!m_transport)
        return QVariant();
    const QMetaObject *meta = m_transport->metaObject();
    const QByteArray latin = name.toLatin1();
    const int index = meta->indexOfProperty(latin.constData());
    if (index < 0)
        return QVariant();   // in missingProperties() since construction
    return meta->property(index).read(m_transport);
}

QString ClientFacade::readString(const QString &name) const
{
    return read(name).toString();
}

bool ClientFacade::readBool(const QString &name) const
{
    return read(name).toBool();
}

int ClientFacade::readInt(const QString &name) const
{
    return read(name).toInt();
}

QVariantList ClientFacade::readList(const QString &name) const
{
    return read(name).toList();
}

QVariantList ClientFacade::trajectory() const
{
    return m_hasTrajectory ? readList(QStringLiteral("trajectory")) : QVariantList();
}

QAbstractItemModel *ClientFacade::transcript() const
{
    QObject *object = read(QStringLiteral("transcript")).value<QObject *>();
    return qobject_cast<QAbstractItemModel *>(object);
}

void ClientFacade::loadEarlierHistory()
{
    call("loadEarlierHistory()");
}

void ClientFacade::setBusyEnter(const QString &mode)
{
    if (!m_transport)
        return;

    const QMetaObject *meta = m_transport->metaObject();
    const int index = meta->indexOfProperty("busyEnter");
    if (index < 0) {
        reportMissing(QStringLiteral("busyEnter"));
        return;
    }

    const QMetaProperty property = meta->property(index);
    if (!property.isWritable()) {
        qWarning().noquote() << "ClientFacade: transport" << m_transportName
                             << "publishes busyEnter read-only; the composer cannot store the preference";
        return;
    }

    // Both transports write the same preference and emit only on a real change,
    // so the facade's own signal comes from theirs and never from here.
    if (!property.write(m_transport, mode))
        qWarning().noquote() << "ClientFacade: writing busyEnter failed on transport" << m_transportName;
}

// ---- derived values -------------------------------------------------------

void ClientFacade::refreshStateText()
{
    // One word for both transports. DshClient calls value 1 `Connecting` and
    // AcpClient calls it `Starting`: the same "not ready yet", so one word can
    // carry it without choosing a transport's vocabulary for the other.
    static const char *const words[] = {"idle", "connecting", "ready", "failed"};
    const int value = readInt(QStringLiteral("state"));
    if (value >= 0 && value < int(sizeof(words) / sizeof(words[0])))
        m_stateText = QString::fromLatin1(words[value]);
    else
        m_stateText = QStringLiteral("unknown");
}

void ClientFacade::refreshParentSeat()
{
    // main.qml's resolveLineage(), in the same order:
    //   * no followed session, no seat;
    //   * the followed row must be in the roster;
    //   * the link alone is not childhood — a fork has a parent and no seat — so
    //     the host's own markers decide it (`origin == "subagent"` or a non-empty
    //     `subagentMode`);
    //   * the seat's name is the parent's `label` (a child's own title is its
    //     delegation prompt), else its `title`, else nothing — the shell prints
    //     the word "parent" for empty rather than "Untitled".
    QString id;
    QString title;

    const QString followed = bareSessionId(readString(QStringLiteral("sessionId")));
    if (!followed.isEmpty()) {
        const QVariantList roster = readList(QStringLiteral("sessions"));
        for (const QVariant &row : roster) {
            const QVariantMap entry = row.toMap();
            if (bareSessionId(entry.value(QStringLiteral("sessionId")).toString()) != followed)
                continue;

            const QString parentId = entry.value(QStringLiteral("parentSessionId")).toString();
            if (parentId.isEmpty())
                break;

            const QString origin = entry.value(QStringLiteral("origin")).toString();
            const QString mode = entry.value(QStringLiteral("subagentMode")).toString();
            if (origin != QLatin1String("subagent") && mode.isEmpty())
                break;

            id = parentId;
            const QString bareParent = bareSessionId(parentId);
            for (const QVariant &parentRow : roster) {
                const QVariantMap parentEntry = parentRow.toMap();
                if (bareSessionId(parentEntry.value(QStringLiteral("sessionId")).toString()) != bareParent)
                    continue;
                title = parentEntry.value(QStringLiteral("label")).toString();
                if (title.isEmpty())
                    title = parentEntry.value(QStringLiteral("title")).toString();
                break;
            }
            break;
        }
    }

    if (id == m_parentSessionId && title == m_parentSessionTitle)
        return;
    m_parentSessionId = id;
    m_parentSessionTitle = title;
    emit parentSeatChanged();
}

void ClientFacade::recountSessions()
{
    const QVariantList roster = readList(QStringLiteral("sessions"));
    int pinned = 0;
    for (const QVariant &row : roster) {
        if (row.toMap().value(QStringLiteral("pinned")).toBool())
            ++pinned;
    }

    // Counts are emitted only on a real change: the transports already emit
    // sessionsChanged only for a real change, but a rename or a re-title moves the
    // list without moving either count, and a rail that repainted its badge for
    // that would be doing work nobody asked for.
    if (roster.size() != m_sessionCount) {
        m_sessionCount = int(roster.size());
        emit sessionCountChanged();
    }
    if (pinned != m_pinnedCount) {
        m_pinnedCount = pinned;
        emit pinnedCountChanged();
    }
}

void ClientFacade::recountSessionMarks()
{
    const int pending = int(readList(QStringLiteral("pendingInteractionSessions")).size());
    const int unread = int(readList(QStringLiteral("unreadSessionIds")).size());

    if (pending != m_pendingInteractionCount) {
        m_pendingInteractionCount = pending;
        emit pendingInteractionCountChanged();
    }
    if (unread != m_unreadCount) {
        m_unreadCount = unread;
        emit unreadCountChanged();
    }
}

void ClientFacade::recountWorkspaces()
{
    const int count = int(readList(QStringLiteral("workspaces")).size());
    if (count != m_workspaceCount) {
        m_workspaceCount = count;
        emit workspaceCountChanged();
    }
}

void ClientFacade::recountJobs()
{
    // The host's liveness rule for a job is "running or stopping"; the chip says
    // "running" only for those, and a settled job keeps its row.
    int live = 0;
    const QVariantList roster = readList(QStringLiteral("jobs"));
    for (const QVariant &value : roster) {
        const QString status = value.toMap().value(QStringLiteral("status")).toString();
        if (status == QLatin1String("running") || status == QLatin1String("stopping"))
            ++live;
    }

    if (live != m_liveJobCount) {
        m_liveJobCount = live;
        emit liveJobCountChanged();
    }
}

void ClientFacade::recountQueue()
{
    const int count = int(readList(QStringLiteral("queue")).size());
    if (count != m_queueCount) {
        m_queueCount = count;
        emit queueCountChanged();
    }
}

void ClientFacade::recountAttachments()
{
    const int count = int(readList(QStringLiteral("pendingAttachments")).size());
    if (count != m_pendingAttachmentCount) {
        m_pendingAttachmentCount = count;
        emit pendingAttachmentCountChanged();
    }
}

// ---- the notify handlers --------------------------------------------------

void ClientFacade::onStateNotify()
{
    refreshStateText();
    emit stateChanged();
}

void ClientFacade::onSessionsNotify()
{
    // Counts and the seat read this same list, so they move with it rather than on
    // a separate signal that could arrive after the shell has already read the old
    // roster.
    recountSessions();
    refreshParentSeat();
    emit sessionsChanged();
}

void ClientFacade::onSessionMarksNotify()
{
    recountSessionMarks();

    // Both marks ride one transport signal, so both are announced: a shell re-reads
    // two small id lists, which is cheaper — and less likely to go stale — than a
    // second connection to the same signal with a filter in front of it.
    emit pendingInteractionSessionsChanged();
    emit unreadSessionIdsChanged();
}

void ClientFacade::onWorkspacesNotify()
{
    recountWorkspaces();
    emit workspacesChanged();
}

void ClientFacade::onJobsNotify()
{
    recountJobs();
    emit jobsChanged();
}

void ClientFacade::onQueueNotify()
{
    recountQueue();
    emit queueChanged();
}

void ClientFacade::onAttachmentsNotify()
{
    recountAttachments();
    emit pendingAttachmentsChanged();
}

void ClientFacade::onSessionIdNotify()
{
    refreshParentSeat();
    emit sessionIdChanged();
}

// ---- the composer's actions -----------------------------------------------

QString ClientFacade::promptMode() const
{
    // main.qml's primaryMode(): with no turn running there is nothing to steer
    // into and the host queues; with one running the reader's own preference
    // decides. A preference that is neither word falls back to "queue" rather than
    // sending an invented mode to the host.
    if (!turnRunning())
        return QStringLiteral("queue");
    const QString preference = busyEnter();
    return preference == QLatin1String("steer") ? QStringLiteral("steer") : QStringLiteral("queue");
}

QString ClientFacade::alternatePromptMode() const
{
    return promptMode() == QLatin1String("steer") ? QStringLiteral("queue") : QStringLiteral("steer");
}

void ClientFacade::sendPrompt(const QString &text, const QString &mode)
{
    const QString resolved = mode.isEmpty() ? promptMode() : mode;

    // DSH's sendPrompt carries the delivery mode; ACP's takes the text alone,
    // because the protocol has no steering. Choosing by the signature the transport
    // actually publishes is the same call QML's two-argument binding is reduced to
    // on ACP today, and it is explicit rather than a silently dropped argument.
    if (transportMethod("sendPrompt(QString,QString)").isValid()) {
        call("sendPrompt(QString,QString)", QGenericReturnArgument(),
             stringArg(text), stringArg(resolved));
        return;
    }
    call("sendPrompt(QString)", QGenericReturnArgument(), stringArg(text));
}

QStringList ClientFacade::slashCommands() const
{
    QStringList commands;
    call("slashCommands()", QGenericReturnArgument("QStringList", &commands));
    return commands;
}

void ClientFacade::runCommand(const QString &line)
{
    call("runCommand(QString)", QGenericReturnArgument(), stringArg(line));
}

void ClientFacade::cancel()
{
    call("cancel()");
}

void ClientFacade::setConfigOption(const QString &configId, const QString &value)
{
    call("setConfigOption(QString,QString)", QGenericReturnArgument(),
         stringArg(configId), stringArg(value));
}

void ClientFacade::refreshPermissionPresets()
{
    call("refreshPermissionPresets()");
}

void ClientFacade::refreshModelCatalog()
{
    call("refreshConfigOptions()");
}

void ClientFacade::setPermissionPreset(const QString &value)
{
    call("setPermissionPreset(QString)", QGenericReturnArgument(), stringArg(value));
}

void ClientFacade::updateQueueItem(const QString &itemId, const QString &kind)
{
    call("updateQueueItem(QString,QString)", QGenericReturnArgument(),
         stringArg(itemId), stringArg(kind));
}

void ClientFacade::clearPendingAttachments()
{
    call("clearPendingAttachments()");
}

void ClientFacade::pasteAttachment()
{
    call("pasteAttachment()");
}

void ClientFacade::uploadAttachment(const QUrl &url)
{
    call("uploadAttachment(QUrl)", QGenericReturnArgument(), urlArg(url));
}

void ClientFacade::clearChangedFiles()
{
    call("clearChangedFiles()");
}

void ClientFacade::killJob(const QString &jobId)
{
    call("killJob(QString)", QGenericReturnArgument(), stringArg(jobId));
}

void ClientFacade::listFileReferences(const QString &prefix)
{
    call("listFileReferences(QString)", QGenericReturnArgument(), stringArg(prefix));
}

void ClientFacade::listDirectory(const QString &path)
{
    call("listDirectory(QString)", QGenericReturnArgument(), stringArg(path));
}

void ClientFacade::toggleDirectory(const QString &path)
{
    call("toggleDirectory(QString)", QGenericReturnArgument(), stringArg(path));
}

void ClientFacade::readFile(const QString &path)
{
    call("readFile(QString)", QGenericReturnArgument(), stringArg(path));
}

void ClientFacade::goUpDirectory()
{
    call("goUpDirectory()");
}

void ClientFacade::respondToPermission(const QString &requestId, bool allow)
{
    call("respondToPermission(QString,bool)", QGenericReturnArgument(), stringArg(requestId),
         boolArg(allow));
}

void ClientFacade::answerQuestions(const QString &eventId, const QVariantList &answers)
{
    call("answerQuestions(QString,QVariantList)", QGenericReturnArgument(), stringArg(eventId),
         listArg(answers));
}

void ClientFacade::goalAction(const QString &action)
{
    call("goalAction(QString)", QGenericReturnArgument(), stringArg(action));
}

void ClientFacade::editGoal(const QString &objective, int maxGoalRounds)
{
    if (!m_hasGoalEditing)
        return;
    call("editGoal(QString,int)", QGenericReturnArgument(), stringArg(objective),
         intArg(maxGoalRounds));
}

void ClientFacade::followSession(const QString &sessionId)
{
    call("followSession(QString)", QGenericReturnArgument(), stringArg(sessionId));
}

void ClientFacade::followSubagent(const QString &parentSessionId, const QString &childSessionId,
                                  const QString &mode)
{
    call("followSubagent(QString,QString,QString)", QGenericReturnArgument(),
         stringArg(parentSessionId), stringArg(childSessionId), stringArg(mode));
}

void ClientFacade::unarchiveSession(const QString &sessionId)
{
    call("unarchiveSession(QString)", QGenericReturnArgument(), stringArg(sessionId));
}

bool ClientFacade::followParentSession()
{
    const QString parent = parentSessionId();
    if (parent.isEmpty())
        return false;

    // main.qml's openParentSession(): a parent that is itself a child has to be
    // opened with the tagged subagent address, because the host answers
    // `session/agent-busy` for a bare child id. The two-argument form asks the host
    // to resolve that child's own mode, which is the safe half of the address.
    QString grandparent;
    for (const QVariant &row : readList(QStringLiteral("sessions"))) {
        const QVariantMap entry = row.toMap();
        if (bareSessionId(entry.value(QStringLiteral("sessionId")).toString()) != bareSessionId(parent))
            continue;
        grandparent = entry.value(QStringLiteral("parentSessionId")).toString();
        break;
    }

    if (!grandparent.isEmpty())
        followSubagent(grandparent, parent);
    else
        followSession(parent);
    return true;
}

void ClientFacade::refreshScheduleTasks()
{
    call("refreshScheduleTasks()");
}

void ClientFacade::saveScheduleTask(const QString &id, const QString &title,
                                    const QString &prompt)
{
    call("saveScheduleTask(QString,QString,QString)", QGenericReturnArgument(), stringArg(id),
         stringArg(title), stringArg(prompt));
}

void ClientFacade::deleteScheduleTask(const QString &id)
{
    call("deleteScheduleTask(QString)", QGenericReturnArgument(), stringArg(id));
}

void ClientFacade::loadScheduleHistory(const QString &id)
{
    call("loadScheduleHistory(QString)", QGenericReturnArgument(), stringArg(id));
}

void ClientFacade::refreshPluginInventory()
{
    call("refreshPluginInventory()");
}

void ClientFacade::setBundleEnabled(const QString &name, bool enabled)
{
    call("setBundleEnabled(QString,bool)", QGenericReturnArgument(), stringArg(name),
         boolArg(enabled));
}

// ---- the pane clients -----------------------------------------------------

AcpClient *ClientFacade::acpPane()
{
    // Created on first use, owned here. The constructor resolves the `dsh`
    // executable and starts nothing; the pane's own open calls start() and
    // createSession(). A window that never opens the pane therefore pays nothing.
    if (!m_acpPane)
        m_acpPane = new AcpClient(this);
    return m_acpPane;
}

HeadlessClient *ClientFacade::headlessClient()
{
    // Same rule: one child per task, started by HeadlessClient::send(), never by
    // construction.
    if (!m_headless)
        m_headless = new HeadlessClient(this);
    return m_headless;
}

bool ClientFacade::acpPaneAvailable() const
{
    // A pane of the same protocol beside the same protocol is a second child for
    // no second view — main.qml's own test for the header toggle.
    return m_transportName != QLatin1String("acp");
}
