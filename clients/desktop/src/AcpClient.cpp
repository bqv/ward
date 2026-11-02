#include "AcpClient.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

// ---------------------------------------------------------------------------
// AcpTranscript
// ---------------------------------------------------------------------------

namespace {

/// What one model update costs the GUI thread, for the client log.
///
/// `dataChanged` is not a mere notification: the delegates bound to that row are
/// re-evaluated inside the emit, on this thread, which is why a timer around the
/// mutation and its emit is an honest measure of the model's share of a frame.
/// The layout that follows is not in scope — it happens on the next frame — and
/// the QML side has a stall probe of its own for that half.
///
/// A line is written for an update slower than `SlowUpdateMs`, and a summary
/// once per window. That summary is the number to quote: "N updates in M ms,
/// worst X ms" is what distinguishes a model that is busy from one that is
/// expensive.
constexpr qint64 SlowUpdateMs = 30;
constexpr qint64 ReportWindowMs = 5000;

class UpdateCost
{
public:
    UpdateCost(const char *name, int row, int rows)
        : m_site(name)
        , m_row(row)
        , m_rows(rows)
    {
        m_clock.start();
    }

    ~UpdateCost()
    {
        Stats &s = stats();
        if (!s.started) {
            s.window.start();
            s.started = true;
        }
        const qint64 ms = m_clock.elapsed();
        ++s.updates;
        s.totalMs += ms;
        if (ms > s.worstMs) {
            s.worstMs = ms;
            s.worstSite = QLatin1String(m_site);
        }

        QStringList out;
        if (ms >= SlowUpdateMs)
            out << QStringLiteral("[model] %1 took %2 ms at row %3 of %4")
                       .arg(QLatin1String(m_site)).arg(ms).arg(m_row).arg(m_rows);
        const qint64 window = s.window.elapsed();
        if (window >= ReportWindowMs) {
            out << QStringLiteral("[model] %1 updates in %2 ms: %3 ms total,"
                                  " worst %4 ms (%5)")
                       .arg(s.updates).arg(window).arg(s.totalMs).arg(s.worstMs)
                       .arg(s.worstSite);
            s = Stats{};
        }
        for (const QString &line : out)
            qInfo().noquote() << line;
    }

    UpdateCost(const UpdateCost &) = delete;
    UpdateCost &operator=(const UpdateCost &) = delete;

private:
    struct Stats
    {
        QElapsedTimer window;
        qint64 updates = 0;
        qint64 totalMs = 0;
        qint64 worstMs = 0;
        QString worstSite;
        bool started = false;
    };

    static Stats &stats()
    {
        static Stats instance;
        return instance;
    }

    QElapsedTimer m_clock;
    /// A literal, not a QString: this object is built on the streaming path,
    /// once per token, and a QString there would be an allocation per token for
    /// a name that never changes.
    const char *m_site = nullptr;
    int m_row = 0;
    int m_rows = 0;
};

}   // namespace

AcpTranscript::AcpTranscript(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AcpTranscript::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

QVariant AcpTranscript::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows.at(index.row());
    switch (role) {
    case KindRole:   return row.kind;
    case TextRole:   return row.text;
    case TitleRole:  return row.title;
    case StatusRole: return row.status;
    case DetailRole: return row.detail;
    case RequestRole: return row.request;
    case TimeRole: return row.time;
    case TurnRole: return row.turn;
    case TurnLeadRole: return row.turnLead;
    case FullTextRole: return row.fullText;
    case KeyRole: return row.key;
    default:         return {};
    }
}

QHash<int, QByteArray> AcpTranscript::roleNames() const
{
    return {
        {KindRole,   "kind"},
        {TextRole,   "text"},
        {TitleRole,  "title"},
        {StatusRole, "status"},
        {DetailRole, "detail"},
        {RequestRole, "request"},
        {TimeRole,   "time"},
        {TurnRole,   "turn"},
        {TurnLeadRole, "turnLead"},
        {FullTextRole, "fullText"},
        {KeyRole, "rowKey"},
    };
}

namespace {

/// The rows a turn's fold stands in for.
///
/// A message is the answer and stays visible; the work that produced it is what
/// folds away. Notices are deliberately not process rows — the Android app keeps
/// them out of the fold too, because a compaction notice or an error is something
/// the reader is meant to see without opening anything.
bool isProcessKind(const QString &kind)
{
    return kind == QLatin1String("tool") || kind == QLatin1String("subtool")
        || kind == QLatin1String("thought");
}

/// The dock's todo shape: `{content, status}` and nothing else.
///
/// The same two fields DshClient publishes, because the QML reads exactly those
/// and either producer here already means them the DSH way — ACP's
/// PlanEntryStatus ("pending" | "in_progress" | "completed") and the harness's
/// `todo_write` schema use the same three words, so nothing is translated. An
/// entry with no content is dropped: it would render as a blank row, and both
/// producers declare content required.
QVariantList todosFromArray(const QJsonArray &entries)
{
    QVariantList todos;
    for (const QJsonValue &entry : entries) {
        const QJsonObject item = entry.toObject();
        const QString content = item.value(QStringLiteral("content")).toString();
        if (content.isEmpty())
            continue;
        todos.append(QVariantMap{
            {QStringLiteral("content"), content},
            {QStringLiteral("status"), item.value(QStringLiteral("status")).toString()},
        });
    }
    return todos;
}

/// Resolve a path a tool named against the session's workspace, so this list has
/// the same absolute form DshClient's change watch reports: the fs tools accept a
/// relative `file_path` and resolve it against the session cwd, which is the
/// workspace this client passed to session/new.
QString workspacePath(const QString &workspace, const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (QDir::isAbsolutePath(clean))
        return clean;
    return QDir::cleanPath(QDir(workspace).absoluteFilePath(clean));
}

}   // namespace

int AcpTranscript::append(const QString &kind, const QString &text,
                          const QString &title, const QString &status,
                          const QString &detail, const QString &key, qint64 time,
                          int turn, const QString &request, const QString &fullText)
{
    // Whether this row is the one that will stand in for its turn when folded.
    // Decided here rather than in the view: a delegate cannot ask the model what
    // came before it, and asking the neighbouring delegate is exactly the
    // cross-delegate read that breaks when a ListView recycles one.
    const bool lead = isProcessKind(kind)
                   && (m_rows.isEmpty() || m_rows.last().turn != turn
                       || !isProcessKind(m_rows.last().kind));

    // A title is a WORD the reader sees, so a title or text that is a Qt object's
    // debug name ("QQuickText(0x…)") means something stringified an object into
    // a row. Measured 2026-10-28: a compaction marker's label painted exactly
    // that on screen, and no literal in this tree produces it, so it arrived
    // through an argument that was meant to be a string. Every row passes here,
    // so this is the one place that can name the row and what it carried.
    if (title.contains(QLatin1String("(0x")) || text.contains(QLatin1String("(0x")))
        qWarning().nospace() << "row with an object-looking string: kind=\"" << kind
                             << "\" title=\"" << title.left(120) << "\" text=\""
                             << text.left(120) << "\"";

    const int row = static_cast<int>(m_rows.size());
    // The Repeater builds this row's delegate synchronously inside
    // `endInsertRows`, markdown conversion included, so an insert is measured
    // like an update rather than assumed to be free.
    const UpdateCost cost("append", row, row + 1);
    beginInsertRows(QModelIndex(), row, row);
    m_rows.append(Row{kind, text, title, status, detail, request, key, time, turn,
                      lead, fullText});
    endInsertRows();
    m_turnCountsStale = true;
    return row;
}

bool AcpTranscript::updateByKey(const QString &key, const QString &status,
                                const QString &detail, const QString &text)
{
    if (key.isEmpty())
        return false;

    for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
        if (m_rows.at(i).key != key)
            continue;
        Row &row = m_rows[i];

        // What actually changed — and therefore what the row's delegates need
        // told. All three roles were emitted for every update, so a row whose
        // *status* settled re-parsed its whole markdown and re-measured its text
        // for nothing, and an update that changed nothing at all still reset
        // every binding on the row.
        bool statusChanged = false;
        bool detailChanged = false;
        bool textChanged = false;
        if (!status.isEmpty() && row.status != status) {
            row.status = status;
            statusChanged = true;
            // A withdrawn row stops counting in turnSummary(), so a withdrawal
            // changes the per-turn counts the fold reads.
            m_turnCountsStale = true;
        }
        if (!detail.isEmpty() && row.detail != detail) {
            row.detail = detail;
            detailChanged = true;
        }
        if (!text.isEmpty() && row.text != text) {
            row.text = text;
            textChanged = true;
        }
        if (!statusChanged && !detailChanged && !textChanged)
            return true;   // found, and already in the state it was asked for

        QList<int> roles;
        roles.reserve(3);
        if (textChanged)
            roles.append(TextRole);
        if (statusChanged)
            roles.append(StatusRole);
        if (detailChanged)
            roles.append(DetailRole);

        const int rows = static_cast<int>(m_rows.size());
        const UpdateCost cost("updateByKey", i, rows);
        emit dataChanged(index(i, 0), index(i, 0), roles);
        return true;
    }
    return false;
}

