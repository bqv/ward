#include "HeadlessClient.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {

/// The child bounds every non-terminal string at 8 KiB and every line at
/// 32 KiB (see the package README). The request/preview we build from one
/// event is held to the same 8 KiB the ACP transport uses, so a tool card does
/// not grow a payload the child already called unrepresentable.
constexpr int PayloadCap = 8 * 1024;

/// A tool call's `input` as the card's IN half. The child sends a decoded JSON
/// value, but it keeps raw TEXT when the arguments do not round-trip (an
/// overflowing number), so a string is passed through rather than re-encoded.
QString readableInput(const QJsonValue &value)
{
    if (value.isObject()) {
        return QString::fromUtf8(
                   QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented))
            .trimmed();
    }
    if (value.isArray()) {
        return QString::fromUtf8(
                   QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented))
            .trimmed();
    }
    if (value.isString())
        return value.toString();
    return QString();
}

QString capped(const QString &text)
{
    if (text.size() <= PayloadCap)
        return text;
    return text.left(PayloadCap) + QChar(0x2026);
}

/// The first useful line of the child's stderr. In `--json` mode stderr keeps
/// only `dsh:` diagnostics, and a usage or loader failure is exactly the case
/// where stdout may carry nothing at all.
QString diagnosticLine(const QByteArray &bytes)
{
    const QString text = QString::fromUtf8(bytes).trimmed();
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        return trimmed.startsWith(QLatin1String("dsh: ")) ? trimmed.mid(5) : trimmed;
    }
    return QString();
}

} // namespace

HeadlessClient::HeadlessClient(QObject *parent)
    : QObject(parent)
{
    // The wrapper is resolved, not hardcoded: `dsh` has lived in a user bin dir
    // and in /usr/bin across installs, and a stale path fails as a child that
    // dies on the spot with nothing on screen to say why. Same candidate list as
    // AcpClient, for the same reason.
    const QStringList candidates{
        QStandardPaths::findExecutable(QStringLiteral("dsh")),
        QDir::homePath() + QStringLiteral("/bin/dsh"),
        QStringLiteral("/usr/bin/dsh"),
        QStringLiteral("/usr/local/bin/dsh"),
    };
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty() || !QFileInfo(candidate).isExecutable())
            continue;
        m_program = candidate;
        break;
    }
    if (m_program.isEmpty()) {
        qCritical() << "HeadlessClient: no `dsh` executable on PATH or in any of the"
                       " usual locations; the headless child cannot start";
    }

    // Session adoption refuses a conversation recorded in a different working
    // directory, so this is chosen once and reused by every child this object
    // starts. The reader's home is where a one-shot task's relative paths should
    // resolve.
    m_cwd = QDir::homePath();
}

HeadlessClient::~HeadlessClient()
{
    shutdown();
}

// --- lifecycle -------------------------------------------------------------

bool HeadlessClient::send(const QString &task)
{
    if (m_busy) {
        setStatus(QStringLiteral("one task at a time — this one is still running"));
        return false;
    }
    if (task.trimmed().isEmpty()) {
        setStatus(QStringLiteral("the task is empty"));
        return false;
    }
    if (m_program.isEmpty()) {
        appendError(QStringLiteral("no `dsh` executable could be found"));
        return false;
    }

    // Nothing from the previous run may leak into this one.
    shutdown();
    m_out.clear();
    m_err.clear();
    m_turnReason.clear();
    m_runError = false;
    m_stopRequested = false;
    m_truncated = false;

    // The prompt is a row before the child exists: a start that fails must not
    // leave the reader wondering whether their words were sent.
    m_transcript.append(QStringLiteral("user"), task);

    QStringList arguments{
        QStringLiteral("--profile"), QStringLiteral("headless"),
        QStringLiteral("--json"),
    };
    // The second task in this pane continues the first. The identity comes from
    // the child's own `session` event, never from a string guessed here.
    if (!m_sessionId.isEmpty())
        arguments << QStringLiteral("--session-id") << m_sessionId;

    m_process = new QProcess(this);
    m_process->setProgram(m_program);
    m_process->setArguments(arguments);
    m_process->setWorkingDirectory(m_cwd);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &HeadlessClient::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &HeadlessClient::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred,
            this, &HeadlessClient::onErrorOccurred);
    connect(m_process, &QProcess::finished,
            this, &HeadlessClient::onFinished);
    // The task goes in once the pipe exists. Writing before `started` is a write
    // to nothing; a pipe is also why the task is not a positional argument — the
    // child joins positional words, while a piped task arrives verbatim.
    connect(m_process, &QProcess::started, this, [this, task]() {
        if (!m_process)
            return;
        QByteArray payload = task.toUtf8();
        if (!payload.endsWith('\n'))
            payload.append('\n');
        m_process->write(payload);
        m_process->closeWriteChannel();
        setStatus(QStringLiteral("Deep diving…"));
    });

    qInfo() << "spawning headless child:" << m_program << arguments << "in" << m_cwd;
    setBusy(true);
    setStatus(QStringLiteral("starting the headless child…"));
    m_process->start();
    return true;
}

