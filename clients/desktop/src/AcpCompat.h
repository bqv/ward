// The part of the transport surface that ACP has no notion of.
//
// The window binds every transport to one name (`dshTransport`), so a property
// that exists on one and not the other is a TypeError per binding, on load. When
// this client grew a dock, a search box, a settings panel and attachments, all of
// it went into DshClient and none into AcpClient — 62 TypeErrors, and an ACP
// window that could not draw. This is the missing surface.
//
// Deliberately not an abstract interface and deliberately not implemented. ACP
// cannot list sessions, browse a workspace, open a PTY or watch files: the host
// never offers it those. Empty containers and no-ops are the honest
// representation, so under ACP a feature is absent rather than erroring.

#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

/// Transport-agnostic, so it lives beside the base rather than in either client.
/// Rail state that has to survive a restart because nothing can re-derive it:
/// which Workspace groups the reader folded away. Keyed by the host's own ids.
/// Client preferences that are not the host's business. Stored beside the
/// window geometry, shared by both transports: a preference about how Enter
/// behaves cannot depend on which transport happens to be running.
namespace Prefs {
/// "queue" or "steer" — what Enter does while a turn is already running.
QString busyEnter();
void setBusyEnter(const QString &mode);
}

namespace RailState {
QStringList collapsed();
void setCollapsed(const QStringList &ids);
/// Session expansion as "<0|1>:<sessionId>" entries; absent means nobody said.
QStringList expanded();
void setExpanded(const QStringList &entries);
}

/// The slash commands this client knows, with their leading `/`.
///
/// One table rather than a copy in each transport and a third in QML: the
/// composer's completion list, the refusal of a name the client cannot run, and
/// the transports' own `slashCommands()` all read this. It is deliberately
/// short — only what the client actually drives (`/compact` for manual
/// compaction, `/permission` behind the access chip) — because a name invented
/// here would be a promise the host has to keep. `/permission` is the one the
/// host contributes (the permission-presets plugin's own command), which is why
/// it is not this client's invention; `commands/execute` remains the authority
/// on what the host accepts, and anything absent is refused with a notice rather
/// than carried to the model as prose.
namespace SlashCommands {
QStringList names();
/// True when `line`'s command word (the first token) is one of `names()`.
bool isKnown(const QString &line);
}   // namespace SlashCommands

class AcpCompat : public QObject
{
    Q_OBJECT

public:
    /// Which transport this is. QML names no transport class, so this is how a
    /// control that only one of them can serve says so (`Plugins…`, the ACP
    /// pane's header toggle) without binding to a class name.
    Q_PROPERTY(QString transportName READ transportName CONSTANT)
    QString transportName() const { return QStringLiteral("acp"); }