bool AcpTranscript::removeRow(int row)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return false;

    const UpdateCost cost("removeRow", row, static_cast<int>(m_rows.size()));
    beginRemoveRows(QModelIndex(), row, row);
    m_rows.removeAt(row);
    endRemoveRows();
    m_turnCountsStale = true;
    return true;
}

void AcpTranscript::noteTurn(int turn, bool open, qint64 startTime)
{
    if (m_activeTurn == turn && m_turnOpen == open && m_turnStartTime == startTime)
        return;
    m_activeTurn = turn;
    m_turnOpen = open;
    m_turnStartTime = startTime;
    emit turnStateChanged();
}

QVariantMap AcpTranscript::turnSummary(int turn) const
{
    if (turn < 0)
        return {};

    if (m_turnCountsStale) {
        m_turnCounts.clear();
        for (const Row &row : m_rows) {
            if (row.turn < 0)
                continue;
            // A withdrawn row draws nothing — the delegate gives it no height —
            // so it is not work the turn did. It stays in the model because the
            // model has no delete, and that is exactly why counting must not
            // read its presence as evidence: a folded turn would otherwise
            // claim messages and thoughts the reader cannot be shown.
            if (row.status == QLatin1String("withdrawn"))
                continue;
            QVariantMap counts = m_turnCounts.value(row.turn);
            if (counts.isEmpty()) {
                counts.insert(QStringLiteral("tools"), 0);
                counts.insert(QStringLiteral("messages"), 0);
                counts.insert(QStringLiteral("thoughts"), 0);
                counts.insert(QStringLiteral("subagents"), 0);
            }
            if (row.kind == QLatin1String("tool") || row.kind == QLatin1String("subtool")) {
                counts[QStringLiteral("tools")] = counts.value(QStringLiteral("tools")).toInt() + 1;
                // Delegations are counted as well as counted in: the web and the
                // Android client both name them separately in the fold summary,
                // and the child's own rows never enter this log to be counted any
                // other way.
                const QString tool = row.title.toLower();
                if (tool == QLatin1String("subagent") || tool == QLatin1String("task")
                    || tool == QLatin1String("delegate")
                    || tool.startsWith(QLatin1String("subagent_"))) {
                    counts[QStringLiteral("subagents")] =
                        counts.value(QStringLiteral("subagents")).toInt() + 1;
                }
            } else if (row.kind == QLatin1String("thought")) {
                counts[QStringLiteral("thoughts")] = counts.value(QStringLiteral("thoughts")).toInt() + 1;
            } else if (row.kind == QLatin1String("agent")) {
                counts[QStringLiteral("messages")] = counts.value(QStringLiteral("messages")).toInt() + 1;
            }
            m_turnCounts.insert(row.turn, counts);
        }
        m_turnCountsStale = false;
    }

    return m_turnCounts.value(turn);
}

void AcpTranscript::appendOrExtend(const QString &kind, const QString &text, qint64 time,
                                   int turn)
{
    if (!m_rows.isEmpty() && m_rows.last().kind == kind) {
        // The time is the row's original one and is deliberately not refreshed:
        // a message that streams over a minute is stamped when it began.
        m_rows.last().text += text;
        const int row = static_cast<int>(m_rows.size()) - 1;
        const UpdateCost cost("appendOrExtend", row, row + 1);
        emit dataChanged(index(row, 0), index(row, 0), {TextRole});
        return;
    }
    append(kind, text, {}, {}, {}, {}, time, turn);
}

int AcpTranscript::prepend(const QString &kind, const QString &text,
                           const QString &title, const QString &status,
                           const QString &detail, const QString &request)
{
    // Insert at row 0 rather than rebuilding: a model reset would render the same
    // rows but lose the view's scroll position, and the reader who asked for
    // earlier history is holding precisely that position.
    const UpdateCost cost("prepend", 0, static_cast<int>(m_rows.size()) + 1);
    beginInsertRows(QModelIndex(), 0, 0);
    m_rows.prepend(Row{kind, text, title, status, detail, request});
    endInsertRows();
    m_turnCountsStale = true;
    return 0;
}

void AcpTranscript::prependOrExtend(const QString &kind, const QString &text)
{
    if (!m_rows.isEmpty() && m_rows.first().kind == kind) {
        m_rows.first().text += text;
        const UpdateCost cost("prependOrExtend", 0, static_cast<int>(m_rows.size()));
        emit dataChanged(index(0, 0), index(0, 0), {TextRole});
        return;
    }
    prepend(kind, text);
}

void AcpTranscript::replaceLast(const QString &kind, const QString &text)
{
    if (!m_rows.isEmpty() && m_rows.last().kind == kind) {
        m_rows.last().text = text;
        const int row = static_cast<int>(m_rows.size()) - 1;
        const UpdateCost cost("replaceLast", row, row + 1);
        emit dataChanged(index(row, 0), index(row, 0), {TextRole});
        return;
    }
    append(kind, text);
}

void AcpTranscript::clear()
{
    // The turn state belongs to the journal the rows came from, so it goes with
    // them: a start that outlived its journal would date the next conversation's
    // first turn from the previous one's beginning. Done before the empty-row
    // guard, because a transcript that holds no rows can still be holding a turn
    // the transport has since left.
    if (m_activeTurn != -1 || m_turnOpen || m_turnStartTime != 0) {
        m_activeTurn = -1;
        m_turnOpen = false;
        m_turnStartTime = 0;
        emit turnStateChanged();
    }
    if (m_rows.isEmpty())
        return;
    // A reset rebuilds every delegate in the view, synchronously, so this is the
    // site a session load's cost appears at. It is measured for that reason.
    const UpdateCost cost("clear", 0, static_cast<int>(m_rows.size()));
    beginResetModel();
    m_rows.clear();
    endResetModel();
    m_turnCountsStale = true;
}

// ---------------------------------------------------------------------------
// AcpClient
// ---------------------------------------------------------------------------

AcpClient::AcpClient(QObject *parent)
    : AcpCompat(parent)
{
    // Transcripts are created per session (see handleResponse), and each one's
    // turn latch is connected there. The facade's turnOpen follows the session
    // in front, so a noteTurn() on a model the reader is not looking at must not
    // notify the composer that its own turn moved.

    // Resolve the wrapper rather than hardcoding one path. `dsh` has lived both
    // in a user bin dir and in /usr/bin across installs, and a stale path fails
    // as a child that dies on the spot: the UI then shows no connection and
    // nothing on screen says why. PATH is consulted too, with the usual
    // absolute locations kept as fallbacks for a GUI launch that inherited a
    // thinner environment than a login shell would have given it.
    const QStringList candidates{
        QStandardPaths::findExecutable(QStringLiteral("dsh")),
        QDir::homePath() + QStringLiteral("/bin/dsh"),
        QStringLiteral("/usr/bin/dsh"),
        QStringLiteral("/usr/local/bin/dsh"),
    };
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable()) {
            m_program = candidate;
            break;
        }
    }
    if (m_program.isEmpty())
        qCritical() << "AcpClient: no `dsh` executable on PATH or in any of the usual"
                       " locations; the ACP child cannot start";
    m_arguments = {QStringLiteral("--profile"), QStringLiteral("acp")};

    const QString home = qEnvironmentVariable("DSH_HOME");
    m_workspace = home.isEmpty() ? QDir::homePath() : home;
    // Absolute and lexical from the start: `session/new` rejects a relative
    // workspace (`-32602 cwd must be an absolute path`) and the child is also
    // given this as its working directory, so one resolved value serves both.
    if (!m_workspace.isEmpty())
        m_workspace = QFileInfo(m_workspace).absoluteFilePath();
}

AcpClient::~AcpClient()
{
    shutdown();
}

void AcpClient::setLaunchCommand(const QString &program, const QStringList &arguments)
{
    m_program = program;
    m_arguments = arguments;
}

void AcpClient::setWorkspace(const QString &path)
{
    m_workspace = path.isEmpty() ? path : QFileInfo(path).absoluteFilePath();
}

// --- session state ---------------------------------------------------------

AcpClient::SessionState *AcpClient::sessionFor(const QString &id) const
{
    return m_sessionsById.value(id);
}

AcpClient::SessionState *AcpClient::currentSession() const
{
    return m_sessionsById.value(m_currentSessionId);
}

QString AcpClient::status() const
{
    // A session's own line is what the reader is looking at; the child's
    // handshake/error line is the fallback while there is no session to speak
    // for. This is why a prompt finishing in another session never overwrites
    // the status of the one in front.
    SessionState *session = currentSession();
    if (session && !session->status.isEmpty())
        return session->status;
    return m_status;
}

bool AcpClient::busy() const
{
    SessionState *session = currentSession();
    return session && session->busy;
}

