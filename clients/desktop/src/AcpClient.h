#pragma once

// AcpClient — the ACP (Agent Client Protocol) transport.
//
// ACP is JSON-RPC 2.0 over newline-delimited JSON on the server's stdio, so the
// whole client is: spawn a child, write one JSON object per line to its stdin,
// read one JSON object per line from its stdout. There is no HTTP, no WebSocket,
// no cookie and no browser anywhere in this process tree.
//
// Design notes that matter:
//
//  * The engine owns this object as a QML singleton instance, NOT as a QML
//    type instantiated by the UI. That is deliberate: when the QML tree is
//    rebuilt on a hot reload, the ACP connection, the session and its
//    transcript all survive, because they live here rather than in QML.
//
//  * Transcript entries are exposed as a QAbstractListModel rather than a
//    QVariantList. Appending one message is then an incremental model update;
//    a QVariantList would re-send the entire conversation to the view on every
//    streaming chunk.
//
//  * One child runs SEVERAL sessions. `dsh-acp`'s protocol contract says so
//    plainly: one connection runs several independent sessions, `session/new`
//    makes another, `session/close` ends one by id, and each session allows one
//    prompt in flight. The client therefore keeps a map of id -> per-session
//    state and routes every `session/update` by its `params.sessionId`. The
//    properties the UI has always read (`transcript`, `sessionId`, `busy`,
//    `todos`, ...) now describe the CURRENT session; `openSessions` is the bar's
//    roster. The old one-session model is not kept as a fallback — it was the
//    bug: two live sessions interleaved into one transcript and a permission
//    prompt could be attributed to the wrong conversation.

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "AcpCompat.h"

/// One selectable option advertised by the agent (model, reasoning effort).
struct AcpConfigOption
{
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString category MEMBER category)
    Q_PROPERTY(QString currentValue MEMBER currentValue)
    Q_PROPERTY(QVariantList choices MEMBER choices)
public:
    QString id;
    QString name;
    QString category;
    QString currentValue;
    /// Flat {value, label, group, description} records, already flattened from
    /// the wire's grouped structure so the UI does not have to walk groups.
    QVariantList choices;
};

/// The transcript: one entry per visible row.
class AcpTranscript : public QAbstractListModel
{
    Q_OBJECT

    /// The turn being written and whether it is still open.
    ///
    /// Read by the transcript delegate as `model.activeTurn` / `model.turnOpen`.
    /// They were plain C++ getters with no Q_PROPERTY, so those two reads were
    /// `undefined` in QML: `turnFoldable` was always false and no completed turn
    /// ever folded. The fold has been dead in both containers since it was
    /// written. Declared here, with the signal noteTurn already emits, so the
    /// binding follows the state it is about.
    Q_PROPERTY(int activeTurn READ activeTurn NOTIFY turnStateChanged)
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnStateChanged)

    /// Epoch ms of the `turn/start` event that opened the live turn, or 0 when
    /// the transport has not named one.
    ///
    /// This is the clock the running line counts from: the web client's own
    /// `Deep diving for Ns` is a TURN clock, not an attempt clock. It cannot be
    /// derived in the view, because a row's `time` is the attempt's — re-stamped
    /// when a new attempt starts, and again when a reconnect replays the journal
    /// — which is precisely why that label used to restart. A turn begins once,
    /// so this is kept on the transcript rather than on any row, and 0 says "not
    /// known", leaving the view to show no number rather than a guessed one.
    Q_PROPERTY(qint64 turnStartTime READ turnStartTime NOTIFY turnStateChanged)