void HeadlessClient::stop()
{
    if (!m_process || m_process->state() == QProcess::NotRunning)
        return;

    m_stopRequested = true;
    setStatus(QStringLiteral("stopping…"));
    QPointer<QProcess> process = m_process;
    // SIGTERM is the ask; it is not a guarantee. The bounded wait is the event
    // loop's timer, and QProcess reaps the child on `finished` either way. The
    // guard is a QPointer, so a child that settled in those three seconds takes
    // its object with it instead of leaving this timer a dangling pointer.
    process->terminate();
    QTimer::singleShot(3000, this, [this, process]() {
        if (process && process.data() == m_process
                && process->state() != QProcess::NotRunning) {
            qWarning() << "HeadlessClient: the child ignored SIGTERM; killing it";
            process->kill();
        }
    });
}

void HeadlessClient::hibernate()
{
    // Minimising the pane: nothing it started may keep running while it is out
    // of sight, but the conversation (identity and rows) is exactly what
    // reopening is for, so it stays.
    stop();
}

void HeadlessClient::reset()
{
    shutdown();
    m_out.clear();
    m_err.clear();
    m_turnReason.clear();
    m_runError = false;
    m_stopRequested = false;
    m_truncated = false;

    if (!m_sessionId.isEmpty()) {
        m_sessionId.clear();
        emit sessionChanged();
    }
    m_transcript.clear();
    setLastError(QString());
    setStatus(QString());
}

void HeadlessClient::shutdown()
{
    if (!m_process)
        return;

    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);

    // Orderly close, then a bounded wait, then force. A wedged child must not
    // keep the window alive or outlive it.
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(2000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    process->deleteLater();
    setBusy(false);
}

// --- process output --------------------------------------------------------

void HeadlessClient::onReadyReadStandardOutput()
{
    if (!m_process)
        return;
    m_out.append(m_process->readAllStandardOutput());

    int newline = -1;
    while ((newline = m_out.indexOf('\n')) >= 0) {
        const QByteArray line = m_out.left(newline);
        m_out.remove(0, newline + 1);
        handleLine(line);
    }

    // One event line is capped at 32 KiB, newline included. A longer fragment
    // without a newline is not a line the reader can trust, so it is dropped
    // rather than grown without bound.
    if (m_out.size() > 64 * 1024) {
        qWarning() << "HeadlessClient: dropping an over-long stdout fragment";
        m_out.clear();
    }
}

void HeadlessClient::onReadyReadStandardError()
{
    if (!m_process)
        return;
    m_err.append(m_process->readAllStandardError());
    // Only one line is ever used, and it is the FIRST: the head of the stream
    // is kept so the line that explains the failure is not the part cut off.
    if (m_err.size() > 8 * 1024)
        m_err = m_err.left(8 * 1024);
}

void HeadlessClient::flushBuffer()
{
    if (m_out.isEmpty())
        return;
    const QByteArray rest = m_out;
    m_out.clear();
    if (!rest.trimmed().isEmpty())
        handleLine(rest);
}

void HeadlessClient::handleLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        // A partial line is flushed on exit; a genuinely broken one means the
        // stream and this reader disagree, which the exit code will report. Do
        // not invent a row for it.
        qWarning() << "HeadlessClient: unparsable event line:"
                   << error.errorString() << trimmed.left(200);
        return;
    }
    handleEvent(document.object());
}