int AcpClient::contextUsed() const
{
    SessionState *session = currentSession();
    return session ? session->contextUsed : 0;
}

int AcpClient::contextSize() const
{
    SessionState *session = currentSession();
    return session ? session->contextSize : 0;
}

QVariantList AcpClient::configOptions() const
{
    SessionState *session = currentSession();
    return session ? session->configOptions : QVariantList();
}

QVariantList AcpClient::openSessions() const
{
    QVariantList list;
    list.reserve(m_sessionOrder.size());
    for (const QString &id : m_sessionOrder) {
        const SessionState *session = m_sessionsById.value(id);
        if (!session)
            continue;
        const QFileInfo cwd(session->cwd);
        const QString cwdName = cwd.fileName().isEmpty() ? session->cwd : cwd.fileName();
        list.append(QVariantMap{
            {QStringLiteral("id"), session->id},
            {QStringLiteral("title"), session->title},
            {QStringLiteral("cwd"), session->cwd},
            {QStringLiteral("cwdName"), cwdName},
            {QStringLiteral("busy"), session->busy},
            {QStringLiteral("awaitingPermission"), !session->pendingPermissionIds.isEmpty()},
        });
    }
    return list;
}

void AcpClient::touchSessions()
{
    emit openSessionsChanged();
}

void AcpClient::syncWidgetState()
{
    // The AcpCompat dock properties are single members on a facade both
    // transports share, so they can only ever describe one session at a time.
    // They mirror the session in front; every other session keeps its own copy
    // in its SessionState and is copied back in on the switch.
    SessionState *session = currentSession();
    const QVariantList todos = session ? session->todos : QVariantList();
    if (m_todos != todos) {
        m_todos = todos;
        emit todosChanged();
    }
    const QVariantMap goal = session ? session->goal : QVariantMap();
    if (m_goal != goal) {
        m_goal = goal;
        emit goalChanged();
    }
    const QStringList changedFiles = session ? session->changedFiles : QStringList();
    if (m_changedFiles != changedFiles) {
        m_changedFiles = changedFiles;
        emit changedFilesChanged();
    }
}

void AcpClient::setCurrentSession(const QString &id)
{
    if (m_currentSessionId == id)
        return;

    m_currentSessionId = id;
    SessionState *session = currentSession();
    AcpTranscript *transcript = session ? session->transcript : nullptr;
    m_transcript = transcript;

    // One emit per fact, not one per property that happens to be derived: the
    // pane binds seven of these and a switch should repaint once.
    emit sessionChanged();
    emit transcriptChanged();
    emit turnOpenChanged();
    emit statusChanged();
    emit busyChanged();
    emit configOptionsChanged();
    emit contextChanged();
    syncWidgetState();
}

void AcpClient::setSessionStatus(SessionState *session, const QString &status)
{
    if (!session || session->status == status)
        return;
    const bool current = session == currentSession();
    session->status = status;
    if (current)
        emit statusChanged();
}

void AcpClient::setSessionBusy(SessionState *session, bool busy)
{
    if (!session || session->busy == busy)
        return;
    session->busy = busy;
    if (session == currentSession())
        emit busyChanged();
    // The bar draws running/idle per session, so a turn changing state in a
    // session that is not on screen still has to redraw its chip.
    touchSessions();
}

void AcpClient::reportSessionFailure(SessionState *session, const QString &text)
{
    // The child is still healthy: one prompt or one config change was refused.
    // Saying it on the session keeps the other sessions — and the connection —
    // usable, which is the whole difference from the old single-session client
    // where any error was fatal.
    if (!session) {
        setStatus(text);
        return;
    }
    setSessionBusy(session, false);
    setSessionStatus(session, text);
    session->transcript->append(QStringLiteral("error"), text);
    touchSessions();
}

void AcpClient::clearSessions()
{
    // Pending permission requests belonged to agents whose sessions are going
    // away; their dialogs must stop asking rather than sit there answerable.
    const QStringList permissionIds = m_pendingPermissions.keys();
    for (const QString &requestId : permissionIds)
        emit permissionWithdrawn(requestId);
    m_pendingPermissions.clear();

    const QList<SessionState *> sessions = m_sessionsById.values();
    for (SessionState *session : sessions) {
        // deleteLater, not delete: the pane's ListView may still be bound to
        // this model inside the emit below, and a model destroyed under a live
        // delegate model is a crash waiting for the next frame.
        if (session->transcript)
            session->transcript->deleteLater();
        delete session;
    }
    m_sessionsById.clear();
    m_sessionOrder.clear();
    m_transcript = nullptr;

    const bool hadSession = !m_currentSessionId.isEmpty();
    m_currentSessionId.clear();
    if (hadSession)
        emit sessionChanged();
    emit transcriptChanged();
    emit turnOpenChanged();
    emit busyChanged();
    emit configOptionsChanged();
    emit contextChanged();
    syncWidgetState();
    touchSessions();
}

void AcpClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void AcpClient::setStatus(const QString &status)
{
    // The published value is status(), not m_status: while a session exists the
    // session's line wins. Comparing the effective value stops a child-level
    // status arriving behind a session from re-notifying a binding that cannot
    // see it.
    const QString before = this->status();
    m_status = status;
    if (this->status() != before)
        emit statusChanged();
}

void AcpClient::fail(const QString &message)
{
    // Logged as well as shown, and this is not decoration. A failed handshake
    // used to leave the log with nothing but the spawn and the exit — no rpc
    // error, no state line — so an `initialize` or `session/new` rejection read
    // exactly like a child that died for no reason. The transcript and the log
    // are the only two places a reader can find out what the child said, and the
    // log is the one that survives the window being closed.
    qWarning().noquote() << "ACP client failed:" << message;

    // A failed child cannot carry the sessions that were running on it. Dropping
    // them here is what stops the bar from showing live-looking chips for
    // conversations whose wire is gone; their rows are not recoverable, because
    // ACP replays no transcript.
    clearSessions();

    m_lastError = message;
    emit lastErrorChanged();
    setStatus(message);
    setState(Failed);
}

// --- lifecycle -------------------------------------------------------------

void AcpClient::start()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Refused rather than silent: a start that quietly did nothing leaves
        // the pane on "starting…" beside a child nothing is talking to, and the
        // only trace would be a missing spawn line. shutdown() now waits for
        // `kill()` to land precisely so this cannot be reached from restart().
        qWarning() << "ACP start refused: a child is still running";
        setStatus(QStringLiteral("the previous ACP server is still running"));
        return;
    }

    // A finished child is still connected to this object, and onFinished() reads
    // THIS object's state rather than the sender's: a late `finished` from the
    // previous conversation would mark the new one Failed. restart() arrives here
    // with exactly that history, so the old handle is detached and disposed of
    // before a new one exists. Never reached with a live process — the guard
    // above returns first.
    if (m_process != nullptr) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }

    setState(Starting);
    setStatus(QStringLiteral("starting the ACP server…"));
    m_lastError.clear();
    emit lastErrorChanged();
    m_stdoutBuffer.clear();
    m_stopping = false;

    m_process = new QProcess(this);
    m_process->setProgram(m_program);
    m_process->setArguments(m_arguments);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_workspace.isEmpty())
        m_process->setWorkingDirectory(m_workspace);

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AcpClient::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &AcpClient::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred,
            this, &AcpClient::onProcessErrorOccurred);
    connect(m_process, &QProcess::finished,
            this, &AcpClient::onFinished);

    qInfo() << "spawning ACP server:" << m_program << m_arguments;
    m_process->start();

    if (!m_process->waitForStarted(15000)) {
        fail(QStringLiteral("could not start %1 — %2").arg(m_program, m_process->errorString()));
        return;
    }

    setStatus(QStringLiteral("handshaking…"));

    // initialize. Capabilities are stated explicitly: this client does not
    // serve filesystem reads and does not provide a terminal, so the agent
    // must not assume it can delegate those.
    QJsonObject fs{
        {QStringLiteral("readTextFile"), false},
        {QStringLiteral("writeTextFile"), false},
    };
    QJsonObject capabilities{
        {QStringLiteral("fs"), fs},
        {QStringLiteral("terminal"), false},
    };
    QJsonObject params{
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("clientCapabilities"), capabilities},
    };
    const int id = beginRequest(QStringLiteral("initialize"), params);
    qInfo() << "ACP handshake: sent initialize (id" << id << ")";
}