public:
    enum Role {
        KindRole = Qt::UserRole + 1,  ///< user | agent | thought | tool | error | notice
        TextRole,
        TitleRole,
        StatusRole,                   ///< for tool rows: pending | completed | failed
        DetailRole,                   ///< the row's payload: a tool's RESULT
        /// The call's REQUEST, decoded: the tool's arguments as readable JSON,
        /// or empty when the row has none.
        ///
        /// A field of its own rather than a second meaning for `detail`. The
        /// tool card shows IN and OUT side by side: IN was bound to `text`, the
        /// one-line summary, so a bash row put its own description in IN and the
        /// request was nowhere on screen; and `detail` cannot carry both, because
        /// it is the result — a call that has not returned yet would then show
        /// its arguments under OUT.
        RequestRole,
        TimeRole,                     ///< epoch ms of the event that opened the row, 0 if none
        TurnRole,                     ///< the journal turn the row belongs to, -1 if none
        TurnLeadRole,                 ///< true on the first process row of its turn
        /// The WHOLE text a row stands in for, when the row itself holds only a
        /// short preview of it. Empty on every row whose `text`/`detail` is
        /// already the whole thing.
        ///
        /// A staged prompt is the case it exists for: its `detail` is a bounded
        /// one-line preview because the row is a delivery *state*, not the
        /// message, and clipping the model's copy of what the reader typed would
        /// leave the view with nothing longer to copy. The preview stays in
        /// `detail`; this is what the reader typed.
        FullTextRole,
        /// The row's stable identity, or empty when the journal gives it none: a
        /// tool call carries its `callId`, so a later frame can address the same
        /// row even after earlier history has been prepended and every index has
        /// moved; an ordinary message has no such id.
        ///
        /// Published for the view: an open row is keyed by identity, because a
        /// prepend renumbers every row below it and an index-keyed set would open
        /// whichever row inherited the index. See `rowId` in qml/main.qml.
        KeyRole,
    };
    Q_ENUM(Role)

    explicit AcpTranscript(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Append a row and return its index.
    ///
    /// `key` is the identity a later frame can address this row by — a tool
    /// call's `callId`. Without one, a tool result has nowhere to go but a second
    /// row, which is how a Bash call came to be listed twice with nothing in it.
    ///
    /// `request` is the call's decoded arguments, kept apart from `detail` (the
    /// result). It is last but one so every existing call site keeps its
    /// positional meaning; `fullText` is appended after it for the same reason.
    ///
    /// `fullText` is the whole text a row only previews — see FullTextRole. It
    /// is empty for every ordinary row, and it is never what the row draws.
    int append(const QString &kind, const QString &text,
               const QString &title = QString(),
               const QString &status = QString(),
               const QString &detail = QString(),
               const QString &key = QString(),
               qint64 time = 0,
               int turn = -1,
               const QString &request = QString(),
               const QString &fullText = QString());

    /// Address a row by `key` and rewrite its status/detail/text in place.
    /// Returns false when no row carries that key, which is not an error: a
    /// result can arrive for a call that was folded into an earlier page.
    bool updateByKey(const QString &key, const QString &status,
                     const QString &detail = QString(),
                     const QString &text = QString());

    bool removeRow(int row);

    /// Told by the transport which journal turn is being written, whether it is
    /// still open, and — when this transport knows it — the epoch ms of the
    /// `turn/start` event that opened it. A turn that has ended is foldable; the
    /// live one is not, because folding it would hide the work while it happens.
    ///
    /// `startTime` defaults to 0, which is also what a closed turn carries: the
    /// caller that opens a turn passes the boundary's own `time`, and every
    /// caller that closes one leaves the clock unset rather than letting a stale
    /// start outlive its turn.
    void noteTurn(int turn, bool open, qint64 startTime = 0);

    int activeTurn() const { return m_activeTurn; }
    bool turnOpen() const { return m_turnOpen; }
    qint64 turnStartTime() const { return m_turnStartTime; }

    /// What sits inside one turn, for the folded row's summary:
    /// `{tools, messages, thoughts}`.
    ///
    /// A row whose status is `withdrawn` is not counted: it draws nothing, so
    /// counting it would have a folded turn claim work that is nowhere on
    /// screen. That is the same property the delegate reads, not a parallel one.
    Q_INVOKABLE QVariantMap turnSummary(int turn) const;

    /// Append to the last row when it is `kind`, else start a new row. This is
    /// what turns a stream of agent_message_chunk frames into one message.
    /// `time` is kept from the row's first chunk, so a message that streams for a
    /// minute is stamped when it began rather than when it finished.
    void appendOrExtend(const QString &kind, const QString &text, qint64 time = 0,
                        int turn = -1);

    /// Insert a row *above* the existing ones and return its index — the mirror
    /// of append().
    ///
    /// This is the only incremental way to reveal earlier history. Rebuilding the
    /// model would show the same rows but throw away the view's scroll position,
    /// which is precisely what a reader paging backwards is holding on to.
    int prepend(const QString &kind, const QString &text,
                const QString &title = QString(),
                const QString &status = QString(),
                const QString &detail = QString(),
                const QString &request = QString());

    /// Extend the *first* row when it is `kind`, else insert a new row above.
    /// The mirror of appendOrExtend(): a page is translated oldest-first, so a
    /// page's later chunks extend the row its earlier ones opened.
    void prependOrExtend(const QString &kind, const QString &text);

    /// Replace the text of the last row when it is `kind`. Used for thought
    /// streams, which arrive as a full replacement rather than a delta.
    void replaceLast(const QString &kind, const QString &text);

    void clear();

signals:
    /// The live turn opened or closed. A folded view unfolds the turn being
    /// written and folds the one that just ended, and this is what tells it to.
    void turnStateChanged();

private:
    struct Row {
        QString kind;
        QString text;
        QString title;
        QString status;
        QString detail;
        /// The call's decoded arguments — what was ASKED — for the tool card's
        /// IN half. `detail` is what came back. Empty on rows that are not calls
        /// and on a call whose arguments the transport never carried.
        QString request;
        /// Empty for rows nothing ever updates. Not an index: rows are prepended
        /// when earlier history is folded in, which moves every index below them.
        QString key;
        /// Epoch milliseconds of the event that opened this row. Zero when the
        /// transport has no time to give — the ACP path carries none.
        qint64 time = 0;
        /// The journal turn this row belongs to, or -1 when the transport does not
        /// number turns (the ACP path). A turn is what folds.
        int turn = -1;
        /// True on the first *process* row of its turn — the row that stands in for
        /// the whole folded turn when the reader has not opened it.
        bool turnLead = false;
        /// The whole text a row only previews, or empty. See FullTextRole. Kept
        /// last because it is the one field no existing row has ever set.
        QString fullText;
    };
    QList<Row> m_rows;
    /// The highest turn seen, and whether it is still being written.
    int m_activeTurn = -1;
    bool m_turnOpen = false;
    /// Epoch ms of the open turn's `turn/start`, or 0 when it is not known.
    /// A fact about the turn rather than about a row: every attempt in a turn
    /// gets a fresh row, and a re-follow replaces all of them, but the turn
    /// began once.
    qint64 m_turnStartTime = 0;
    /// Rebuilt on demand; every mutation marks it stale. Cheaper than recomputing
    /// a per-turn scan for every visible folded row on every frame.
    mutable QHash<int, QVariantMap> m_turnCounts;
    mutable bool m_turnCountsStale = true;
};