void HeadlessClient::handleEvent(const QJsonObject &event)
{
    // The child flags any event it had to bound; one notice per run says so
    // without pretending the capped text was the whole payload.
    if (event.value(QStringLiteral("truncated")).toBool())
        m_truncated = true;

    const QString type = event.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("session")) {
        // The opening event. Its identity is what a second task adopts, and its
        // cwd is what the session recorded — the next child must run there or
        // adoption refuses the conversation.
        const QString identity = event.value(QStringLiteral("sessionId")).toString();
        const QString directory = event.value(QStringLiteral("cwd")).toString();
        bool changed = false;
        if (!identity.isEmpty() && identity != m_sessionId) {
            m_sessionId = identity;
            changed = true;
        }
        if (!directory.isEmpty() && directory != m_cwd) {
            m_cwd = directory;
            changed = true;
        }
        if (changed)
            emit sessionChanged();
        return;
    }

    if (type == QLatin1String("status")) {
        handleStatus(event);
        return;
    }

    if (type == QLatin1String("text")) {
        // One row per committed assistant message, which is what the stream
        // promises: a retried attempt never reaches it.
        const QString text = event.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            m_transcript.append(QStringLiteral("agent"), text);
        return;
    }

    if (type == QLatin1String("thinking")) {
        const QString text = event.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            m_transcript.append(QStringLiteral("thought"), text);
        return;
    }

    if (type == QLatin1String("tool_call")) {
        const QString callId = event.value(QStringLiteral("callId")).toString();
        const QString tool = event.value(QStringLiteral("tool")).toString();
        const QString request = capped(readableInput(event.value(QStringLiteral("input"))));
        // The call is keyed by `callId` so its result settles THIS row. A result
        // with no call would otherwise be a second row that says nothing about
        // what produced it.
        m_transcript.append(QStringLiteral("tool"), QString(),
                            tool.isEmpty() ? QStringLiteral("tool") : tool,
                            QStringLiteral("pending"), QString(), callId,
                            0, -1, request);
        return;
    }

    if (type == QLatin1String("tool_result")) {
        const QString callId = event.value(QStringLiteral("callId")).toString();
        // The event's own vocabulary is completed|error; the row vocabulary the
        // transcript delegate reads is pending|completed|failed.
        const QString status =
            event.value(QStringLiteral("status")).toString() == QLatin1String("error")
                ? QStringLiteral("failed")
                : QStringLiteral("completed");
        const QString result = event.value(QStringLiteral("result")).toString();
        if (!m_transcript.updateByKey(callId, status, result)) {
            // A call whose opening line never arrived (the line cap can drop
            // structured fields): still show the result rather than lose it.
            m_transcript.append(QStringLiteral("tool"), QString(),
                                QStringLiteral("tool"), status, result);
        }
        return;
    }

    if (type == QLatin1String("final")) {
        // The terminal answer, lossless and deliberately NOT capped. The last
        // committed `text` row usually carries the same words — replace it
        // rather than print the answer twice; if the row's copy was the capped
        // prefix, this is where the whole answer replaces it.
        const QString text = event.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            return;

        bool settled = false;
        const int count = m_transcript.rowCount();
        if (count > 0) {
            const QModelIndex last = m_transcript.index(count - 1, 0);
            const QString kind =
                m_transcript.data(last, AcpTranscript::KindRole).toString();
            const QString body =
                m_transcript.data(last, AcpTranscript::TextRole).toString();
            if (kind == QLatin1String("agent")) {
                if (body == text) {
                    settled = true;
                } else if (!body.isEmpty() && text.startsWith(body)) {
                    m_transcript.replaceLast(QStringLiteral("agent"), text);
                    settled = true;
                }
            }
        }
        if (!settled)
            m_transcript.append(QStringLiteral("agent"), text);
        return;
    }

    if (type == QLatin1String("error")) {
        // Raised OUTSIDE a turn: a usage error, an unusable --session-id, a
        // loader failure. The stream then ends without `final`.
        const QString message = event.value(QStringLiteral("message")).toString();
        appendError(message.isEmpty()
                        ? QStringLiteral("the headless child reported an error")
                        : message);
        return;
    }
}