void AcpClient::shutdown()
{
    // Any start still queued from restart() belongs to the conversation that is
    // being put away, not the one that was being opened: a pane closed in the
    // turn between restart() and its deferred spawn must not leave a child
    // running behind it. Bumped before the early returns, because the state this
    // matters in — a pending start, no process yet — is exactly the state they
    // return from.
    ++m_restartGeneration;

    if (!m_process)
        return;
    if (m_process->state() == QProcess::NotRunning)
        return;

    m_stopping = true;

    // Best-effort orderly close of EVERY session, then a bounded wait, then
    // force. A wedged server must not keep the window alive, and the close is
    // what lets the child flush each session's persistence rather than having it
    // torn down by EOF. The frames are written in one pass and flushed once — a
    // beginRequest per session would wait for the pipe between each pair, so a
    // bar with four sessions would block the window for seconds.
    if (!m_sessionOrder.isEmpty()) {
        const QList<QString> order = m_sessionOrder;
        for (const QString &id : order) {
            const QJsonObject close{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), m_nextId++},
                {QStringLiteral("method"), QStringLiteral("session/close")},
                {QStringLiteral("params"), QJsonObject{{QStringLiteral("sessionId"), id}}},
            };
            QByteArray payload =
                QJsonDocument(close).toJson(QJsonDocument::Compact);
            payload.append('\n');
            m_process->write(payload);
        }
        m_process->waitForBytesWritten(500);
    }

    // The pane-close exit ends every conversation it was holding. Keeping the
    // transcripts behind a closed pane would only leave models nothing can
    // return to: the child is about to be gone, and ACP cannot reattach to a
    // process it did not keep.
    clearSessions();

    // Closing the write channel is what ends the child: the ACP profile binds
    // stdin EOF to a bounded *successful* shutdown (`exitOnStdinEnd` in
    // @deepseek-ai/dsh-cmdline), so a stop started here is indistinguishable in
    // the log from a child that walked away — both are "exited with code 0".
    // That is why the stop is now named in the log rather than inferred.
    m_process->closeWriteChannel();
    if (!m_process->waitForFinished(3000)) {
        m_process->terminate();
        if (!m_process->waitForFinished(2000))
            m_process->kill();
    }

    // `kill()` does not wait, so the process can still be Running when this
    // returns. A restart() in the same event-loop turn would then reach start()
    // with a live handle, be refused as "already running", and the new
    // conversation would never start at all. Wait for the state to settle so the
    // next start() always gets a fresh pipe.
    if (m_process->state() != QProcess::NotRunning)
        m_process->waitForFinished(1000);
}

void AcpClient::restart()
{
    // Stop before forgetting, not after: session/close needs the id and the
    // write channel that resetConversation() throws away, and a child left alive
    // because the order was reversed would be exactly the orphan this exists to
    // prevent.
    shutdown();
    resetConversation();

    // Started from a clean event-loop turn, never from inside the caller. The
    // pane reaches this from a QML click handler, and start() has to block to
    // spawn (`waitForStarted`) and to flush the initialize frame
    // (`waitForBytesWritten`); a nested event loop entered in the middle of a
    // QML signal handler is the one structural difference between the pane and
    // the window's own transport, which has always started from
    // `QTimer::singleShot(0, …)`. Deferring costs one turn of the event loop,
    // and the pane already reads "starting the ACP server…" in that turn.
    //
    // The generation is what makes the delay safe: a stop or a second restart
    // before the timer fires supersedes this one, so the only child that can
    // ever be spawned is the one the last press asked for.
    const quint64 generation = ++m_restartGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != m_restartGeneration)
            return;
        start();
    });
}

void AcpClient::resetConversation()
{
    // Every session, its rows, its dock state and its pending permission
    // requests go together: they all belonged to the child that is being
    // replaced. clearSessions() also discards each transcript's turn latch,
    // which is what stops the next conversation's first turn being dated from
    // this one's.
    clearSessions();

    // Facts about the child that just went away. Each is emitted rather than
    // silently overwritten because every one of them is read by a binding, and a
    // stale agent line would describe a process that no longer exists.
    m_agentInfo.clear();
    emit agentInfoChanged();

    // Requests still awaiting an answer belonged to the old child's wire. Their
    // responses can never arrive, so their ids must not be reusable for the new
    // one — beginRequest() starts again at 1, which is safe only once the old
    // tables are gone.
    m_pendingMethods.clear();
    m_requestSession.clear();
    m_pendingPermissions.clear();
    m_nextId = 1;
    m_stdoutBuffer.clear();

    if (!m_lastError.isEmpty()) {
        m_lastError.clear();
        emit lastErrorChanged();
    }
    setStatus(QString());
}

void AcpClient::onProcessErrorOccurred(QProcess::ProcessError error)
{
    if (!m_process)
        return;
    // A crash after start is reported through finished(); only report here when
    // the process never came up, so the UI does not get two conflicting stories.
    if (error == QProcess::FailedToStart) {
        fail(QStringLiteral("failed to start: %1").arg(m_process->errorString()));
    }
}

void AcpClient::onFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString how = (status == QProcess::CrashExit)
        ? QStringLiteral("crashed")
        : QStringLiteral("exited with code %1").arg(exitCode);

    // A child that was asked to go is not a failure, and saying it "exited with
    // code 0" as one is what made a closed pane read like a dead server. The
    // ACP profile exits 0 on stdin EOF and drains to 0 on SIGTERM by design, so
    // the exit code alone cannot tell the two apart — this flag can.
    if (m_stopping) {
        qInfo() << "ACP server stopped on request (" << how << ")";
        // shutdown() already did this, but a child may also leave after a
        // forced stop that never reached that line.
        clearSessions();
        if (m_state != Failed) {
            setState(Idle);
            setStatus(QStringLiteral("stopped"));
        }
        return;
    }

    qInfo() << "ACP server" << how;

    // The sessions died with the child. Their rows cannot be replayed by ACP, so
    // there is nothing to keep them for: the bar must not offer a conversation
    // whose wire is gone.
    clearSessions();

    if (m_state != Failed) {
        setState(Failed);
        setStatus(QStringLiteral("the ACP server %1").arg(how));
        m_lastError = m_status;
        emit lastErrorChanged();
        qWarning().noquote() << "ACP client failed:" << m_lastError;
    }
}

// --- wire ------------------------------------------------------------------

void AcpClient::writeMessage(const QJsonObject &message)
{
    if (!m_process) {
        qWarning() << "write with no process";
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        qWarning() << "write while the process is not running";
        return;
    }

    QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    payload.append('\n');   // ACP is newline-delimited JSON

    const qint64 written = m_process->write(payload);
    // A write that silently does nothing looks exactly like a server that never
    // answers, so a failed flush is reported rather than swallowed.
    if (!m_process->waitForBytesWritten(2000)) {
        qWarning() << "write of" << written << "bytes did not flush:"
                   << m_process->errorString();
    }
}

int AcpClient::beginRequest(const QString &method, const QJsonObject &params,
                            const QString &sessionId)
{
    const int id = m_nextId++;
    m_pendingMethods.insert(id, method);
    // Recorded so the ANSWER goes back to the session that asked. Without this,
    // a prompt finishing in a session the reader had switched away from would
    // clear the busy flag of whatever happened to be in front.
    if (!sessionId.isEmpty())
        m_requestSession.insert(id, sessionId);

    QJsonObject message{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
    writeMessage(message);
    return id;
}

void AcpClient::onReadyReadStandardOutput()
{
    if (!m_process)
        return;
    const QByteArray chunk = m_process->readAllStandardOutput();
    m_stdoutBuffer.append(chunk);

    // Split on newlines. A partial trailing line stays in the buffer: ACP
    // frames are newline-delimited, and a frame can always be split across
    // two reads.
    //
    // The whole read is consumed in one pass and the parsed prefix removed once.
    // Removing it per frame shifted every byte still to come, so one read that
    // carried many frames cost the square of its own length in memmove — and a
    // journal replay arriving over stdio is exactly that: one large read holding
    // a page of records. The lines are taken out before any of them is handled,
    // so a handler that pumps the socket cannot append to a buffer being walked.
    QList<QByteArray> lines;
    int consumed = 0;
    int newline;
    while ((newline = m_stdoutBuffer.indexOf('\n', consumed)) >= 0) {
        lines.append(m_stdoutBuffer.mid(consumed, newline - consumed).trimmed());
        consumed = newline + 1;
    }
    if (consumed > 0)
        m_stdoutBuffer.remove(0, consumed);

    for (const QByteArray &line : lines) {
        if (!line.isEmpty())
            handleLine(line);
    }
}

void AcpClient::onReadyReadStandardError()
{
    if (!m_process)
        return;

    const QByteArray raw = m_process->readAllStandardError();
    for (const QByteArray &line : raw.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        // The ACP profile prints one banner line on startup. Echoing every
        // stderr line would drown the client's own diagnostics in it, so only
        // anything that actually looks like a failure is re-emitted.
        if (trimmed.startsWith("Error") || trimmed.startsWith("error")
            || trimmed.contains("ELIFECYCLE") || trimmed.contains("Cannot find")
            || trimmed.contains("EACCES") || trimmed.contains("ENOENT")) {
            qWarning() << "ACP server:" << trimmed;
        } else {
            qDebug() << "ACP server:" << trimmed;
        }
    }
}

QString AcpClient::transcriptAsText() const
{
    QString out;
    // The headless path holds exactly one session, so "the transcript" is the
    // one in front. With several, only the session being shown is exported;
    // each session's rows live in its own model and there is no combined text.
    if (!m_transcript)
        return out;
    const int rows = m_transcript->rowCount();
    for (int i = 0; i < rows; ++i) {
        const QModelIndex idx = m_transcript->index(i, 0);
        const QString kind = m_transcript->data(idx, AcpTranscript::KindRole).toString();
        const QString text = m_transcript->data(idx, AcpTranscript::TextRole).toString();
        const QString title = m_transcript->data(idx, AcpTranscript::TitleRole).toString();
        const QString status = m_transcript->data(idx, AcpTranscript::StatusRole).toString();

        out += QStringLiteral("%1: ").arg(kind, -8);
        if (!title.isEmpty())
            out += QStringLiteral("%1 [%2] ").arg(title, status);
        out += text;
        out += QLatin1Char('\n');
    }
    return out;
}

void AcpClient::handleLine(const QByteArray &line)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "non-JSON line from ACP server:" << line.left(200);
        return;
    }

    const QJsonObject message = doc.object();

    // Three shapes share the wire. A server response carries an id and no
    // method; a notification carries a method and no id; an agent-initiated
    // request carries both.
    const bool hasId = message.contains(QStringLiteral("id"));
    const bool hasMethod = message.contains(QStringLiteral("method"));

    if (hasMethod && hasId)
        handleAgentRequest(message);
    else if (hasMethod)
        handleNotification(message);
    else if (hasId)
        handleResponse(message);
}