    /// The plan / todo list. ACP carries it as a `plan` update, and DSH's ACP
    /// bridge carries the same list as the arguments of its `todo_write` call;
    /// AcpClient is where both are read into this property.
    Q_PROPERTY(QVariantList todos MEMBER m_todos NOTIFY todosChanged)
    /// Queued input. ACP has no inbox: the agent never reports a prompt it has
    /// accepted but not consumed, and this client stages nothing behind a running
    /// turn, so under ACP `queue` is empty — there is no wire fact to put here.
    Q_PROPERTY(QVariantList queue MEMBER m_queue NOTIFY queueChanged)
    Q_PROPERTY(QVariantList jobs MEMBER m_jobs NOTIFY jobsChanged)
    /// The Automation task catalog. ACP has no schedule namespace at all and the
    /// host's tasks are not on this wire, so these stay empty and every action
    /// is a no-op; they exist because the QML page binds one `dshTransport` and
    /// a missing property is a TypeError per binding rather than a fallback.
    Q_PROPERTY(QVariantList scheduleTasks MEMBER m_scheduleTasks NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleLoading MEMBER m_scheduleLoading NOTIFY scheduleChanged)
    Q_PROPERTY(QString scheduleNotice MEMBER m_scheduleNotice NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleNoticeError MEMBER m_scheduleNoticeError NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleSaving MEMBER m_scheduleSaving NOTIFY scheduleChanged)
    Q_PROPERTY(QVariantList scheduleHistory MEMBER m_scheduleHistory NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(QString scheduleHistoryFor MEMBER m_scheduleHistoryFor NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(bool scheduleHistoryLoading MEMBER m_scheduleHistoryLoading NOTIFY scheduleHistoryChanged)
    /// The host's plugin inventory. ACP carries neither a Loader tree nor a
    /// Remote surface, so this is empty rather than a guess.
    Q_PROPERTY(QVariantList pluginInventory MEMBER m_pluginInventory NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsLoading MEMBER m_pluginsLoading NOTIFY pluginsChanged)
    Q_PROPERTY(QString pluginsNotice MEMBER m_pluginsNotice NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsNoticeError MEMBER m_pluginsNoticeError NOTIFY pluginsChanged)
    Q_PROPERTY(QVariantList fileCandidates MEMBER m_fileCandidates NOTIFY fileCandidatesChanged)
    Q_PROPERTY(QVariantList permissionPresets MEMBER m_permissionPresets NOTIFY permissionPresetsChanged)
    /// Files staged before sending. ACP carries an attachment only as prompt
    /// content at send time and this client stages nothing, so uploadAttachment
    /// and pasteAttachment are no-ops here and this list stays empty rather than
    /// claiming a file is waiting to be sent.
    Q_PROPERTY(QVariantList pendingAttachments MEMBER m_pendingAttachments NOTIFY pendingAttachmentsChanged)
    Q_PROPERTY(QVariantList settingsSections MEMBER m_settingsSections NOTIFY settingsSectionsChanged)
    Q_PROPERTY(QVariantList searchResults MEMBER m_searchResults NOTIFY searchResultsChanged)
    Q_PROPERTY(QVariantList fileEntries MEMBER m_fileEntries NOTIFY fileEntriesChanged)
    Q_PROPERTY(QVariantList workspaces MEMBER m_workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(QVariantList sessions MEMBER m_sessions NOTIFY sessionsChanged)
    /// The rail's two marks. ACP does have an approval — its bridge carries
    /// `session/request_permission` — but it has no question waterfall and no
    /// session roster: its "sessions" list is empty, so a row to mark does not
    /// exist on this transport. ACP also never pulls a background roster, so no
    /// session can finish "while you were not looking". Both stay empty: absence
    /// reads as "nothing marked", which is the truth here, and the property exists
    /// so the rail's binding is not a TypeError on load.
    Q_PROPERTY(QVariantList pendingInteractionSessions MEMBER m_pendingInteractionSessions
               NOTIFY sessionMarksChanged)
    Q_PROPERTY(QVariantList unreadSessionIds MEMBER m_unreadSessionIds
               NOTIFY sessionMarksChanged)
    /// Files this session has changed. ACP has no workspace watch, so AcpClient
    /// derives the list from the completed tool calls that name a path.
    Q_PROPERTY(QStringList changedFiles MEMBER m_changedFiles NOTIFY changedFilesChanged)
    /// The active goal. ACP has no goal projection; AcpClient decodes the goal
    /// tools' own result, which is where the phase and round count appear.
    Q_PROPERTY(QVariantMap goal MEMBER m_goal NOTIFY goalChanged)
    Q_PROPERTY(QString permissionPreset MEMBER m_permissionPreset NOTIFY permissionPresetChanged)
    /// The strip's chip notices. ACP has no permission catalog and no host
    /// command surface, so these stay empty unless a call is refused here; they
    /// exist because the QML reads them on whichever transport is bound, and a
    /// missing property is a TypeError per binding rather than a fallback.
    Q_PROPERTY(QString accessNotice MEMBER m_accessNotice NOTIFY accessNoticeChanged)
    Q_PROPERTY(QString modelNotice MEMBER m_modelNotice NOTIFY modelNoticeChanged)
    Q_PROPERTY(QString searchQuery MEMBER m_searchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QString filePath MEMBER m_filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString fileText MEMBER m_fileText NOTIFY fileTextChanged)
    Q_PROPERTY(QString fileSelectedPath MEMBER m_fileSelectedPath NOTIFY fileSelectedPathChanged)
    Q_PROPERTY(QString fileError MEMBER m_fileError NOTIFY fileErrorChanged)
    Q_PROPERTY(QString terminalId MEMBER m_terminalId NOTIFY terminalIdChanged)
    Q_PROPERTY(bool hasEarlier MEMBER m_hasEarlier NOTIFY hasEarlierChanged)
    Q_PROPERTY(bool loadingEarlier MEMBER m_loadingEarlier NOTIFY loadingEarlierChanged)
    Q_PROPERTY(bool sessionsLoading MEMBER m_sessionsLoading NOTIFY sessionsLoadingChanged)
    Q_PROPERTY(bool openingSession MEMBER m_openingSession NOTIFY openingSessionChanged)
    Q_PROPERTY(bool terminalOpen MEMBER m_terminalOpen NOTIFY terminalOpenChanged)
    Q_PROPERTY(bool fileTruncated MEMBER m_fileTruncated NOTIFY fileTruncatedChanged)
    Q_PROPERTY(bool searchLoading MEMBER m_searchLoading NOTIFY searchLoadingChanged)
    Q_PROPERTY(bool searchHasMore MEMBER m_searchHasMore NOTIFY searchHasMoreChanged)
    Q_PROPERTY(int attachmentsRevision MEMBER m_attachmentsRevision NOTIFY attachmentsRevisionChanged)
    Q_PROPERTY(int fileBytes MEMBER m_fileBytes NOTIFY fileBytesChanged)
    Q_PROPERTY(QAbstractItemModel *trajectory MEMBER m_trajectory CONSTANT)

public:
    using QObject::QObject;

    Q_INVOKABLE void loadEarlierHistory() {}
    Q_INVOKABLE void searchSessions(const QString &) {}
    Q_INVOKABLE void refreshPermissionPresets() {}
    /// ACP has no permission catalog and no `commands/execute`, so a preset
    /// cannot be switched on this transport. The chip is hidden there (no
    /// catalog, no current value), so the refusal is recorded for any caller
    /// that does reach it instead of returning silently.
    Q_INVOKABLE void setPermissionPreset(const QString &value)
    {
        if (value.isEmpty())
            return;
        m_accessNotice = QStringLiteral("this transport cannot change permission presets");
        emit accessNoticeChanged();
    }

    /// What the composer's `/` completion offers, and what a typed command is
    /// matched against. Both transports answer with the same table, so the list
    /// does not change when the fallback is in use — see SlashCommands::names().
    Q_INVOKABLE QStringList slashCommands() const { return SlashCommands::names(); }

    /// ACP has no `commands/execute`, so this is overridden in AcpClient to say
    /// so in the transcript rather than let a typed command vanish. Virtual for
    /// that reason alone; the base stays a no-op for anything else that derives
    /// from it.
    Q_INVOKABLE virtual void runCommand(const QString &) {}

    Q_INVOKABLE void refreshSettings() {}
    Q_INVOKABLE void setSetting(const QString &, const QString &, const QVariant &, int) {}
    Q_INVOKABLE void uploadAttachment(const QUrl &) {}
    Q_INVOKABLE void pasteAttachment() {}
    Q_INVOKABLE void clearPendingAttachments() {}
    Q_INVOKABLE void refreshJobs() {}
    Q_INVOKABLE void killJob(const QString &) {}
    // Automation tasks and the plugin inventory are Remote namespaces ACP has no
    // notion of: the containers above stay empty and these do nothing. The page
    // says so from `transportName`, rather than pretending an empty list is a
    // catalog with no tasks.
    Q_INVOKABLE void refreshScheduleTasks() {}
    Q_INVOKABLE void saveScheduleTask(const QString &, const QString &, const QString &) {}
    Q_INVOKABLE void deleteScheduleTask(const QString &) {}
    Q_INVOKABLE void loadScheduleHistory(const QString &) {}
    Q_INVOKABLE void refreshPluginInventory() {}
    // The per-session actions take the id LAST and may omit it — the same shape
    // as DshClient's, because QML binds to whichever transport is selected and a
    // call the DSH client accepts must not throw here. ACP has no workspace, so
    // these stay no-ops; the point is that the signature matches.
    Q_INVOKABLE void forkSession(const QString & = QString()) {}
    /// A goal's Pause/Resume/×. ACP has no request that changes a goal, but the
    /// row can be rendered — AcpClient decodes the goal from the agent's goal
    /// tools — so AcpClient overrides this to say the transport cannot steer it
    /// rather than returning silently.
    Q_INVOKABLE virtual void goalAction(const QString &);
    Q_INVOKABLE void listFileReferences(const QString &) {}
    /// Virtual because the list is not always empty: AcpClient derives it and is
    /// the only transport that has to be able to empty it from the dock's ×.
    Q_INVOKABLE virtual void clearChangedFiles();
    Q_INVOKABLE void updateQueueItem(const QString &, const QString &, const QString & = QString()) {}
    Q_INVOKABLE void loadAttachment(const QString &) {}
    Q_INVOKABLE void answerQuestions(const QString &, const QVariantList &) {}
    Q_INVOKABLE void renameSession(const QString &, const QString & = QString()) {}
    Q_INVOKABLE void archiveSession(const QString & = QString()) {}
    Q_INVOKABLE void unarchiveSession(const QString & = QString()) {}
    Q_INVOKABLE void pinSession(const QString & = QString()) {}
    Q_INVOKABLE void unpinSession(const QString & = QString()) {}
    /// The rail's two Workspace controls. ACP has no Workspace registry and no
    /// directory chooser at all: `workspaces` stays empty and either call would be
    /// a promise this transport cannot keep. They exist so the controls bind
    /// under whichever transport is selected, and each answers through
    /// `railNotice` rather than returning in silence — the honest form of "no".
    Q_INVOKABLE void createWorkspaceFromPicker();
    Q_INVOKABLE void createSessionInWorkspace(const QString &workspaceId);
    Q_INVOKABLE void refreshSessions() {}
    Q_INVOKABLE void followSession(const QString &) {}
    Q_INVOKABLE void followSubagent(const QString &, const QString &, const QString & = QString()) {}
    Q_INVOKABLE void openTerminal(int cols = 80, int rows = 24) {}
    Q_INVOKABLE void listDirectory(const QString &) {}
    Q_INVOKABLE void toggleDirectory(const QString &) {}
    Q_INVOKABLE void readFile(const QString &) {}
    Q_INVOKABLE void goUpDirectory() {}
    Q_INVOKABLE QString attachmentUrl(const QString &) const { return QString(); }

    /// Replace this process with a freshly started copy of itself — the same
    /// argv, a new image. See DshClient::restartSelf.
    Q_INVOKABLE void restartSelf();

    /// Folded Workspace groups, as a list of group keys. Empty is a valid answer
    /// and means "nothing folded", which is also where a fresh install starts.
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

    /// Whether a turn may be open, so the composer can choose between queue and
    /// steer from evidence rather than from the optimistic `busy` flag.
    ///
    /// The base answer is "no": AcpCompat is the shared surface, and a transport
    /// that cannot tell must not claim a turn. AcpClient overrides this with its
    /// transcript's own latch, which is what the existing `turnOpen` property on
    /// AcpTranscript already tracks. The name is on the facade because QML binds
    /// one `dshTransport`, and a missing property there is a TypeError per
    /// binding rather than a fallback.
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnOpenChanged)
    virtual bool turnOpen() const { return false; }

    /// Whether the composer is holding a new session that does not exist yet.
    ///
    /// The base answer is "no", and on ACP it always is: an ACP conversation is
    /// created in the child this client started, so there is no host roster for
    /// an eager create to litter and nothing to defer. The name is on the facade
    /// because QML binds one `dshTransport`, and a missing property there is a
    /// TypeError per binding rather than a fallback.
    Q_PROPERTY(bool pendingSession READ pendingSession NOTIFY pendingSessionChanged)
    virtual bool pendingSession() const { return false; }

signals:
    void todosChanged();
    void queueChanged();
    void jobsChanged();
    void fileCandidatesChanged();
    void permissionPresetsChanged();
    void pendingAttachmentsChanged();
    void settingsSectionsChanged();
    void searchResultsChanged();
    void fileEntriesChanged();
    void workspacesChanged();
    void sessionsChanged();
    void sessionMarksChanged();
    void changedFilesChanged();
    void goalChanged();
    void permissionPresetChanged();
    void accessNoticeChanged();
    void modelNoticeChanged();
    void searchQueryChanged();
    void filePathChanged();
    void fileTextChanged();
    void fileSelectedPathChanged();
    void fileErrorChanged();
    void terminalIdChanged();
    void hasEarlierChanged();
    void loadingEarlierChanged();
    void sessionsLoadingChanged();
    void openingSessionChanged();
    void terminalOpenChanged();
    void fileTruncatedChanged();
    void searchLoadingChanged();
    void searchHasMoreChanged();
    void attachmentsRevisionChanged();
    void fileBytesChanged();
    /// The host asks the agent a question, and withdraws an approval. ACP does
    /// neither, but the QML connects to both and an unmatched handler is a
    /// warning plus a dead dialog.
    void busyEnterChanged();
    void scheduleChanged();
    void scheduleHistoryChanged();
    void pluginsChanged();
    /// turnOpen() changed — see its declaration. Emitted by AcpClient when the
    /// transcript's turn latch moves, and never by the base, which has no latch.
    void turnOpenChanged();
    /// pendingSession() changed. Never emitted by ACP, which has no seat to take;
    /// the signal exists so the window's handler is one shape on both transports.
    void pendingSessionChanged();
    /// A prompt typed into a deferred seat could not be delivered. ACP has no
    /// seat, so it never emits this; the window binds it either way.
    void pendingPromptDropped(const QString &text, const QString &reason);
    void questionsRequested(const QString &eventId, const QVariantList &questions);
    void permissionWithdrawn(const QString &requestId);
    /// One line for the rail's notice surface, already in the voice the rail
    /// shows it in. `isError` true is a refusal — something a person pressed that
    /// the transport could not do; false is a fact worth saying after a control
    /// that did act. The ACP transport uses it for the two Workspace controls it
    /// has no API for; DshClient uses it for the picker and create answers and
    /// for their failures. Both transports carry it because the rail binds one
    /// name, `dshTransport`.
    void railNotice(const QString &text, bool isError);

protected:
    /// The five the ACP transport can fill from its own wire, so they are written
    /// by AcpClient rather than only ever being the empty defaults. Everything
    /// below stays private: a property nobody can populate has no writer.
    QVariantList m_todos;
    QVariantList m_queue;
    QVariantList m_pendingAttachments;
    QStringList m_changedFiles;
    QVariantMap m_goal;

private:
    QVariantList m_jobs;
    QVariantList m_scheduleTasks;
    QString m_scheduleNotice;
    QVariantList m_scheduleHistory;
    QString m_scheduleHistoryFor;
    QVariantList m_pluginInventory;
    QString m_pluginsNotice;
    QVariantList m_fileCandidates;
    QVariantList m_permissionPresets;
    QVariantList m_settingsSections;
    QVariantList m_searchResults;
    QVariantList m_fileEntries;
    QVariantList m_workspaces;
    QVariantList m_sessions;
    QVariantList m_pendingInteractionSessions;
    QVariantList m_unreadSessionIds;
    QString m_permissionPreset;
    QString m_accessNotice;
    QString m_modelNotice;
    QString m_searchQuery;
    QString m_filePath;
    QString m_fileText;
    QString m_fileSelectedPath;
    QString m_fileError;
    QString m_terminalId;
    bool m_hasEarlier = false;
    bool m_loadingEarlier = false;
    bool m_sessionsLoading = false;
    bool m_openingSession = false;
    bool m_terminalOpen = false;
    bool m_fileTruncated = false;
    bool m_searchLoading = false;
    bool m_searchHasMore = false;
    bool m_scheduleLoading = false;
    bool m_scheduleSaving = false;
    bool m_scheduleNoticeError = false;
    bool m_scheduleHistoryLoading = false;
    bool m_pluginsLoading = false;
    bool m_pluginsNoticeError = false;
    int m_attachmentsRevision = 0;
    int m_fileBytes = 0;
    QAbstractItemModel *m_trajectory = nullptr;
};
