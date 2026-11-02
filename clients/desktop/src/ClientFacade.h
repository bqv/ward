#pragma once

// ClientFacade — the shell's view of whichever transport `main.cpp` selected.
//
// `main.cpp` binds one context property, `dshTransport`, and points it at a
// `DshClient` or an `AcpClient` at launch. QML binds to it by property NAME, so a
// property one transport does not publish is a TypeError per binding rather than
// a fallback — which is what `AcpCompat` exists to prevent, by publishing the
// whole surface as empty containers and no-ops.
//
// A C++ shell cannot bind by name. This is the alternative: one `QObject` around
// the selected transport, publishing the subset the Widgets shell reads as typed
// properties and typed signals. It is a facade in the literal sense — it starts
// nothing, owns no connection and decides nothing. Every value is read from the
// transport through its meta-object and every signal is re-emitted from that
// transport's own NOTIFY, so there is one owner of every fact and the shell
// cannot drift from the transport it is showing.
//
// WHY THIS SUBSET, AND NOT THE 64 PROPERTIES
//
// `DshClient` publishes 64 properties and 69 invokables; `AcpCompat` plus
// `AcpClient` publish 60. The plan (§3.3, risk 7) says not to port that surface:
// the shell reads what its chrome draws. The list below is exactly that — the
// header's dot, the rail's rows and their marks, the status strip, and the
// composer with its dock. The transcript, terminal, files, search, settings,
// automation and plugin surfaces stay on `dshTransport` inside the retained QML
// panes, which is where they are drawn until their own stage.
//
//   facade property            transport property   the two transports
//   -------------------------  -------------------  ---------------------------
//   available                  (the object)         false when the transport is gone
//   transportName              transportName        "dsh" / "acp"; cached
//   missingProperties          (the binder)         names the transport did not publish
//   state                      state                both; 1 is Connecting vs Starting
//   stateText                  state                 derived: the dot's word for one
//   status                     status               both
//   lastError                  lastError            both
//   sessionId                  sessionId            both
//   busy                       busy                 both
//   turnOpen                   turnOpen             both (DSH latch, ACP transcript latch)
//   pendingSession             pendingSession       both; ACP never true
//   busyEnter                  busyEnter            both, one Prefs store, same setter
//   sessions                   sessions             both; ACP's is a compat empty list
//   sessionCount               sessions             derived count
//   pinnedCount                sessions             derived count (rows carry `pinned`)
//   unreadSessionIds           unreadSessionIds     both; ACP's is a compat empty list
//   unreadCount                unreadSessionIds     derived count
//   pendingInteractionSessions pendingInteraction…  both; ACP's is a compat empty list
//   pendingInteractionCount    pendingInteraction…  derived count
//   workspaces                 workspaces           both; ACP's is a compat empty list
//   workspaceCount             workspaces           derived count
//   sessionsLoading            sessionsLoading      both
//   openingSession             openingSession       both
//   hasSessionList             transportName        capability: ACP has no roster
//   hasWorkspaces              transportName        capability: ACP has no registry
//   parentSessionId            sessions+sessionId   derived: the strip's parent seat
//   parentSessionTitle         sessions+sessionId   derived: the seat's name
//   permissionPreset           permissionPreset     both; ACP's is empty
//   permissionPresets          permissionPresets    both; ACP's is an empty list
//   accessNotice               accessNotice         both
//   hasPermissionCatalog       transportName        capability: ACP has no catalog
//   configOptions              configOptions        both; ACP publishes its own list
//   modelNotice                modelNotice          both
//   contextUsed                contextUsed          both
//   contextSize                contextSize          both
//   jobs                       jobs                 both; ACP's is a compat empty list
//   liveJobCount               jobs                 derived count (running|stopping)
//   todos                      todos                both
//   goal                       goal                 both
//   changedFiles               changedFiles         both
//   queue                      queue                both; ACP's is a compat empty list
//   queueCount                 queue                derived count
//   pendingAttachments         pendingAttachments   both; ACP's is a compat empty list
//   pendingAttachmentCount     pendingAttachments   derived count
//   hasAttachmentStaging       transportName        capability: ACP's upload is a no-op
//   fileCandidates             fileCandidates       both; ACP's is a compat empty list
//
// WHERE THE TWO TRANSPORTS DISAGREE, AND WHAT THIS DOES ABOUT IT
//
//  * `trajectory` is here as the `QVariantList` DshClient publishes. AcpCompat
//    publishes a null `QAbstractItemModel *` under the same name, so an ACP
//    transport reads as an empty ledger and `hasTrajectory` is false there.
//  * `authority`, `terminalCols` and `terminalRows` are DshClient-only, and are
//    terminal-pane facts (stage 2). Declared absent rather than defaulted.
//  * `openSessions` is AcpClient-only by design — it is the ACP PANE's bar, not a
//    transport fact — and is reached through `acpPane()->openSessions`, not here.
//  * `transcript` exists on both but with different lifecycles (DshClient's is
//    CONSTANT for the process, AcpClient's is per session and NOTIFY). It is the
//    conversation pane's model, not shell chrome, so the pane keeps reading it.
//  * The ACP empties above are the compat layer's own containers, not inventions
//    of this class. An empty container is not a capability, though, so the four
//    `has*` flags say once — rather than in every control — which of them a
//    Widgets control may offer. They are keyed on `transportName` because that is
//    the test the plan (§3.3) prescribes: `typeof openTerminal === "function"` is
//    true on ACP, and so is `sessions !== undefined`, and both are false
//    positives. A transport this class does not know is treated as having
//    nothing, which is the honest default for a stub.
//  * `state`'s index 1 is `Connecting` on DshClient and `Starting` on AcpClient.
//    Same value, same meaning (not ready yet); `stateText` says "connecting" for
//    both so a shell control has one word to draw.
//  * `setPermissionPreset` really switches on DSH and records a refusal in
//    `accessNotice` on ACP, which is why the access chip hides when the catalog
//    and the preset are both empty.
//  * `sendPrompt` takes the mode on DSH and only the text on ACP (the protocol has
//    no steering). The facade resolves the composer's mode and forwards it only to
//    a transport whose signature accepts it, so the ACP call is the one-argument
//    call QML's two-argument binding is reduced to today.
//  * `followSession`/`followSubagent` are no-ops on ACP. The parent seat is drawn
//    from the roster, and ACP's roster is empty, so the seat that would call them
//    never appears.
//
// NOTHING HERE STARTS ANYTHING. The constructor reads properties and connects
// signals; the two pane clients are created on first use (see acpPane() and
// headlessClient()) because they are separate children, not transports, and a
// window that never opens their pane must not pay for them.