void AcpClient::handleResponse(const QJsonObject &message)
{
    const QJsonValue idValue = message.value(QStringLiteral("id"));
    const int id = idValue.isDouble() ? idValue.toInt() : -1;
    const QString method = m_pendingMethods.take(id);
    // Which session asked, for the methods that are about one. Empty for
    // initialize and session/new — the latter's id is in its answer.
    const QString requestedSession = m_requestSession.take(id);

    const QJsonObject result = message.value(QStringLiteral("result")).toObject();

    if (message.contains(QStringLiteral("error"))) {
        const QJsonObject error = message.value(QStringLiteral("error")).toObject();
        const QString detail = error.value(QStringLiteral("message")).toString();
        const QJsonValue codeValue = error.value(QStringLiteral("code"));
        const int code = codeValue.isDouble() ? codeValue.toInt() : 0;

        // The code and any `data` are part of the answer, so the status line
        // carries them: "-32602 invalid params: cwd must be an absolute path" is
        // actionable, "session/new failed" is not. The whole object also goes to
        // the log, bounded, because the pane's status is truncated at the edge
        // and a JSON-RPC error can carry the server's own stack or detail field.
        const QJsonValue data = error.value(QStringLiteral("data"));
        QString text;
        if (code != 0) {
            text = QStringLiteral("%1 failed (%2): %3")
                       .arg(method.isEmpty() ? QStringLiteral("request") : method)
                       .arg(code)
                       .arg(detail);
        } else {
            text = QStringLiteral("%1 failed: %2")
                       .arg(method.isEmpty() ? QStringLiteral("request") : method, detail);
        }

        QString raw = QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact));
        if (raw.size() > 400)
            raw = raw.left(400) + QStringLiteral("…");
        qWarning().noquote() << "ACP error response for" << method << "(id" << id << "):" << raw;
        if (!data.isUndefined() && !data.isNull())
            qWarning().noquote() << "ACP error data:" << data.toVariant().toString().left(400);

        // A refusal of one session's call is that session's failure, not the
        // child's. The old client held one session, so any JSON-RPC error was
        // fatal and that was right; now a refused prompt or config change must
        // leave the other sessions — and the connection — running.
        if (method == QLatin1String("session/prompt")
            || method == QLatin1String("session/set_config_option")) {
            reportSessionFailure(sessionFor(requestedSession), text);
            return;
        }
        if (method == QLatin1String("session/close")) {
            // The session is usually already gone from the bar (closeSession
            // drops it eagerly); a refusal means the child still holds it, which
            // is worth a line in the log.
            qWarning().noquote() << "session/close was refused:" << text;
            reportSessionFailure(sessionFor(requestedSession), text);
            return;
        }
        // A second session/new that fails (a cwd the child refuses, say) must
        // not tear down the sessions already running on that child.
        if ((method == QLatin1String("session/new")
             || method == QLatin1String("session/resume"))
            && !m_sessionsById.isEmpty()) {
            reportSessionFailure(currentSession(), text);
            return;
        }
        fail(text);
        return;
    }

    if (method == QLatin1String("initialize")) {
        const QJsonObject info = result.value(QStringLiteral("agentInfo")).toObject();
        m_agentInfo = QStringLiteral("%1 %2")
                          .arg(info.value(QStringLiteral("name")).toString(),
                               info.value(QStringLiteral("version")).toString());
        emit agentInfoChanged();

        // The two lines that make a failed handshake legible: the transport
        // reaches Ready here, BEFORE any session exists, which is why a window
        // bound to `--transport acp` could look healthy while every session it
        // asked for was refused.
        qInfo() << "ACP handshake: initialize answered by" << m_agentInfo;

        setState(Ready);
        setStatus(QStringLiteral("ready"));
        createSession();
        return;
    }

    if (method == QLatin1String("session/new") || method == QLatin1String("session/resume")) {
        // `session/new` names the id it made; `session/resume`'s answer carries
        // only configOptions (see the ACP schema), so a resume's id is the one
        // the request named.
        QString sessionId = result.value(QStringLiteral("sessionId")).toString();
        if (sessionId.isEmpty())
            sessionId = requestedSession;
        if (sessionId.isEmpty()) {
            // No id means no way to address this conversation again. Treat it
            // like a refused create rather than inventing one.
            const QString text =
                QStringLiteral("%1 answered without a session id").arg(method);
            qWarning().noquote() << text;
            if (m_sessionsById.isEmpty())
                fail(text);
            else
                reportSessionFailure(currentSession(), text);
            return;
        }

        SessionState *session = sessionFor(sessionId);
        if (!session) {
            session = new SessionState;
            session->id = sessionId;
            session->cwd = m_workspace;
            // ACP carries no title of its own. The first prompt names the chip;
            // until then the id prefix is the only honest label.
            session->title = QStringLiteral("session %1").arg(sessionId.left(8));
            session->transcript = new AcpTranscript(this);
            // The facade's turnOpen follows the session in front. A noteTurn on
            // a model the reader is not looking at must not notify the composer
            // that its own turn moved, which is why the guard is here rather
            // than on the transcript.
            connect(session->transcript, &AcpTranscript::turnStateChanged,
                    this, [this, transcript = session->transcript]() {
                        if (transcript == m_transcript)
                            emit turnOpenChanged();
                    });
            m_sessionsById.insert(sessionId, session);
            m_sessionOrder.append(sessionId);
        }
        setSessionStatus(session, QStringLiteral("ready"));

        // A fresh session's dock starts empty. Unlike the journal transport
        // there is no snapshot to re-read, and ACP replays no history on resume
        // either — whatever the previous connection captured is all there is.
        session->pendingTodos.clear();
        session->pendingWritePaths.clear();
        session->goalCalls.clear();
        session->todos.clear();
        session->goal.clear();
        session->changedFiles.clear();
        refreshConfigOptions(session, result.value(QStringLiteral("configOptions")).toArray());

        qInfo() << "session established:" << sessionId;
        // A new session is what the reader pressed `+` for, so it comes to the
        // front. A resumed one is being reattached for the same reason.
        setCurrentSession(sessionId);
        touchSessions();
        return;
    }

    if (method == QLatin1String("session/set_config_option")) {
        SessionState *session = sessionFor(requestedSession);
        if (session)
            refreshConfigOptions(session,
                                 result.value(QStringLiteral("configOptions")).toArray());
        return;
    }

    if (method == QLatin1String("session/prompt")) {
        SessionState *session = sessionFor(requestedSession);
        if (!session) {
            qWarning() << "prompt answer for a session this client no longer holds:"
                       << requestedSession;
            return;
        }
        const QString stop = result.value(QStringLiteral("stopReason")).toString();
        setSessionBusy(session, false);
        setSessionStatus(session, QStringLiteral("ready"));
        if (stop != QLatin1String("end_turn") && !stop.isEmpty())
            session->transcript->append(QStringLiteral("notice"),
                                        QStringLiteral("turn ended: %1").arg(stop));
        return;
    }
}

void AcpClient::handleNotification(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    if (method != QLatin1String("session/update"))
        return;

    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    // The session is the routing key. This is the line that used to be missing:
    // the old client dropped `params.sessionId` and appended every update to one
    // transcript, so two live sessions interleaved and neither could be trusted.
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();
    SessionState *session = sessionFor(sessionId);
    if (!session) {
        // Normal in one case and only one: a frame already in flight when the
        // reader closed that session. The rows have nowhere to go, because the
        // model went with the session.
        qInfo() << "session/update for a session this client does not hold:" << sessionId;
        return;
    }
    applySessionUpdate(session, params.value(QStringLiteral("update")).toObject());
}