class AcpClient : public AcpCompat
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    /// The CURRENT session's turn state. `busy` is per session, because the
    /// protocol allows one prompt in flight in each: a turn running in a session
    /// the reader is not looking at must not make this one's send button say
    /// "stop". Switching sessions re-notifies it.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// The current session's transcript model, or null when no session is open.
    /// NOTIFY rather than CONSTANT: each session owns its own model, and
    /// switching swaps which one the ListView is bound to. The rows an inactive
    /// session collected stay in ITS model, untouched.
    Q_PROPERTY(AcpTranscript *transcript READ transcript NOTIFY transcriptChanged)
    /// The bar's roster, in creation order: one map per open session with
    /// `id`, `title`, `cwd`, `cwdName`, `busy` and `awaitingPermission`.
    ///
    /// Deliberately on AcpClient and not on AcpCompat: this is the PANE's state,
    /// reached as `dshAcpPane.openSessions`, not a fact about a transport. The
    /// window's `dshTransport` must not grow a property for a bar only one pane
    /// can draw.
    Q_PROPERTY(QVariantList openSessions READ openSessions NOTIFY openSessionsChanged)
    Q_PROPERTY(QVariantList configOptions READ configOptions NOTIFY configOptionsChanged)
    Q_PROPERTY(QString agentInfo READ agentInfo NOTIFY agentInfoChanged)
    Q_PROPERTY(int contextUsed READ contextUsed NOTIFY contextChanged)
    Q_PROPERTY(int contextSize READ contextSize NOTIFY contextChanged)