#include <QMetaMethod>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class AcpClient;
class HeadlessClient;
class QAbstractItemModel;

class ClientFacade : public QObject
{
    Q_OBJECT

public:
    /// `transport` is the object `main.cpp` selected — a `DshClient` or an
    /// `AcpClient` — or null when there is none (a stub build, a test). The
    /// facade does not take ownership. A null transport is legal and is what
    /// `available()` is for: every property then reads its empty value, and the
    /// shell has one fact to ask instead of a null dereference.
    explicit ClientFacade(QObject *transport, QObject *parent = nullptr);
    ~ClientFacade() override;

    /// The transport behind the facade, or null once it has been destroyed.
    QObject *transport() const { return m_transport.data(); }

    /// The transport properties the shell reads, by the name they have on BOTH
    /// transports. The constructor binds each one and reports the ones the
    /// transport in hand does not publish, so a missing property is a line in the
    /// log and an entry in `missingProperties()` rather than a silent empty value.
    static QStringList requiredProperties();

    /// The transport's own name ("dsh", "acp"), cached at construction so it
    /// survives the transport's destruction.
    Q_PROPERTY(QString transportName READ transportName CONSTANT)
    QString transportName() const { return m_transportName; }

    /// False only when there is no transport object (or it has been destroyed).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    bool available() const { return !m_transport.isNull(); }

    /// The names in requiredProperties() this transport did not publish. Empty is
    /// the healthy answer. Read once at construction and never changes: a
    /// transport that gains a property mid-run is not a thing.
    Q_PROPERTY(QStringList missingProperties READ missingProperties CONSTANT)
    QStringList missingProperties() const { return m_missing; }