void AcpClient::handleAgentRequest(const QJsonObject &message)
{
    // The only request an ACP agent raises at a client that declared no fs and
    // no terminal is a permission prompt.
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue idValue = message.value(QStringLiteral("id"));

    if (method != QLatin1String("session/request_permission")) {
        // Refuse anything else explicitly rather than leaving the agent waiting
        // forever on an answer that never comes.
        QJsonObject refusal{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), idValue},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), -32601},
                {QStringLiteral("message"), QStringLiteral("unsupported client method: %1").arg(method)},
            }},
        };
        writeMessage(refusal);
        return;
    }

    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    const QJsonObject toolCall = params.value(QStringLiteral("toolCall")).toObject();
    const QString requestId = idValue.isString()
        ? idValue.toString()
        : QString::number(idValue.toInt());
    // The request names its session, and the answer must be attributed to that
    // session even if the reader has switched away by the time it is answered.
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();

    const QString toolName = toolCall.value(QStringLiteral("title")).toString().isEmpty()
        ? toolCall.value(QStringLiteral("kind")).toString()
        : toolCall.value(QStringLiteral("title")).toString();

    QString detail;
    const QJsonArray content = toolCall.value(QStringLiteral("content")).toArray();
    for (const QJsonValue &entry : content) {
        const QJsonObject block = entry.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("content")) {
            detail = block.value(QStringLiteral("content")).toObject()
                         .value(QStringLiteral("text")).toString();
            break;
        }
    }

    SessionState *session = sessionFor(sessionId);
    if (!session) {
        // Nothing can answer this: the session it belongs to is gone. Refusing
        // at once is the only way to stop the agent waiting on a dialog that
        // will never be shown.
        QJsonValue replyId = requestId;
        bool numeric = false;
        const int asInt = requestId.toInt(&numeric);
        if (numeric)
            replyId = asInt;
        writeMessage(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), replyId},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("outcome"), QStringLiteral("selected")},
                {QStringLiteral("optionId"), QStringLiteral("reject-once")},
            }},
        });
        qWarning() << "permission request for a session this client does not hold:"
                   << sessionId;
        return;
    }

    m_pendingPermissions.insert(requestId, PendingPermission{sessionId, toolName, detail});
    session->pendingPermissionIds.append(requestId);

    // A question for the session in front is asked at once. One for a session
    // off screen is RETAINED, not dropped: it is still blocking that agent, so
    // the bar marks the session and the dialog is offered when the reader
    // switches to it (see reofferCurrentPermission). A modal over a transcript
    // the reader cannot see would be a question with no context.
    if (session == currentSession() && session->pendingPermissionIds.size() == 1) {
        // The first question for this session is the one on screen. A second
        // one, if an agent ever raises it, waits behind it rather than
        // overwriting the dialog the reader is already reading.
        emit permissionRequested(requestId, toolName, detail);
    }
    // The chip's amber dot follows this whether the session is in front or not:
    // it is the mark that a session is blocked on a question.
    touchSessions();
}

void AcpClient::respondToPermission(const QString &requestId, bool allow)
{
    const PendingPermission permission = m_pendingPermissions.take(requestId);
    SessionState *session = sessionFor(permission.sessionId);
    if (session) {
        session->pendingPermissionIds.removeAll(requestId);
        touchSessions();
    }

    QJsonValue idValue = requestId;
    bool numeric = false;
    const int asInt = requestId.toInt(&numeric);
    if (numeric)
        idValue = asInt;

    // ACP permission outcomes are a one-of: pick the option the agent offered
    // that matches our decision, falling back to the canonical optionId.
    const QString optionId = allow ? QStringLiteral("allow-once") : QStringLiteral("reject-once");

    QJsonObject outcome{
        {QStringLiteral("outcome"), QStringLiteral("selected")},
        {QStringLiteral("optionId"), optionId},
    };

    QJsonObject response{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), idValue},
        {QStringLiteral("result"), outcome},
    };
    writeMessage(response);

    if (!session) {
        // Answered after its session went away. The wire still gets a decision,
        // but there is no transcript left to record it in.
        qWarning() << "permission answered for a session this client no longer holds:"
                   << permission.sessionId;
        return;
    }

    session->transcript->append(QStringLiteral("notice"),
                                QStringLiteral("%1: %2")
                                    .arg(permission.toolName.isEmpty()
                                             ? QStringLiteral("permission")
                                             : permission.toolName,
                                         allow ? QStringLiteral("allowed")
                                               : QStringLiteral("rejected")));

    // If this session is in front and its agent asked again while the first
    // question was open, offer the next one; otherwise leave it for the switch.
    if (session == currentSession())
        reofferCurrentPermission();
}

void AcpClient::applySessionUpdate(SessionState *session, const QJsonObject &update)
{
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();

    // Every append below goes into THIS session's model. The row count of the
    // conversation on screen does not move because another session said
    // something, which is the whole point of routing by sessionId.
    if (kind == QLatin1String("agent_message_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        session->transcript->appendOrExtend(QStringLiteral("agent"),
                                            content.value(QStringLiteral("text")).toString());
        return;
    }

    if (kind == QLatin1String("agent_thought_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        session->transcript->appendOrExtend(QStringLiteral("thought"),
                                            content.value(QStringLiteral("text")).toString());
        return;
    }

    if (kind == QLatin1String("user_message_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        session->transcript->appendOrExtend(QStringLiteral("user"),
                                            content.value(QStringLiteral("text")).toString());
        return;
    }

    if (kind == QLatin1String("usage_update")) {
        // Token accounting arrives for free on every turn. Kept as its own
        // property rather than folded into the transcript: it is live state
        // that outlives any single message row. Per session, because two
        // sessions have two context windows.
        session->contextUsed = update.value(QStringLiteral("used")).toInt();
        session->contextSize = update.value(QStringLiteral("size")).toInt();
        if (session == currentSession())
            emit contextChanged();
        // Deliberately no setStatus here. It used to write "context N / M" into
        // the status line on every pressure update, which is where the header's
        // status sits — so the one line that reports what the client is doing
        // ("thinking…", "connected to …", an upload failing) was overwritten by a
        // number the header already shows, and last updated at that.
        return;
    }

    if (kind == QLatin1String("tool_call") || kind == QLatin1String("tool_call_update")) {
        const QString callId = update.value(QStringLiteral("toolCallId")).toString();
        const QString status = update.value(QStringLiteral("status")).toString();

        // Both kinds describe one call, but they carry different halves of it:
        // the opening frame the arguments, the closing one the verdict. So the
        // session-level consequences are captured here and committed there.
        if (kind == QLatin1String("tool_call_update"))
            applyToolCallResult(session, callId, status, update);

        const QString title = update.value(QStringLiteral("title")).toString().isEmpty()
            ? update.value(QStringLiteral("kind")).toString()
            : update.value(QStringLiteral("title")).toString();
        // ACP carries a call's arguments in `rawInput`. The card's IN half is the
        // REQUEST, and the journal path fills it with the whole decoded argument
        // object; leaving this empty would render the same call differently
        // depending on the transport, which is the one divergence this file exists
        // to prevent. Bounded at the same 8 KB, marked when cut.
        QString request;
        const QJsonValue rawInput = update.value(QStringLiteral("rawInput"));
        if (rawInput.isObject()) {
            request = QString::fromUtf8(
                          QJsonDocument(rawInput.toObject()).toJson(QJsonDocument::Indented))
                          .trimmed();
            static constexpr int RequestCap = 8 * 1024;
            if (request.size() > RequestCap)
                request = request.left(RequestCap) + QChar(0x2026);
        }

        if (kind == QLatin1String("tool_call") && !callId.isEmpty()) {
            // The whole todo list reaches this transport as the arguments of the
            // `todo_write` call: DSH's ACP bridge emits no `plan` update, and the
            // tool's schema is the complete list replacing any previous one. Held
            // by call id until the call finishes, because the tool validates the
            // list first and a rejected call never became session state.
            const QJsonValue todos = rawInput.toObject().value(QStringLiteral("todos"));
            if (title == QLatin1String("todo_write") && todos.isArray())
                session->pendingTodos.insert(callId, todosFromArray(todos.toArray()));

            // No workspace-diff stream exists under ACP, so the files a session
            // touched are derived from the calls that name one. Held until the
            // call completes for the same reason: a failed write changed nothing.
            const QStringList paths = pathsModifiedBy(update);
            if (!paths.isEmpty())
                session->pendingWritePaths.insert(callId, paths);

            if (title == QLatin1String("get_goal") || title == QLatin1String("create_goal")
                || title == QLatin1String("update_goal"))
                session->goalCalls.insert(callId);
        }

        session->transcript->append(QStringLiteral("tool"), QString(), title, status,
                                    QString(), QString(), 0, -1, request);
        return;
    }

    if (kind == QLatin1String("plan")) {
        // ACP's plan is the todo list: every update carries the complete list and
        // replaces the previous one (see PlanEntry in the ACP schema). `priority`
        // is dropped — the dock has no slot for it and DshClient's shape does not
        // carry it, so a row cannot differ by transport.
        const QJsonValue entries = update.value(QStringLiteral("entries"));
        if (entries.isArray())
            session->todos = todosFromArray(entries.toArray());
        session->transcript->append(QStringLiteral("notice"),
                                    QStringLiteral("plan updated"));
        // The dock only shows the session in front; an inactive one keeps its
        // list until it is switched to.
        if (session == currentSession())
            syncWidgetState();
        return;
    }

    // Unknown kinds are surfaced, not swallowed: a silent drop here would look
    // exactly like an agent that stopped talking.
    qInfo() << "unhandled session update kind:" << kind;
}