public:
    enum State { Idle, Starting, Ready, Failed };
    Q_ENUM(State)

    explicit AcpClient(QObject *parent = nullptr);
    ~AcpClient() override;

    State state() const { return m_state; }
    /// The CURRENT session's id, or empty when no session is open.
    QString sessionId() const { return m_currentSessionId; }
    /// The current session's status line, falling back to the child's own while
    /// no session exists (starting, handshaking, failed). One line for a child
    /// that now has several sessions: the bar says which one is which, this says
    /// what the one in front is doing.
    QString status() const;
    QString lastError() const { return m_lastError; }
    bool busy() const;
    QString agentInfo() const { return m_agentInfo; }
    int contextUsed() const;
    int contextSize() const;
    QVariantList configOptions() const;
    AcpTranscript *transcript() const { return m_transcript; }
    QVariantList openSessions() const;

    /// The facade's turn latch (see AcpCompat), answered from the current
    /// transcript's own. ACP records its turn boundaries with
    /// AcpTranscript::noteTurn(), so reading that is the one source of truth; a
    /// second flag here could disagree with the transcript the reader is looking
    /// at.
    bool turnOpen() const override { return m_transcript && m_transcript->turnOpen(); }

    /// The command used to start the ACP server. Overridable so a profile,
    /// a debug build or a different harness checkout can be pointed at without
    /// recompiling.
    Q_INVOKABLE void setLaunchCommand(const QString &program, const QStringList &arguments);

    /// Start the ACP server process and run the handshake.
    Q_INVOKABLE void start();

    /// Stop whatever child is running, forget EVERY session it was holding, and
    /// start again.
    ///
    /// This is no longer "another conversation" — that is createSession(), which
    /// adds one to the same child. This is the pane's restart: a fresh process
    /// with no sessions, used when the child has failed or the pane is opened
    /// after every exit. The old child is STOPPED first rather than left running
    /// beside the new one, because a second `dsh` process holding sessions
    /// nothing can reach is exactly the orphan the exit exists to prevent.
    Q_INVOKABLE void restart();

    /// The workspace every session on this child is created in. Must be set
    /// before createSession().
    Q_INVOKABLE void setWorkspace(const QString &path);

    /// session/new — create a fresh agent session on the SAME child and switch
    /// to it. The bar's `+`. Refused only when the child is not ready.
    Q_INVOKABLE void createSession();

    /// Switch the pane to `id` — no wire traffic at all.
    ///
    /// ACP has no transcript replay: a session's history is only ever what the
    /// client captured from `session/update`, and this client captures it into
    /// that session's own model while the session is off screen. So switching
    /// back re-requests nothing; the rows are already in the model, and those
    /// rows are all there will ever be. Reconnecting, on the other hand, loses
    /// whatever happened while the connection was down — no protocol message
    /// carries it.
    Q_INVOKABLE void switchToSession(const QString &id);

    /// session/close — end `id` on the child and drop its rows from the bar.
    ///
    /// Closing the LAST session does NOT stop the child: the bar's × is a
    /// session action, and the two ways out of the pane (its close button and
    /// Escape) are what stop it. A child with no sessions is still a live
    /// connection, and `+` creates a session on it without a fresh handshake.
    Q_INVOKABLE void closeSession(const QString &id);

    /// Re-emit the current session's pending permission request, if it has one.
    ///
    /// A prompt raised for a session that is not on screen is retained rather
    /// than dropped (it is still blocking that agent), but its dialog must not
    /// appear over a hidden pane. The pane calls this when it is shown again or
    /// switched to, so the question is offered exactly when it can be answered.
    Q_INVOKABLE void reofferCurrentPermission();

    /// session/prompt — send a user turn to the CURRENT session. One prompt in
    /// flight per session, as the protocol allows; a second send to the same
    /// session is refused with a status, never queued.
    Q_INVOKABLE void sendPrompt(const QString &text);

    /// session/cancel — stop the current session's turn. A NOTIFICATION on the
    /// wire, not a request: the server registers it under onNotification, so
    /// sending it with an id got a method-not-found error back and could mark a
    /// healthy child Failed.
    Q_INVOKABLE void cancel();

    /// session/set_config_option — change model or reasoning effort on the
    /// CURRENT session. The option list is per session; another session keeps
    /// its own.
    Q_INVOKABLE void setConfigOption(const QString &configId, const QString &value);

    /// Answer a permission request the agent raised. The request's own session
    /// gets the notice row, which is not necessarily the one on screen: the
    /// dialog can only be answered while that session is in front, but the
    /// attribution is by request id, not by what is visible.
    Q_INVOKABLE void respondToPermission(const QString &requestId, bool allow);

    /// ACP has no `commands/execute`; the DSH host's command vocabulary is not
    /// part of this protocol. The composer routes a typed `/command` here
    /// anyway, so the refusal has to be visible: a slash line that quietly did
    /// nothing would look exactly like one the agent received.
    void runCommand(const QString &line) override;

    /// Empty the derived changed-files list — the dock row's ×. See AcpCompat.
    Q_INVOKABLE void clearChangedFiles() override;

    /// The goal row's Pause/Resume/×: ACP has no goal request, so this says the
    /// transport cannot do it rather than returning silently.
    Q_INVOKABLE void goalAction(const QString &action) override;

    /// Stop the child process and forget every session it held. This is the
    /// pane-close exit, not the bar's session ×.
    Q_INVOKABLE void shutdown();

    /// The CURRENT session's transcript as plain text. Used by the headless path
    /// and by debugging; the UI reads the model instead.
    Q_INVOKABLE QString transcriptAsText() const;

