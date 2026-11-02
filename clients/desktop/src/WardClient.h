/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

#include <cbor.h>

class AcpTranscript;
class QSocketNotifier;

class WardClient : public QObject
{
    Q_OBJECT

public:
    enum State { Idle = 0, Connecting = 1, Ready = 2, Failed = 3 };
    Q_ENUM(State)

    Q_PROPERTY(QString transportName READ transportName CONSTANT)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnOpenChanged)
    Q_PROPERTY(bool pendingSession READ pendingSession NOTIFY pendingSessionChanged)
    Q_PROPERTY(QString busyEnter READ busyEnter WRITE setBusyEnter NOTIFY busyEnterChanged)

    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList pendingInteractionSessions READ pendingInteractionSessions
               NOTIFY sessionMarksChanged)
    Q_PROPERTY(QVariantList unreadSessionIds READ unreadSessionIds NOTIFY sessionMarksChanged)
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(bool sessionsLoading READ sessionsLoading NOTIFY sessionsLoadingChanged)
    Q_PROPERTY(bool openingSession READ openingSession NOTIFY openingSessionChanged)

    Q_PROPERTY(QString permissionPreset READ permissionPreset NOTIFY permissionPresetChanged)
    Q_PROPERTY(QVariantList permissionPresets READ permissionPresets NOTIFY permissionPresetsChanged)
    Q_PROPERTY(QString accessNotice READ accessNotice NOTIFY accessNoticeChanged)
    Q_PROPERTY(QVariantList configOptions READ configOptions NOTIFY configOptionsChanged)
    Q_PROPERTY(QString modelNotice READ modelNotice NOTIFY modelNoticeChanged)
    Q_PROPERTY(int contextUsed READ contextUsed NOTIFY contextChanged)
    Q_PROPERTY(int contextSize READ contextSize NOTIFY contextSizeChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)

    Q_PROPERTY(QVariantList todos READ todos NOTIFY todosChanged)
    Q_PROPERTY(QVariantMap goal READ goal NOTIFY goalChanged)
    Q_PROPERTY(QVariantList changedFiles READ changedFiles NOTIFY changedFilesChanged)
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(QVariantList pendingAttachments READ pendingAttachments
               NOTIFY pendingAttachmentsChanged)
    Q_PROPERTY(QVariantList fileCandidates READ fileCandidates NOTIFY fileCandidatesChanged)
    Q_PROPERTY(QVariantList fileEntries READ fileEntries NOTIFY fileEntriesChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(QString fileText READ fileText NOTIFY fileTextChanged)
    Q_PROPERTY(QString fileSelectedPath READ fileSelectedPath NOTIFY fileSelectedPathChanged)
    Q_PROPERTY(int fileBytes READ fileBytes NOTIFY fileBytesChanged)
    Q_PROPERTY(bool fileTruncated READ fileTruncated NOTIFY fileTruncatedChanged)
    Q_PROPERTY(QString fileError READ fileError NOTIFY fileErrorChanged)

    Q_PROPERTY(QVariantList scheduleTasks READ scheduleTasks NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleLoading READ scheduleLoading NOTIFY scheduleChanged)
    Q_PROPERTY(QString scheduleNotice READ scheduleNotice NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleNoticeError READ scheduleNoticeError NOTIFY scheduleChanged)
    Q_PROPERTY(bool scheduleSaving READ scheduleSaving NOTIFY scheduleChanged)
    Q_PROPERTY(QVariantList scheduleHistory READ scheduleHistory NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(QString scheduleHistoryFor READ scheduleHistoryFor NOTIFY scheduleHistoryChanged)
    Q_PROPERTY(bool scheduleHistoryLoading READ scheduleHistoryLoading
               NOTIFY scheduleHistoryChanged)

    Q_PROPERTY(QVariantList pluginInventory READ pluginInventory NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsLoading READ pluginsLoading NOTIFY pluginsChanged)
    Q_PROPERTY(QString pluginsNotice READ pluginsNotice NOTIFY pluginsChanged)
    Q_PROPERTY(bool pluginsNoticeError READ pluginsNoticeError NOTIFY pluginsChanged)

    Q_PROPERTY(QVariantList trajectory READ trajectory NOTIFY trajectoryChanged)
    Q_PROPERTY(AcpTranscript *transcript READ transcript CONSTANT)
    Q_PROPERTY(bool hasEarlier READ hasEarlier NOTIFY hasEarlierChanged)
    Q_PROPERTY(bool loadingEarlier READ loadingEarlier NOTIFY hasEarlierChanged)

    Q_PROPERTY(bool terminalOpen READ terminalOpen NOTIFY terminalChanged)
    Q_PROPERTY(QString terminalId READ terminalId NOTIFY terminalChanged)
    Q_PROPERTY(int terminalCols READ terminalCols NOTIFY terminalSizeChanged)
    Q_PROPERTY(int terminalRows READ terminalRows NOTIFY terminalSizeChanged)

    explicit WardClient(const QString &socketPath, QObject *parent = nullptr);
    ~WardClient() override;

    QString transportName() const { return QStringLiteral("ward"); }

    State state() const { return m_state; }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    QString sessionId() const { return m_sessionId; }
    bool busy() const { return m_busy; }
    bool turnOpen() const { return m_turnOpen; }
    bool pendingSession() const { return m_pendingSession; }
    QString busyEnter() const { return m_busyEnter; }
    void setBusyEnter(const QString &mode);

    QVariantList sessions() const { return m_sessions; }
    QVariantList pendingInteractionSessions() const { return m_pendingInteractionSessions; }
    QVariantList unreadSessionIds() const { return m_unreadSessionIds; }
    QVariantList workspaces() const { return m_workspaces; }
    bool sessionsLoading() const { return m_sessionsLoading; }
    bool openingSession() const { return m_openingSession; }

    QString permissionPreset() const { return m_permissionPreset; }
    QVariantList permissionPresets() const { return m_permissionPresets; }
    QString accessNotice() const { return m_accessNotice; }
    QVariantList configOptions() const { return m_configOptions; }
    QString modelNotice() const { return m_modelNotice; }
    int contextUsed() const { return m_contextUsed; }
    int contextSize() const { return m_contextSize; }
    QVariantList jobs() const { return m_jobs; }

    QVariantList todos() const { return m_todos; }
    QVariantMap goal() const { return m_goal; }
    QVariantList changedFiles() const { return m_changedFiles; }
    QVariantList queue() const { return m_queue; }
    QVariantList pendingAttachments() const { return m_pendingAttachments; }
    QVariantList fileCandidates() const { return m_fileCandidates; }
    QVariantList fileEntries() const { return m_fileEntries; }
    QString filePath() const { return m_filePath; }
    QString fileText() const { return m_fileText; }
    QString fileSelectedPath() const { return m_fileSelectedPath; }
    int fileBytes() const { return m_fileBytes; }
    bool fileTruncated() const { return m_fileTruncated; }
    QString fileError() const { return m_fileError; }

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

    QVariantList trajectory() const { return m_trajectory; }
    AcpTranscript *transcript() const { return m_transcript; }
    bool hasEarlier() const { return false; }
    bool loadingEarlier() const { return m_loadingEarlier; }
    bool terminalOpen() const { return m_terminalOpen; }
    QString terminalId() const { return m_terminalId; }
    int terminalCols() const { return m_terminalCols; }
    int terminalRows() const { return m_terminalRows; }

    Q_INVOKABLE void connectToDaemon();
    Q_INVOKABLE void createSession();
    Q_INVOKABLE void createSessionNow();
    Q_INVOKABLE void createSessionInWorkspace(const QString &workspaceId);
    Q_INVOKABLE void followSession(const QString &sessionId);
    Q_INVOKABLE void sendPrompt(const QString &text);
    Q_INVOKABLE void sendPrompt(const QString &text, const QString &mode);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QStringList slashCommands() const;
    Q_INVOKABLE void runCommand(const QString &line);
    Q_INVOKABLE void respondToPermission(const QString &requestId, bool allow);
    Q_INVOKABLE void answerQuestions(const QString &eventId, const QVariantList &answers);
    Q_INVOKABLE void loadEarlierHistory();
    Q_INVOKABLE void refreshConfigOptions();
    Q_INVOKABLE void refreshPermissionPresets();
    Q_INVOKABLE void refreshModelCatalog();
    Q_INVOKABLE void setPermissionPreset(const QString &value);
    Q_INVOKABLE void setConfigOption(const QString &configId, const QString &value);
    Q_INVOKABLE void pinSession(const QString &sessionId);
    Q_INVOKABLE void unpinSession(const QString &sessionId);
    Q_INVOKABLE void archiveSession(const QString &sessionId);
    Q_INVOKABLE void unarchiveSession(const QString &sessionId);
    Q_INVOKABLE void renameSession(const QString &name, const QString &sessionId);
    Q_INVOKABLE void renameSession(const QString &name);
    Q_INVOKABLE void restartSelf();
    Q_INVOKABLE void goalAction(const QString &action);
    Q_INVOKABLE void killJob(const QString &jobId);
    Q_INVOKABLE void refreshScheduleTasks();
    Q_INVOKABLE void refreshPluginInventory();
    Q_INVOKABLE QStringList railExpandedSessions() const { return m_railExpanded; }
    Q_INVOKABLE QStringList railCollapsed() const { return m_railCollapsed; }
    Q_INVOKABLE void setRailExpandedSessions(const QStringList &entries);
    Q_INVOKABLE void setRailCollapsed(const QStringList &entries);
    Q_INVOKABLE void loadAttachment(const QString &path);
    Q_INVOKABLE QString attachmentUrl(const QString &ref) const;
    Q_INVOKABLE void deleteScheduleTask(const QString &id);
    Q_INVOKABLE void saveScheduleTask(const QString &id, const QString &title,
                                      const QString &prompt);
    Q_INVOKABLE void listFileReferences(const QString &prefix);
    Q_INVOKABLE void listDirectory(const QString &path);
    Q_INVOKABLE void toggleDirectory(const QString &path);
    Q_INVOKABLE void readFile(const QString &path);
    Q_INVOKABLE void goUpDirectory();
    Q_INVOKABLE void clearChangedFiles();
    Q_INVOKABLE void clearPendingAttachments();
    Q_INVOKABLE void pasteAttachment();
    Q_INVOKABLE void uploadAttachment(const QUrl &url);
    Q_INVOKABLE void updateQueueItem(const QString &itemId, const QString &kind);
    Q_INVOKABLE void editGoal(const QString &objective, int maxGoalRounds);
    Q_INVOKABLE void loadScheduleHistory(const QString &id);
    Q_INVOKABLE void setBundleEnabled(const QString &name, bool enabled);
    Q_INVOKABLE void replay(uint64_t sub, uint64_t from);
    Q_INVOKABLE void openTerminal(int cols, int rows);
    Q_INVOKABLE void writeTerminal(const QByteArray &data);
    Q_INVOKABLE void resizeTerminal(int cols, int rows);
    Q_INVOKABLE void closeTerminal();
    Q_INVOKABLE void readBlob(const QString &path, qint64 offset, qint64 len);

    Q_INVOKABLE void wardCall(const QString &method, const QVariantMap &params);
    Q_INVOKABLE QStringList protocolMethods() const;

signals:
    void stateChanged();
    void statusChanged();
    void lastErrorChanged();
    void sessionChanged();
    void busyChanged();
    void turnOpenChanged();
    void pendingSessionChanged();
    void busyEnterChanged();
    void sessionsChanged();
    void sessionMarksChanged();
    void workspacesChanged();
    void sessionsLoadingChanged();
    void openingSessionChanged();
    void permissionPresetChanged();
    void permissionPresetsChanged();
    void accessNoticeChanged();
    void configOptionsChanged();
    void modelNoticeChanged();
    void contextChanged();
    void contextSizeChanged();
    void jobsChanged();
    void todosChanged();
    void goalChanged();
    void changedFilesChanged();
    void queueChanged();
    void pendingAttachmentsChanged();
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
    void hasEarlierChanged();
    void permissionRequested(const QString &requestId, const QString &toolName,
                             const QString &detail);
    void permissionWithdrawn(const QString &requestId);
    void questionsRequested(const QString &eventId, const QVariantList &questions);
    void eventReceived(const QString &kind, const QVariantMap &payload);
    void railNotice(const QString &text, bool error);
    void terminalOutput(const QByteArray &data);
    void terminalState(const QString &state, int exitCode);
    void terminalChanged();
    void terminalSizeChanged();
    void blobRead(const QString &path, qint64 offset, const QByteArray &data,
                  const QString &error);

private slots:
    void onReadable();

private:
    struct Pending {
        QString method;
    };

    void setState(State state, const QString &status);
    void fail(const QString &message);
    void sendFrame(uint16_t type, uint64_t sub, uint64_t seq, cbor_item_t *body);
    void sendRequest(const QString &method, const QVariantMap &params);
    void sendRequest(const QString &method);
    void handleFrame(uint16_t type, uint64_t sub, uint64_t seq, const QByteArray &body);
    void handleResponse(const QString &method, bool ok, const QVariant &value,
                        const QVariantMap &error);
    void handleEvent(uint64_t sub, uint64_t seq, const QString &kind,
                     const QVariantMap &payload);
    void applySession(const QVariantMap &session);
    void setSessions(const QVariantList &sessions);
    void rebuildWorkspaces();
    void subscribe(const QString &sessionId);
    void doPrompt(const QString &text);
    void appendRow(const QString &kind, const QString &text, const QString &title = {},
                   const QString &status = {}, const QString &detail = {},
                   const QString &key = {}, int turn = -1);
    void addTrajectory(const QVariantMap &cell);
    void updateTrajectory(const QString &id, const QVariantMap &patch);
    void clearTranscript();

    QString m_socketPath;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_in;
    quint64 m_nextId = 1;
    QHash<quint64, Pending> m_pending;
    QHash<QString, quint64> m_subBySession;
    QString m_subscribedSession;
    QString m_subscribing;
    quint64 m_sub = 0;
    quint64 m_lastSeq = 0;
    bool m_handshaken = false;

    State m_state = Idle;
    QString m_status;
    QString m_lastError;
    QString m_sessionId;
    bool m_busy = false;
    bool m_turnOpen = false;
    bool m_pendingSession = false;
    QString m_busyEnter = QStringLiteral("queue");
    int m_turn = -1;

    QVariantList m_sessions;
    QVariantList m_pendingInteractionSessions;
    QVariantList m_unreadSessionIds;
    QVariantList m_workspaces;
    bool m_sessionsLoading = false;
    bool m_openingSession = false;

    QString m_permissionPreset;
    QVariantList m_permissionPresets;
    QString m_accessNotice;
    QVariantList m_configOptions;
    QString m_modelNotice;
    int m_contextUsed = 0;
    int m_contextSize = 0;
    QVariantList m_jobs;

    QVariantList m_todos;
    QVariantMap m_goal;
    QVariantList m_changedFiles;
    QVariantList m_queue;
    QVariantList m_pendingAttachments;
    QVariantList m_fileCandidates;
    QVariantList m_fileEntries;
    QString m_filePath;
    QString m_fileText;
    QString m_fileSelectedPath;
    int m_fileBytes = 0;
    bool m_fileTruncated = false;
    QString m_fileError;

    QVariantList m_scheduleTasks;
    bool m_scheduleLoading = false;
    QString m_scheduleNotice;
    bool m_scheduleNoticeError = false;
    bool m_scheduleSaving = false;
    QVariantList m_scheduleHistory;
    QString m_scheduleHistoryFor;
    bool m_scheduleHistoryLoading = false;

    QVariantList m_pluginInventory;
    bool m_pluginsLoading = false;
    QString m_pluginsNotice;
    bool m_pluginsNoticeError = false;

    QVariantList m_trajectory;
    AcpTranscript *m_transcript = nullptr;
    bool m_loadingEarlier = false;

    QStringList m_railExpanded;
    QStringList m_railCollapsed;

    QString m_createCwd;
    QString m_blobPath;
    qint64 m_blobOffset = 0;
    qint64 m_blobLen = 0;
    bool m_terminalOpen = false;
    QString m_terminalId;
    int m_terminalCols = 0;
    int m_terminalRows = 0;

    QString m_queuedPrompt;
    QString m_pendingApproval;
};
