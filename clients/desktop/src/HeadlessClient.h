#pragma once

// HeadlessClient — one `dsh --profile headless` child, rendered as a pane.
//
// This is deliberately NOT a third transport. A transport speaks to a live
// session that outlives a turn; this object runs one task per child, exactly as
// the command line does, and the child exits by itself when the task settles.
// main.qml therefore keeps binding `dshTransport` to the two real transports and
// reaches this object only from the headless pane.
//
// The contract it implements is the child's own, read from
//   /usr/lib64/node_modules/@deepseek-ai/dsh/node_modules/@deepseek-ai/dsh-headless
// (`--json` writes one JSON object per line on stdout; `--session-id` adopts a
// persisted session; exit 0 means the turn completed and 1 means it did not).
// The event field names are not guessable, so they are named once, in
// HeadlessClient::handleEvent:
//
//   session      {sessionId, cwd}
//   status       {phase: turn_start | step_start | step_end | turn_end,
//                 turn, step?, reason?, usage?}
//   text         {text}
//   thinking     {text}
//   tool_call    {callId, tool, input}
//   tool_result  {callId, status: "completed" | "error", result}
//   final        {text}
//   error        {message}
//   truncated    true on any line the child had to bound
//
// Rows are AcpTranscript rows — the same model the ACP transport fills — rather
// than a second vocabulary that would drift from it. That is why this file
// includes AcpClient.h: the model IS the shared transcript vocabulary, used by
// a different producer.

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "AcpClient.h"

class HeadlessClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)
    Q_PROPERTY(QString cwd READ cwd NOTIFY sessionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(AcpTranscript *transcript READ transcript CONSTANT)

public:
    explicit HeadlessClient(QObject *parent = nullptr);
    ~HeadlessClient() override;

    bool busy() const { return m_busy; }
    QString sessionId() const { return m_sessionId; }
    QString cwd() const { return m_cwd; }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    AcpTranscript *transcript() { return &m_transcript; }

    /// Run one task on a fresh child. The task goes in on stdin rather than as a
    /// positional argument: the child joins its words with spaces, while a piped
    /// task is sent verbatim, and stdin has no argument-length or leading-dash
    /// trap.
    ///
    /// Returns false when nothing was started — a task is already running, the
    /// task is blank, or no `dsh` could be found. On success the child's own
    /// `session` event decides the identity the NEXT task continues.
    Q_INVOKABLE bool send(const QString &task);

    /// Ask the child to stop: SIGTERM first, then SIGKILL after a bounded wait.
    /// QProcess reaps the child on `finished`, so nothing is left behind. The
    /// run is recorded as a notice, not an error: the reader did it.
    Q_INVOKABLE void stop();

    /// Put the child away and keep the conversation: the session id and every
    /// row stay, so reopening the pane continues where this left off. Used by
    /// the pane's minimise exit.
    Q_INVOKABLE void hibernate();

    /// Shut everything: stop the child, forget the session id and clear the
    /// rows. The pane's close exit, and the only exit that frees the
    /// conversation.
    Q_INVOKABLE void reset();

    /// Stop and reap without touching the rows or the session identity. The
    /// destructor calls it so a quit cannot orphan the child.
    Q_INVOKABLE void shutdown();

signals:
    void busyChanged();
    void sessionChanged();
    void statusChanged();
    void lastErrorChanged();
    /// `ok` is the child's own verdict: exit 0 AND a completed turn.
    void runFinished(bool ok);

private:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);
    void handleLine(const QByteArray &line);
    void handleEvent(const QJsonObject &event);
    void handleStatus(const QJsonObject &event);
    /// Flush a last line that arrived without its newline (a cut stream).
    void flushBuffer();
    void appendError(const QString &message);
    void appendNotice(const QString &message);
    void setStatus(const QString &status);
    void setBusy(bool busy);
    void setLastError(const QString &error);
    /// End the current run's bookkeeping, once, whichever way it ended.
    void settle(bool ok);

    QProcess *m_process = nullptr;
    /// stdout split on newlines. One event line is capped at 32 KiB by the
    /// child, so anything much larger is not a line this reader can trust.
    QByteArray m_out;
    /// stderr is diagnostics only in `--json` mode; kept bounded for the one
    /// line an error row needs when the stream carried no `error` event.
    QByteArray m_err;

    QString m_program;
    /// The directory every child runs in. Adoption refuses a session recorded
    /// in another working directory, so this is fixed for the object's life
    /// (the child's own `session` event can correct it to what was recorded).
    QString m_cwd;
    QString m_sessionId;
    QString m_status;
    QString m_lastError;

    AcpTranscript m_transcript;

    bool m_busy = false;
    /// The last turn/end kind, so a zero-information exit can still say why.
    QString m_turnReason;
    /// An error row was already written for this run.
    bool m_runError = false;
    /// The reader asked for this stop, so the exit is not a failure.
    bool m_stopRequested = false;
    /// The child bounded at least one event; said once, at the end of the run.
    bool m_truncated = false;
};