signals:
    void stateChanged();
    /// The current session's id changed: a switch, a create, a close, a reset.
    void sessionChanged();
    /// The current transcript model changed — see the property. Switching is
    /// why this is not CONSTANT.
    void transcriptChanged();
    /// A session was added, removed, retitled, started or finished a turn, or
    /// began waiting for permission: the bar's roster needs redrawing.
    void openSessionsChanged();
    void statusChanged();
    void lastErrorChanged();
    void busyChanged();
    void configOptionsChanged();
    void agentInfoChanged();
    void contextChanged();

    /// The agent raised a permission request; the UI must answer it.
    void permissionRequested(const QString &requestId, const QString &toolName,
                             const QString &detail);

private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onProcessErrorOccurred(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    /// Everything one ACP session owns. Held in `m_sessionsById` by its wire id; the
    /// transcript is the rows, and the rest is what the bar and the dock show.
    ///
    /// A plain struct rather than a QObject: nothing in QML binds a session
    /// object directly — the pane reads the flat `openSessions` list and the
    /// current session's properties — so an object tree here would add
    /// lifetime questions for no interface.
    struct SessionState
    {
        QString id;
        /// The bar's label: the first prompt's first line, or "session <id8>"
        /// before the first prompt. ACP carries no title of its own.
        QString title;
        /// The absolute workspace session/new was given. Shown in the bar and
        /// used to resolve a tool's relative `file_path` for changedFiles.
        QString cwd;
        /// This session's status line ("ready", "thinking…", a failure). Falls
        /// back to the child's while it is empty.
        QString status;
        bool busy = false;
        /// True once a prompt has supplied the bar's title, so a later prompt
        /// does not rename the session under the reader.
        bool titled = false;
        AcpTranscript *transcript = nullptr;

        /// The session's own dock state. Kept here, not on the client, because
        /// two sessions have two todo lists, two goals and two changed-file
        /// sets; the AcpCompat members mirror whichever one is current.
        QVariantList todos;
        QVariantMap goal;
        QStringList changedFiles;
        QVariantList configOptions;
        int contextUsed = 0;
        int contextSize = 0;

        /// Call bookkeeping, per session: a call id is only unique within its
        /// own session's wire.
        QHash<QString, QVariantList> pendingTodos;
        QHash<QString, QStringList> pendingWritePaths;
        QSet<QString> goalCalls;

        /// Permission request ids the agent is waiting on for THIS session, in
        /// arrival order. At most one prompt can be in flight, so in practice
        /// this holds one id; the list keeps the answer order honest if an agent
        /// ever raises two.
        QStringList pendingPermissionIds;
    };

    /// One permission request, keyed by the agent's own id. The session is
    /// carried because the request is answered BY ID and may outlive a switch.
    struct PendingPermission
    {
        QString sessionId;
        QString toolName;
        /// The request's own words, kept so a re-offer after a switch shows the
        /// same question rather than a blanked-out one.
        QString detail;
    };

    void handleLine(const QByteArray &line);
    void handleResponse(const QJsonObject &message);
    void handleNotification(const QJsonObject &message);
    void handleAgentRequest(const QJsonObject &message);
    void applySessionUpdate(SessionState *session, const QJsonObject &update);

    /// Replace `session`'s option list from a session/new, resume or
    /// set_config_option answer. Emits configOptionsChanged only when that
    /// session is the one on screen.
    void refreshConfigOptions(SessionState *session, const QJsonArray &options);

    /// The paths a tool call's arguments name as modified, before resolution
    /// against the workspace. Empty for a call that modifies nothing.
    ///
    /// ACP has no workspace-diff stream and this client may not read the
    /// workspace itself, so the calls are the whole account of what a session
    /// touched. Two spellings carry it and both are read: DSH's own tool names
    /// (`write`, `edit`) with their `file_path` argument, and a standard agent's
    /// `kind: "edit"` with the affected files in `locations`. A rendered title
    /// such as "Edit src/x.cpp" is deliberately not parsed — that would be a
    /// guess at another agent's wording.
    static QStringList pathsModifiedBy(const QJsonObject &call);

    /// Commit what a *finished* tool call said about the session: the todo list
    /// its arguments carried, the goal its result carried, the files it named.
    /// Both live in `tool_call` and only their verdict is in `tool_call_update`,
    /// so nothing is recorded until the call reaches a terminal status — a list
    /// the tool rejected, or a write that failed, never became session state.
    void applyToolCallResult(SessionState *session, const QString &callId,
                             const QString &status, const QJsonObject &update);

    /// Decode a goal tool's result and publish it as `goal`. See the comment at
    /// the definition: ACP has no goal projection, and this is the only frame
    /// that carries the phase.
    void applyGoalResult(SessionState *session, const QJsonObject &update);

    /// Write one JSON object followed by a newline. ACP is newline-delimited.
    void writeMessage(const QJsonObject &message);

    /// Start a request and return the id to correlate its response with.
    /// `sessionId` is recorded so a prompt's or a config change's answer is
    /// applied to the session that asked, not to whatever is on screen when it
    /// arrives.
    int beginRequest(const QString &method, const QJsonObject &params,
                     const QString &sessionId = QString());

    /// The session with this wire id, or null. Null is a normal answer for a
    /// frame that named a session this client has closed.
    SessionState *sessionFor(const QString &id) const;
    SessionState *currentSession() const;

    /// Make `id` the session in front and publish its facts as the ones the UI
    /// reads: the transcript model and the AcpCompat dock members (`todos`,
    /// `goal`, `changedFiles`). Emits session/transcript/status/busy once, so a
    /// switch is one update rather than a burst. A no-op when `id` is already
    /// current — an ordinary `session/update` must not look like a switch.
    void setCurrentSession(const QString &id);

    /// Copy the current session's per-session dock state into the AcpCompat
    /// members, with change checks. The config options and context counts are
    /// getters over the session, so they need no copy.
    void syncWidgetState();

    /// Tell the bar its roster moved (a session added/removed/retitled, a turn
    /// started or ended, a permission request arrived or was answered).
    void touchSessions();

    void setSessionStatus(SessionState *session, const QString &status);
    void setSessionBusy(SessionState *session, bool busy);

    /// A per-session call failed: say so on that session, not on the child. The
    /// pane used to hold one session, so any JSON-RPC error killed the client;
    /// with several, one refused prompt or config change must leave the others
    /// running.
    void reportSessionFailure(SessionState *session, const QString &text);

    /// Forget every session, its transcript model and its pending permission
    /// requests, without touching the process. Used by restart, shutdown, a
    /// dead child and a handshake failure.
    void clearSessions();

    /// Forget everything that belonged to the child — every session, the agent
    /// info, every request still in flight — without touching the process.
    ///
    /// Private because "start over" is the only coherent use: a caller that
    /// emptied the sessions but kept the child would keep prompting
    /// conversations the reader can no longer see.
    void resetConversation();

    void setState(State state);
    void setStatus(const QString &status);
    void fail(const QString &message);

    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;

    State m_state = Idle;
    /// Every open session, by wire id, plus creation order for the bar: QHash
    /// iteration is arbitrary, and a bar that reorders itself on every redraw is
    /// unusable.
    QHash<QString, SessionState *> m_sessionsById;
    QList<QString> m_sessionOrder;
    /// Which session the pane is showing. Empty when the child holds none.
    QString m_currentSessionId;
    QString m_status;
    QString m_lastError;
    QString m_agentInfo;
    /// Whether the child now going away was asked to go. Set by shutdown() and
    /// cleared by start(), it is what lets onFinished() tell a deliberate stop
    /// ("we closed the pane") from a child that left on its own — the two are
    /// otherwise indistinguishable in the log, because the ACP profile treats
    /// stdin EOF and SIGTERM as a *successful* bounded shutdown and exits 0.
    bool m_stopping = false;
    /// Which restart a queued start() belongs to. restart() defers its spawn by
    /// one event-loop turn, and a pane closed in that turn must not leave a
    /// child running behind it: anything that stops this client bumps the
    /// counter, and a start whose generation is stale does nothing.
    quint64 m_restartGeneration = 0;

    int m_nextId = 1;
    QHash<int, QString> m_pendingMethods;
    /// The session each in-flight request belongs to, for the methods that are
    /// about one session (prompt, set_config_option, close, resume). A
    /// `session/new` has no id to record — the answer supplies it.
    QHash<int, QString> m_requestSession;
    /// Permission requests arrive keyed by an id *the agent chose*, so they are
    /// tracked separately from our own outbound request ids.
    QHash<QString, PendingPermission> m_pendingPermissions;

    QString m_program;
    QStringList m_arguments;
    QString m_workspace;

    /// The transcript model of the session in front, or null. Kept as a member
    /// only so `transcript()` is a plain read; it always points into
    /// m_sessionsById.
    AcpTranscript *m_transcript = nullptr;
};