void HeadlessClient::handleStatus(const QJsonObject &event)
{
    const QString phase = event.value(QStringLiteral("phase")).toString();
    const int turn = event.value(QStringLiteral("turn")).toInt(-1);

    if (phase == QLatin1String("turn_start")) {
        m_turnReason.clear();
        m_transcript.noteTurn(turn, true, QDateTime::currentMSecsSinceEpoch());
        // The main transcript's "Deep diving for Ns" is the turn's own line;
        // this is its cousin, in the pane's bar rather than as a row, because
        // there is no live attempt row here to hang it on.
        setStatus(QStringLiteral("Deep diving…"));
        return;
    }

    if (phase == QLatin1String("turn_end")) {
        m_transcript.noteTurn(turn, false);
        // A turn that fails IN-turn still ends with `final` and no `error`
        // event, so this reason — and the exit code — is the failure signal.
        const QJsonObject reason = event.value(QStringLiteral("reason")).toObject();
        m_turnReason = reason.value(QStringLiteral("kind")).toString();
        if (m_turnReason != QLatin1String("completed")) {
            QString message = reason.value(QStringLiteral("error")).toObject()
                                  .value(QStringLiteral("message")).toString();
            if (message.isEmpty()) {
                message = QStringLiteral("the turn ended: %1")
                              .arg(m_turnReason.isEmpty() ? QStringLiteral("unknown reason")
                                                          : m_turnReason);
            }
            appendError(message);
        } else {
            setStatus(QStringLiteral("completed"));
        }
        return;
    }

    // step_start / step_end exist to order the stream and carry usage; neither
    // is a row, and a step's token counts are not what this pane is for.
}

void HeadlessClient::onErrorOccurred(QProcess::ProcessError error)
{
    if (!m_process)
        return;
    // Only report a child that never came up here: a crash or a signal after
    // start is reported through finished(), and two stories for one failure is
    // worse than one.
    if (error != QProcess::FailedToStart)
        return;

    appendError(QStringLiteral("could not start %1 — %2")
                    .arg(m_program, m_process->errorString()));
    settle(false);
}

void HeadlessClient::onFinished(int exitCode, QProcess::ExitStatus status)
{
    // A failure path may already have settled this run (a child that never
    // started never emits finished, but a signal is not a contract about
    // ordering), and then there is nothing left to report twice.
    if (!m_process)
        return;

    // `finished` is not a promise that every readyRead was delivered first: drain
    // both pipes here so a last event written as the child exits is not lost.
    m_out.append(m_process->readAllStandardOutput());
    m_err.append(m_process->readAllStandardError());
    if (m_err.size() > 8 * 1024)
        m_err = m_err.left(8 * 1024);
    flushBuffer();

    const bool crashed = status == QProcess::CrashExit;
    bool ok = false;

    if (m_stopRequested) {
        appendNotice(QStringLiteral("Stopped."));
        setStatus(QStringLiteral("stopped"));
    } else if (crashed) {
        appendError(QStringLiteral("the headless child crashed"));
    } else if (exitCode != 0) {
        if (!m_runError) {
            const QString detail = diagnosticLine(m_err);
            QString message =
                QStringLiteral("the headless child exited with code %1").arg(exitCode);
            if (!detail.isEmpty())
                message += QStringLiteral(" — %1").arg(detail);
            else if (!m_turnReason.isEmpty() && m_turnReason != QLatin1String("completed"))
                message += QStringLiteral(" (the turn ended: %1)").arg(m_turnReason);
            appendError(message);
        }
    } else {
        ok = !m_runError;
        if (ok)
            setStatus(QStringLiteral("completed"));
    }

    if (m_truncated) {
        appendNotice(QStringLiteral("An event was cut at the child's 8 KiB string cap."));
    }

    settle(ok);
}

void HeadlessClient::settle(bool ok)
{
    if (m_process) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        process->deleteLater();
    }
    setBusy(false);
    emit runFinished(ok);
}

// --- rows and properties ---------------------------------------------------

void HeadlessClient::appendError(const QString &message)
{
    m_runError = true;
    m_transcript.append(QStringLiteral("error"), message);
    setLastError(message);
    setStatus(message);
}

void HeadlessClient::appendNotice(const QString &message)
{
    // Notices are the transcript's row for something that is not the model
    // talking: the producer is `headless`, the pane's own name.
    m_transcript.append(QStringLiteral("notice"), message,
                        QStringLiteral("headless"));
}

void HeadlessClient::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void HeadlessClient::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void HeadlessClient::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}
