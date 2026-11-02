#pragma once

// DshClient — the WebSocket transport, talking to a local DSH host's own API.
//
// This is the second transport alongside AcpClient. Where ACP is a stdio child
// limited to chat-and-tools, this speaks the host's real Remote surface over
// /api/remote.mux, which is what the web UI and dsh-android use — so it is the
// route to terminal, panels and trajectory parity.
//
// Two wire facts that shape the code:
//
//  * Unary calls are POST /api/<ns>/<method> carrying the harness envelope
//    {type:"client-request", rpcId, method, payload:{args:{…}}}, answered with
//    {type:"server-response", rpcId, result:{ok:true,value} | {ok:false,error}}.
//    Argument keys are the *host method parameter names*, which is why
//    `session/list` takes `_request` and not `request`.
//  * Streaming is one WebSocket multiplexing many logical streams, each named by
//    a client-minted streamId. A socket close aborts every logical stream, so
//    reconnect means reopening them.
//
// A followed session is one such logical stream (`session/follow`), but its items
// are the host's *durable* journal vocabulary (`assistant/message`, `tool/call`,
// `tool/result`, …), not ACP's `sessionUpdate` vocabulary. Rather than let two
// dialects reach the UI, the frame handler translates the journal into exactly
// the updates AcpClient::applySessionUpdate consumes, so the transcript renders
// identically whichever transport is driving it.
//
// Auth is `BrowserAuth`: the client mints its own signed session cookie from the
// secret in the harness home. Crucially the cookie's authority and the request's
// Host must match exactly — a cookie minted for 127.0.0.1:8081 is rejected when
// the request presents Host: localhost.

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "AcpClient.h"   // reuse AcpTranscript — the transcript is transport-agnostic

class BrowserAuth;
class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;

class DshClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString authority READ authority NOTIFY authorityChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    /// Whether the host may have a turn open — the latch described at m_turnOpen,
    /// exposed because the composer's Enter mode has to agree with the host and
    /// `busy` cannot: `busy` follows the process-local attempt stream, so it is
    /// false during a tool call, false in the gap after `turn/end`, and false
    /// after a reconnect until the snapshot arrives. A composer that trusted it
    /// sent `queue` while the reader's preference was `steer`, which is how a
    /// steer became a queued message with nothing in the log to say so.
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnOpenChanged)
    bool turnOpen() const { return m_turnOpen; }

    /// True while a `session/list` is in flight. The host's session-query read can
    /// take minutes when it is occupied, and the rail is empty for the whole of it,
    /// so a view needs to be able to say "waiting" rather than show nothing.
    Q_PROPERTY(bool sessionsLoading READ sessionsLoading NOTIFY sessionsLoadingChanged)

    /// True from the moment a follow is requested until its snapshot has been
    /// applied. The transcript is deliberately empty during that window, which on
    /// screen is indistinguishable from a session that has nothing in it. It is
    /// cleared *after* the snapshot's rows are appended, not before: a view using
    /// the change as "the load is over" must not be told that while rows are
    /// still arriving.
    Q_PROPERTY(bool openingSession READ openingSession NOTIFY openingSessionChanged)

    /// True while the composer holds a new session that **does not exist yet**.
    ///
    /// A Session cannot be unmade on the host, so one created eagerly leaves a
    /// roster row behind for good. Pressing New Session therefore records *where*
    /// the session will be made and issues nothing at all: no id, no roster row,
    /// no `session/create`. The first send re-resolves that seat against the
    /// roster as it is at that moment — reusing a blank the host has meanwhile
    /// acquired there, adopting one, or creating one — and that is the only call
    /// that can add a row.
    ///
    /// While this is true `sessionId` is empty on purpose. A view that printed an
    /// id, or highlighted a roster row, in this state would be naming a session
    /// that does not exist.
    Q_PROPERTY(bool pendingSession READ pendingSession NOTIFY pendingSessionChanged)

    Q_PROPERTY(AcpTranscript *transcript READ transcript CONSTANT)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)

    /// The sessions whose agent is parked on a person, as ids, for the rail's
    /// amber mark. Both blocking waterfalls count: an `approval/request` and a
    /// `user-questions/request`. The roster's own `userQuestions` projection
    /// covers a question, but an approval is a waterfall and not a projection, so
    /// the pending map is the only source that spans both — and it is client-wide,
    /// which is what lets the mark outlive the reader switching away from the
    /// session that raised it.
    ///
    /// Ids are given in both forms the host uses (`session-<uuid>` and the bare
    /// uuid), because a row carries whichever form its own summary used. A
    /// waterfall with no `agentId` names no row and is absent rather than guessed
    /// at. Empty on the ACP transport, which has no waterfalls at all.
    Q_PROPERTY(QVariantList pendingInteractionSessions READ pendingInteractionSessions
               NOTIFY sessionMarksChanged)
    QVariantList pendingInteractionSessions() const { return m_pendingInteractionSessions; }

    /// The sessions that finished a turn while the reader was not looking at
    /// them, as ids, for the rail's green mark. "Unread" is the same fact both
    /// reference clients draw: dsh-android's `completedSessionIds` and the web's
    /// green `done` dot, which is a session that stopped since the last roster
    /// sample and was not the open one. It is deliberately not derived from
    /// `updatedAt` — a rename moves that too — and it clears the moment the
    /// session is followed.
    Q_PROPERTY(QVariantList unreadSessionIds READ unreadSessionIds
               NOTIFY sessionMarksChanged)
    QVariantList unreadSessionIds() const;

    /// The host's Workspace registry, in the host's own order, each entry
    /// `{workspaceId, path, title, sessionIds}`. The rail groups by this rather
    /// than by `cwd`: a Workspace is an identity the host owns and orders, and
    /// two Workspaces can legitimately share a directory. Archived Sessions are
    /// already filtered out of `sessions`; they never reach a view.
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(bool terminalOpen READ terminalOpen NOTIFY terminalChanged)
    Q_PROPERTY(QString terminalId READ terminalId NOTIFY terminalChanged)
    Q_PROPERTY(int terminalCols READ terminalCols NOTIFY terminalSizeChanged)
    Q_PROPERTY(int terminalRows READ terminalRows NOTIFY terminalSizeChanged)
    Q_PROPERTY(QVariantList configOptions READ configOptions NOTIFY configOptionsChanged)
    /// A sentence about the last model switch that did not take, or empty. It is
    /// the strip's own surface for the model chip: a refusal, an empty route or an
    /// answer the client cannot use must not leave the old model looking chosen.
    Q_PROPERTY(QString modelNotice READ modelNotice NOTIFY modelNoticeChanged)
    Q_PROPERTY(QString agentInfo READ agentInfo NOTIFY agentInfoChanged)
    Q_PROPERTY(int contextUsed READ contextUsed NOTIFY contextChanged)
    Q_PROPERTY(int contextSize READ contextSize NOTIFY contextChanged)
    /// The session's todo list, `[{content, status}]` with status one of
    /// pending | in_progress | completed. It arrives in the follow snapshot's
    /// projections and was previously discarded.
    Q_PROPERTY(QVariantList todos READ todos NOTIFY todosChanged)
    /// The durable goal, `{objective, phase, roundsStarted, maxRounds}`, empty
    /// when the session has none. `phase` is active | paused | blocked | complete.
    Q_PROPERTY(QVariantMap goal READ goal NOTIFY goalChanged)
    /// Input the agent has accepted but not yet consumed, `[{id, text, steering}]`
    /// in delivery order. `steering` items go to the next step boundary, the rest
    /// to the next turn.
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    /// The opening window left older history behind it. A property rather than
    /// only the invokable below, because a banner that appears when older history
    /// exists has to bind to something.
    Q_PROPERTY(bool hasEarlier READ canLoadEarlier NOTIFY hasEarlierChanged)
    /// A `session/page` request is in flight.
    Q_PROPERTY(bool loadingEarlier READ loadingEarlier NOTIFY hasEarlierChanged)
    /// Which transport this is. QML names no transport class, so this is how a
    /// control that only one of them can serve says so without binding to a
    /// class name.
    Q_PROPERTY(QString transportName READ transportName CONSTANT)
    /// Host content search (session/search). The rail's own filter only narrows
    /// the sessions already loaded, which cannot answer "where did I discuss X" —
    /// that needs the message text, and only the host has it.
    ///
    /// `searchResults` is `[{sessionId, title, snippet}]`, the title joined in from
    /// the session list so a hit can be read without opening it.
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(bool searchLoading READ searchLoading NOTIFY searchChanged)
    Q_PROPERTY(bool searchHasMore READ searchHasMore NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    /// The selectable permission presets from `permissionPresets/catalog`, each
    /// `{value, name, description?}`. This is the web's "always allow": a preset
    /// bundles a sandbox mode with an approval policy, so picking one is how a
    /// session stops asking — every approval here was one-shot before.
    Q_PROPERTY(QVariantList permissionPresets READ permissionPresets NOTIFY presetsChanged)
    /// The preset in force now: a configured key, "auto", or derived "custom".
    Q_PROPERTY(QString permissionPreset READ permissionPreset NOTIFY presetsChanged)
    /// A sentence about the last access switch that did not take, or empty. Shown
    /// beside the access chip: the owner's complaint was partly that a refused
    /// switch left the old value in place and said nothing.
    Q_PROPERTY(QString accessNotice READ accessNotice NOTIFY accessNoticeChanged)
    /// Bumped whenever an attachment's bytes arrive, so a delegate's `source`
    /// binding re-evaluates. `attachmentUrl` is a lookup, not a source of truth:
    /// an image's bytes are fetched once and kept.
    Q_PROPERTY(int attachmentsRevision READ attachmentsRevision NOTIFY attachmentsChanged)
    /// What the host's configuration currently holds, `[{ns, fields:[{key, value}]}]`
    /// from settings/describe. Top-level scalars only: this shows what a section is
    /// set to, it is not yet an editor.
    Q_PROPERTY(QVariantList settingsSections READ settingsSections NOTIFY settingsChanged)
    /// Files staged for the next prompt, `[{name, ref}]` where `ref` is the host's
    /// own FileAttachmentRef. Cleared once the prompt carries them.
    Q_PROPERTY(QVariantList pendingAttachments READ pendingAttachments NOTIFY pendingAttachmentsChanged)
    /// Background jobs the host is running or has retained, as the host's own
    /// `JobView` projection: `[{id, kind, label, status, progress, detail,
    /// startedAt, finishedAt}]`. A long command used to vanish: the agent started
    /// it, nothing showed it, and the only sign was that the turn never ended.
    /// The set is fenced by the followed session and replaced whole by each
    /// `job/list` frame, so this is never merged with a stale roster.
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    /// The host's Automation task catalog, from `schedule/catalog`: every active
    /// and inactive Host reminder whatever session it belongs to, each
    /// `{id, title, prompt, kind, rule, scheduledAt, nextRun, due, status,
    /// sessionId, sessionTitle, lastDeliveredAt}`.
    ///
    /// The page bound to this is read/write only as far as the host is: the
    /// catalog is a list, `schedule/update` replaces a name or an instruction,
    /// and `schedule/delete` removes a row. The Remote namespace has no create,
    /// no enable/disable and no run-now — creation is a model tool
    /// (`schedule_create`) reached by asking in a session — so this page must
    /// not grow controls for them.
    Q_PROPERTY(QVariantList scheduleTasks READ scheduleTasks NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleLoading READ scheduleLoading NOTIFY scheduleChanged)
    /// The last answer about a task action, already a sentence. The `...Error`
    /// flag decides whether it is drawn as a refusal or as a fact.
    Q_PROPERTY(QString scheduleNotice READ scheduleNotice NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleNoticeError READ scheduleNoticeError NOTIFY scheduleChanged)
    /// A mutation is in flight, so a second press cannot race the first.
    Q_PROPERTY(bool scheduleSaving READ scheduleSaving NOTIFY scheduleChanged)
    /// `schedule/history` for `scheduleHistoryFor`, newest first, each
    /// `{deliveredAt, deliveredLabel, scheduledAt, scheduledLabel, prompt}`.
    Q_PROPERTY(QVariantList scheduleHistory READ scheduleHistory NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(QString scheduleHistoryFor READ scheduleHistoryFor NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(bool scheduleHistoryLoading READ scheduleHistoryLoading NOTIFY scheduleHistoryChanged)

    /// The profile's plugin bundles, from `pluginManager/listBundles`: sections
    /// of `{title, note, rows:[{name, title, version, description, enabled,
    /// installed, optional, status, hasError, errorSummary, readOnly,
    /// readOnlyText, changing, switchable}]}`. A bundle is a package with one
    /// enable switch, which is what `setBundleEnabled` addresses; the
    /// loader-entry inventory is a different, much larger surface.
    Q_PROPERTY(QVariantList pluginInventory READ pluginInventory NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsLoading READ pluginsLoading NOTIFY pluginsChanged)
    Q_PROPERTY(QString pluginsNotice READ pluginsNotice NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsNoticeError READ pluginsNoticeError NOTIFY pluginsChanged)

    /// Path completion for an `@` mention in the composer, `[{path, kind}]` with
    /// kind file or directory. The host resolves them against the session cwd.
    Q_PROPERTY(QVariantList fileCandidates READ fileCandidates NOTIFY fileCandidatesChanged)
    /// Paths the current session's workspace has seen change since the follow
    /// began, from a `workspaceFiles/changes` watch. This is live, not a record of
    /// what any one turn did — the watch starts when the session is opened.
    Q_PROPERTY(QStringList changedFiles READ changedFiles NOTIFY changedFilesChanged)
    // The workspace file tree. `fileEntries` is the flattened, *visible* rows —
    // see the workspace-files section below for why the flattening is here rather
    // than in QML.
    Q_PROPERTY(QVariantList fileEntries READ fileEntries NOTIFY fileEntriesChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString fileText READ fileText NOTIFY fileTextChanged)
    Q_PROPERTY(QString fileSelectedPath READ fileSelectedPath NOTIFY fileSelectedPathChanged)
    Q_PROPERTY(int fileBytes READ fileBytes NOTIFY fileBytesChanged)
    Q_PROPERTY(bool fileTruncated READ fileTruncated NOTIFY fileTruncatedChanged)
    Q_PROPERTY(QString fileError READ fileError NOTIFY fileErrorChanged)
    // The trajectory: the same session seen as the *model's* input/output
    // ledger rather than as chat bubbles. See the trajectory section below —
    // there is no host endpoint behind this, it is a projection of the journal.
    Q_PROPERTY(QVariantList trajectory READ trajectory NOTIFY trajectoryChanged)

public:
    enum State { Idle, Connecting, Ready, Failed };
    Q_ENUM(State)

    explicit DshClient(QObject *parent = nullptr);
    ~DshClient() override;

    State state() const { return m_state; }
    QString authority() const { return m_authority; }
    QString sessionId() const { return m_sessionId; }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    bool busy() const { return m_busy; }

    bool sessionsLoading() const { return m_sessionsLoading; }
    bool openingSession() const { return m_openingSession; }
    bool pendingSession() const { return m_pendingSeat.active; }
    AcpTranscript *transcript() const { return m_transcript; }
    QString agentInfo() const { return m_agentInfo; }
    int contextUsed() const { return m_contextUsed; }
    int contextSize() const { return m_contextSize; }
    QVariantList todos() const { return m_todos; }
    QVariantMap goal() const { return m_goal; }
    QVariantList queue() const { return m_queue; }
    QVariantList searchResults() const { return m_searchResults; }
    bool searchLoading() const { return m_searchLoading; }
    bool searchHasMore() const { return m_searchHasMore; }
    QString searchQuery() const { return m_searchQuery; }
    QVariantList permissionPresets() const { return m_permissionPresets; }
    QString permissionPreset() const { return m_permissionPreset; }
    QString accessNotice() const { return m_accessNotice; }
    QString modelNotice() const { return m_modelNotice; }
    int attachmentsRevision() const { return m_attachmentsRevision; }
    QVariantList settingsSections() const { return m_settingsSections; }
    QVariantList pendingAttachments() const { return m_pendingAttachments; }
    QVariantList jobs() const { return m_jobs; }
    QVariantList scheduleTasks() const { return m_scheduleTasks; }
    bool scheduleLoading() const { return m_scheduleLoading; }
    QString scheduleNotice() const { return m_scheduleNotice; }
    bool scheduleNoticeError() const { return m_scheduleNoticeError; }
    bool scheduleSaving() const { return m_scheduleSaving; }
    QVariantList scheduleHistory() const { return m_scheduleHistory; }
    QString scheduleHistoryFor() const { return m_scheduleHistoryFor; }
    bool scheduleHistoryLoading() const { return m_scheduleHistoryLoading; }
    QVariantList pluginInventory() const { return m_pluginInventory; }
    bool pluginsLoading() const { return m_pluginsLoading; }
    QString pluginsNotice() const { return m_pluginsNotice; }
    bool pluginsNoticeError() const { return m_pluginsNoticeError; }
    QVariantList fileCandidates() const { return m_fileCandidates; }
    QStringList changedFiles() const { return m_changedFiles; }
    bool loadingEarlier() const { return m_loadingEarlier; }
    QString transportName() const { return QStringLiteral("dsh"); }
    QVariantList configOptions() const { return m_configOptions; }
    QVariantList fileEntries() const { return m_fileEntries; }
    QString filePath() const { return m_filePath; }
    QString fileText() const { return m_fileText; }
    QString fileSelectedPath() const { return m_fileSelectedPath; }
    int fileBytes() const { return m_fileBytes; }
    bool fileTruncated() const { return m_fileTruncated; }
    QString fileError() const { return m_fileError; }

    /// The flattened trajectory ledger: one entry per journal record that is a
    /// cell in the model's input/output ledger, oldest first. Each entry carries
    /// `id`, `kind`, `lane`, `status`, `seq`, `turn`, `step`, `text`, `summary`,
    /// `result`, `resultPreview`, `input`, `output`, `thinking`, `toolName` and
    /// `callId`. The summary and the detail are both kept because the row shows
    /// the first and expands to the second.
    QVariantList trajectory() const { return m_trajectoryList; }

    /// Connect: mint a cookie, probe the candidate authorities, then open the mux.
    Q_INVOKABLE void connectToHost();

    /// Disconnect and cancel every logical stream.
    Q_INVOKABLE void disconnectFromHost();

    /// Unary call. Emits `rpcFinished` with the decoded value, or `rpcFailed`.
    ///
    /// `promptRequestId` is only passed by a prompt: the call itself knows nothing
    /// about transcripts, but a refused or timed-out prompt has a row waiting that
    /// must be taken back, and the id to address it is not in the envelope.
    /// `commandLine` is only passed by `commands/execute`: the reply carries the
    /// command's result but not the line that produced it, and a permission
    /// switch has to name the preset the host accepted or refused.
    Q_INVOKABLE void call(const QString &method, const QJsonObject &args = {},
                          const QString &promptRequestId = QString(),
                          const QString &commandLine = QString());

    /// List sessions (session/list). Result is emitted as `sessionsListed`.
    Q_INVOKABLE void listSessions();

    /// The session list, flattened for a UI: each entry is a map with `sessionId`,
    /// `title`, `cwd`, `updatedAt`, `running` and `blank`. Refreshed automatically
    /// when the host reports a session added, removed or changing status, so a UI
    /// bound to this stays current without polling.
    QVariantList sessions() const { return m_sessions; }

    /// The Workspace registry in host order; empty until the workspace stream's
    /// baseline lands, which a view must treat as "no grouping information yet"
    /// rather than as "no workspaces".
    QVariantList workspaces() const { return m_workspaces; }

    /// Re-read the session list now.
    Q_INVOKABLE void refreshSessions();

    // --- terminal ----------------------------------------------------------
    //
    // The PTY lives on the HOST, not here: this client creates it, streams its
    // output and forwards keystrokes back. Input control follows the attachment,
    // so the follow stream must stay open and read for as long as the panel
    // intends to accept typing — a stream that is open but unread is not an
    // attachment in any useful sense (verified; see docs/terminal-contract.md).

    /// Open a terminal for this session and attach to it. Idempotent while one is
    /// open: it re-attaches rather than creating a second shell.
    Q_INVOKABLE void openTerminal(int cols, int rows);

    /// Forward keystrokes to the host PTY. Raw bytes: the terminal emulator has
    /// already turned a key into the escape sequence the shell expects.
    Q_INVOKABLE void writeTerminal(const QByteArray &data);

    /// Tell the host the panel was resized. The emulator and the PTY must agree,
    /// or a full-screen program draws to the wrong grid.
    Q_INVOKABLE void resizeTerminal(int cols, int rows);

    /// Close the terminal and detach.
    Q_INVOKABLE void closeTerminal();

    bool terminalOpen() const { return !m_terminalId.isEmpty(); }
    QString terminalId() const { return m_terminalId; }
    int terminalCols() const { return m_terminalCols; }
    int terminalRows() const { return m_terminalRows; }

    // --- workspace files ----------------------------------------------------
    //
    // The files belong to the HOST. This client browses them through
    // `workspaceFiles/*` on the same unary Remote surface as everything else and
    // never opens a local QFile — the workspace is remote in principle, exactly
    // as the PTY and the sessions are.
    //
    // The scope is the *session*: `workspaceFileScopeId` is this session's id
    // verbatim — exactly the string the session list carries. It is not a
    // normalised form; probing the live host showed `session-<uuid>` resolving
    // for a listed session while its bare uuid did not, and an automation's full
    // `dsh-automation-session-<uuid>` resolving while its bare uuid did not. The
    // host confines every path to the scope's workspace and answers
    // `workspace-file/outside-workspace` for anything above it.
    //
    // Two measured wire facts shape the code:
    //   * `list` takes {workspaceFileScopeId, path}. `path` is required and is
    //     normalised relative to the workspace root, which is spelled "." and
    //     echoes back as "". Entries are {name, type:"directory"|"file"}, with
    //     `size` present on files;
    //   * `read` takes {workspaceFileScopeId, path, range:{offset?, limit?}},
    //     where `offset` is a 1-based *line* number and `limit` a line count
    //     (max 5000). It is therefore a head-of-file read, not a byte range.

    /// List a directory and adopt it as the current one. `path` is
    /// workspace-relative; "." and "" both mean the root.
    Q_INVOKABLE void listDirectory(const QString &path);

    /// Unfold or fold one directory in the tree, fetching its children the first
    /// time it is unfolded. Collapsing is free — the cache is kept.
    Q_INVOKABLE void toggleDirectory(const QString &path);

    /// Read a file's contents. `path` is workspace-relative, as the tree's rows
    /// carry it.
    Q_INVOKABLE void readFile(const QString &path);

    /// Move the current directory to its parent. A no-op at the root, so the
    /// affordance never walks out of the workspace.
    Q_INVOKABLE void goUpDirectory();

    /// Attach to one session and stream its journal into the transcript.
    /// Only one session is followed at a time: a new call cancels the old stream.
    Q_INVOKABLE void followSession(const QString &sessionId);

    /// Attach to a *subagent child* session and stream its journal.
    ///
    /// A child is not addressable as an ordinary session: the host answers
    /// `session/agent-busy` ("subagent Sessions require their durable parent
    /// address") and expects the tagged `subagent` variant instead, which names
    /// the durable parent, the child and how the child was delegated.
    ///
    /// `mode` is that delegation — "one-shot" for a child fired off as one task,
    /// "continuable" for one its parent may keep talking to. It has to match what
    /// the host recorded for the child, or the host refuses the stream with
    /// `subagent/unauthorized` ("subagent mode does not match the supplied
    /// address"). "unknown" tells the host to resolve the child's own mode and is
    /// therefore the safe choice when the caller does not know: the mode is the
    /// `values.subagent.mode` of a `session/list` item. The two-argument form is
    /// that safe default, spelled as an overload rather than a default argument
    /// because QML does not apply C++ defaults.
    Q_INVOKABLE void followSubagent(const QString &parentSessionId,
                                    const QString &childSessionId,
                                    const QString &mode);
    Q_INVOKABLE void followSubagent(const QString &parentSessionId,
                                    const QString &childSessionId);

    /// Detach from the followed session and forget it.
    Q_INVOKABLE void unfollowSession();

    /// Fetch the history page immediately before the oldest record held
    /// (`session/page`) and place its rows above the ones already loaded.
    /// Repeated calls walk backwards one page at a time.
    Q_INVOKABLE void loadEarlierHistory();

    /// True while an earlier page is still to be fetched: the snapshot the
    /// transcript opened on — or the last page loaded — reported `hasMore` and
    /// left a cursor behind it.
    Q_INVOKABLE bool canLoadEarlier() const;

    /// The earlier history loaded so far, oldest first, in the same
    /// {kind,text,title,status,detail} shape the transcript model exposes.
    ///
    /// These rows belong *above* the transcript, and AcpTranscript can only
    /// append, so they are staged here rather than inserted — see
    /// loadEarlierHistory(). A UI may render them above the transcript; nothing
    /// consumes them yet, and they are also what makes the paging path testable.
    Q_INVOKABLE QVariantList earlierHistoryRows() const;

    /// Send a user turn (session/prompt). The session must be followed first.
    ///
    /// `mode` decides what happens when a turn is already running:
    ///   "queue" (default) — admit the message and let the running turn finish;
    ///   "steer"           — deliver into the running turn at the NEXT STEP
    ///                       BOUNDARY. It does not cancel anything: the host routes
    ///                       steer to inbox `next-step`, and `session/cancel` is
    ///                       the only thing that interrupts work in flight. Do not
    ///                       label it "interrupt" in the UI.
    ///
    /// Steering was previously unreachable: the composer turned its Send button
    /// into Stop while busy, so cancelling was the only thing a user could do
    /// mid-turn. The mode field itself was already being sent (hardcoded to
    /// "queue"), which is what makes this a wiring change rather than a new
    /// protocol.
    ///
    /// Staged attachments ride the same message after the text. The content is
    /// built (and every staged entry validated) before the transcript echo and
    /// before the call: a staged file that never became a receipt, or one whose
    /// receipt belongs to another session, is refused by name with a status and
    /// nothing is sent — the host would otherwise answer `gateway/input-invalid`
    /// or `session/attachment-invalid` after the prompt had been claimed.
    ///
    /// On a `pendingSession` seat this is the first real use: the turn is held,
    /// the seat is materialised against the roster as it is now, and only then is
    /// the prompt delivered into the session it became. A seat that cannot be
    /// materialised returns the text through `pendingPromptDropped` instead of
    /// swallowing it.
    Q_INVOKABLE void sendPrompt(const QString &text,
                                const QString &mode = QStringLiteral("queue"));

    /// Cancel the followed session's active turn (session/cancel).
    Q_INVOKABLE void cancel();

    /// Mutate one pending queued message (session/updateQueue).
    ///
    /// `kind` is the host's own QueueAction vocabulary:
    ///   "remove" — drop it before it is delivered;
    ///   "steer"  — promote it into the running turn;
    ///   "edit"   — replace its text, which `text` carries.
    Q_INVOKABLE void updateQueueItem(const QString &itemId, const QString &kind,
                                     const QString &text = QString());

    /// Search message text across every session (session/search).
    ///
    /// A literal phrase, not a query language, and one page of 20 — the host caps
    /// it. Calling twice with the same query is a no-op, and a query of one
    /// character clears the results rather than searching: a single letter matches
    /// nearly every session and costs a round trip to say so.
    Q_INVOKABLE void searchSessions(const QString &query);

    /// Read the permission catalog (permissionPresets/catalog).
    Q_INVOKABLE void refreshPermissionPresets();

    /// Switch the session's permission posture.
    ///
    /// The host's own current-session control is its `/permission <preset>`
    /// command (the permission-presets plugin registers it), and the web's
    /// popup picker drives exactly that line. The settings namespace's
    /// `defaultPreset` is a different thing — the default for *future* sessions —
    /// so it is deliberately not what this chip writes.
    Q_INVOKABLE void setPermissionPreset(const QString &value);

    /// Run one host command line against this session (commands/execute). `/compact`
    /// is the reason this exists beyond permissions: manual compaction is the only
    /// lever on context pressure.
    ///
    /// A line whose command word is not in `slashCommands()` is refused here with a
    /// notice rather than posted: the host would answer, but a mistyped command is
    /// a local mistake and should say so at once, with the names that would work.
    /// A `/permission <preset>` line whose preset is not in the host's catalog is
    /// refused the same way, because a value the host has not advertised cannot be
    /// a switch target.
    Q_INVOKABLE void runCommand(const QString &line);

    /// The names a `/` line may use, with their slash — the composer's completion
    /// list and `runCommand`'s own check both read this one table
    /// (SlashCommands::names()), so QML never keeps a second copy of the strings.
    Q_INVOKABLE QStringList slashCommands() const;

    /// A data URL for an attachment id, or empty while its bytes are outstanding.
    Q_INVOKABLE QString attachmentUrl(const QString &id) const;

    /// Fetch one durable image's bytes (session/attachment). Idempotent: an id
    /// already held is not fetched again.
    Q_INVOKABLE void loadAttachment(const QString &id);

    /// Read the host's configuration (settings/describe).
    Q_INVOKABLE void refreshSettings();

    /// Write one scalar setting (settings/mutate).
    ///
    /// The op vocabulary is `{op:'set', path:[…], value}` / `{op:'unset', path}`,
    /// and `expectedRevision` refuses a stale write rather than silently
    /// overwriting a concurrent change — so the revision read with the section
    /// travels back with this.
    Q_INVOKABLE void setSetting(const QString &ns, const QString &key,
                                const QVariant &value, int revision);

    /// Stage a local file for the next prompt.
    ///
    /// Upload is not a JSON RPC: it is a raw octet-stream POST to
    /// /api/session/uploadFileBinary with sessionId and name as query parameters,
    /// answering {ok, value:{receiptId, file}} — `FileUploadValue` in
    /// @deepseek-ai/dsh-client-file-upload. BOTH are kept: `file` is the durable
    /// reference a transcript shows, and `receiptId` is the only authority the
    /// prompt's `{type:'file'}` arm accepts. Sending the ref under any other name
    /// is a part that matches no union arm, and the host refuses the whole prompt
    /// with `gateway/input-invalid` on `request`.
    ///
    /// A file whose bytes are one of the four images the prompt union declares is
    /// not uploaded at all: its bytes are staged and sent inline as an image part.
    Q_INVOKABLE void uploadAttachment(const QUrl &fileUrl);

    /// Drop every staged file without sending it.
    Q_INVOKABLE void clearPendingAttachments();

    /// Stage whatever image the clipboard holds, as `pasted.png`.
    ///
    /// A screenshot is the attachment people actually send, and saving it to a
    /// file first so the file picker can find it is a step nobody wants.
    Q_INVOKABLE void pasteAttachment();

    /// Stage raw bytes as a named attachment. The one path both the picker and
    /// the clipboard go through.
    ///
    /// The bytes decide the arm: a PNG/JPEG/WebP/GIF is staged as an image part
    /// (base64, inline) and a receipt is never minted for it; anything else is
    /// uploaded and must come back with one.
    void uploadBytes(const QString &name, const QByteArray &bytes);

    /// Re-read the job roster by reopening its stream. `job/list` is a *stream*,
    /// not a call: it yields `{type:"rows", jobs:[…]}` and pushes again whenever
    /// the registry changes, fenced by the session in the request.
    Q_INVOKABLE void refreshJobs();

    /// Open (or reopen) the job roster stream for the followed session.
    void openJobStream();
    /// Reopen the roster once after its stream fails or ends. `job/list` has no
    /// natural end — the host's rows generator loops until the carrier aborts —
    /// so an end is an infrastructure event, not the roster saying it has
    /// finished. One retry keeps a transient close from freezing the chip
    /// without spinning against a host that keeps refusing.
    void recoverJobStream();
    /// Drop the roster when the followed session changes or closes. It is
    /// fenced by the session and must not put one conversation's work on
    /// another's chip.
    void clearJobs();
    /// Apply one `{type:"rows", jobs}` frame.
    void handleJobRows(const QJsonObject &value);

    /// Read one `commands/execute` answer. `line` is the line that was sent: the
    /// reply names the command's result but not the command, and a permission
    /// switch has to name the preset it asked for.
    ///
    /// Three shapes are distinguishable and all three are handled:
    ///   - `null`            the host resolved no command (an unknown name), and
    ///                       it answers ok with no value rather than an error;
    ///   - `{result:{kind:"error", text}}`  the command ran and refused;
    ///   - `{result:{kind:"success", text}}` the command ran.
    /// `/compact` shares this path, so a success must not be turned into a
    /// visible error, and a refusal must not be dropped.
    void handleCommandReply(const QString &line, const QJsonValue &value);

    QString m_jobStreamId;
    /// One automatic reopen per working roster. A frame that arrived proves the
    /// stream healthy and resets this; so does a fresh follow.
    bool m_jobStreamRecovered = false;

    /// Terminate one job (job/kill).
    Q_INVOKABLE void killJob(const QString &jobId);

    // --- Automation tasks (schedule/*) --------------------------------------
    //
    // The host's own task catalog, not the automation plugin's session list:
    // `schedule/catalog` returns every active and inactive Host reminder with
    // its original session binding, which is the only surface that can name a
    // task's rule and its state without opening a session.

    /// Read `schedule/catalog` now.
    Q_INVOKABLE void refreshScheduleTasks();

    /// Rename one task and/or replace its instruction. `schedule/update` is
    /// compare-and-set on the complete record observed when editing began, so
    /// the raw record read with the catalog is kept here and sent back as
    /// `expected`; QML names only the task and the two fields.
    Q_INVOKABLE void saveScheduleTask(const QString &id, const QString &title,
                                      const QString &prompt);

    /// Remove one task (schedule/delete), leaving queued messages alone.
    Q_INVOKABLE void deleteScheduleTask(const QString &id);

    /// Read one task's saved deliveries (schedule/history), newest first.
    Q_INVOKABLE void loadScheduleHistory(const QString &id);

    // --- Plugins (pluginManager/*) ------------------------------------------
    //
    // One bundle is one row with one switch: `listBundles` reads them and
    // `setBundleEnabled` selects or drops the bundle's patch layer. The
    // loader-entry inventory (`pluginInventory/list`, `pluginManager/listPlugins`)
    // is deliberately not what this page shows.

    /// Read `pluginManager/listBundles` now.
    Q_INVOKABLE void refreshPluginInventory();

    /// Select or drop one bundle's layer. The list is re-read after the host
    /// answers, so a switch never moves on the strength of the request alone.
    Q_INVOKABLE void setBundleEnabled(const QString &name, bool enabled);

    /// Branch a session (session/fork). Omit the seq to branch at the latest
    /// completed turn, which is the only cut a transcript can name itself.
    ///
    /// The id is trailing and optional on every per-session action below: empty
    /// means the followed session, so the rail's row menu can name the session
    /// it is about without anyone having to follow it first, and a caller that
    /// has no id still works.
    Q_INVOKABLE void forkSession(const QString &sessionId = QString());

    /// Change the current goal's lifecycle: "pause", "resume", "clear" or
    /// "complete". Every goal mutation is compare-and-set on the exact revision
    /// the client last read, so a stale bar cannot clobber a concurrent change.
    Q_INVOKABLE void goalAction(const QString &action);

    Q_INVOKABLE void editGoal(const QString &objective, int maxGoalRounds);

    /// Report one failed call: emits `rpcFailed` and, unless the method is
    /// background polling, puts the reason in front of the reader.
    ///
    /// `commandLine` is supplied for `commands/execute` so a refused permission
    /// switch can name itself on the access chip as well as in the status line.
    void reportRpcFailure(const QString &method, const QString &code,
                          const QString &message,
                          const QString &commandLine = QString());

    /// Replace this process with a freshly started copy of itself.
    ///
    /// The client can be rebuilt while it is running, and nothing kills the old
    /// binary — so the window keeps executing code that no longer exists on
    /// disk, and the only way to pick up a change was for someone outside to
    /// kill it. Restarting from inside is the same act, done by the person
    /// looking at the window.
    Q_INVOKABLE void restartSelf();

    /// Re-read this session's projections (session/projections). One call
    /// refreshes todos, goal, queue, permissions and pressure together.
    void refreshProjections();

    /// Folded Workspace groups, shared with the ACP client: the rail is the same
    /// rail whichever transport fills it.
    Q_INVOKABLE QStringList railCollapsed() const;
    Q_INVOKABLE void setRailCollapsed(const QStringList &ids);

    /// Session expansion, as "<0|1>:<sessionId>" entries — see RailState::expanded.
    Q_INVOKABLE QStringList railExpandedSessions() const;
    Q_INVOKABLE void setRailExpandedSessions(const QStringList &entries);

    /// What Enter means while the agent is running: "queue" or "steer".
    /// Ctrl+Enter always means the other one.
    Q_PROPERTY(QString busyEnter READ busyEnter WRITE setBusyEnter NOTIFY busyEnterChanged)
    QString busyEnter() const;
    void setBusyEnter(const QString &mode);

    /// Ask for path candidates after an `@` (fileReferences/list). An empty query
    /// clears them, which is what closing the popup does.
    Q_INVOKABLE void listFileReferences(const QString &query);

    /// Forget the accumulated change list.
    Q_INVOKABLE void clearChangedFiles();

    /// Open the workspace change watch for the followed session.
    void openChangesStream();
    void handleChangeFrame(const QJsonObject &value);

    QString m_changesStreamId;
    QStringList m_changedFiles;

    /// Change one advertised model/reasoning option (session/selectModel).
    Q_INVOKABLE void setConfigOption(const QString &configId, const QString &value);

    /// Re-read the host's model catalog and rebuild `configOptions`.
    Q_INVOKABLE void refreshConfigOptions();

    /// Begin a new session **without making one**.
    ///
    /// This used to call `session/create` and adopt the reply, which left an
    /// empty roster row on the host the moment the reader pressed New Session —
    /// and nothing can remove it. Now it records a seat (see `pendingSession`),
    /// clears the followed session so the composer stands alone, and issues no
    /// call; the first send is what materialises the seat.
    ///
    /// With no Workspace named the seat is resolved from the followed session, so
    /// a new conversation still starts in the place the reader is. This is what
    /// the rail's empty state and its menu call; a Workspace header's own + goes
    /// through createSessionInWorkspace instead.
    Q_INVOKABLE void createSession();

    /// New Workspace, in two steps: the host's own directory chooser, then
    /// `workspace/create` at the path it answers. A null answer means the person
    /// cancelled and is not a failure — nothing is said and nothing is created.
    /// The Workspace itself arrives through the `workspace/follow` upsert, never
    /// by being appended locally.
    Q_INVOKABLE void createWorkspaceFromPicker();

    /// The control behind a Workspace header's own +: a new seat IN that
    /// Workspace. Like createSession() it makes nothing; the Workspace's id is
    /// recorded on the seat, and `session/create` is issued at the first send,
    /// when the host resolves the directory from that id.
    Q_INVOKABLE void createSessionInWorkspace(const QString &workspaceId);

    /// The second half of New Workspace, once the picker has answered. Not a QML
    /// entry point: a path has to come from the host's own chooser, and a
    /// helper the rail could call would be a second, easier way to create a
    /// Workspace the host never offered.
    void createWorkspaceAt(const QString &path);

    /// Rename a session (session/rename). The host answers with the title it
    /// actually stored, which is what the rail should show. An empty
    /// `sessionId` names the followed session, so the title is the only
    /// required argument.
    Q_INVOKABLE void renameSession(const QString &title, const QString &sessionId = QString());

    /// Archive or restore a session (workspace/archiveSession and its mirror).
    /// Archiving is what the rail's "Archived" filter then hides; nothing is
    /// destroyed, so this is the safe way to clear a list out.
    Q_INVOKABLE void archiveSession(const QString &sessionId = QString());
    Q_INVOKABLE void unarchiveSession(const QString &sessionId = QString());

    /// Pin or unpin a session (workspace/pinSession and its mirror). A pinned
    /// session leads its group in either ordering.
    Q_INVOKABLE void pinSession(const QString &sessionId = QString());
    Q_INVOKABLE void unpinSession(const QString &sessionId = QString());

    /// Answer a permission request the agent raised.
    ///
    /// NOT YET IMPLEMENTED on this transport. It exists because the UI binds to
    /// it for both transports, and a QML `Connections` handler with no matching
    /// signal is a warning plus a dead dialog. Calling it appends a visible
    /// notice rather than pretending to answer: silently doing nothing is the
    /// worst outcome, because the agent would sit waiting on a host approval
    /// this client never subscribed to.
    Q_INVOKABLE void respondToPermission(const QString &requestId, bool allow);

    /// Open a logical stream on the mux.
    Q_INVOKABLE QString openStream(const QString &endpoint, const QJsonObject &args = {});

    /// Cancel a logical stream previously opened.
    Q_INVOKABLE void cancelStream(const QString &streamId);

    /// Open the forwarded-event stream (`$events`).
    ///
    /// This is a *second, permanent* logical stream on the same mux, and it is
    /// not optional: it carries the host's waterfalls, and a waterfall that is
    /// never answered stalls the agent permanently. The `ready` frame it sends
    /// first carries the `clientId` every answer must quote.
    ///
    /// Opened automatically by connectToHost(), because a client that omits it
    /// looks perfectly healthy right up until the first approval.
    Q_INVOKABLE void openEventStream();

    /// Subscribe to `workspace/follow`. Opened with the event stream on connect,
    /// because the archived set and the workspace order are what the rail needs
    /// before it can show anything correctly.
    void openWorkspaceStream();

    /// Answer a pending waterfall. `value` is the event-specific answer — for an
    /// approval, "allowed-once" or "rejected" — sent as
    /// {"kind":"result","value":value}.
    Q_INVOKABLE void answerWaterfall(const QString &eventId, const QString &value);

    /// Answer a `user-questions/request`. `answers` is one entry per question:
    /// `{id, selected: [labels], custom?: text}`. The host's own encoding — the
    /// result value is an object, not a string, which is why this cannot go through
    /// `answerWaterfall`.
    Q_INVOKABLE void answerQuestions(const QString &eventId, const QVariantList &answers);

    /// Post an already-built outcome for a waterfall, and forget the pending entry.
    void postWaterfallOutcome(const QString &eventId, const QJsonObject &outcome);

    /// Delegate a waterfall back to the host's own chain. The safe default for an
    /// event this client does not understand, and strictly better than silence:
    /// the host's own `noAnswerer` would otherwise fail the request outright.
    Q_INVOKABLE void delegateWaterfall(const QString &eventId);

    /// Events still awaiting an answer, as a list of maps with `eventId`, `event`
    /// and `request`, so a UI can render whatever it understands.
    Q_INVOKABLE QVariantList pendingWaterfalls() const;

signals:
    void stateChanged();
    void authorityChanged();
    void sessionChanged();
    void statusChanged();
    void lastErrorChanged();
    void busyChanged();
    /// m_turnOpen changed. One signal for the whole file — every assignment goes
    /// through setTurnOpen() so a view cannot miss a transition.
    void turnOpenChanged();

    void sessionsLoadingChanged();
    void openingSessionChanged();
    /// The new-session seat was taken or given up — see pendingSession().
    void pendingSessionChanged();
    /// A prompt typed into a seat was not delivered because the seat could not
    /// be materialised. The text comes back so a composer can restore it rather
    /// than lose what the reader wrote.
    void pendingPromptDropped(const QString &text, const QString &reason);
    void configOptionsChanged();
    void agentInfoChanged();
    void contextChanged();
    void todosChanged();
    void goalChanged();
    void queueChanged();
    void searchChanged();
    void presetsChanged();
    void accessNoticeChanged();
    void modelNoticeChanged();
    void attachmentsChanged();
    void settingsChanged();
    void pendingAttachmentsChanged();
    void jobsChanged();
    /// The task catalog, its loading state, or its notice changed.
    void scheduleChanged();
    /// One task's delivery records, or the task they belong to, changed.
    void scheduleHistoryChanged();
    /// The plugin inventory or its loading state changed.
    void pluginsChanged();
    void busyEnterChanged();
    void fileCandidatesChanged();
    void changedFilesChanged();

    /// canLoadEarlier() has flipped — a session was followed, or a page arrived
    /// and either left another page behind it or reached the start of the log.
    void hasEarlierChanged();

    /// A page of earlier history was fetched and translated. `rows` is how many
    /// transcript rows it added (staged, not inserted — see loadEarlierHistory),
    /// and `hasMore` whether a further page exists behind it.
    void earlierHistoryLoaded(int rows, bool hasMore);

    /// An approval waterfall, raised by the host on `$events` and **blocking the
    /// agent until answered**. `requestId` is the eventId to answer with.
    ///
    /// Emitted only for the followed session: an approval raised by another
    /// session is retained and offered when that session is followed, so it
    /// cannot be granted from a transcript it does not belong to.
    void permissionRequested(const QString &requestId, const QString &toolName,
                             const QString &detail);

    /// The host has withdrawn that waterfall — almost always because the approval
    /// was granted somewhere else, such as the web UI. Anything still asking the
    /// question must stop: answering a settled waterfall is an error. Also emitted
    /// when the client stops offering an approval or question because the reader
    /// followed a different session; that request stays pending and is offered
    /// again there.
    void permissionWithdrawn(const QString &requestId);

    /// A `user-questions/request` waterfall: the agent's `ask_user_question` call
    /// is blocked until this is answered. `questions` is the host's own array,
    /// passed through unchanged — each item is
    /// `{id, question, detail?, header?, options?: [{label, description?}],
    ///   multiSelect?, intent?}`.
    ///
    /// This used to be delegated along with every other unknown waterfall, which
    /// was survivable while a browser was also listening. With the web UI gone
    /// there is no other answerer, so delegating means the tool fails with
    /// NO_PROVIDER or the question silently goes stale.
    ///
    /// Emitted only for the followed session: a question raised by another session
    /// is retained and offered when that session is followed, so an answer can
    /// never be journalled into a transcript the reader is not looking at. One
    /// event is offered at a time — a batch of questions arrives as one event.
    void questionsRequested(const QString &eventId, const QVariantList &questions);

    /// A waterfall this client cannot render. It is delegated back to the host
    /// automatically; this is informational, so a stall is never silent.
    void waterfallDelegated(const QString &eventId, const QString &event);

    /// An ordinary forwarded host event: `api-session/added|removed|status|
    /// activity|error`, `settings/document-updated`, and the rest of the
    /// allow-list. `args` is the Cordis emit argument array.
    ///
    /// This is how the host tells a client that something changed *elsewhere* —
    /// another session started, went busy, or finished — so it is the raw
    /// material for any live session list. Nothing consumes it yet; it is
    /// surfaced rather than logged and dropped so that a consumer has somewhere
    /// to attach when one exists.
    void hostEvent(const QString &event, const QJsonArray &args);

    /// A unary call settled. `value` is the decoded `result.value`.
    void rpcFinished(const QString &method, const QJsonObject &value);
    void rpcFailed(const QString &method, const QString &code, const QString &message);

    /// One line for the rail's notice surface, already in the voice the rail
    /// shows it in. `isError` true is a refusal — something a person pressed
    /// that the transport could not do; false is a fact worth saying after a
    /// control that did act (a Workspace that already existed, for instance).
    /// The rail has no status bar of its own, and a failed create that only
    /// changed the window's status line was the silence this exists to end.
    void railNotice(const QString &text, bool isError);

    /// session/list settled; `sessions` is the raw array of session summaries.
    void sessionsListed(const QJsonArray &sessions);

    /// The flattened session list changed (a refresh, or a host event).
    void sessionsChanged();

    /// The rail's two marks moved: the set of sessions parked on a person, or
    /// the set that finished while the reader was elsewhere. One signal for both
    /// because they are drawn in one slot and a view re-reads both; it is emitted
    /// only on a real change, for the same reason sessionsChanged() is.
    void sessionMarksChanged();

    /// The Workspace registry or the archived/pinned sets moved.
    void workspacesChanged();

    /// Raw PTY output from the host, exactly as it arrived. The emulator consumes
    /// bytes, so nothing here is interpreted: escape sequences may be split across
    /// emissions, and reassembling them is the emulator's job.
    void terminalOutput(const QByteArray &data);

    /// Creation, attach, exit or close. `state` is the host's own word.
    void terminalChanged();
    void terminalState(const QString &state, int exitCode);

    /// The host's grid agrees with the panel's.
    void terminalSizeChanged();

    /// The flattened file tree changed — a listing arrived, a directory was
    /// unfolded, or a refusal took one back. Emitted only on a real change, for
    /// the same reason sessionsChanged() is: re-emitting an identical list makes
    /// every bound view rebuild itself for nothing.
    void fileEntriesChanged();

    /// The current directory, the selected file's text (or its total size and
    /// whether only a head of it is shown), and the last refusal, each on a
    /// real change.
    void filePathChanged();
    void fileTextChanged();
    void fileSelectedPathChanged();
    void fileBytesChanged();
    void fileTruncatedChanged();
    void fileErrorChanged();

    /// The flattened trajectory ledger changed — a record was folded in, a tool
    /// call completed, or a page of earlier history was merged. Emitted only on
    /// a real change, for the same reason sessionsChanged() is: re-emitting an
    /// identical list makes every bound view rebuild itself for nothing.
    void trajectoryChanged();

    /// A logical stream produced an item.
    void streamItem(const QString &streamId, const QJsonObject &value);
    void streamError(const QString &streamId, const QString &code, const QString &message);
    void streamEnded(const QString &streamId);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onSocketError();

    /// Mux dispatch for one logical stream, wired in the constructor.
    void onStreamItem(const QString &streamId, const QJsonObject &value);
    void onStreamError(const QString &streamId, const QString &code, const QString &message);
    void onStreamEnded(const QString &streamId);

private:
    void setState(State state);
    void setStatus(const QString &status);
    void setAccessNotice(const QString &notice);
    void setModelNotice(const QString &notice);
    void setBusy(bool busy);

    /// Both emit only on a real change, so a view is never asked to repaint for a
    /// state it already had.
    void setSessionsLoading(bool loading);
    void setOpeningSession(bool opening);
    void setPendingSession(bool pending);
    void fail(const QString &message);
    void sendJson(const QJsonObject &message);
    void resolveAuthority();

    // --- the deferred new-session seat -------------------------------------
    //
    // dsh-android's PendingSessionTarget and SessionTargets, in this client's
    // vocabulary. A seat is a target and nothing else: no id, because no
    // session exists, so nothing here may name one.
    //
    // The reference's seat also carries an agent preset and an access mode, both
    // applied at materialisation. This client's does not: the access chip and a
    // preset both need a session before `commands/execute` can address them, so
    // they are set after the seat becomes a session. That is a stated gap between
    // this lane and the reference, not a forgotten field.

    /// Where a "new session" press said the session should be made. At most one
    /// of the two is set; both empty means the host's own default, which is what
    /// createSession() sent when the reader was not in a session at all.
    struct PendingSeat {
        bool active = false;
        QString workspaceId;  ///< a real Workspace id, never the automations bucket
        QString cwd;          ///< a directory, when the seat came from a session's cwd
    };

    /// A turn typed on a seat, held until the seat has become a session.
    struct HeldPrompt {
        bool active = false;
        QString text;
        QString mode;
    };

    /// What one "start a session here" intent resolves to against the roster as
    /// it is now — dsh-android's SessionTargetPlan. `Reuse` and `Adopt` add no
    /// session; `Create` is the one branch that can add a row, and it runs only
    /// at materialisation.
    enum class SessionPlanKind { Reuse, Adopt, Create };
    struct SessionPlan {
        SessionPlanKind kind = SessionPlanKind::Create;
        QString sessionId;    ///< Reuse and Adopt
        QString workspaceId;  ///< Adopt and Create, when the seat names one
        QString cwd;          ///< Create, when the seat names a directory
        bool idle = true;     ///< a reused/adopted blank has no turn running
    };

    /// Resolve a seat against `sessions` and `workspaces` *now*.
    ///
    /// dsh-android's `materialize` passes `currentSessionId = null`: the seat
    /// holds no session, so the only choice is between reusing a blank the host
    /// has at the target and making one. The roster is read here rather than
    /// remembered from the press because a materialisation can be seconds or
    /// minutes later, and the reuse rules must hold at the moment of the one
    /// call that can add a row.
    SessionPlan planSessionTarget(const QString &workspaceId, const QString &cwd) const;

    /// A Workspace's directory or its readable name, from the registry, or empty
    /// when the baseline has not landed or the id is not a real Workspace.
    QString workspacePath(const QString &workspaceId) const;
    QString workspaceLabel(const QString &workspaceId) const;

    /// Record the seat, clear the followed session, and make the composer stand
    /// alone. Issues no `session/create`: this is the whole deferral.
    void beginPendingSession(const QString &workspaceId, const QString &cwd);

    /// What the strip says while a seat is held: where it will be made, and that
    /// nothing exists yet. Never an id — there is none to name.
    QString pendingSeatStatus() const;

    /// Take the held prompt and make its session: reuse, adopt or create now.
    void materialisePendingPrompt();
    /// The session exists (or was found): give up the seat and deliver the turn.
    void finishMaterialisation(const QString &sessionId, bool idle);
    /// The body sendPrompt used to be, now shared by the ordinary path and the
    /// materialised one. `knownIdle` says the session was just made or is a
    /// blank, so the first prompt starts a turn rather than being staged behind
    /// one that cannot exist.
    void deliverPrompt(const QString &text, const QString &mode, bool knownIdle);

    PendingSeat m_pendingSeat;
    HeldPrompt m_heldPrompt;
    /// A `session/create` was issued for the held seat. Its reply delivers the
    /// held turn; its failure returns the text and leaves the seat in place.
    bool m_seatCreateInFlight = false;
    /// The follow now running is the seat *becoming* a session, not the reader
    /// opening something else, so followAddress must not drop the seat under it.
    bool m_seatFollow = false;

    /// Rebuild `scheduleTasks` from a `schedule/catalog` answer, keeping the raw
    /// records that `schedule/update` must send back as `expected`.
    void applyScheduleCatalog(const QJsonArray &items);
    /// The complete record for `id` in the shape the schedule decoder accepts:
    /// the catalog row carries `sessionId`, `status` and `lastDelivery`, and any
    /// extra key makes `expected` invalid.
    QJsonObject scheduleExpectedRecord(const QString &id) const;
    /// Emit `scheduleChanged()` only when the sentence or its tone changed.
    void setScheduleNotice(const QString &notice, bool isError);

    /// Take a `pluginManager/listBundles` answer and redraw the page from it.
    void applyPluginBundles(const QJsonArray &bundles);
    /// Rebuild `pluginInventory` from the stored bundle answer and the bundle
    /// whose change is in flight.
    void rebuildPluginSections();
    /// Read one `pluginManager/setBundleEnabled` answer and say what it did.
    void applyBundleChange(const QJsonValue &value);
    void setPluginsNotice(const QString &notice, bool isError);

    /// Build a QNetworkRequest carrying the minted cookie and the matching Host.
    QNetworkRequest makeRequest(const QString &path) const;

    /// The shared body of followSession/followSubagent: adopt `address` as the
    /// followed session and open its stream. Keeping the address — not just the
    /// session id — is what lets `session/page` be asked for a subagent child's
    /// history later, since it takes the same tagged union.
    void followAddress(const QJsonObject &address, const QString &sessionId);

    /// Re-open the same follow after the mux dropped, keeping the transcript.
    ///
    /// A snapshot has no cursor to resume from, but its records carry `seq`, and
    /// the rows already on screen are exactly those at or below the highest seq
    /// this client has translated. The snapshot's overlapping page is therefore a
    /// cursor, not a replacement: the records above that cut are appended and the
    /// rest are skipped. Returns false when there is nothing to continue (no
    /// snapshot has ever built a transcript for this address), in which case the
    /// caller must run the ordinary reset-and-replay path.
    bool resumeFollowAddress(const QJsonObject &address, const QString &sessionId);

    /// The single writer of m_turnOpen, so the property signal cannot be missed.
    /// Callers pass what the evidence says; it is deliberately not derived from
    /// m_busy (see the comment at the member).
    void setTurnOpen(bool open);

    /// Note that `event` has been translated into the live transcript. The seq is
    /// the high-water mark a reconnect snapshot is cut against.
    void noteAppliedSeq(const QJsonObject &event);

    /// Forget the transcript and everything derived from it, and start the
    /// sequence high-water at nothing. Shared by a session switch and by a
    /// reconnect snapshot that cannot be continued (a surface replace, or a gap
    /// wider than one page), so both rebuild exactly the same way.
    void resetFollowTranscript();

    /// Forget every paging cursor: a new session is a new log.
    void resetEarlierHistory();

    /// Decode one session/page result, stage its rows and advance the cursor.
    /// Dispatch one item from the `$events` stream (ready/waterfall/cancel/emit).
    void handleEventStreamItem(const QJsonObject &value);

    /// Raise or delegate one host waterfall. Must always end in an answer.
    void handleWaterfall(const QJsonObject &frame);

    /// Forward declaration: the definition sits with the pending-waterfall map
    /// below, and this helper only needs the type by reference.
    struct PendingWaterfall;

    /// Whether a retained waterfall belongs to the session being followed.
    /// `agentId` is the host's Agent identity, which is the session id (the host
    /// asserts the two are equal at registration), and for a subagent child it
    /// is the child's own session id — the id `followSubagent` stores. An empty
    /// `agentId` is an older host or a frame that omitted it: it is treated as
    /// belonging, so the waterfall is shown rather than hidden.
    bool waterfallInFollowedSession(const PendingWaterfall &pending) const;

    /// Surface at most one pending approval for the followed session, oldest
    /// first. Appends its "waiting for approval" row at the moment it is
    /// surfaced and emits permissionRequested; leaves the rest retained.
    void offerPendingApproval();

    /// Draw an approval's "waiting for approval" row once, in the transcript on
    /// screen now. No-op if it was already drawn there.
    void drawApprovalNotice(const QString &eventId);

    /// Stop showing the approval offered for the session just left. The request
    /// stays in m_pendingWaterfalls so it is offered again where it belongs.
    void releaseOfferedApproval();

    /// Surface at most one pending `user-questions/request` for the followed
    /// session, oldest first. Appends its notice row at the moment it is surfaced
    /// and emits questionsRequested; leaves the rest retained. "One" means one
    /// event, because a single event carries a whole batch of questions.
    void offerPendingQuestion();

    /// Draw a question's notice row once, in the transcript on screen now. No-op
    /// if it was already drawn there.
    void drawQuestionNotice(const QString &eventId);

    /// Stop showing the question offered for the session just left. The request
    /// stays in m_pendingWaterfalls so it is offered again where it belongs.
    void releaseOfferedQuestion();

    void applyEarlierPage(const QJsonObject &value);

    /// Emit hasEarlierChanged() only when canLoadEarlier() has actually flipped.
    void refreshHasEarlier();

    /// The row sink the translator writes through. Normally it is the transcript
    /// itself; while an earlier page is being translated it is `m_pageRows`,
    /// because those rows belong above the transcript and it cannot insert there.
    ///
    /// `request` is a tool call's decoded arguments, which the tool card shows as
    /// IN; `detail` remains the result it shows as OUT. Last, so every existing
    /// call site keeps its positional meaning.
    void appendRow(const QString &kind, const QString &text, const QString &title = QString(),
                   const QString &status = QString(), const QString &detail = QString(),
                   const QString &key = QString(), qint64 time = 0, int turn = -1,
                   const QString &request = QString());
    /// Append one chunk of a journal message, or extend the row the previous chunk
    /// opened. Two properties of the model decide that, and both are read from the
    /// model rather than remembered in a flag: the tail row must belong to the
    /// same turn (a row never spans two turns), and it must not be a provisional
    /// row (one still streaming will be replaced by the settlement; one already
    /// withdrawn draws nothing and would swallow the text).
    void extendRow(const QString &kind, const QString &text, qint64 time = 0, int turn = -1);

    /// The turn of the transcript's last row, or -1 when it has none. Used by
    /// extendRow, which may only join two chunks of one turn.
    int tailTurn() const;

    /// Whether the transcript's last row is one of an attempt's provisional rows:
    /// still streaming, or withdrawn and left in place because the model has no
    /// delete. Read from the row — its kind and status — not from a remembered
    /// flag, so it cannot outlive the row it is about.
    bool tailIsProvisional() const;

    /// Address a tool result to the call row it belongs to in the page being
    /// translated. Returns false when the call is not in the page.
    bool attachPageToolResult(const QString &callId, const QString &status,
                              const QString &detail);

    /// Rewrite a compaction row in place — the running announcement or the
    /// durable marker — wherever this window holds it: the page being translated,
    /// or the live transcript. Returns false when no row carries the key.
    ///
    /// The page is searched first for the same reason attachPageToolResult
    /// searches it: a page is translated before its rows reach the transcript, so
    /// the row it is assembling can only be found there. An empty field is
    /// "leave it", exactly as AcpTranscript::updateByKey means it.
    bool updateCompactionRow(const QString &key, const QString &status,
                             const QString &detail, const QString &text);

    /// Whether a row with this key is held by the pages being translated or by
    /// the live transcript. The transcript has no read-only existence probe; an
    /// empty `updateByKey` is the one call that finds a row and writes nothing,
    /// the same probe the delivered-prompt path uses.
    bool hasCompactionRow(const QString &key);

    /// Take back the row a prompt put in the transcript, and forget the id: the
    /// call failed, so the host will never journal that message. The error row
    /// reportRpcFailure appends is what explains it.
    void abandonLocalPrompt(const QString &requestId);

    /// One log line for what became of a prompt's wire request and its local row.
    ///
    /// The prompt path used to log nothing at all, so a steer that came back
    /// queued could not be attributed to the composer's mode, a host refusal
    /// (`session/steer-unavailable`), or a request that died with the socket.
    /// `outcome` is the wire result and `action` what was done to the row; the
    /// row's own state (staged/echoed/gone) is read here so the caller cannot
    /// report it from memory. An empty requestId (every non-prompt call) prints
    /// nothing.
    void logPromptOutcome(const QString &method, const QString &requestId,
                          const QString &outcome, const QString &action,
                          const QString &detail = QString());

    /// Take back every row *staged* while a turn ran, keeping the ids. A cancel,
    /// a disconnect, a stream that ends or a session that is closed all end the
    /// wait without the message necessarily being withdrawn — the host keeps
    /// inbox work across a cancel — so the row must stop saying "steering…" but
    /// the durable copy must still be able to become it.
    void withholdStagedPrompts();

    /// Stop one staged row claiming to steer, without forgetting the prompt: used
    /// when the call ended without an answer we can trust (a timeout, a dead
    /// connection), where the host may still deliver the message later. An echoed
    /// row is left alone — it already holds the message.
    void withholdLocalPrompt(const QString &requestId);

    /// Drop the dock row whose prompt `requestId` has just been delivered, and
    /// say whether one was found.
    ///
    /// The dock is a snapshot of the host's inbox, and the client had no writer
    /// for it on delivery: the row for a message that had landed in the
    /// transcript stayed in the footer until something else happened to refresh
    /// the snapshot — and the next snapshot then slid the *next* pending message
    /// into the row the reader was looking at. Delivery is the one moment the
    /// client knows a row must go, so it goes here rather than waiting for a
    /// round trip.
    bool dropDeliveredQueueItem(const QString &requestId);

    void syncStagedPromptRows();

    /// Settle the transcript's staged row after the host accepted a
    /// `session/updateQueue` mutation. The answer carries only
    /// `{accepted:true}`, so the item, the action and the replacement text are
    /// remembered at the call in `m_queueMutation`.
    void reconcileQueueMutation();

    /// The mutation this client most recently asked for, kept only until its
    /// answer arrives: `session/updateQueue` replies with `{accepted:true}` and
    /// nothing else, so neither the item nor the action can be recovered from the
    /// reply. Two clicks inside one round trip would overwrite this; the
    /// projection refresh that follows every answer is what heals the dock, and
    /// losing only the transcript-side reconcile is the bounded cost.
    struct QueueMutation {
        QString itemId;
        QString kind;
        QString text;
    };
    QueueMutation m_queueMutation;

    /// A one-line description of a tool call, lifted from its own arguments: the
    /// command, the path, the query. Without it a tool row is its own name and
    /// nothing else.
    static QString toolSummary(const QString &name, const QJsonObject &arguments);

    /// The printable text of a tool result, for the OUT pane.
    static QString toolResultText(const QJsonObject &message);

    /// Decode one session/follow item and route it by frame type.
    void applyFollowFrame(const QJsonObject &value);

    /// Translate one durable journal event into ACP-vocabulary updates.
    void applySessionEvent(const QJsonObject &event);

    // --- live assistant streaming ------------------------------------------
    //
    // `session/follow` with `assistantStream:true` carries process-local
    // `start` / `chunk` / `end` frames for the attempt the model is writing. The
    // deltas are real presentation, not history: the journal commits the same
    // text again as one `assistant/message`, which this client also receives.
    //
    // So the rows a live attempt builds are PROVISIONAL. They exist only until
    // that message arrives, and the message then REPLACES them in place — text
    // rewritten, status cleared — rather than being appended beneath them. The
    // identity is (turn, step): the durable event carries no attemptId, but the
    // host commits exactly one assistant message per step, and `start` states
    // the turn and step the attempt belongs to. `seq > startedAfterSeq` plus an
    // `append` surface op are the two further discriminators the web client's
    // own fold uses, and they are checked here for the same reason.
    //
    // Anything that cannot be lined up exactly — a missing chunk, a second
    // start, an abandoned attempt, a message whose shape is not the two rows
    // held — WITHDRAWS the provisional rows instead of guessing. A withdrawn row
    // draws nothing — and, since a row that draws nothing is not work, is not
    // counted by turnSummary — and the journal's own rows then render the truth
    // once. That is the whole guard against every token appearing twice.
    struct ProvisionalAttempt {
        QString attemptId;
        int turn = -1;
        int step = -1;
        qint64 startedAfterSeq = -1;  ///< the journal was at this seq when it began
        qint64 settledSeq = -1;       ///< from `end`, when no message has arrived yet
        qint64 startTime = 0;         ///< epoch ms the row is stamped with (it began)
        int nextIndex = 0;            ///< the dense chunk position expected next
        QString thoughtText;          ///< what the provisional thought row holds
        QString agentText;            ///< what the provisional agent row holds

        bool alive() const { return !attemptId.isEmpty(); }
        void reset() { *this = ProvisionalAttempt{}; }
    };

    /// The key a provisional row answers to: its attempt id, so the field the
    /// frames carry is the row's identity. The thought row takes a suffix —
    /// `attemptId` alone is the agent row, which is the one a reader sees first.
    QString provisionalKey(bool thought) const;

    /// Decode one `assistant-stream` frame: start, chunk or end.
    void applyAssistantFrame(const QJsonObject &frame);

    /// Turn the reconnect baseline's in-flight attempt into the same two rows the
    /// live frames would have built, from the compact stream it carries.
    void seedProvisionalFromBaseline(const QJsonObject &active);

    /// Append the two provisional rows for the current attempt (thought first, so
    /// reasoning cannot end up below the answer) and carry any text already held.
    void drawProvisionalRows();

    /// Extend one provisional row by one delta. `updateByKey` REWRITES the row's
    /// text, so the whole prefix is passed, never the delta alone.
    void appendProvisionalChunk(bool thought, const QString &delta);

    /// Take the rows back and forget the attempt: a cancel, a disconnect, a
    /// stream that ends, a session switch, a turn that ends.
    ///
    /// Take back means WITHDRAW, not delete: the model has no delete, so the row
    /// keeps its place and is told to draw nothing. Nothing may therefore treat
    /// its continued presence as work done — see `turnSummary`, which counts only
    /// rows that draw, and `tailIsProvisional`, which is what keeps the journal's
    /// text from being merged into it.
    void discardProvisional();

    /// Replace the provisional rows with `data`'s journal text, or return false
    /// and leave the message to the ordinary translation. Claiming is recorded by
    /// resetting the attempt — a settlement happens once.
    bool reconcileProvisionalMessage(const QJsonObject &event, const QJsonObject &data);

    ProvisionalAttempt m_attempt;

    /// The single place that turns an ACP-vocabulary update into transcript
    /// rows and context properties. The DSH twin of AcpClient's version.
    /// `time` is the journal event's own epoch ms, carried onto whichever row the
    /// event opens so a message can be stamped with when it happened rather than
    /// with when the view got round to drawing it; `turn` likewise, and it is what
    /// lets a completed turn fold.
    void applySessionUpdate(const QJsonObject &update, qint64 time = 0, int turn = -1);

    /// The turn the last event belonged to. Journal events all carry `turn`; this
    /// stands in for the ones that do not.
    int m_transcriptTurn = -1;

    /// Fold one `SessionProjectionBaseline` (or `SessionProjectionsValue`) into
    /// the context properties and the current model selection.
    void applyProjections(const QJsonObject &projections);

    /// Build the flattened `configOptions` list from the host's ModelCatalog.
    void applyModelCatalog(const QJsonObject &catalog);

    BrowserAuth *m_auth = nullptr;
    QWebSocket *m_socket = nullptr;
    QNetworkAccessManager *m_network = nullptr;

    /// Whether a connection is *wanted* right now. Set when a connect is asked
    /// for and cleared by an explicit disconnect, which is what separates "the
    /// host dropped us, try again" from "the user closed it, stop". Without it a
    /// drop is indistinguishable from a deliberate close, and the client sits
    /// idle — red or grey — while the host is perfectly reachable again.
    bool m_wantConnection = false;
    /// Backoff for the retry, doubling to a ceiling. A host that is restarting is
    /// usually back within seconds, so the first few tries are close together; one
    /// that is gone for good should not be hammered.
    int m_reconnectDelayMs = 1000;
    QTimer *m_reconnectTimer = nullptr;
    void scheduleReconnect();

    /// Retries since the connection was last *stable*, not since it was last
    /// opened. A socket that comes up and is dropped seconds later is a flap, not
    /// a recovery: resetting here would put every retry back at one second, which
    /// is how a single drop became forty-two.
    int m_reconnectAttempt = 0;
    /// What the last close was, in words, for the retry log. The close itself has
    /// already been logged by then; this is what makes the next attempt's line
    /// self-contained.
    QString m_lastCloseReason;
    /// Fires once a connection has stayed up long enough to be believed, and is
    /// what resets the attempt count and the delay. Started on connect, stopped
    /// by a drop — a single-shot timer cannot fire early.
    QTimer *m_stableTimer = nullptr;

    /// How the *event loop*, rather than the socket, is behaving.
    ///
    /// The gateway terminates a peer that misses two pings, and a Qt client
    /// answers a Ping only when its event loop gets round to the socket. A drop
    /// under load is therefore diagnosed by loop lag, not by the close code: this
    /// timer samples its own interval, so a blocked loop shows up as one very
    /// late tick. `m_lastFrameAt` is the same fact from the wire side.
    QTimer *m_loopLagTimer = nullptr;
    QElapsedTimer m_loopLagSample;
    QElapsedTimer m_lastFrameAt;
    qint64 m_maxLoopLagMs = 0;
    QElapsedTimer m_connectedAt;

    /// Set while the current follow is a continuation after a drop. Consumed by
    /// applyFollowFrame's snapshot branch, which skips the records already held.
    bool m_followContinuation = false;
    /// The highest journal seq translated into the live transcript, or -1 before
    /// any. Not cleared by a drop: it is what the next snapshot is cut against.
    qint64 m_transcriptThroughSeq = -1;

    State m_state = Idle;
    QString m_authority;          ///< "127.0.0.1:8081" — cookie authority and Host
    QString m_status;
    QString m_lastError;
    bool m_busy = false;
    /// Whether the host may have a turn open — and nothing else.
    ///
    /// This is deliberately not `m_busy`. `busy` is an attempt/optimism flag: it
    /// is set by an assistant stream, cleared by a refusal, a drop or a snapshot
    /// with no `activeAttempt`, and the host clears `activeAttempt` between
    /// attempts. So during a turn whose model call has ended and whose tool is
    /// running, a client that (re)attached a moment ago sees `busy == false`
    /// while the host's turn is open — and a prompt sent then was echoed as a
    /// delivered bubble and, worse, marked so the durable copy was swallowed as a
    /// duplicate later.
    ///
    /// The one question this member answers is "may I claim this message has been
    /// sent?", so it is only ever moved by evidence of a turn boundary (live or in
    /// a snapshot) and it errs towards *open* when the answer is not known: a
    /// staged row is corrected by the delivery, a bubble cannot be.
    bool m_turnOpen = false;
    /// Set while a follow snapshot's records are being replayed.
    ///
    /// A snapshot replays the newest page of the journal, and those are not
    /// deliveries happening now: re-reading the inbox once per replayed human
    /// message would ask the host fifty times on every follow, and the snapshot
    /// carries the projections that answer the same question in the same frame.
    bool m_replayingSnapshot = false;
    bool m_sessionsLoading = false;
    bool m_openingSession = false;

    int m_nextRpcId = 1;
    /// rpcId must be a *string* — the host validates the envelope with
    /// `rpcIdSchema = z.string()`, and a numeric id is rejected as an invalid
    /// client-request before the method is ever looked at.
    QHash<QString, QString> m_pendingRpc;      ///< rpcId -> method
    QHash<QNetworkReply *, QString> m_replyMethod;

    qint64 m_nextStreamId = 1;
    QHash<QString, QString> m_streamEndpoints; ///< streamId -> endpoint

    /// The flattened session list a UI binds to, and the helper that builds it.
    QVariantList m_sessions;
    void applySessionList(const QJsonArray &items);

    /// The rail's amber mark: the ids of sessions with a blocking waterfall held
    /// in `m_pendingWaterfalls`, rebuilt whenever that map changes. Kept as a
    /// ready-to-read list so a row's binding is a lookup and not a walk over the
    /// pending map per delegate.
    QVariantList m_pendingInteractionSessions;
    void refreshSessionMarks();

    /// The rail's green mark: sessions that stopped running while they were not
    /// the followed one. Membership, not a list — the property sorts a copy for
    /// the view, so the set can be added to and intersected in place.
    QSet<QString> m_unreadSessions;
    /// The `running` bit as the previous roster row stated it, so the transition
    /// to stopped is what raises the green mark. Held here rather than derived
    /// from `updatedAt`, which a rename or a title change also moves.
    QSet<QString> m_runningSessions;
    /// Drop a session's unread mark because the reader is now looking at it.
    void clearUnreadSession(const QString &sessionId);

    /// Fold one `api-session/added` payload into the roster overlay and rebuild
    /// the rows. The payload is the host's own `SessionSummary` — the exact object
    /// a `session/list` item is — so it is stored as the host sent it and
    /// rebuildSessionRows() builds its row through the same code every listed row
    /// goes through. It is kept apart from `m_rawSessions` so a list that was
    /// already in flight cannot erase it: the list still wins wherever it is the
    /// newer answer (see applySessionList).
    void adoptSessionSummary(const QJsonObject &summary);

    /// Record the live flags one `api-session/*` emit stated outright — the
    /// running bit, or that a session's agent has gone — so a row does not have
    /// to wait for a `session/list`, which can take minutes, to hear them.
    void patchSessionFlags(const QString &sessionId, const QJsonObject &flags);

    /// Reopen the session the reader was in when the window last closed.
    ///
    /// Runs the first time a roster arrives with nothing followed. The remembered
    /// id is followed with the address form the host accepts for it — see
    /// followResolvedSession() — and when that form cannot be determined the most
    /// recent top-level Session is opened instead, because a restore that fails
    /// leaves the window blank with nothing to click.
    void restoreLastSession();

    /// The one roster row for `sessionId`, or an empty object when the roster
    /// does not know it. The host stores an id as `session-<uuid>` and bare, so
    /// both spellings are matched.
    QJsonObject sessionListItem(const QString &sessionId) const;

    /// Whether the roster's own `origin` says `sessionId` is a subagent child,
    /// and what its durable parent is (empty when the roster has none).
    ///
    /// `origin` — not the presence of a parent link — is the host's rule:
    /// `validateAddress` refuses a plain session address only when
    /// `header.origin == "subagent"`, while a `session/fork` result carries
    /// `parentSession` with no `origin` and is an ordinary Session that the plain
    /// address follows. A parent id alone must therefore not be read as "child".
    bool subagentParentOf(const QString &sessionId, QString *parentId) const;

    /// Follow `sessionId` with the address the host will accept for it.
    ///
    /// Returns false when the roster says it is a subagent child whose parent
    /// cannot be named: the plain address is refused (`session/agent-busy`) and
    /// the tagged form needs a parent, so there is nothing safe to send and the
    /// caller must open something else instead of a doomed follow.
    bool followResolvedSession(const QString &sessionId);

    /// The most recently updated Session that is neither archived nor a subagent
    /// child, or an empty id when there is none.
    QString mostRecentTopLevelSessionId() const;

    /// Whether `sessionId` is in the host's archived set, in either spelling.
    bool isArchivedSession(const QString &sessionId) const;

    /// The restore runs once. Without this, a host that re-lists while the first
    /// restore is still failing would append its explanation row on every
    /// refresh.
    bool m_restoreAttempted = false;

    /// Whether the current follow stream has delivered its snapshot. A stream
    /// that ends before it never populated the transcript, and the end frame was
    /// otherwise silent — which is exactly how a refused restore looked.
    bool m_followSnapshotSeen = false;

    /// One `session/list` in flight at a time. A Session appearing or going
    /// re-lists, so without this a slow host accumulates requests that all land
    /// together — and a host that stops answering leaves the rail blank with
    /// nothing on screen to say why.
    bool m_listInFlight = false;

    /// A list was asked for while one was already in flight, so it must run when
    /// that one ends. The answer in flight was composed before the event that
    /// asked for it, so it cannot contain the session that event reported — and
    /// dropping the request (which is what "one in flight" alone does) is exactly
    /// how a newly created session sat unlisted for minutes. One bit, never a
    /// queue: any number of events during one list coalesce into a single
    /// follow-up, so two lists are never in flight together.
    bool m_listQueued = false;

    /// What the forwarded `api-session/*` emits have said about one Session, and
    /// how old that statement is. `m_rosterEpoch` only ever goes up; a statement
    /// made after the current `session/list` was requested outranks the answer,
    /// and anything older is dropped when the answer lands (applySessionList).
    /// So a row can never be staler than the roster beside it, and a list that
    /// postdates a statement still wins outright.
    struct HostSessionOverlay {
        QJsonObject summary;  ///< a whole `SessionSummary` (`api-session/added`)
        QJsonObject flags;    ///< fields a `status`/`removed` stated outright
        quint64 epoch = 0;    ///< m_rosterEpoch when the statement arrived
    };
    QHash<QString, HostSessionOverlay> m_sessionOverlay;  ///< keyed by bare id
    quint64 m_rosterEpoch = 0;

    /// The roster epoch when the in-flight `session/list` was asked for. Only one
    /// is ever in flight, so one scalar is enough. See applySessionList().
    quint64 m_listRequestEpoch = 0;

    /// One `directoryPicker/pick` at a time. The host's chooser is modal on ITS
    /// desktop, so a double-click here could stack two dialogs for one intent;
    /// cleared by the answer, by a reported failure, and by a reconnect.
    bool m_workspacePickInFlight = false;

    /// The raw `session/list` items. Kept so the archived filter and the
    /// Workspace mapping can be re-applied without another round trip — either
    /// can arrive after the list itself does.
    QJsonArray m_rawSessions;
    void rebuildSessionRows();

    /// `workspace/follow` state. The host sends one `baseline` per generation and
    /// then increments; the registry order IS the array order, which is exactly
    /// the order the rail must group in.
    QString m_workspaceStreamId;
    QVariantList m_workspaces;
    QHash<QString, QString> m_workspaceOfSession;  ///< sessionId -> workspaceId
    QSet<QString> m_archivedSessionIds;
    /// The registry-global pin set, most recently pinned first. Pinned Sessions
    /// lead their group in both of the rail's orders.
    QSet<QString> m_pinnedSessionIds;
    void handleWorkspaceItem(const QJsonObject &value);

    /// The terminal. `m_terminalId` is caller-minted so a re-attach adopts the
    /// same shell rather than racing a second one into existence; `m_attachmentId`
    /// is what `controllerId` must equal for typing to be accepted.
    QString m_terminalId;
    QString m_attachmentId;
    QString m_terminalStreamId;
    int m_terminalCols = 0;
    int m_terminalRows = 0;
    QString m_terminalState;

    /// Open the follow stream for the current terminal. Writing only works
    /// while this attachment is live, so it is opened on create and on re-attach.
    void attachTerminal();

    void handleTerminalItem(const QJsonObject &value);

    // --- workspace files ----------------------------------------------------

    /// The host's scope for `workspaceFiles/*`: this session's id verbatim, or
    /// empty while no session is followed (in which case there is no workspace).
    QString workspaceScopeId() const;

    /// Normalise a path the way the host does: "." and "" are the root, a leading
    /// "./" and a trailing "/" are noise. Paths the tree builds are already in
    /// this form; this is what keeps a caller-supplied one from becoming a second
    /// spelling of the same directory in the cache.
    static QString normaliseWorkspacePath(const QString &path);

    /// A child's workspace-relative path, which is the row identity the tree and
    /// a later readFile() both use.
    static QString joinWorkspacePath(const QString &directory, const QString &name);

    /// Ask the host for one directory's children. The answer lands in
    /// applyDirectoryList().
    void requestDirectory(const QString &path);

    /// Cache a workspaceFiles/list result and rebuild the visible rows.
    void applyDirectoryList(const QJsonObject &value);

    /// Take a workspaceFiles/read result into the file view.
    void applyFileRead(const QJsonObject &value);

    /// Append one directory's visible rows, depth-first. Recurses only into
    /// directories a reader has unfolded and that are already cached.
    void appendFileRows(const QString &directory, int depth, QVariantList &out) const;

    /// Recompute `m_fileEntries` and emit only when it actually changed.
    void rebuildFileRows();

    /// Forget the whole tree. A different session is a different workspace, so
    /// carrying the old rows over would attribute one host's files to another.
    void resetWorkspaceFiles();

    /// Replace the last refusal, emitting only on a change.
    void setFileError(const QString &message);

    /// Listings cached by the host's own normalised path ("" is the root), the
    /// directories a reader has unfolded, and the flattened projection the UI
    /// binds to. Keeping the cache here — rather than re-listing on every expand
    /// — is what makes lazy expansion lazy.
    QHash<QString, QVariantList> m_directories;
    QSet<QString> m_expandedDirectories;
    QVariantList m_fileEntries;

    /// The directory the panel is browsing, and the last one asked for, so a
    /// refused listing can take back the expansion it optimistically added.
    QString m_filePath;
    QString m_lastDirectoryRequest;

    /// The selected file: its workspace-relative path, the head of its text, the
    /// host's total byte count, and whether only part of it is shown.
    QString m_fileSelectedPath;
    QString m_fileText;
    int m_fileBytes = 0;
    bool m_fileTruncated = false;

    QString m_fileError;

    // --- trajectory ---------------------------------------------------------
    //
    // The same journal records the transcript consumes, read as the model's
    // input/output ledger. There is deliberately no RPC behind this: the host
    // answers 404 for `session/trajectory` and friends, so the ledger is a
    // client-side projection of `session/follow` and `session/page`.
    //
    // The fold lives here rather than in a class of its own because it needs the
    // *raw* journal fields — `callId`, `arguments`, `isError`, `source.kind`, the
    // PTC parent link — and the journal -> ACP translation above throws exactly
    // those away. A separate class would have to be handed the same stream a
    // second time, and two decoders of one journal drift.

    /// One ledger cell: the web's `TrajectoryCellProps`, as far as this journal
    /// can fill it. The raw detail is kept beside the derived summary for the
    /// same reason the web keeps both — a row shows the summary, an expand shows
    /// the source.
    struct TrajectoryCell {
        QString id;             ///< stable identity: the call id, else the record's seq
        QString kind;           ///< system|user|context|message|compacted|tool|subtool
        int lane = 0;           ///< 0 Input, 1 Model, 2 Tools
        QString status;         ///< running|complete|error
        qint64 seq = -1;
        int turn = 0;
        int step = 0;
        QString text;           ///< the kind's short prefix: a tool name, "Tool call only"…
        QString summary;        ///< the bounded one-line preview
        QString result;         ///< a tool's short verdict: "error", "No output", or empty
        QString resultPreview;  ///< the bounded preview of what the tool returned
        QString input;          ///< retained raw detail, bounded
        QString output;
        QString thinking;
        QString toolName;
        QString callId;
    };

    /// The ledger and the id index the fold needs to complete a call or a
    /// compaction whose opening record came earlier. One object rather than two
    /// members so the earlier-page sink below has the ledger's own type.
    struct TrajectoryLedger {
        QVector<TrajectoryCell> cells;
        QHash<QString, int> byId;
        int indexOf(const QString &id) const { return byId.value(id, -1); }
    };

    /// The ledger the fold writes through. Normally the ledger itself; while an
    /// earlier page is being translated it is `m_pageTrajectory`, because a page
    /// is older than everything held and belongs in front of it rather than
    /// appended — the same reason the transcript stages page rows.
    TrajectoryLedger &trajectorySink();

    /// Fold one journal record into the ledger. Returns true when it changed
    /// something, which is what gates the rebuild: half the journal is turn,
    /// step and delivery bookkeeping that is not a cell at all.
    bool foldTrajectoryRecord(const QJsonObject &event);

    /// Fold one record and, only if it changed the ledger, republish the
    /// projection. The single entry point applySessionEvent calls.
    void applyTrajectoryEvent(const QJsonObject &event);

    /// Append one cell to the active sink and register its id, returning its
    /// index in that sink.
    int appendTrajectoryCell(TrajectoryCell cell);

    /// The cell at `index` in the active sink, recorded so the next publication
    /// patches that one entry instead of rebuilding the projection. Every
    /// in-place completion goes through here — a tool result, a compaction's
    /// summary or end, a PTC re-tag — while a fresh append needs no help because
    /// the projection can simply grow at the end.
    TrajectoryCell &editTrajectoryCell(int index);

    /// One cell in the shape the view reads it. The published keys live here
    /// alone so there is no second description of the projection to drift.
    QVariantMap flattenTrajectoryCell(const TrajectoryCell &cell) const;

    /// Publish the projection: patch the entries that changed, grow the tail,
    /// and emit only when a reader would see a difference.
    void rebuildTrajectory();

    /// Forget the ledger: a different session is a different log.
    void resetTrajectory();

    /// Merge a translated earlier page in front of the ledger and index it.
    void mergeTrajectoryPage();

    /// The ledger itself, and the page sink the earlier-history translation
    /// writes through. See trajectorySink().
    TrajectoryLedger m_trajectory;
    TrajectoryLedger m_pageTrajectory;

    /// The projection a view binds to, and the reason rebuildTrajectory exists.
    QVariantList m_trajectoryList;

    /// Ledger indices whose published entry is behind the ledger. A record
    /// normally leaves one here — the cell it appended, or the older call it
    /// completed — so publishing one record costs one entry, not the ledger.
    QVector<int> m_trajectoryDirty;

    /// Set when a prepended earlier page shifted every index: the projection
    /// cannot be patched across that, so the next publication flattens the whole
    /// ledger once. Pages arrive once per page, not once per record.
    bool m_trajectoryReindexed = false;

    /// The wire's PTC parent links (`tool/ptc-dispatch-start` / `tool/ptc-dispatch`,
    /// `subCallId` -> `parentCallId`). A call that has one is a `subtool` cell,
    /// exactly as it is in the web's ledger — the trajectory stays flat, the link
    /// is only what tags the row.
    QHash<QString, QString> m_ptcParents;

    /// compactionId -> the cell it opened, so `compaction/summary` and
    /// `compaction/end` complete the cell `compaction/start` created instead of
    /// each adding another row for one compaction.
    QHash<QString, QString> m_compactionCells;

    /// The last turn and step boundary seen. A tool call and an assistant
    /// message carry their own; a user message, an injected notice and a
    /// compaction do not, so they inherit the one running.
    int m_trajectoryTurn = 0;
    int m_trajectoryStep = 0;

    /// The `$events` stream and the clientId its `ready` frame supplied. Every
    /// waterfall answer must quote that clientId, so the stream is useless until
    /// it arrives — which is why `ready` is tracked separately from the stream id.
    QString m_eventStreamId;
    QString m_eventClientId;

    /// Waterfalls awaiting an answer, keyed by eventId. Held rather than answered
    /// immediately so a UI can decide; an entry that is never removed is an agent
    /// that is stuck, so this is deliberately inspectable.
    struct PendingWaterfall {
        QString event;
        /// The host's Agent identity. It is the session id: the host refuses to
        /// register an Agent whose `id` differs from its `session.id`
        /// (`@deepseek-ai/dsh-agent`), and a subagent child's is the child's
        /// own id. Empty when an older host or a frame omitted it.
        QString agentId;
        QJsonObject request;
        /// Arrival order, so retained waterfalls are offered oldest first and a
        /// re-delivery of an already-held request keeps its place.
        qint64 seq = 0;
        /// The "waiting for approval" / "asking a question" notice is in the
        /// transcript on screen now. Reset with the transcript, so re-offering
        /// after a switch draws it again into the transcript it belongs to.
        bool rowDrawn = false;
    };
    QHash<QString, PendingWaterfall> m_pendingWaterfalls;

    /// The next arrival order for a retained waterfall.
    qint64 m_nextWaterfallSeq = 0;

    /// The approval whose dialog is offered, or empty when none is. One at a
    /// time, so the modal cannot be replaced underneath a reader mid-decision.
    QString m_offeredApprovalId;

    /// The question event whose dialog is offered, or empty when none is. One
    /// event at a time — the batch under that event is answered together — so a
    /// second prompt cannot replace the one a reader is deciding on.
    QString m_offeredQuestionId;

    /// The one session being followed, and the logical stream carrying it.
    /// Empty `m_followStreamId` means nothing is attached.
    QString m_followStreamId;
    QString m_sessionId;

    /// The address the followed stream was opened with, verbatim, because
    /// `session/page` takes the same tagged union and a subagent child's history
    /// must be paged with its subagent address rather than its bare session id.
    QJsonObject m_followAddress;

    /// One row a prepend would insert, in the shape AcpTranscript stores. Private
    /// to it, so mirrored here — this is what the earlier-history sink collects.
    /// `request` mirrors the transcript's own field: a page's tool rows carry
    /// their arguments exactly as a live one does, or they would be the only rows
    /// whose IN half is empty.
    struct EarlierRow {
        QString kind;
        QString text;
        QString title;
        QString status;
        QString detail;
        QString request;
        /// The identity a result later in the same page addresses its call by.
        /// A page is translated on its own, so without this a `tool/call` and its
        /// `tool/result` became two rows — the second with no name and no output.
        /// Empty where nothing addresses the row.
        QString key;
        /// The journal turn, so consecutive chunks of one kind merge only within
        /// a turn. A page is cut at a message boundary, which can be a turn
        /// boundary, so the kind alone is not enough.
        int turn = -1;
    };

    /// Paging state. `m_pageCursor` is the inclusive cut the next earlier page
    /// ends at: one *below* the oldest record held, because `throughSeq` is
    /// inclusive (verified against the live host — asking with the oldest record's
    /// own seq hands that record back a second time).
    qint64 m_pageCursor = -1;
    bool m_hasEarlier = false;
    bool m_loadingEarlier = false;

    /// The session a page in flight was asked for. A follow in the meantime makes
    /// the answer stale; merging it would attribute one log's records to another.
    QString m_pageSessionId;

    /// What hasEarlierChanged() last advertised, so it is emitted on a change of
    /// the answer rather than on every page.
    bool m_hasEarlierAdvertised = false;

    /// Earlier history: the page being translated and everything already staged,
    /// oldest first. A page is older than all of it, so each page goes on top.
    bool m_earlierSink = false;
    QList<EarlierRow> m_pageRows;
    QList<EarlierRow> m_earlierRows;

    /// Model route for the followed session, as the host reports it. Kept so a
    /// reasoning-effort change can resend the unchanged provider and model.
    QString m_provider;
    QString m_model;
    QString m_reasoningEffort;

    /// What the row a prompt put in the transcript is standing in for. Which of
    /// the two it is decides what the durable `user/message` means when it
    /// arrives: the same message twice, or the arrival the row was waiting for.
    enum LocalPromptState {
        /// The prompt started a turn, so the row *is* the message and the host
        /// journals it as it is delivered. The durable copy is a duplicate.
        LocalPromptEchoed,
        /// The host accepted the prompt into its inbox while a turn is running.
        /// Nothing is journaled until that message is delivered, so the row is a
        /// delivery *state* until then — the durable copy replaces it.
        LocalPromptStaged,
    };

    /// Prompt request ids this client has a transcript row for, by the id the
    /// host will journal the message under (`source.rpcId`).
    ///
    /// An entry must never outlive its row: if the transcript is replaced (a
    /// re-follow, a session switch) the row is gone and the durable copy has to
    /// render. So this is cleared wherever the transcript is — a set that outlived
    /// its rows used to swallow the delivery instead, which is how an accepted
    /// prompt could vanish from the transcript entirely.
    QHash<QString, LocalPromptState> m_localPrompts;

    QSet<QString> m_stagedPromptRows;

    /// What one compaction's `compaction/summary` record said, by compaction
    /// identity. The summary and the counts are only on that record; the durable
    /// marker is the checkpoint, which follows it in the live journal but can be
    /// delivered first when a page boundary falls between the two. Items and
    /// tokens are -1 when the record did not carry them — never invented.
    struct CompactionFacts {
        QString summary;
        int items = -1;
        int tokens = -1;
    };
    QHash<QString, CompactionFacts> m_compactionFacts;

    /// compaction identity -> the durable marker row's key: the transcript's
    /// counterpart of m_compactionCells. A compaction's live announcement and its
    /// checkpoint marker are two rows with different keys, and this is what tells
    /// a replayed checkpoint (or a page's summary) that the marker already
    /// stands, so the compaction never doubles.
    QHash<QString, QString> m_compactionRows;

    /// The callIds whose row carries a change diff as its payload, rather than
    /// the tool's result text.
    ///
    /// A successful `edit` or `write` result is the host's own confirmation ("The
    /// file … has been updated successfully."), which says less than the counts on
    /// the row; writing it into `detail` would replace the diff with a sentence.
    /// So a result settles the STATUS and leaves the payload alone — and this is
    /// how that path knows which rows it applies to, because the result record
    /// names only its call, never its tool. A failure is the exception: it
    /// replaces the diff, because an edit that failed changed nothing.
    ///
    /// Cleared with the transcript, because every entry describes a row in it.
    QSet<QString> m_diffPayloadCalls;

    /// The host's ModelCatalog, cached so a selection change can rebuild the
    /// picker without another round trip.
    QJsonObject m_catalog;

    QString m_agentInfo;
    int m_contextUsed = 0;
    int m_contextSize = 0;
    QVariantList m_todos;
    QVariantMap m_goal;
    QVariantList m_queue;
    QVariantList m_searchResults;
    QVariantList m_permissionPresets;
    QString m_permissionPreset;
    /// The preset the last `/permission` command asked for, while the host's
    /// refreshed projection has not yet confirmed it. Empty when there is nothing
    /// awaiting confirmation — the label itself stays host-driven.
    QString m_pendingPermissionPreset;
    QString m_accessNotice;
    QString m_modelNotice;
    /// attachment id -> data URL. Small images are held inline; a session
    /// with many large ones would want eviction, which this does not do.
    QHash<QString, QString> m_attachmentUrls;
    int m_attachmentsRevision = 0;
    QVariantList m_settingsSections;
    /// Staged input that has not been sent yet, in staging order. Two shapes,
    /// because the host's `PromptContentPart` has two attachment arms:
    ///
    ///   file  — {kind:"file", name, session, ref, receipt}. The prompt sends
    ///           {type:"file", receiptId:receipt}; `ref` is kept for metadata and
    ///           display, and `session` names the Agent scope the receipt was
    ///           minted in, which is the only scope that can spend it.
    ///   image — {kind:"image", name, mediaType, data}. `data` is canonical
    ///           base64; the prompt sends {type:"image", mediaType, data, name?}
    ///           and the bytes never touch the upload route.
    ///
    /// A file entry without a receipt cannot become a prompt part at all, so
    /// sendPrompt refuses it by name rather than sending a guessed field.
    QVariantList m_pendingAttachments;
    QVariantList m_jobs;
    /// The Automation task catalog, its raw records, and the session each one
    /// belongs to. The raw record is kept because `schedule/update` is
    /// compare-and-set on the complete record the caller observed.
    QVariantList m_scheduleTasks;
    QHash<QString, QJsonObject> m_scheduleRecords;
    QHash<QString, QString> m_scheduleSessions;
    /// How many ACTIVE tasks each Session hosts, folded out of the same catalog
    /// answer the page reads and keyed by the catalog's own `sessionId`.
    ///
    /// This is the rail's schedule mark, and the only place the count exists:
    /// `session/list` does not carry one. The host builds a Session summary out of
    /// the header, a few flags and its projections (`dsh-api-session-controller`,
    /// `summaryFor`), the Schedule plugin registers no session projection, and
    /// both other clients fold the one host-wide `schedule/catalog` answer
    /// instead — the web UI in `selectSessionScheduleFacts`, the Android client in
    /// `scheduleCounts`. So there is one source here too, and a rail row cannot
    /// disagree with the Automations page about which Sessions host a task.
    ///
    /// Only an active task counts: an ended one will never fire again and is no
    /// reason to mark its Session. A host without the Schedule bundle never
    /// answers the call and leaves this empty, which marks nothing.
    QHash<QString, int> m_scheduleCounts;
    bool m_scheduleLoading = false;
    bool m_scheduleSaving = false;
    /// A catalog read has succeeded at least once, so `schedule/changed` is worth
    /// following. The rail needs the catalog whether or not any page is opened —
    /// it is what marks a Session that a schedule delivers into — so the first
    /// roster read asks for it once (see `applySessionList`), and a fresh socket
    /// re-reads it (see `onConnected`).
    bool m_scheduleLoaded = false;
    /// The roster has asked for the catalog once on this connection. At most one
    /// read is attempted per socket while nothing has answered, so a host without
    /// the Schedule bundle costs one refused call per connection rather than one
    /// per roster refresh. `onConnected` re-arms it.
    bool m_scheduleAsked = false;
    QString m_scheduleNotice;
    bool m_scheduleNoticeError = false;
    QVariantList m_scheduleHistory;
    QString m_scheduleHistoryFor;
    bool m_scheduleHistoryLoading = false;
    /// The bundle answer as the host sent it, and the same rows flattened into
    /// sections for the page.
    QJsonArray m_pluginBundles;
    QVariantList m_pluginInventory;
    bool m_pluginsLoading = false;
    bool m_pluginsLoaded = false;
    /// The bundle whose switch is waiting on the host, empty when none is.
    QString m_pluginChanging;
    QString m_pluginsNotice;
    bool m_pluginsNoticeError = false;
    QVariantList m_fileCandidates;
    QString m_searchQuery;
    bool m_searchLoading = false;
    bool m_searchHasMore = false;
    /// Set when the host has no search index, so a host that cannot answer
    /// stops being asked on every keystroke.
    bool m_searchUnavailable = false;
    QVariantList m_configOptions;

    AcpTranscript *m_transcript = nullptr;
};