    // ---- connection state, and what the header's dot and text need ---------
    //
    // The header's dot is the transport's state and nothing else. The value is
    // the transport's own enum: Idle 0, starting/connecting 1, Ready 2, Failed 3
    // — the same four on both, which is why one int serves the dot.
    //
    // `stateText` is the one-word form ("idle", "connecting", "ready", "failed")
    // for a control that has room for a word and not for a switch: the header
    // shows no status word today, and the shell should not have to invent one
    // from an int. `status` and `lastError` are the sentences the strip shows.
    enum State { Idle = 0, Connecting = 1, Ready = 2, Failed = 3 };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    State state() const { return static_cast<State>(readInt(QStringLiteral("state"))); }

    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    QString stateText() const { return m_stateText; }

    /// The transport's single status string — `thinking…`, `queued`, `steering`,
    /// `ready`, a connection message, a follow failure. One line, one owner; the
    /// strip prints it whole.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    QString status() const { return readString(QStringLiteral("status")); }

    /// The last failure the transport recorded, as a sentence.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    QString lastError() const { return readString(QStringLiteral("lastError")); }

    // ---- the conversation in front -----------------------------------------
    //
    // `sessionId` is empty while a deferred new session holds a seat
    // (`pendingSession`), which is the state in which no id exists yet and a view
    // that printed one would name a session that does not exist.
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionIdChanged)
    QString sessionId() const { return readString(QStringLiteral("sessionId")); }

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    bool busy() const { return readBool(QStringLiteral("busy")); }

    /// Whether the host may have a turn open. The composer's mode is decided from
    /// this and `busy` together — see turnRunning().
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnOpenChanged)
    bool turnOpen() const { return readBool(QStringLiteral("turnOpen")); }

    Q_PROPERTY(bool pendingSession READ pendingSession NOTIFY pendingSessionChanged)
    bool pendingSession() const { return readBool(QStringLiteral("pendingSession")); }

    /// What Enter means while the agent is running: "queue" (default) or "steer".
    /// Both transports store it in the same preference, so the value does not
    /// change with the transport. Writable, because the composer's own toggle is
    /// the only editor.
    Q_PROPERTY(QString busyEnter READ busyEnter WRITE setBusyEnter NOTIFY busyEnterChanged)
    QString busyEnter() const { return readString(QStringLiteral("busyEnter")); }
    void setBusyEnter(const QString &mode);

    /// Whether a turn is running, which is the question the delivery mode
    /// actually asks. `busy` alone drops false during a tool call and across a
    /// reconnect while the host still has a turn open; `turnOpen` is the latch
    /// that survives those gaps. This mirrors main.qml's `turnRunning()`.
    bool turnRunning() const { return busy() || turnOpen(); }

    // ---- the rail: rows, pins, marks and workspaces ------------------------
    //
    // The rows are the host's own summaries; the rail's tree, its sorts and its
    // group folds are shell layout and stay in the shell. `pinned` rides the row,
    // which is why there is a count here and no separate pin set.
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    QVariantList sessions() const { return readList(QStringLiteral("sessions")); }

    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionCountChanged)
    int sessionCount() const { return m_sessionCount; }

    Q_PROPERTY(int pinnedCount READ pinnedCount NOTIFY pinnedCountChanged)
    int pinnedCount() const { return m_pinnedCount; }

    /// Sessions parked on a person (an approval or a question), as ids, for the
    /// rail's amber mark. Empty on ACP, which has no waterfalls.
    Q_PROPERTY(QVariantList pendingInteractionSessions
               READ pendingInteractionSessions NOTIFY pendingInteractionSessionsChanged)
    QVariantList pendingInteractionSessions() const
    { return readList(QStringLiteral("pendingInteractionSessions")); }

    Q_PROPERTY(int pendingInteractionCount
               READ pendingInteractionCount NOTIFY pendingInteractionCountChanged)
    int pendingInteractionCount() const { return m_pendingInteractionCount; }

    /// Sessions that finished a turn while the reader was elsewhere, for the
    /// rail's green mark. Empty on ACP, which never pulls a background roster.
    Q_PROPERTY(QVariantList unreadSessionIds READ unreadSessionIds NOTIFY unreadSessionIdsChanged)
    QVariantList unreadSessionIds() const
    { return readList(QStringLiteral("unreadSessionIds")); }

    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged)
    int unreadCount() const { return m_unreadCount; }

    /// The host's Workspace registry, in the host's own order, each entry
    /// `{workspaceId, path, title, sessionIds}`. Grouping is the host's, not a
    /// guess from `cwd`.
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    QVariantList workspaces() const { return readList(QStringLiteral("workspaces")); }

    Q_PROPERTY(int workspaceCount READ workspaceCount NOTIFY workspaceCountChanged)
    int workspaceCount() const { return m_workspaceCount; }

    Q_PROPERTY(bool sessionsLoading READ sessionsLoading NOTIFY sessionsLoadingChanged)
    bool sessionsLoading() const { return readBool(QStringLiteral("sessionsLoading")); }

    /// A follow is in flight: the transcript is deliberately empty, so a view
    /// that showed nothing would be indistinguishable from an empty session.
    Q_PROPERTY(bool openingSession READ openingSession NOTIFY openingSessionChanged)
    bool openingSession() const { return readBool(QStringLiteral("openingSession")); }

    /// Whether the transport publishes a session roster at all. ACP publishes
    /// `sessions` as a compat empty list, so the property's presence is not
    /// support — the rail is drawn from the roster only when this is true.
    Q_PROPERTY(bool hasSessionList READ hasSessionList CONSTANT)
    bool hasSessionList() const { return m_hasSessionList; }

    /// Whether the transport has a Workspace registry, so the rail may group.
    Q_PROPERTY(bool hasWorkspaces READ hasWorkspaces CONSTANT)
    bool hasWorkspaces() const { return m_hasWorkspaces; }

    // ---- the status strip --------------------------------------------------
    //
    // The parent seat: shown only for an addressed subagent child, decided by the
    // host's own markers (`origin == "subagent"` or a non-empty `subagentMode`)
    // and never by the parent link alone — a fork has a parent and is still a
    // top-level session. This mirrors main.qml's `resolveLineage()`; the seat's
    // own navigation is followParentSession(), which the shell calls.
    Q_PROPERTY(QString parentSessionId READ parentSessionId NOTIFY parentSeatChanged)
    QString parentSessionId() const { return m_parentSessionId; }

    /// The parent's readable name (its catalog `label`, else its `title`), empty
    /// when the row has neither — which the QML renders as the word "parent".
    Q_PROPERTY(QString parentSessionTitle READ parentSessionTitle NOTIFY parentSeatChanged)
    QString parentSessionTitle() const { return m_parentSessionTitle; }

    /// The permission posture in force for the followed session: a catalog key, a
    /// derived "custom"/"auto", or empty when the transport has no catalog.
    Q_PROPERTY(QString permissionPreset READ permissionPreset NOTIFY permissionPresetChanged)
    QString permissionPreset() const { return readString(QStringLiteral("permissionPreset")); }

    /// The catalog's entries `{value, name, description}`. Empty on ACP.
    Q_PROPERTY(QVariantList permissionPresets READ permissionPresets NOTIFY permissionPresetsChanged)
    QVariantList permissionPresets() const { return readList(QStringLiteral("permissionPresets")); }

    /// The last access switch that did not take, as a sentence, or empty.
    Q_PROPERTY(QString accessNotice READ accessNotice NOTIFY accessNoticeChanged)
    QString accessNotice() const { return readString(QStringLiteral("accessNotice")); }

    Q_PROPERTY(bool hasPermissionCatalog READ hasPermissionCatalog CONSTANT)
    bool hasPermissionCatalog() const { return m_hasPermissionCatalog; }

    /// The host's config catalog: `[{id, category, currentValue, choices}]`, with
    /// `choices` a flat list of `{value, label, group, description, provider}`.
    /// The model chip and the effort chip are both read out of this — which entry
    /// is "model", which is effort, and how a route is named are the strip's own
    /// naming rules and stay in the shell, exactly as main.qml's statusBar
    /// derives them (the two shells are concurrent, so that code has to exist in
    /// both places for as long as both exist).
    Q_PROPERTY(QVariantList configOptions READ configOptions NOTIFY configOptionsChanged)
    QVariantList configOptions() const { return readList(QStringLiteral("configOptions")); }

    /// A sentence about the last model or effort switch that did not take.
    Q_PROPERTY(QString modelNotice READ modelNotice NOTIFY modelNoticeChanged)
    QString modelNotice() const { return readString(QStringLiteral("modelNotice")); }

    Q_PROPERTY(int contextUsed READ contextUsed NOTIFY contextChanged)
    int contextUsed() const { return readInt(QStringLiteral("contextUsed")); }

    Q_PROPERTY(int contextSize READ contextSize NOTIFY contextSizeChanged)
    int contextSize() const { return readInt(QStringLiteral("contextSize")); }

    /// The job roster. A job is a long-running command, so it is a fact about the
    /// session, not input — the chip sits on the strip and its popover is a shell
    /// dialog.
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    QVariantList jobs() const { return readList(QStringLiteral("jobs")); }

    /// How many jobs are unsettled: the host's liveness rule is "running or
    /// stopping", and the chip says "running" only when this is non-zero.
    Q_PROPERTY(int liveJobCount READ liveJobCount NOTIFY liveJobCountChanged)
    int liveJobCount() const { return m_liveJobCount; }

    // ---- the composer's dock and its actions -------------------------------
    //
    // The dock above the entry shows what has been admitted but not delivered:
    // todos, the goal, the queue, staged files and the workspace's changed files.
    // They are here because the composer draws them; the panes that also read
    // them (the goal row, the changes dialog) go through that.
    Q_PROPERTY(QVariantList todos READ todos NOTIFY todosChanged)
    QVariantList todos() const { return readList(QStringLiteral("todos")); }

    Q_PROPERTY(QVariantMap goal READ goal NOTIFY goalChanged)
    QVariantMap goal() const { return read(QStringLiteral("goal")).toMap(); }

    Q_PROPERTY(bool hasGoalEditing READ hasGoalEditing CONSTANT)
    bool hasGoalEditing() const { return m_hasGoalEditing; }

    Q_PROPERTY(QStringList changedFiles READ changedFiles NOTIFY changedFilesChanged)
    QStringList changedFiles() const
    { return read(QStringLiteral("changedFiles")).toStringList(); }

    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    QVariantList queue() const { return readList(QStringLiteral("queue")); }

    Q_PROPERTY(int queueCount READ queueCount NOTIFY queueCountChanged)
    int queueCount() const { return m_queueCount; }

    Q_PROPERTY(QVariantList pendingAttachments READ pendingAttachments NOTIFY pendingAttachmentsChanged)
    QVariantList pendingAttachments() const
    { return readList(QStringLiteral("pendingAttachments")); }

    Q_PROPERTY(int pendingAttachmentCount READ pendingAttachmentCount NOTIFY pendingAttachmentCountChanged)
    int pendingAttachmentCount() const { return m_pendingAttachmentCount; }

    /// Whether a file can be staged at all. On ACP `uploadAttachment` and
    /// `pasteAttachment` are no-ops and the pending list is always empty, so the
    /// attach affordance has nothing to open; the flag is what lets the shell
    /// hide it rather than offer a dialog that stages nothing.
    Q_PROPERTY(bool hasAttachmentStaging READ hasAttachmentStaging CONSTANT)
    bool hasAttachmentStaging() const { return m_hasAttachmentStaging; }

    /// The `@` path completion candidates, `[{path, name, …}]` as the transport
    /// sends them. Empty on ACP, whose list call is a no-op, so the popup simply
    /// never opens there.
    Q_PROPERTY(QVariantList fileCandidates READ fileCandidates NOTIFY fileCandidatesChanged)
    QVariantList fileCandidates() const { return readList(QStringLiteral("fileCandidates")); }

    Q_PROPERTY(bool hasFileSurface READ hasFileSurface CONSTANT)
    bool hasFileSurface() const { return m_hasFileSurface; }

    Q_PROPERTY(QVariantList fileEntries READ fileEntries NOTIFY fileEntriesChanged)
    QVariantList fileEntries() const { return readList(QStringLiteral("fileEntries")); }

    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    QString filePath() const { return readString(QStringLiteral("filePath")); }

    Q_PROPERTY(QString fileText READ fileText NOTIFY fileTextChanged)
    QString fileText() const { return readString(QStringLiteral("fileText")); }

    Q_PROPERTY(QString fileSelectedPath READ fileSelectedPath NOTIFY fileSelectedPathChanged)
    QString fileSelectedPath() const { return readString(QStringLiteral("fileSelectedPath")); }

    Q_PROPERTY(int fileBytes READ fileBytes NOTIFY fileBytesChanged)
    int fileBytes() const { return readInt(QStringLiteral("fileBytes")); }

    Q_PROPERTY(bool fileTruncated READ fileTruncated NOTIFY fileTruncatedChanged)
    bool fileTruncated() const { return readBool(QStringLiteral("fileTruncated")); }

    Q_PROPERTY(QString fileError READ fileError NOTIFY fileErrorChanged)
    QString fileError() const { return readString(QStringLiteral("fileError")); }
    Q_PROPERTY(QVariantList scheduleTasks READ scheduleTasks NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleLoading READ scheduleLoading NOTIFY scheduleChanged)
    Q_PROPERTY(QString scheduleNotice READ scheduleNotice NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleNoticeError READ scheduleNoticeError NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleSaving READ scheduleSaving NOTIFY scheduleChanged)
    Q_PROPERTY(QVariantList scheduleHistory READ scheduleHistory NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(QString scheduleHistoryFor READ scheduleHistoryFor NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(bool scheduleHistoryLoading
               READ scheduleHistoryLoading NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(QVariantList pluginInventory READ pluginInventory NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsLoading READ pluginsLoading NOTIFY pluginsChanged)
    Q_PROPERTY(QString pluginsNotice READ pluginsNotice NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsNoticeError READ pluginsNoticeError NOTIFY pluginsChanged)

    QVariantList scheduleTasks() const { return readList(QStringLiteral("scheduleTasks")); }
    bool scheduleLoading() const { return readBool(QStringLiteral("scheduleLoading")); }
    QString scheduleNotice() const { return readString(QStringLiteral("scheduleNotice")); }
    bool scheduleNoticeError() const { return readBool(QStringLiteral("scheduleNoticeError")); }
    bool scheduleSaving() const { return readBool(QStringLiteral("scheduleSaving")); }
    QVariantList scheduleHistory() const { return readList(QStringLiteral("scheduleHistory")); }
    QString scheduleHistoryFor() const
    { return readString(QStringLiteral("scheduleHistoryFor")); }
    bool scheduleHistoryLoading() const
    { return readBool(QStringLiteral("scheduleHistoryLoading")); }
    QVariantList pluginInventory() const { return readList(QStringLiteral("pluginInventory")); }
    bool pluginsLoading() const { return readBool(QStringLiteral("pluginsLoading")); }
    QString pluginsNotice() const { return readString(QStringLiteral("pluginsNotice")); }
    bool pluginsNoticeError() const { return readBool(QStringLiteral("pluginsNoticeError")); }
    QAbstractItemModel *transcript() const;

    bool hasEarlier() const { return readBool(QStringLiteral("hasEarlier")); }
    bool loadingEarlier() const { return readBool(QStringLiteral("loadingEarlier")); }
    void loadEarlierHistory();

    Q_PROPERTY(bool hasTrajectory READ hasTrajectory CONSTANT)
    bool hasTrajectory() const { return m_hasTrajectory; }

    Q_PROPERTY(QVariantList trajectory READ trajectory NOTIFY trajectoryChanged)
    QVariantList trajectory() const;

    // ---- the composer's actions -------------------------------------------
    //
    // These forward to the transport by name and signature. A transport that
    // cannot do the thing records it (ACP's `runCommand` writes a refusal into
    // the transcript, `setPermissionPreset` into `accessNotice`) or the control
    // that would call it is not drawn (the capability flags above).

    /// Send the composer's text. `mode` is the host's own vocabulary — "queue"
    /// starts a turn, "steer" folds the text into the turn already running — and
    /// an empty `mode` means "whatever Enter means now" (promptMode()), which is
    /// main.qml's `submit()` rule in one place rather than in every shell caller.
    void sendPrompt(const QString &text, const QString &mode = QString());

    /// The mode Enter would use right now, and the other one (Ctrl+Enter).
    QString promptMode() const;
    QString alternatePromptMode() const;

    /// The slash commands the client can drive (the same table QML's completion
    /// reads), and the command path itself: never queued, never steered, never
    /// carried to the model as prose.
    QStringList slashCommands() const;
    void runCommand(const QString &line);

    void cancel();
    void setConfigOption(const QString &configId, const QString &value);
    void refreshPermissionPresets();
    void refreshModelCatalog();
    void setPermissionPreset(const QString &value);
    void updateQueueItem(const QString &itemId, const QString &kind);
    void clearPendingAttachments();
    void pasteAttachment();
    void uploadAttachment(const QUrl &url);
    void clearChangedFiles();
    void killJob(const QString &jobId);
    void listFileReferences(const QString &prefix);

    void listDirectory(const QString &path);
    void toggleDirectory(const QString &path);
    void readFile(const QString &path);
    void goUpDirectory();

    void respondToPermission(const QString &requestId, bool allow);
    void answerQuestions(const QString &eventId, const QVariantList &answers);
    void goalAction(const QString &action);
    void editGoal(const QString &objective, int maxGoalRounds);

    /// Follow a session, and the parent seat's own two-argument form for a parent
    /// that is itself a child (the host answers `session/agent-busy` to a bare
    /// child id, which is why the mode travels with the address).
    void followSession(const QString &sessionId);
    void followSubagent(const QString &parentSessionId, const QString &childSessionId,
                        const QString &mode = QString());

    /// Follow the seat `parentSessionId()` names — the arrow's action. False when
    /// there is no parent seat on screen, so a click has nothing to do.
    bool followParentSession();

    void unarchiveSession(const QString &sessionId);

    void refreshScheduleTasks();
    void saveScheduleTask(const QString &id, const QString &title, const QString &prompt);
    void deleteScheduleTask(const QString &id);
    void loadScheduleHistory(const QString &id);
    void refreshPluginInventory();
    void setBundleEnabled(const QString &name, bool enabled);

    // ---- the pane clients, which are NOT the transport --------------------
    //
    // The ACP pane is a second, independent `AcpClient` with its own child and its
    // own transcript, and the headless pane is a `HeadlessClient` — one child per
    // task. Neither is the window's transport, so neither is reached through the
    // property binder above. Both are created on FIRST USE and owned by the
    // facade: their constructors resolve the `dsh` executable and start nothing,
    // and a window that never opens the pane must not pay for them.
    AcpClient *acpPane();
    HeadlessClient *headlessClient();

    /// Whether the header may offer the ACP toggle. False when the window's own
    /// transport IS ACP (`--transport acp`): a pane of the same protocol beside
    /// itself would be a second child for no second view. This is main.qml's own
    /// rule (`visible: dshTransport.transportName !== "acp"`).
    bool acpPaneAvailable() const;

signals:
    void availableChanged();
    void stateChanged();
    void statusChanged();
    void lastErrorChanged();
    void sessionIdChanged();
    void busyChanged();
    void turnOpenChanged();
    void pendingSessionChanged();
    void busyEnterChanged();

    void sessionsChanged();
    void sessionCountChanged();
    void pinnedCountChanged();
    void pendingInteractionSessionsChanged();
    void pendingInteractionCountChanged();
    void unreadSessionIdsChanged();
    void unreadCountChanged();
    void workspacesChanged();
    void workspaceCountChanged();
    void sessionsLoadingChanged();
    void openingSessionChanged();

    void parentSeatChanged();
    void permissionPresetChanged();
    void permissionPresetsChanged();
    void accessNoticeChanged();
    void configOptionsChanged();
    void modelNoticeChanged();
    void contextChanged();
    void contextSizeChanged();
    void jobsChanged();
    void liveJobCountChanged();

    void todosChanged();
    void goalChanged();
    void changedFilesChanged();
    void queueChanged();
    void queueCountChanged();
    void pendingAttachmentsChanged();
    void pendingAttachmentCountChanged();
    void fileCandidatesChanged();
    void fileEntriesChanged();
    void filePathChanged();
    void fileTextChanged();
    void fileSelectedPathChanged();
    void fileBytesChanged();
    void fileTruncatedChanged();
    void fileErrorChanged();

    void scheduleChanged();
    void scheduleHistoryChanged();
    void pluginsChanged();
    void trajectoryChanged();
    void transcriptChanged();
    void hasEarlierChanged();

    void permissionRequested(const QString &requestId, const QString &toolName,
                             const QString &detail);
    void permissionWithdrawn(const QString &requestId);
    void questionsRequested(const QString &eventId, const QVariantList &questions);

private Q_SLOTS:
    /// The recomputing half of the binder. A transport notify that feeds a derived
    /// value lands here, so the count and the forwarded list move together.
    void onStateNotify();
    void onSessionsNotify();
    void onSessionMarksNotify();
    void onWorkspacesNotify();
    void onJobsNotify();
    void onQueueNotify();
    void onAttachmentsNotify();
    void onSessionIdNotify();

private:
    // ---- the binder --------------------------------------------------------
    void bindAll();
    /// Connect one transport property's NOTIFY to one facade signal or slot, and
    /// record the property in m_missing when the transport does not publish it.
    /// Binding is a no-op for a constant property and a reported absence for a
    /// property the transport lacks.
    void bind(const char *property, const char *facadeSignal, const char *facadeSlot);
    /// The transport's method for `signature`, or an invalid QMetaMethod when it
    /// has none. Callers pass the exact signature so the two transports'
    /// differences are visible at the call site.
    QMetaMethod transportMethod(const char *signature) const;

    /// Look the method up and call it, with one report for either failure: a
    /// transport that has no such method, or one that refused the call. Every
    /// transport action below goes through here, so "the transport could not do
    /// it" is never a silent no-op in this class.
    bool call(const char *signature,
              QGenericReturnArgument result = QGenericReturnArgument(),
              QGenericArgument a0 = QGenericArgument(),
              QGenericArgument a1 = QGenericArgument(),
              QGenericArgument a2 = QGenericArgument()) const;

    void reportMissing(const QString &name);

    // ---- the readers -------------------------------------------------------
    //
    // One reader, converted at the getter. A property the transport does not have
    // reads as the empty value — and that is never silent: the constructor lists
    // it, and missingProperties() keeps the list for anything that wants to ask.
    QVariant read(const QString &name) const;
    QString readString(const QString &name) const;
    bool readBool(const QString &name) const;
    int readInt(const QString &name) const;
    QVariantList readList(const QString &name) const;

    // ---- derived values ----------------------------------------------------
    void refreshStateText();
    void refreshParentSeat();
    void recountSessions();
    void recountSessionMarks();
    void recountWorkspaces();
    void recountJobs();
    void recountQueue();
    void recountAttachments();

    QPointer<QObject> m_transport;
    QString m_transportName;
    QStringList m_missing;
    /// Notify-signature -> receiver-signature pairs already connected. Two
    /// transport properties can share one notify (unreadSessionIds and
    /// pendingInteractionSessions both ride `sessionMarksChanged`), and one slot
    /// serves both; without this the facade would emit twice for one change.
    QSet<QString> m_bound;

    QString m_stateText = QStringLiteral("idle");
    int m_sessionCount = 0;
    int m_pinnedCount = 0;
    int m_unreadCount = 0;
    int m_pendingInteractionCount = 0;
    int m_workspaceCount = 0;
    int m_liveJobCount = 0;
    int m_queueCount = 0;
    int m_pendingAttachmentCount = 0;
    QString m_parentSessionId;
    QString m_parentSessionTitle;

    bool m_hasSessionList = false;
    bool m_hasWorkspaces = false;
    bool m_hasPermissionCatalog = false;
    bool m_hasAttachmentStaging = false;
    bool m_hasFileSurface = false;
    bool m_hasTrajectory = false;
    bool m_hasGoalEditing = false;

    AcpClient *m_acpPane = nullptr;
    HeadlessClient *m_headless = nullptr;
};