QStringList AcpClient::pathsModifiedBy(const QJsonObject &call)
{
    QStringList paths;

    // A standard ACP agent says which files a call affects in `locations`, and
    // only the modifying kinds mean "changed": `kind:"read"` carries locations
    // too, which is why the kind is required here and not just the list.
    const QString kind = call.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("edit") || kind == QLatin1String("delete")) {
        for (const QJsonValue &entry : call.value(QStringLiteral("locations")).toArray()) {
            const QString path = entry.toObject().value(QStringLiteral("path")).toString();
            if (!path.isEmpty())
                paths.append(path);
        }
    }

    // DSH's ACP bridge sends `kind:"other"` for every tool, the harness's own
    // tool name in `title`, and the arguments in `rawInput`. `write` and `edit`
    // are the two that modify a workspace file. `bash` can too, but a path inside
    // a shell command is not something this side may guess at, and a rendered
    // title ("Edit src/x.cpp") is another agent's wording, not a field.
    const QString title = call.value(QStringLiteral("title")).toString();
    if (title == QLatin1String("write") || title == QLatin1String("edit")) {
        const QString path = call.value(QStringLiteral("rawInput")).toObject()
                                 .value(QStringLiteral("file_path")).toString();
        if (!path.isEmpty())
            paths.append(path);
    }

    paths.removeDuplicates();
    return paths;
}

void AcpClient::applyToolCallResult(SessionState *session, const QString &callId,
                                    const QString &status, const QJsonObject &update)
{
    if (!session || callId.isEmpty())
        return;

    // An agent may stream intermediate statuses. Nothing is committed until the
    // call reaches a terminal one: before that the arguments are an intention,
    // not something the session did.
    const bool completed = status == QLatin1String("completed");
    if (!completed && status != QLatin1String("failed"))
        return;

    if (session->pendingTodos.contains(callId)) {
        const QVariantList todos = session->pendingTodos.take(callId);
        // A failed call leaves the previous list in force: `todo_write` validates
        // the whole list before appending anything, so a rejection changed nothing.
        if (completed)
            session->todos = todos;
    }

    if (session->pendingWritePaths.contains(callId)) {
        const QStringList paths = session->pendingWritePaths.take(callId);
        if (completed) {
            for (const QString &raw : paths) {
                // Against THIS session's workspace, not a client-wide one: the
                // client holds several sessions now, and a relative file_path
                // resolves against the cwd its own session/new named.
                const QString path = workspacePath(session->cwd, raw);
                if (path.isEmpty() || session->changedFiles.contains(path))
                    continue;
                session->changedFiles.append(path);
            }
        }
    }

    if (session->goalCalls.remove(callId) && completed)
        applyGoalResult(session, update);

    // The dock mirrors the session in front; an inactive session's list is kept
    // on its SessionState and published when it is switched to.
    if (session == currentSession())
        syncWidgetState();
}

void AcpClient::applyGoalResult(SessionState *session, const QJsonObject &update)
{
    // ACP has no goal projection: no update carries the phase, revision or round
    // count, which is why the goal row could never appear under ACP. The one frame
    // that does carry them is the result of `get_goal` / `create_goal` /
    // `update_goal`, whose render is the goal value as JSON text (`get_goal` on a
    // session with no goal answers `{"goal":null}`). Decoded only when it parses to
    // that shape with an id and a phase, so a reworded, truncated or pruned result
    // leaves the property as it was instead of half-filling the row.
    QJsonObject value;
    for (const QJsonValue &entry : update.value(QStringLiteral("content")).toArray()) {
        const QJsonObject block = entry.toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("content"))
            continue;
        const QString text =
            block.value(QStringLiteral("content")).toObject()
                 .value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        if (doc.isObject() && doc.object().contains(QStringLiteral("goal"))) {
            value = doc.object();
            break;
        }
    }
    if (value.isEmpty())
        return;

    QVariantMap goal;
    const QJsonObject snapshot = value.value(QStringLiteral("goal")).toObject();
    if (!snapshot.isEmpty()) {
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        const QString phase = snapshot.value(QStringLiteral("phase")).toString();
        if (id.isEmpty() || phase.isEmpty())
            return;
        // The same six fields DshClient publishes: the QML reads `phase`,
        // `objective`, `roundsStarted` and `maxRounds`, and `id`/`revision` are
        // what a lifecycle change is addressed to. `maxGoalRounds` is the goal
        // tool's own spelling of `maxRounds`.
        goal.insert(QStringLiteral("objective"),
                    snapshot.value(QStringLiteral("objective")).toString());
        goal.insert(QStringLiteral("phase"), phase);
        goal.insert(QStringLiteral("id"), id);
        goal.insert(QStringLiteral("revision"), snapshot.value(QStringLiteral("revision")).toInt());
        goal.insert(QStringLiteral("roundsStarted"),
                    snapshot.value(QStringLiteral("roundsStarted")).toInt());
        goal.insert(QStringLiteral("maxRounds"),
                    snapshot.value(QStringLiteral("maxGoalRounds")).toInt());
    }
    // An empty map is the tools' "no goal" answer, and the only way this transport
    // learns a goal was cleared. The row's own buttons cannot do it — see
    // goalAction.
    session->goal = goal;
    if (session == currentSession())
        syncWidgetState();
}

void AcpClient::clearChangedFiles()
{
    // The dock's × empties the session in front; the other sessions keep their
    // own change lists, because each list is a fact about its own workspace.
    SessionState *session = currentSession();
    if (session && !session->changedFiles.isEmpty()) {
        session->changedFiles.clear();
        syncWidgetState();
        return;
    }
    if (m_changedFiles.isEmpty())
        return;
    m_changedFiles.clear();
    emit changedFilesChanged();
}

void AcpClient::goalAction(const QString &action)
{
    // The goal row renders from the agent's own goal tools, but ACP has no request
    // that changes a goal: only the agent's tools do, and the agent is not
    // listening to a client button mid-turn. Saying so is the honest outcome — the
    // one thing it must not do is look like it worked.
    const QString message = QStringLiteral("the ACP transport cannot %1 a goal").arg(action);
    SessionState *session = currentSession();
    if (session)
        setSessionStatus(session, message);
    else
        setStatus(message);
}

void AcpClient::refreshConfigOptions(SessionState *session, const QJsonArray &options)
{
    if (!session)
        return;
    QVariantList flattened;

    for (const QJsonValue &entry : options) {
        const QJsonObject option = entry.toObject();
        QVariantList choices;

        // The wire groups options under `options: [{group, name, options:[]}]`.
        // Flattening here keeps that shape out of the QML.
        //
        // `provider` is empty on every choice: ACP has config-option groups, not
        // LLM providers, and its `group` name is the option's own heading rather
        // than a provider. The model picker sections by `provider` only, so an
        // empty one is what leaves this transport's list flat and unsectioned —
        // and it is written out rather than left absent so both transports
        // advertise the same keys.
        for (const QJsonValue &groupValue : option.value(QStringLiteral("options")).toArray()) {
            const QJsonObject group = groupValue.toObject();
            const QString groupName = group.value(QStringLiteral("name")).toString();

            if (group.contains(QStringLiteral("options"))) {
                for (const QJsonValue &choiceValue : group.value(QStringLiteral("options")).toArray()) {
                    const QJsonObject choice = choiceValue.toObject();
                    choices.append(QVariantMap{
                        {QStringLiteral("value"), choice.value(QStringLiteral("value")).toString()},
                        {QStringLiteral("label"), choice.value(QStringLiteral("name")).toString()},
                        {QStringLiteral("group"), groupName},
                        {QStringLiteral("provider"), QString()},
                        {QStringLiteral("description"), choice.value(QStringLiteral("description")).toString()},
                    });
                }
            } else {
                choices.append(QVariantMap{
                    {QStringLiteral("value"), group.value(QStringLiteral("value")).toString()},
                    {QStringLiteral("label"), groupName},
                    {QStringLiteral("group"), QString()},
                    {QStringLiteral("provider"), QString()},
                    {QStringLiteral("description"), group.value(QStringLiteral("description")).toString()},
                });
            }
        }

        flattened.append(QVariantMap{
            {QStringLiteral("id"), option.value(QStringLiteral("id")).toString()},
            {QStringLiteral("name"), option.value(QStringLiteral("name")).toString()},
            {QStringLiteral("category"), option.value(QStringLiteral("category")).toString()},
            {QStringLiteral("currentValue"), option.value(QStringLiteral("currentValue")).toString()},
            {QStringLiteral("choices"), choices},
        });
    }

    session->configOptions = flattened;
    // Only the session in front feeds the model chip; the others' lists are
    // held on their SessionState and published when they are switched to.
    if (session == currentSession())
        emit configOptionsChanged();
}

// --- commands --------------------------------------------------------------

void AcpClient::createSession()
{
    if (m_state != Ready) {
        setStatus(QStringLiteral("not ready yet"));
        return;
    }

    // `session/new` validates its workspace: `dsh-acp`'s validateWorkspaceParams
    // rejects a `cwd` that is not an absolute path with `-32602`, and the agent
    // it publishes is anchored on that directory. The default is QDir::homePath()
    // or $DSH_HOME, which are absolute, but `--workspace` is taken verbatim from
    // the command line — a relative one would fail here with a message about the
    // protocol rather than about the argument. Resolved once, at the point of
    // use, so what the child receives is what the log says it received.
    if (m_workspace.isEmpty())
        m_workspace = QDir::homePath();
    if (!QFileInfo(m_workspace).isAbsolute())
        m_workspace = QFileInfo(m_workspace).absoluteFilePath();
    if (!QFileInfo(m_workspace).isDir()) {
        // Not fatal here: the child is the authority and its refusal names the
        // path. Saying it first puts the path in the client's own log beside the
        // spawn, which is where a reader looks.
        qWarning() << "ACP session workspace is not a directory:" << m_workspace;
    }

    QJsonObject params{
        {QStringLiteral("cwd"), m_workspace},
        {QStringLiteral("mcpServers"), QJsonArray{}},
    };
    // The status line belongs to whichever session is in front once one exists,
    // so a `+` pressed beside a running session must not overwrite that
    // session's "thinking…". Before the first session there is nothing else to
    // show, and the line is the only sign the handshake is moving.
    if (!currentSession())
        setStatus(QStringLiteral("creating session…"));
    const int id = beginRequest(QStringLiteral("session/new"), params);
    qInfo() << "ACP handshake: sent session/new (id" << id << ") cwd" << m_workspace;
}

void AcpClient::switchToSession(const QString &id)
{
    SessionState *session = sessionFor(id);
    if (!session) {
        qWarning() << "switch to a session this client does not hold:" << id;
        return;
    }
    if (id == m_currentSessionId)
        return;

    // No request of any kind goes on the wire here. ACP has no transcript
    // replay: the rows a session accumulated are the rows this client captured
    // from `session/update`, including while the session was off screen. They
    // are already in that session's model, and those rows are all there will
    // ever be — so switching back shows them and asks the child for nothing.
    setCurrentSession(id);

    // A permission question that arrived while this session was off screen is
    // still blocking its agent. Now that it has a transcript on screen, offer
    // it. The pane has already closed any dialog belonging to the session being
    // left (its request stays pending and is offered again when it returns).
    reofferCurrentPermission();
    touchSessions();
}

void AcpClient::closeSession(const QString &id)
{
    SessionState *session = sessionFor(id);
    if (!session)
        return;

    // session/close is addressed to one session and ends only it; the other
    // sessions on this child keep running. The chip is removed eagerly rather
    // than left dead until the answer: a close that takes a moment is still a
    // close the reader asked for.
    QJsonObject params{{QStringLiteral("sessionId"), id}};
    beginRequest(QStringLiteral("session/close"), params, id);

    // Questions this session's agent was waiting on can no longer be answered.
    for (const QString &requestId : session->pendingPermissionIds)
        m_pendingPermissions.remove(requestId);
    if (!session->pendingPermissionIds.isEmpty()) {
        const QStringList requestIds = session->pendingPermissionIds;
        session->pendingPermissionIds.clear();
        for (const QString &requestId : requestIds)
            emit permissionWithdrawn(requestId);
    }

    const int index = static_cast<int>(m_sessionOrder.indexOf(id));
    if (index >= 0)
        m_sessionOrder.removeAt(index);
    m_sessionsById.remove(id);
    if (session->transcript)
        session->transcript->deleteLater();
    delete session;

    // If the closed session was the one in front, the next one takes its place:
    // the session after it, or the one before when it was last. Closing the last
    // session leaves the child RUNNING with no session in front — the bar shows
    // none and `+` creates another. Stopping the child here would make the bar's
    // × a second way out of the pane, and the two exits are the pane's close and
    // Escape.
    if (m_currentSessionId == id) {
        const int last = static_cast<int>(m_sessionOrder.size()) - 1;
        const int nextIndex = m_sessionOrder.isEmpty() ? -1
            : qBound(0, index, last);
        setCurrentSession(nextIndex < 0 ? QString() : m_sessionOrder.at(nextIndex));
    }

    // With no session left, status() falls back to the child's own line; without
    // this it would still say "creating session…" from the handshake that made
    // the session just closed.
    if (m_sessionOrder.isEmpty())
        setStatus(QStringLiteral("ready"));

    touchSessions();
}

void AcpClient::reofferCurrentPermission()
{
    SessionState *session = currentSession();
    if (!session || session->pendingPermissionIds.isEmpty())
        return;
    const QString requestId = session->pendingPermissionIds.first();
    const PendingPermission permission = m_pendingPermissions.value(requestId);
    emit permissionRequested(requestId, permission.toolName, permission.detail);
}

void AcpClient::sendPrompt(const QString &text)
{
    if (m_state != Ready) {
        setStatus(QStringLiteral("not ready yet"));
        return;
    }
    SessionState *session = currentSession();
    if (!session) {
        setStatus(QStringLiteral("no session to prompt"));
        return;
    }
    if (text.trimmed().isEmpty())
        return;
    // One prompt in flight per session, which is what the protocol allows. A
    // second send to the SAME session is refused rather than queued: ACP has no
    // queue on this wire, and the protocol says the slot is single. Another
    // session's turn may still be running — that is a different slot.
    if (session->busy) {
        setSessionStatus(session,
                         QStringLiteral("one prompt is already running in this session"));
        return;
    }

    session->transcript->append(QStringLiteral("user"), text);
    // The bar's label is the first prompt's first line. ACP carries no title of
    // its own, and a chip that only ever shows an id names nothing.
    if (!session->titled) {
        QString title = text.simplified();
        if (title.size() > 40)
            title = title.left(39) + QChar(0x2026);
        if (!title.isEmpty()) {
            session->title = title;
            session->titled = true;
            touchSessions();
        }
    }

    QJsonObject block{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), text},
    };
    QJsonObject params{
        {QStringLiteral("sessionId"), session->id},
        {QStringLiteral("prompt"), QJsonArray{block}},
    };

    setSessionBusy(session, true);
    setSessionStatus(session, QStringLiteral("thinking…"));
    beginRequest(QStringLiteral("session/prompt"), params, session->id);
}

void AcpClient::cancel()
{
    SessionState *session = currentSession();
    if (!session)
        return;
    // `session/cancel` is a NOTIFICATION in ACP (the server registers it under
    // onNotification and answers nothing). It used to be sent through
    // beginRequest(), so the server returned method-not-found for a method it
    // handles, handleResponse() saw an error, and a cancel could mark a healthy
    // child Failed.
    QJsonObject params{{QStringLiteral("sessionId"), session->id}};
    writeMessage(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("session/cancel")},
        {QStringLiteral("params"), params},
    });
    setSessionStatus(session, QStringLiteral("cancelling…"));
}

void AcpClient::runCommand(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    // Nothing here can run it: `commands/execute` belongs to the host transport,
    // and ACP has no equivalent. Silence would be the worst answer, because the
    // composer has already taken the line out of the draft — so the refusal is a
    // row in the transcript and a word in the status, the same shape a host
    // refusal takes on the other transport.
    const QString message =
        QStringLiteral("%1 needs the DSH host: the ACP transport cannot run commands")
            .arg(trimmed.section(QLatin1Char(' '), 0, 0));
    SessionState *session = currentSession();
    if (session) {
        session->transcript->append(QStringLiteral("error"), message);
        setSessionStatus(session, message);
    } else {
        setStatus(message);
    }
}

void AcpClient::setConfigOption(const QString &configId, const QString &value)
{
    SessionState *session = currentSession();
    if (!session) {
        setStatus(QStringLiteral("no session yet"));
        return;
    }
    QJsonObject params{
        {QStringLiteral("sessionId"), session->id},
        {QStringLiteral("configId"), configId},
        {QStringLiteral("value"), value},
    };
    beginRequest(QStringLiteral("session/set_config_option"), params, session->id);
}
