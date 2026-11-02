#include "DshClient.h"
#include "AcpCompat.h"
#include "BrowserAuth.h"

#include <QDebug>
#include <functional>

#include <QAbstractSocket>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>
#include <QSettings>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QJsonValue>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QDir>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

namespace {

/// The mux path; every logical stream rides this one socket.
constexpr auto MUX_PATH = "/api/remote.mux";

/// Retry schedule. The first attempt is close, because a host that restarted is
/// usually back in a second; doubling to a ceiling keeps a host that is gone for
/// good from being hammered once a second forever.
constexpr int RECONNECT_BASE_MS = 1000;
constexpr int RECONNECT_CAP_MS = 30000;
/// How long a socket must stay up before the schedule is believed reset. The
/// gateway's own heartbeat is two seconds and terminates after two missed pings,
/// so anything up for this long has answered many of them; a flap that comes back
/// for five seconds has not "recovered" and must keep its rising delay.
constexpr int RECONNECT_STABLE_MS = 30000;
/// Interval of the self-sampling timer that measures event-loop lag. Short
/// enough that a starvation of a few seconds shows as a large gap, cheap enough
/// that it is not itself load.
constexpr int LOOP_LAG_SAMPLE_MS = 500;

/// Jitter one delay by ±20%. A hundred clients that all reconnect on the same
/// second would otherwise arrive together; more importantly, a client whose
/// event loop is starved reconnects at a moment that has nothing to do with the
/// host's state, and the spread is what keeps one drop from becoming a rhythm.
int reconnectDelayWithJitter(int base)
{
    const int spread = qMax(1, base / 5);
    return base + QRandomGenerator::global()->bounded(2 * spread + 1) - spread;
}

/// What a close code means here, in words.
///
/// Qt's QWebSocket starts with closeCode 1000 and only overwrites it when a close
/// frame passes in either direction (QWebSocketPrivate's constructor sets
/// CloseCodeNormal; processClose calls close(code) for the peer's frame). So an
/// abrupt TCP drop — a `socket.terminate()` on the host, a killed carrier, a
/// reset — is reported as 1000 with an empty reason and RemoteHostClosedError.
/// The gateway never calls close(1000) anywhere: its own closes are 1001 (peer
/// left), 1003 (binary frame), 1008 (bad request) and 1011 (failure). A 1000 with
/// no reason therefore identifies a *termination*, not a close, and its most
/// likely author is the mux heartbeat after two unanswered pings.
QString describeMuxClose(int closeCode, const QString &reason,
                         QAbstractSocket::SocketError error)
{
    const bool abrupt = error == QAbstractSocket::RemoteHostClosedError
                     && closeCode == QWebSocketProtocol::CloseCodeNormal
                     && reason.isEmpty();
    if (abrupt) {
        return QStringLiteral("no close frame: the peer terminated the TCP connection, "
                              "which Qt reports as its default code 1000 (the gateway's "
                              "heartbeat calls socket.terminate() after two missed pings; "
                              "its clean closes are 1001/1003/1008/1011)");
    }
    if (!reason.isEmpty())
        return QStringLiteral("close frame %1: %2").arg(closeCode).arg(reason);
    return QStringLiteral("close code %1 with no reason given").arg(closeCode);
}

/// The opaque encoding of a model choice. ACP advertises a model option's value
/// as the JSON pair [provider, model] rather than an invented token, and the
/// flattened picker stores whatever the transport advertises — so the host's
/// ModelCatalog is flattened into the same encoding and the QML never learns
/// which transport it is talking to.
QString modelChoiceValue(const QString &provider, const QString &model)
{
    const QJsonArray pair{provider, model};
    return QString::fromUtf8(QJsonDocument(pair).toJson(QJsonDocument::Compact));
}

/// One provider/model route's `reasoning` block from a ModelCatalog, or an empty
/// object when the route declares none (in which case reasoning is not offered).
QJsonObject reasoningFor(const QJsonObject &catalog, const QString &provider,
                         const QString &model)
{    for (const QJsonValue &groupValue : catalog.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = groupValue.toObject();
        if (group.value(QStringLiteral("id")).toString() != provider)
            continue;
        for (const QJsonValue &modelValue : group.value(QStringLiteral("models")).toArray()) {
            const QJsonObject entry = modelValue.toObject();
            if (entry.value(QStringLiteral("id")).toString() == model)
                return entry.value(QStringLiteral("reasoning")).toObject();
        }
    }
    return {};
}

/// A short, stable label for a session id. The host's ids carry a `session-`
/// prefix (`dsh-automation-session-` for automations), so truncating from the
/// left the way the ACP path does would show nothing but the prefix.
QString shortSessionId(const QString &sessionId)
{
    const QString marker = QStringLiteral("session-");
    const int at = sessionId.lastIndexOf(marker);
    return at >= 0 ? sessionId.mid(at + marker.size(), 8) : sessionId.left(8);
}

/// The seq of one journal record. Every backwards page is cut at a seq, and the
/// page's own first record is what the next cursor is derived from.
qint64 recordSeq(const QJsonObject &record)
{
    return qint64(record.value(QStringLiteral("event")).toObject()
                      .value(QStringLiteral("seq")).toDouble(-1));
}

/// The seq of one event (the record's payload), which is what a live frame and a
/// snapshot record both carry. A reconnect cuts the snapshot's page against the
/// highest seq already translated, so this is the identity that matters.
qint64 eventSeq(const QJsonObject &event)
{
    return qint64(event.value(QStringLiteral("seq")).toDouble(-1));
}

/// How much of a file the panel asks for. `workspaceFiles/read`'s range is
/// line-based, so this is a line count, not a byte count — a minified bundle is
/// one line and would still arrive whole, which is what the character cap below
/// is for.
constexpr int FILE_HEAD_LINES = 400;

/// The most text handed to a Text element, whatever the host returned. A single
/// enormous line is the case the line range cannot bound, and a scene graph that
/// has to shape megabytes of text is a frozen window, not a slow one.
constexpr int FILE_TEXT_CHAR_CAP = 128 * 1024;

/// The automation plugin's session prefix. Automations are not a host feature —
/// they come from a third-party plugin — so `origin` says nothing about them and
/// the plugin's authoritative `runs` table sits behind a bridge a native client
/// cannot reach. The id is what is visible, and this is the convention the
/// plugin's own client bundle keys on.
constexpr const char *kAutomationPrefix = "dsh-automation-session";

/// The synthetic group those sessions are lifted into. Not a Workspace and never
/// sent to the host as one: it exists so a scheduled run stops padding out the
/// directory it happened to run in. The view needs no matching literal — it reads
/// the group's `virtual` flag and its title off the registry it is already given.
constexpr const char *kAutomationsGroup = "__automations__";

// ---- trajectory -----------------------------------------------------------
//
// The ledger's own budgets, mirrored from the web's `trajectory-preview.ts` via
// dsh-android's Trajectory.kt: read 2 048 source characters, emit at most 512.
// A ledger row is a summary, not the payload.

constexpr int TRAJECTORY_PREVIEW_SOURCE = 2048;
constexpr int TRAJECTORY_PREVIEW_CHARS = 512;

/// The most raw detail one cell retains. Bounded on purpose: a tool result in
/// the session this was written against ran to 22 KB, and a ledger of a few
/// thousand such cells re-sent to a QML Text on every change is the frozen
/// window FILE_TEXT_CHAR_CAP already guards against elsewhere. The detail is
/// evidence beside the summary, not a payload store.
constexpr int TRAJECTORY_DETAIL_CAP = 8 * 1024;

/// The web's bounded one-line preview: whitespace collapsed, then cut to the
/// output budget. Reading only the first `TRAJECTORY_PREVIEW_SOURCE` characters
/// of a long body before collapsing is what keeps the cost proportional to the
/// preview rather than to the record.
QString trajectoryPreview(const QString &text)
{
    const QString compact = text.simplified();
    if (compact.size() <= TRAJECTORY_PREVIEW_CHARS)
        return compact;
    const QString source = text.left(TRAJECTORY_PREVIEW_SOURCE).simplified();
    return source.left(TRAJECTORY_PREVIEW_CHARS).trimmed() + QChar(0x2026);
}

/// Bound a retained raw detail, marking the cut so a truncated record is never
/// mistaken for a complete one.
QString trajectoryDetail(const QString &text)
{
    if (text.size() <= TRAJECTORY_DETAIL_CAP)
        return text;
    return text.left(TRAJECTORY_DETAIL_CAP) + QChar(0x2026);
}

/// The call's decoded arguments, as the tool card's IN half shows them.
///
/// This is the web's own generic body — `JSON.stringify(args, null, 2)` — kept
/// whole rather than reduced to a key or two, because IN is the request and a
/// summary is not the request. Bounded the way a ledger cell's detail is: a
/// `write_file` call carries the whole file it is writing.
///
/// `arguments` is already decoded by the caller; the wire's `arguments` is an
/// encoded JSON *string* on a tool call, which is why the caller has to parse it
/// before either this or toolSummary() can see a field at all.
QString toolRequestText(const QJsonObject &arguments)
{
    if (arguments.isEmpty())
        return {};
    return trajectoryDetail(
               QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented)))
        .trimmed();
}

/// Whether a call is a shell command. The identifier decides — `bash` and `pwsh`
/// are the two this host ships — and a call with no identifier is judged by
/// carrying a `command` at all, which is the only field it could be summarised by.
bool isShellCall(const QString &name, const QJsonObject &arguments)
{
    const QString id = name.toLower();
    if (id.isEmpty())
        return !arguments.value(QStringLiteral("command")).toString().trimmed().isEmpty();
    static const QStringList families{
        QStringLiteral("bash"),   QStringLiteral("pwsh"), QStringLiteral("shell"),
        QStringLiteral("terminal"), QStringLiteral("exec"), QStringLiteral("command"),
    };
    for (const QString &family : families) {
        if (id.contains(family))
            return true;
    }
    return false;
}

/// A byte count as a person reads it. Deliberately blunt: a file row's job is to
/// say roughly how big the thing is, not to be a byte counter. Zero is `0 B`
/// rather than absent, because a zero-byte file really is zero bytes.
QString byteSizeText(int bytes)
{
    if (bytes < 0)
        return {};   // the host did not say; guessing 0 B would claim it did
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0) {
        return kb < 10.0 ? QStringLiteral("%1 KB").arg(QString::number(kb, 'f', 1))
                         : QStringLiteral("%1 KB").arg(int(kb + 0.5));
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return mb < 10.0 ? QStringLiteral("%1 MB").arg(QString::number(mb, 'f', 1))
                         : QStringLiteral("%1 MB").arg(int(mb + 0.5));
    }
    const double gb = mb / 1024.0;
    return gb < 10.0 ? QStringLiteral("%1 GB").arg(QString::number(gb, 'f', 1))
                     : QStringLiteral("%1 GB").arg(int(gb + 0.5));
}

/// The dim meta line beside a file row's name: what the host told us about the
/// attachment, in the order a reader wants it — size, then media type. A part
/// the host did not send is omitted rather than invented: a row that said
/// "0 B" for an unknown size would be lying about the file.
QString fileMetaLine(int bytes, const QString &mediaType)
{
    QStringList parts;
    if (bytes >= 0)
        parts += byteSizeText(bytes);
    const QString media = mediaType.trimmed();
    if (!media.isEmpty())
        parts += media;
    return parts.join(QStringLiteral(" \u00b7 "));
}

/// The media type of bytes that are one of the four images the prompt union's
/// image arm accepts, or empty for anything else. Sniffed from the bytes rather
/// than the name: an extension is a claim, and the host verifies the declared
/// type against the decoded image, so guessing from a name only moves the
/// refusal later. The four values are exactly `ImageMediaType` in the Host's
/// session-controller schema (`image/png`, `image/jpeg`, `image/webp`,
/// `image/gif`).
QString promptImageMediaType(const QByteArray &bytes)
{
    if (bytes.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("image/png");
    if (bytes.startsWith("\xff\xd8\xff"))
        return QStringLiteral("image/jpeg");
    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
        return QStringLiteral("image/gif");
    if (bytes.size() >= 12 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP")
        return QStringLiteral("image/webp");
    return QString();
}

/// How much of a not-yet-delivered prompt its state row carries: enough to
/// recognise which message is waiting, never enough to read as its body. The
/// row is a delivery state, so the text it previews is not yet a message.
constexpr int STAGED_PREVIEW_CHARS = 140;

/// The prompt as one bounded line, for the row that stands in for it while the
/// host has not delivered it. Collapsed, because a newline in a one-line state
/// row would be dropped anyway and the row's height is a single line.
QString stagedPreview(const QString &text)
{
    const QString compact = text.simplified();
    return compact.size() <= STAGED_PREVIEW_CHARS
        ? compact
        : compact.left(STAGED_PREVIEW_CHARS).trimmed() + QChar(0x2026);
}

/// The text of every `text` block in a message's content, in order.
QString contentText(const QJsonArray &content)
{
    QStringList parts;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("text"))
            continue;
        const QString text = block.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            parts += text;
    }
    return parts.join(QStringLiteral("\n"));
}

/// The text of every block of one type — `reasoning` and `text` are separate
/// detail panes for an assistant message, and the transcript renders them as
/// separate rows too.
QString blocksOfType(const QJsonArray &content, const QString &type)
{
    QStringList parts;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != type)
            continue;
        const QString text = block.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            parts += text;
    }
    return parts.join(QStringLiteral("\n"));
}

/// A compaction's identity across its records. `compaction/start|summary|end`
/// carry `compactionId` at the top level of `data`; the checkpoint carries it on
/// `source`. `sourceCommandId` stands in when the compaction id is absent, so a
/// manual compaction's live announcement and its durable marker still meet.
QString compactionIdentity(const QString &compactionId, const QString &sourceCommandId)
{
    return compactionId.isEmpty() ? sourceCommandId : compactionId;
}

/// The transcript key one compaction row is addressed by. The announcement and
/// the marker use *different* roles on purpose: the checkpoint retires the
/// announcement and appends the marker, and a later `compaction/end` must retire
/// only the announcement — one shared key would make it retire the marker.
QString compactionRowKey(const QString &identity, const QString &role)
{
    return QStringLiteral("compaction-%1:%2").arg(role, identity);
}

/// The web's line beside the compaction marker. The counts are on
/// `compaction/summary` — `shadowedSeqs` is the list of history records the
/// compaction replaced, so its length is the item count, and
/// `shadowedTokenCount` is the token count. The checkpoint never carries either,
/// so a marker drawn before its summary has none, and none is invented: a record
/// that supplied no counts falls back to the web's disclosure copy, and one with
/// no summary at all shows no second line.
QString compactionPreview(const QString &summary, int items, int tokens)
{
    if (items >= 0 && tokens >= 0)
        return QStringLiteral("Compacted %1 history items (~%2 tokens)").arg(items).arg(tokens);
    if (items >= 0)
        return QStringLiteral("Compacted %1 history items").arg(items);
    if (tokens >= 0)
        return QStringLiteral("Compacted history (~%1 tokens)").arg(tokens);
    if (!summary.isEmpty())
        return QStringLiteral("View compaction summary");
    return {};
}

/// One line per content block, for the raw detail pane. The journal's blocks are
/// richer than the transcript ever renders — `image`, `tool-add`, `tool-removal`
/// — and a ledger that silently dropped them would claim the model saw less than
/// it did.
QString contentDump(const QJsonArray &content)
{
    QStringList lines;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text") || type == QLatin1String("reasoning")) {
            const QString text = block.value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                lines += text;
        } else if (type == QLatin1String("image")) {
            lines += QStringLiteral("[image]");
        } else {
            lines += QStringLiteral("%1: %2").arg(
                type, QString::fromUtf8(QJsonDocument(block).toJson(QJsonDocument::Compact)));
        }
    }
    return lines.join(QStringLiteral("\n"));
}

/// One stored instant as the reader's local clock. The host stores UTC; a value
/// that does not parse is shown exactly as stored rather than as an epoch zero.
///
/// At file scope rather than beside the task catalog because the unary reply
/// handler, which is far above that section, formats delivery instants too.
QString scheduleInstant(const QString &iso)
{
    if (iso.isEmpty())
        return QString();
    const QDateTime at = QDateTime::fromString(iso, Qt::ISODate);
    return at.isValid() ? at.toLocalTime().toString(QStringLiteral("d MMM yyyy, HH:mm"))
                        : iso;
}

} // namespace

DshClient::DshClient(QObject *parent)
    : QObject(parent)
    , m_auth(new BrowserAuth())
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_network(new QNetworkAccessManager(this))
    , m_transcript(new AcpTranscript(this))
{
    connect(m_socket, &QWebSocket::connected, this, &DshClient::onConnected);
    connect(m_socket, &QWebSocket::disconnected, this, &DshClient::onDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived,
            this, &DshClient::onTextMessageReceived);
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this, [this](QAbstractSocket::SocketError) { onSocketError(); });

    // The transcript feed listens to our own stream signals rather than being
    // called from the frame parser: the parser stays a pure decoder, and a second
    // consumer (terminal, panels, trajectory) can attach to the same stream.
    connect(this, &DshClient::streamItem, this, &DshClient::onStreamItem);

    // One single-shot timer, restarted per attempt. A repeating one would fire
    // during an attempt that is still in flight and stack a second socket open.
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_wantConnection)
            return;
        connectToHost();
    });

    // The backoff is only reset by a connection that lasts, not by one that
    // opens. See m_reconnectAttempt: a flap that reconnects, is dropped again and
    // resets the delay each time is the storm itself.
    m_stableTimer = new QTimer(this);
    m_stableTimer->setSingleShot(true);
    connect(m_stableTimer, &QTimer::timeout, this, [this]() {
        if (m_state != Ready)
            return;
        if (m_reconnectAttempt > 0)
            qInfo() << "mux stable for" << RECONNECT_STABLE_MS / 1000 << "s;"
                    << "backoff reset after" << m_reconnectAttempt << "attempt(s)";
        m_reconnectAttempt = 0;
        m_reconnectDelayMs = RECONNECT_BASE_MS;
    });

    // The loop's own pulse. A blocked event loop cannot answer the gateway's ping,
    // and that — not the close code — is what a load-related drop looks like from
    // here; a late tick of this timer is the evidence, recorded and reported when
    // the socket goes. It samples only while connected (see onConnected).
    m_loopLagTimer = new QTimer(this);
    m_loopLagTimer->setInterval(LOOP_LAG_SAMPLE_MS);
    connect(m_loopLagTimer, &QTimer::timeout, this, [this]() {
        if (!m_loopLagSample.isValid()) {
            m_loopLagSample.start();
            return;
        }
        m_maxLoopLagMs = qMax<qint64>(m_maxLoopLagMs, m_loopLagSample.restart());
    });

    // A list that never arrives leaves the rail empty — and the event-driven
    // refresh is gated on the list being non-empty, so nothing would ever try
    // again and the client would sit blank until the session was reopened. The
    // host's session-query read can take minutes when it is busy, so a failure
    // while there is nothing to show is retried.
    connect(this, &DshClient::rpcFailed, this,
            [this](const QString &method, const QString &, const QString &) {
                if (method != QLatin1String("session/list") || !m_sessions.isEmpty())
                    return;
                QTimer::singleShot(5000, this, [this]() {
                    if (m_sessions.isEmpty())
                        listSessions();
                });
            });
    connect(this, &DshClient::streamError, this, &DshClient::onStreamError);
    connect(this, &DshClient::streamEnded, this, &DshClient::onStreamEnded);

    // A prompt that the host refuses never opens a turn, so nothing would ever
    // clear the optimistic busy state and the UI would be stuck on "Stop".
    connect(this, &DshClient::rpcFailed, this,
            [this](const QString &method, const QString &, const QString &message) {
                if (method == QLatin1String("session/prompt"))
                    setBusy(false);
                // A refused page must not leave the client believing one is still
                // in flight, or every later attempt is silently dropped.
                if (method == QLatin1String("session/page"))
                    m_loadingEarlier = false;
                // A refused listing would otherwise leave the directory looking
                // permanently empty: take back the expansion it was optimistically
                // given, and say why rather than showing an empty folder.
                if (method.startsWith(QLatin1String("workspaceFiles/"))) {
                    if (method == QLatin1String("workspaceFiles/list"))
                        m_expandedDirectories.remove(m_lastDirectoryRequest);
                    setFileError(message);
                    rebuildFileRows();
                }
            });
}

DshClient::~DshClient() = default;

void DshClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void DshClient::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void DshClient::setAccessNotice(const QString &notice)
{
    if (m_accessNotice == notice)
        return;
    m_accessNotice = notice;
    emit accessNoticeChanged();
}

void DshClient::setModelNotice(const QString &notice)
{
    if (m_modelNotice == notice)
        return;
    m_modelNotice = notice;
    emit modelNoticeChanged();
}

void DshClient::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void DshClient::setTurnOpen(bool open)
{
    if (m_turnOpen == open)
        return;
    m_turnOpen = open;
    emit turnOpenChanged();
}

void DshClient::noteAppliedSeq(const QJsonObject &event)
{
    const qint64 seq = eventSeq(event);
    // A record without a seq proves nothing about the cut, so it must not move it;
    // the next snapshot's overlap check stays conservative either way.
    if (seq >= 0)
        m_transcriptThroughSeq = qMax(m_transcriptThroughSeq, seq);
}

void DshClient::setSessionsLoading(bool loading)
{
    if (m_sessionsLoading == loading)
        return;
    m_sessionsLoading = loading;
    emit sessionsLoadingChanged();
}

void DshClient::setOpeningSession(bool opening)
{
    if (m_openingSession == opening)
        return;
    m_openingSession = opening;
    emit openingSessionChanged();
}

void DshClient::setPendingSession(bool pending)
{
    if (m_pendingSeat.active == pending)
        return;
    m_pendingSeat.active = pending;
    emit pendingSessionChanged();
}

void DshClient::fail(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
    setStatus(message);
    setState(Failed);
    setBusy(false);
    // The turn state came from the journal, and there is no journal now: unknown
    // is treated as "a turn may be open", so the next prompt is staged rather
    // than claimed as delivered.
    setTurnOpen(true);
    // A failed connection cannot deliver anything, and the rows of a transcript
    // that is still on screen must not keep saying "steering…".
    withholdStagedPrompts();
    // Nor must they keep claiming to be mid-answer: nothing will ever settle
    // that attempt on this connection.
    discardProvisional();
}

void DshClient::sendJson(const QJsonObject &message)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    m_socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

QNetworkRequest DshClient::makeRequest(const QString &path) const
{
    QNetworkRequest request(QUrl(QStringLiteral("http://%1%2").arg(m_authority, path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    // Both halves matter: the Host the server validates against, and a cookie
    // minted for that *same* authority. A mismatch is a bare 401.
    request.setRawHeader("Host", m_authority.toUtf8());

    QString error;
    const QString cookie = m_auth->cookieHeader(m_authority, &error);
    if (!cookie.isEmpty())
        request.setRawHeader("Cookie", cookie.toUtf8());

    return request;
}

void DshClient::resolveAuthority()
{
    const QStringList candidates = BrowserAuth::candidateAuthorities();
    QString error;

    // Resolve by minting, not by probing: minting is local and cheap, and the
    // first candidate is the authority the running deployment actually validates
    // (the carrier behind the auth proxy).
    for (const QString &candidate : candidates) {
        if (!m_auth->cookieHeader(candidate, &error).isEmpty()) {
            m_authority = candidate;
            emit authorityChanged();
            qInfo() << "resolved authority:" << m_authority;
            return;
        }
    }

    fail(error.isEmpty()
             ? QStringLiteral("no usable authority for the local host")
             : QStringLiteral("cannot authenticate: %1").arg(error));
}

void DshClient::connectToHost()
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        setStatus(QStringLiteral("already connected"));
        return;
    }

    // Timed from this attempt, not from the last one. A handshake that fails
    // before `connected` must not report the previous socket's uptime as its own.
    m_connectedAt.invalidate();
    m_lastFrameAt.invalidate();
    m_maxLoopLagMs = 0;
    // A pick that was in flight when the socket went cannot answer now, and a
    // latch left set would make the rail's New Workspace refuse every later
    // click on a connection that is perfectly healthy again.
    m_workspacePickInFlight = false;

    setState(Connecting);
    // A retry is not a first connection, and saying "authenticating…" made the
    // window look like it had started over. Keep the attempt number visible: it is
    // the one fact that tells a reader (and the log) that this is not the first
    // try.
    if (m_reconnectAttempt > 0)
        setStatus(QStringLiteral("reconnecting (attempt %1)…").arg(m_reconnectAttempt));
    else
        setStatus(QStringLiteral("authenticating…"));
    m_lastError.clear();
    emit lastErrorChanged();
    // From here until an explicit disconnect, a lost socket means "try again"
    // rather than "stop".
    m_wantConnection = true;

    if (m_authority.isEmpty())
        resolveAuthority();
    if (m_state == Failed)
        return;

    QString error;
    const QString cookie = m_auth->cookieHeader(m_authority, &error);
    if (cookie.isEmpty()) {
        fail(QStringLiteral("cannot mint a session cookie: %1").arg(error));
        return;
    }

    // Not overwritten on a retry: "opening the mux…" would replace the attempt
    // number that the status line is carrying.
    if (m_reconnectAttempt == 0)
        setStatus(QStringLiteral("opening the mux…"));

    QNetworkRequest request(QUrl(QStringLiteral("ws://%1%2").arg(m_authority, QString::fromLatin1(MUX_PATH))));
    request.setRawHeader("Host", m_authority.toUtf8());
    request.setRawHeader("Cookie", cookie.toUtf8());

    m_socket->open(request);
}

void DshClient::disconnectFromHost()
{
    // The user asked for this one, so it must not be undone by the retry.
    m_wantConnection = false;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_stableTimer)
        m_stableTimer->stop();
    if (m_loopLagTimer)
        m_loopLagTimer->stop();
    m_reconnectDelayMs = RECONNECT_BASE_MS;
    m_reconnectAttempt = 0;
    m_lastCloseReason.clear();
    m_streamEndpoints.clear();
    m_followStreamId.clear();
    // The clientId belongs to the socket that carried `ready`, so it dies with it.
    m_eventStreamId.clear();
    m_eventClientId.clear();
    // The workspace registry is per-connection state too: a new socket gets a
    // fresh baseline, so anything cached from the old one must not survive.
    m_workspaceStreamId.clear();
    // A pending waterfall cannot be answered through a closed socket. Close any
    // prompt still asking, rather than leaving it to answer a request that died
    // with the connection.
    if (!m_offeredApprovalId.isEmpty()) {
        const QString stale = m_offeredApprovalId;
        m_offeredApprovalId.clear();
        emit permissionWithdrawn(stale);
    }
    if (!m_offeredQuestionId.isEmpty()) {
        const QString stale = m_offeredQuestionId;
        m_offeredQuestionId.clear();
        emit permissionWithdrawn(stale);
    }
    m_pendingWaterfalls.clear();
    m_nextWaterfallSeq = 0;
    // No approved or asked waterfall is held any more, so no row is amber. The
    // requests themselves survive on the host and are re-announced on the next
    // generation; the marks come back with them.
    refreshSessionMarks();
    resetEarlierHistory();
    setBusy(false);
    // Likewise: with the stream gone the turn's state is unknown, and unknown is
    // treated as open (see m_turnOpen), because a false "delivered" cannot be
    // taken back while a staged row is corrected by the delivery.
    setTurnOpen(true);
    // Nothing can deliver a staged prompt once the socket is closed, and the
    // transcript is not cleared by a disconnect — so the rows stop claiming to
    // steer rather than sitting there for as long as the client stays closed.
    withholdStagedPrompts();
    // Same for a live attempt: the frames that would have finished it died with
    // the socket, so its rows are taken back now rather than left mid-answer
    // through however long the client stays closed.
    discardProvisional();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->close();
    setState(Idle);
    setStatus(QStringLiteral("disconnected"));
}

void DshClient::onConnected()
{
    // Nothing is cleared here. A socket that comes up and dies seconds later is a
    // flap, and the retry delay it earned must survive it; m_stableTimer is the
    // only thing that resets the schedule, and only after a connection that lasts.
    m_connectedAt.start();
    m_lastFrameAt.start();
    m_loopLagSample.invalidate();
    m_maxLoopLagMs = 0;
    if (m_loopLagTimer)
        m_loopLagTimer->start();
    if (m_stableTimer)
        m_stableTimer->start(RECONNECT_STABLE_MS);

    qInfo() << "mux connected to" << m_authority
            << "| attempt" << m_reconnectAttempt
            << "| last close:" << (m_lastCloseReason.isEmpty() ? QStringLiteral("-")
                                                               : m_lastCloseReason);
    setState(Ready);
    // A socket is not the session. Until the follow snapshot lands the conversation
    // is not back, so a reconnect says so rather than showing a healthy session
    // over a mux that has not yet re-subscribed.
    if (m_reconnectAttempt > 0 && !m_sessionId.isEmpty())
        setStatus(QStringLiteral("reconnected · re-subscribing…"));
    else
        setStatus(QStringLiteral("connected to %1").arg(m_authority));

    // DSH has no ACP-style initialize handshake to name an agent, so the host
    // itself is what the UI can honestly report until a followed session's
    // snapshot names its agent preset.
    const QString info = QStringLiteral("DSH host %1").arg(m_authority);
    if (m_agentInfo != info) {
        m_agentInfo = info;
        emit agentInfoChanged();
    }

    // The forwarded-event stream is opened here rather than on demand. It carries
    // the host's waterfalls, and a waterfall that nobody is listening for is an
    // agent that stalls with no visible cause — so the subscription must already
    // exist before the first approval can be raised.
    openEventStream();

    // The Workspace registry, likewise up front: until its baseline lands the rail
    // cannot tell an archived Session from a live one, nor order the groups. It is
    // a stream rather than a call because the host pushes `archived`/`order`
    // changes as they happen, and a rail that re-fetched on every guess would
    // flicker.
    openWorkspaceStream();

    // The rail's schedule marks come from the host's task catalog, not from the
    // roster, so a fresh socket has to ask for it again: a task created, ended or
    // deleted while this client was away would otherwise leave a mark missing or
    // stale for good, because `schedule/changed` only reports changes the host
    // makes while we are listening. Nothing is read before the first roster has
    // asked once — see applySessionList — so a client that never lists pays
    // nothing. One ask per socket either way: a host without the Schedule bundle
    // is refused once here and not once per roster refresh.
    m_scheduleAsked = m_scheduleLoaded;
    if (m_scheduleLoaded)
        refreshScheduleTasks();

    // A reconnect is not a new window, and it used to behave like one: the two
    // streams above were the only ones that came back, so the followed session's
    // `session/follow` was never re-opened. The host is still running the
    // conversation; this client has simply stopped listening, which is a blank
    // transcript with a connected header — exactly how a dropped mux looked to the
    // reader.
    //
    // The follow used to be re-opened through the session-switch path, which
    // clears the transcript and replays the snapshot's page from the top. Under
    // the 2026-10-25 storm that ran once every nine seconds: the view was rebuilt
    // forty-two times and the reader saw "my session view keeps refreshing".
    //
    // A snapshot has no cursor, but its records do carry `seq`, and the rows on
    // screen are exactly the records at or below the highest seq translated. So a
    // drop of the *same* follow is resumed through resumeFollowAddress(), which
    // keeps the rows and makes the overlapping page a cursor. A session switch, a
    // first view that never saw a snapshot, and a snapshot that cannot be laid on
    // top (a compaction replaced the surface, or more than a page was missed) all
    // still take the ordinary reset path — correctness before continuity.
    if (!m_sessionId.isEmpty()) {
        // The address kept from the last follow, not a plain-session reconstruction
        // of its id: a subagent child was followed under its parent's address, and
        // `followSession(childId)` is refused with `session/agent-busy`. Rebuilding
        // the plain form here left a child's window blank after every drop even
        // though the host was still running the conversation — the same defect the
        // startup restore had, one reconnect later.
        bool resumed = false;
        if (!m_followAddress.isEmpty())
            resumed = resumeFollowAddress(m_followAddress, m_sessionId);
        if (!resumed) {
            if (m_followAddress.isEmpty())
                followResolvedSession(m_sessionId);
            else
                followAddress(m_followAddress, m_sessionId);
        }

        // The terminal is a stream too. Its PTY is retained on the host and its id
        // was minted here, so all that is lost with the socket is the attachment —
        // re-attach rather than leaving the panel writing into a dead stream.
        if (!m_terminalId.isEmpty())
            attachTerminal();
    }
}

void DshClient::onDisconnected()
{
    // Logged, and loudly, because a disconnect that is only shown in the UI cannot
    // be diagnosed: this exact failure was reported as "connects then drops" and
    // the client's own log was 371 bytes of clean startup with no error in it. The
    // host logs nothing about the mux either, so before this the event left no
    // trace anywhere on the machine.
    //
    // The attribution matters more than the code. A 1000 here is Qt's default for
    // a socket that was terminated without a close frame, not the host's own
    // answer — see describeMuxClose(). The other half of the attribution is the
    // client's own event loop: Qt answers a Ping only when the loop runs, so a
    // loop that was blocked is what makes the host's heartbeat terminate us. The
    // lag figures below are that evidence.
    const int closeCode = int(m_socket->closeCode());
    const QString closeReason = m_socket->closeReason();
    const auto socketError = m_socket->error();
    m_lastCloseReason = describeMuxClose(closeCode, closeReason, socketError);

    qWarning() << "mux disconnected:" << m_lastCloseReason
               << "| raw closeCode" << closeCode
               << "closeReason" << (closeReason.isEmpty() ? QStringLiteral("(none given)")
                                                          : closeReason)
               << "socketError" << int(socketError) << m_socket->errorString()
               << "| uptime" << (m_connectedAt.isValid() ? m_connectedAt.elapsed() : -1) << "ms"
               // QtWebSockets has no signal for an inbound Ping or the Pong it
               // sends back, so this cannot be counted, only inferred. Say so
               // rather than printing a zero that reads as "none answered".
               << "| pings answered: unobservable (Qt replies to a Ping inside the event"
                  " loop and exposes no signal for it; loop lag is the proxy)"
               << "| since last text frame"
               << (m_lastFrameAt.isValid() ? m_lastFrameAt.elapsed() : -1) << "ms"
               << "| max event-loop lag" << m_maxLoopLagMs << "ms over"
               << RECONNECT_STABLE_MS / 1000 << "s"
               << "| local" << m_socket->state()
               << "| open streams:" << m_streamEndpoints.keys()
               << "| follow:" << (m_followStreamId.isEmpty() ? QStringLiteral("-") : m_followStreamId)
               << "| events:" << (m_eventStreamId.isEmpty() ? QStringLiteral("-") : m_eventStreamId)
               << "| transcript rows:" << m_transcript->rowCount()
               << "through seq" << m_transcriptThroughSeq;

    if (m_loopLagTimer)
        m_loopLagTimer->stop();
    if (m_stableTimer)
        m_stableTimer->stop();

    // A socket close aborts every logical stream, so nothing is worth keeping —
    // and that means EVERY id, not the two that used to be cleared here. A stream
    // id belongs to the mux socket that minted it, so the rest were dead but still
    // remembered, and each `open…` function guards on its own id being empty: after
    // a reconnect `openEventStream()` returned early and the client never
    // subscribed to `$events` again. It stopped hearing the host entirely — session
    // list changes, status changes, waterfalls — while looking connected, and
    // nothing ever re-opened them because their only other caller is a fresh
    // follow.
    m_streamEndpoints.clear();
    m_followStreamId.clear();
    m_workspaceStreamId.clear();
    m_eventStreamId.clear();
    // The clientId was minted by the socket that carried `ready`, so it dies with
    // it; a new one arrives before anything may be answered.
    m_eventClientId.clear();
    m_jobStreamId.clear();
    m_changesStreamId.clear();
    m_terminalStreamId.clear();
    // Earlier-history paging state is deliberately NOT reset: the rows it offered
    // are still in the transcript, and a continuing re-follow must keep the same
    // cursor or "load earlier" would re-fetch rows that are already on screen. A
    // real reset happens with the transcript, in resetFollowTranscript().
    setBusy(false);
    // The follow stream carried the turn state; a drop makes it unknown, and the
    // re-follow's snapshot is what answers it again. Unknown means open (see
    // m_turnOpen), which is also what the composer's Enter mode must see while
    // the transport is down: a steer preference that becomes a queue is exactly
    // the confusion this latch exists to prevent.
    setTurnOpen(true);
    // The follow stream is gone, so nothing can deliver the staged prompts this
    // client is waiting on. Their rows must stop claiming to steer now rather
    // than sit there through a reconnect that will replace them anyway.
    withholdStagedPrompts();
    // …and say which ones, because the row alone cannot distinguish a message the
    // host accepted and has not delivered yet from one whose delivery channel just
    // died with the mux. The request left over HTTP, so it may well have been
    // admitted; this is the line that says the client no longer knows.
    for (auto it = m_localPrompts.cbegin(); it != m_localPrompts.cend(); ++it) {
        if (it.value() == LocalPromptStaged) {
            qWarning() << "session/prompt delivery channel lost with the mux: requestId"
                       << it.key() << "- the row is held, not withdrawn; a re-follow may"
                          " still deliver it";
        }
    }
    // A dropped socket aborts the attempt mid-word. Its rows are withdrawn here.
    // A continuing re-follow re-seeds them from the new snapshot's baseline if
    // the same attempt is still running; a rebuild draws them from the journal.
    discardProvisional();
    if (m_wantConnection) {
        // A drop, not a close, so the client is still trying and must say so.
        // Staying in `Connecting` is the honest state — and it is what keeps the
        // header dot amber while the retry is pending, instead of leaving a
        // failure colour up over a host that is about to come back.
        scheduleReconnect();
        return;
    }

    if (m_state == Ready) {
        setState(Idle);
        setStatus(QStringLiteral("disconnected"));
    }
}

void DshClient::scheduleReconnect()
{
    if (!m_wantConnection || !m_reconnectTimer || m_reconnectTimer->isActive())
        return;

    // Jittered per attempt, so a client that was starved into a drop does not
    // come back on the same beat every time. The cap is applied after the jitter:
    // a ceiling that randomisation may exceed is not a ceiling.
    const int delay = qMin(reconnectDelayWithJitter(m_reconnectDelayMs), RECONNECT_CAP_MS);
    ++m_reconnectAttempt;

    setState(Connecting);
    setStatus(QStringLiteral("reconnecting (attempt %1 in %2 s)…")
                  .arg(m_reconnectAttempt)
                  .arg(delay / 1000.0, 0, 'f', 1));
    qWarning() << "mux reconnect attempt" << m_reconnectAttempt << "in" << delay << "ms"
               << "| reason:" << m_lastCloseReason
               << "| uptime" << (m_connectedAt.isValid() ? m_connectedAt.elapsed() : -1) << "ms"
               << "| retry delay was" << m_reconnectDelayMs << "ms";
    m_reconnectTimer->start(delay);

    // Back off for the next attempt, up to a ceiling. A host that is restarting is
    // usually back within seconds, so the first tries are close together; one that
    // is gone for good should not be hammered once a second forever.
    m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, RECONNECT_CAP_MS);
}

void DshClient::onSocketError()
{
    if (m_state == Failed)
        return;
    // RemoteHostClosedError is the host dropping us, not a local failure, and the
    // distinction decides where to look. Qt carries the host's own words only when
    // it sent a close frame; describeMuxClose() says which shape this is. The
    // event-loop lag is here as well as on the disconnect line because this is the
    // first line of the pair and a reader may stop at it.
    qWarning() << "mux socket error:" << m_socket->errorString()
               << "code" << int(m_socket->error())
               << "closeCode" << int(m_socket->closeCode())
               << "closeReason" << (m_socket->closeReason().isEmpty()
                                    ? QStringLiteral("(none given)")
                                    : m_socket->closeReason())
               << "=>" << describeMuxClose(int(m_socket->closeCode()),
                                           m_socket->closeReason(), m_socket->error())
               << "state" << m_socket->state()
               << "| uptime" << (m_connectedAt.isValid() ? m_connectedAt.elapsed() : -1) << "ms"
               << "| since last text frame"
               << (m_lastFrameAt.isValid() ? m_lastFrameAt.elapsed() : -1) << "ms"
               << "| max event-loop lag" << m_maxLoopLagMs << "ms"
               << "| open streams:" << m_streamEndpoints.keys();
    fail(QStringLiteral("socket error: %1").arg(m_socket->errorString()));
}

// --- unary RPC --------------------------------------------------------------

void DshClient::call(const QString &method, const QJsonObject &args,
                     const QString &promptRequestId, const QString &commandLine)
{
    if (m_authority.isEmpty()) {
        resolveAuthority();
        if (m_state == Failed) {
            // The request was never posted, so nothing will ever be journaled
            // for it: a prompt's row must not be left saying "steering…".
            abandonLocalPrompt(promptRequestId);
            return;
        }
    }

    // A string id: the host's envelope schema requires `z.string()` and answers
    // `gateway/bad-request` for anything else.
    const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingRpc.insert(rpcId, method);

    QJsonObject envelope{
        {QStringLiteral("type"), QStringLiteral("client-request")},
        {QStringLiteral("rpcId"), rpcId},
        {QStringLiteral("method"), method},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("args"), args}}},
    };

    QNetworkReply *reply = m_network->post(
        makeRequest(QStringLiteral("/api/") + method),
        QJsonDocument(envelope).toJson(QJsonDocument::Compact));

    m_replyMethod.insert(reply, method);

    // Nothing bounded a request before this, and a host that accepts a call and
    // then never answers left the UI blank with no error anywhere to explain it —
    // which is exactly how the session rail sat empty. The deadline is generous,
    // because a real `session/list` on this box has taken minutes while the
    // session-query index was busy, but an answer has to arrive eventually.
    //
    // `directoryPicker/pick` is the one call whose wait is a PERSON, not a host:
    // the chooser is modal on the host's desktop and browsing a deep tree can
    // take as long as it takes. Aborting it at the ordinary deadline would say
    // "the host did not answer" while the dialog was still on screen, and the
    // host would still create the Workspace when the person finally chose —
    // a failure message over a call that in fact succeeded. Ten minutes is still
    // bounded, so a chooser left open overnight does not leak the reply.
    constexpr int RpcDeadlineMs = 120000;
    constexpr int PickerDeadlineMs = 600000;
    const int deadlineMs = method == QLatin1String("directoryPicker/pick")
                               ? PickerDeadlineMs : RpcDeadlineMs;
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this,
            [this, reply, method, promptRequestId, commandLine, deadlineMs]() {
        if (reply->isFinished())
            return;
        // Marked before abort(): abort() emits finished synchronously, and the
        // handler below must not report the same failure a second time.
        reply->setProperty("dshTimedOut", true);
        reply->abort();
        // A timeout is not a refusal: the host may have accepted the prompt and
        // deliver it minutes from now, so the row stops claiming to steer but the
        // id still lets the delivery replace it.
        withholdLocalPrompt(promptRequestId);
        logPromptOutcome(method, promptRequestId, QStringLiteral("timeout"),
                         QStringLiteral("held (no longer claims to steer; a later delivery "
                                        "still replaces it)"),
                         QStringLiteral("no answer within %1 s").arg(deadlineMs / 1000));
        reportRpcFailure(method, QStringLiteral("timeout"),
                         QStringLiteral("the host did not answer within %1 s")
                             .arg(deadlineMs / 1000),
                         commandLine);
    });
    deadline->start(deadlineMs);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, method, rpcId, promptRequestId, commandLine]() {
        reply->deleteLater();
        m_replyMethod.remove(reply);
        m_pendingRpc.remove(rpcId);
        // Cleared for every ending, including the ones that report failure.
        if (method == QLatin1String("session/list")) {
            m_listInFlight = false;
            // A list asked for while this one was in flight runs after this answer
            // has been applied — hence the zero-timer: it must not stamp a new
            // request epoch before the payload below is stored (see
            // applySessionList). Dropping the request instead is how a new row
            // waited for the next list, minutes later.
            //
            // The loading flag stays up across the handover when a follow-up is
            // going, so the rail does not flicker between the two, and it is only
            // given up if the follow-up itself cannot post.
            if (m_listQueued) {
                m_listQueued = false;
                QTimer::singleShot(0, this, [this]() { listSessions(); });
            } else {
                setSessionsLoading(false);
            }
        }
        if (method == QLatin1String("directoryPicker/pick"))
            m_workspacePickInFlight = false;
        if (reply->property("dshTimedOut").toBool())
            return; // the deadline already reported this one

        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError && payload.isEmpty()) {
            // No answer at all: the request may or may not have reached the host,
            // so a prompt's row stops claiming to steer without being forgotten.
            withholdLocalPrompt(promptRequestId);
            logPromptOutcome(method, promptRequestId, QStringLiteral("no-answer"),
                             QStringLiteral("held (no longer claims to steer; a later delivery "
                                            "still replaces it)"),
                             QStringLiteral("HTTP %1: %2").arg(status).arg(reply->errorString()));
            reportRpcFailure(method, QStringLiteral("network"),
                             QStringLiteral("HTTP %1: %2").arg(status).arg(reply->errorString()),
                             commandLine);
            return;
        }

        // 401/403 are plain-text bodies, not envelopes — surface them as auth
        // failures rather than a JSON parse error, which is what they look like.
        if (status == 401 || status == 403) {
            // Refused at the door, so nothing was admitted and nothing will ever
            // be journaled under this id.
            abandonLocalPrompt(promptRequestId);
            logPromptOutcome(method, promptRequestId, QStringLiteral("unauthorized"),
                             QStringLiteral("withdrawn (nothing was admitted)"),
                             QString::fromUtf8(payload).trimmed().left(200));
            reportRpcFailure(method, QStringLiteral("unauthorized"),
                           QString::fromUtf8(payload).trimmed().isEmpty()
                               ? QStringLiteral("the host rejected the minted cookie")
                               : QString::fromUtf8(payload).trimmed(),
                           commandLine);
            return;
        }

        const QJsonObject response = QJsonDocument::fromJson(payload).object();
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();

        if (!result.value(QStringLiteral("ok")).toBool()) {
            const QJsonObject error = result.value(QStringLiteral("error")).toObject();
            // An answered refusal is definite: the prompt was not admitted, so a
            // staged row goes and the error row is the whole story.
            abandonLocalPrompt(promptRequestId);
            logPromptOutcome(method, promptRequestId,
                             error.value(QStringLiteral("code")).toString(),
                             QStringLiteral("withdrawn (the host refused it)"),
                             error.value(QStringLiteral("message")).toString());
            reportRpcFailure(method,
                             error.value(QStringLiteral("code")).toString(),
                             error.value(QStringLiteral("message")).toString(),
                             commandLine);
            return;
        }

        const QJsonValue value = result.value(QStringLiteral("value"));
        emit rpcFinished(method, value.toObject());

        if (method == QLatin1String("commands/execute")) {
            handleCommandReply(commandLine, value);
            return;
        }

        if (method == QLatin1String("session/prompt")) {
            const bool staged = m_localPrompts.value(promptRequestId, LocalPromptEchoed)
                                == LocalPromptStaged;
            logPromptOutcome(
                method, promptRequestId, QStringLiteral("succeeded"),
                staged ? QStringLiteral("accepted; the row stays a delivery state until the "
                                        "host journals the message")
                       : QStringLiteral("echoed; the row is the message"));
            if (staged) {
                // The inbox is a projection, and the follow stream sends projections
                // only in its snapshot — nothing pushes them when a message is
                // admitted. So the dock's queue stayed empty for a message the host
                // had just taken, for the same reason the transcript row above is
                // needed: accepted and invisible. Read it now, while the send is
                // known to have landed.
                refreshProjections();
                return;
            }
        }

        if (method == QLatin1String("terminal/create")) {
            // Only now is the id real; attach so output starts flowing.
            attachTerminal();
            emit terminalChanged();
            return;
        }

        if (method == QLatin1String("fileReferences/list")) {
            // A bare array, not an envelope: the host adapter returns the provider's
            // candidates directly.
            QVariantList candidates;
            for (const QJsonValue &entry : value.toArray()) {
                const QJsonObject candidate = entry.toObject();
                candidates.append(QVariantMap{
                    {QStringLiteral("path"), candidate.value(QStringLiteral("path")).toString()},
                    {QStringLiteral("kind"), candidate.value(QStringLiteral("kind")).toString()},
                });
            }
            if (candidates != m_fileCandidates) {
                m_fileCandidates = candidates;
                emit fileCandidatesChanged();
            }
            return;
        }

        if (method == QLatin1String("session/fork")) {
            const QString forked =
                value.toObject().value(QStringLiteral("sessionId")).toString();
            // Branches accumulate a suffix rather than repeating "(fork)": forking
            // a fork is an ordinary thing to do, and a title that says "(fork)
            // (fork)" stops telling you anything after the second one.
            QString parentTitle;
            for (const QJsonValue &entry : m_rawSessions) {
                const QJsonObject item = entry.toObject();
                if (item.value(QStringLiteral("sessionId")).toString() != m_sessionId)
                    continue;
                parentTitle = item.value(QStringLiteral("title")).toString();
                break;
            }
            if (!forked.isEmpty()) {
                if (!parentTitle.isEmpty()) {
                    static const QRegularExpression forkSuffix(
                        QStringLiteral(" \\(fork(?: (\\d+))?\\)$"));
                    const QRegularExpressionMatch match = forkSuffix.match(parentTitle);
                    QString title;
                    if (match.hasMatch()) {
                        const int n = match.captured(1).toInt();
                        title = parentTitle.left(match.capturedStart())
                                + QStringLiteral(" (fork %1)").arg(n + 1);
                    } else {
                        title = parentTitle + QStringLiteral(" (fork)");
                    }
                    renameSession(title, forked);
                }
                // A fork is an ordinary Session even when its source was a
                // subagent: the host creates it with `meta.parentSession` (the
                // source) and never `meta.origin`, and its address validation
                // refuses the plain form only for `origin == "subagent"`. So the
                // plain follow is correct here — unlike the restore path, which
                // has to ask the roster, because the remembered session may be a
                // real child. The fork is not in the roster yet at this point
                // anyway (`listSessions` below is what will list it).
                followSession(forked);
                listSessions();
                setStatus(QStringLiteral("forked"));
            }
            return;
        }

        if (method == QLatin1String("session/create")) {
            // The only caller is now a seat's materialisation: the session this
            // reply names is the one the held first turn was waiting for. (`Adopt`
            // answers with an existing blank's id, which is the same thing here —
            // either way it must be adopted, followed and then prompted.)
            const bool materialising = m_seatCreateInFlight;
            const QString created = value.toObject().value(QStringLiteral("sessionId")).toString();
            if (!created.isEmpty() && created != m_sessionId) {
                m_sessionId = created;
                // A different session is a different workspace, so the file tree
                // is dropped with it rather than left pointing at the old root.
                resetWorkspaceFiles();
                emit sessionChanged();
                qInfo() << "adopted created session:" << created;
                // Adopting the id is not enough to talk to it. The transcript is
                // only populated by a follow, and the rail is built from
                // session/list, which does not know the session exists until it is
                // read again — so without both of these a new session opens as an
                // empty pane that is not even listed.
                // A freshly created Session has no parent and no `origin`, so
                // the plain address is the one the host accepts; there is no
                // roster row to consult until `listSessions` below returns.
                // An adopted blank is followed plainly too: `planSessionTarget`
                // never offers a subagent child, so there is no tagged address to
                // rebuild here.
                m_seatFollow = materialising;
                followSession(created);
                m_seatFollow = false;
                listSessions();
            }
            if (materialising) {
                // The follow above emits sessionChanged while the seat is still
                // marked pending, which is how the window knows the text in the
                // composer was typed on the seat and now belongs to this session.
                // Only now is the seat given up and the held turn delivered.
                finishMaterialisation(created, true);
            }
            return;
        }

        if (method == QLatin1String("directoryPicker/pick")) {
            // `directoryPicker/pick` answers a path, or null when the person
            // cancelled. A cancel is not a failure and must do nothing at all:
            // no status, no error row, no notice. Anything else is the path the
            // host's own chooser returned, and the create is the next call.
            const QString path = value.toString();
            if (!path.isEmpty())
                createWorkspaceAt(path);
            // A null answer (the person cancelled) falls through to nothing at
            // all: the status the rail already had is left exactly as it was,
            // because a cancelled picker must not be able to claim "ready" over
            // an agent that is still thinking.
            return;
        }

        if (method == QLatin1String("workspace/create")) {
            // The reply is `WorkspaceCreateValue`; `created` false means the
            // registry already had a Workspace at that path. Either way nothing is
            // added to the model here — the follow stream's `upsert` is what makes
            // the Workspace appear, which is what keeps the rail showing the
            // host's registry and nothing else.
            const QJsonObject answered = value.toObject();
            if (!answered.value(QStringLiteral("created")).toBool()) {
                // `created` false: the registry already had a Workspace at that
                // path, so there is nothing to add and nothing to follow. The
                // copy says what happened rather than claiming the row is on
                // screen: the rail is the host's registry, and if the follow
                // stream is behind, "it is now on screen" would be a claim this
                // client cannot keep.
                const QString title = answered.value(QStringLiteral("workspace")).toObject()
                                          .value(QStringLiteral("title")).toString();
                emit railNotice(
                    title.isEmpty()
                        ? QStringLiteral("That folder is already a workspace; nothing new was added.")
                        : QStringLiteral("%1 is already a workspace; nothing new was added.").arg(title),
                    false);
                setStatus(QStringLiteral("workspace already exists"));
            } else {
                setStatus(QStringLiteral("workspace added"));
            }
            return;
        }

        if (method == QLatin1String("settings/mutate")) {
            // Re-read rather than trusting the local edit: the host resolves the
            // schema, so what it stores is the only authority on what is now set.
            setStatus(QStringLiteral("saved"));
            refreshSettings();
            return;
        }

        if (method == QLatin1String("schedule/catalog")) {
            applyScheduleCatalog(value.toArray());
            return;
        }

        if (method == QLatin1String("schedule/update")) {
            m_scheduleSaving = false;
            const QJsonObject answer = value.toObject();
            if (answer.value(QStringLiteral("updated")).toBool()) {
                setScheduleNotice(QStringLiteral("Task saved."), false);
            } else {
                // A compare-and-set miss is not a transport failure: the host
                // answered, and the row it answered about is no longer the row
                // the page edited. Say which, then re-read so the page shows the
                // host's record rather than a stale draft.
                const QString code = answer.value(QStringLiteral("code")).toString();
                if (code == QLatin1String("schedule_not_found"))
                    setScheduleNotice(QStringLiteral("That task no longer exists."), true);
                else if (code == QLatin1String("schedule_ended"))
                    setScheduleNotice(
                        QStringLiteral("That task has ended and can no longer be changed."), true);
                else if (code == QLatin1String("schedule_conflict"))
                    setScheduleNotice(
                        QStringLiteral("The task changed elsewhere; the list was refreshed."), true);
                else
                    setScheduleNotice(answer.value(QStringLiteral("message")).toString(
                                          QStringLiteral("The task was not saved.")), true);
            }
            refreshScheduleTasks();
            emit scheduleChanged();
            return;
        }

        if (method == QLatin1String("schedule/delete")) {
            m_scheduleSaving = false;
            const QJsonObject answer = value.toObject();
            setScheduleNotice(answer.value(QStringLiteral("deleted")).toBool()
                                  ? QStringLiteral("Task deleted.")
                                  : QStringLiteral("That task was already gone."),
                              false);
            refreshScheduleTasks();
            emit scheduleChanged();
            return;
        }

        if (method == QLatin1String("schedule/history")) {
            m_scheduleHistoryLoading = false;
            const QJsonObject answer = value.toObject();
            QVariantList records;
            for (const QJsonValue &entry : answer.value(QStringLiteral("records")).toArray()) {
                const QJsonObject record = entry.toObject();
                const QString deliveredAt =
                    record.value(QStringLiteral("deliveredAt")).toString();
                const QString scheduledAt =
                    record.value(QStringLiteral("scheduledAt")).toString();
                records.append(QVariantMap{
                    {QStringLiteral("deliveredAt"), deliveredAt},
                    {QStringLiteral("deliveredLabel"), scheduleInstant(deliveredAt)},
                    {QStringLiteral("scheduledAt"), scheduledAt},
                    {QStringLiteral("scheduledLabel"), scheduleInstant(scheduledAt)},
                    {QStringLiteral("prompt"), record.value(QStringLiteral("prompt")).toString()},
                });
            }
            if (answer.contains(QStringLiteral("code")))
                setScheduleNotice(QStringLiteral("That task's delivery records are gone."), true);
            m_scheduleHistory = records;
            emit scheduleHistoryChanged();
            return;
        }

        if (method == QLatin1String("pluginManager/listBundles")) {
            if (value.isArray()) {
                applyPluginBundles(value.toArray());
            } else {
                m_pluginsLoading = false;
                setPluginsNotice(QStringLiteral("The host's plugin list could not be read."), true);
            }
            return;
        }

        if (method == QLatin1String("pluginManager/setBundleEnabled")) {
            applyBundleChange(value);
            return;
        }

        if (method == QLatin1String("settings/describe")) {
            const QJsonObject result = value.toObject();
            QVariantList sections;
            for (const QJsonValue &entry : result.value(QStringLiteral("namespaces")).toArray()) {
                const QJsonObject space = entry.toObject();
                const QString ns = space.value(QStringLiteral("ns")).toString();
                if (ns.isEmpty())
                    continue;

                // Top-level scalars only. A full schemastery form is a project of
                // its own; until then, seeing what is actually set is most of the
                // value and none of the risk.
                // Flattened to dotted paths, because most of what is worth editing
                // is not at the top level and the panel was silently dropping it.
                // The same dot path is what a write splits back into the array the
                // host's `{op:'set', path:[…]}` expects.
                QVariantList fields;
                std::function<void(const QString &, const QJsonObject &)> walk =
                    [&](const QString &prefix, const QJsonObject &object) {
                        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                            const QString key = prefix.isEmpty()
                                                    ? it.key()
                                                    : prefix + QLatin1Char('.') + it.key();
                            if (it.value().isObject()) {
                                walk(key, it.value().toObject());
                            } else if (it.value().isString() || it.value().isDouble()
                                       || it.value().isBool()) {
                                fields.append(QVariantMap{
                                    {QStringLiteral("key"), key},
                                    {QStringLiteral("value"), it.value().toVariant()},
                                });
                            }
                        }
                    };
                walk(QString(), space.value(QStringLiteral("value")).toObject());
                if (fields.isEmpty())
                    continue;
                sections.append(QVariantMap{
                    {QStringLiteral("ns"), ns},
                    {QStringLiteral("fields"), fields},
                    {QStringLiteral("revision"),
                     space.value(QStringLiteral("revision")).toInt()},
                });
            }
            m_settingsSections = sections;
            emit settingsChanged();
            return;
        }

        if (method == QLatin1String("session/attachment")) {
            const QJsonObject result = value.toObject();
            const QJsonObject attachment = result.value(QStringLiteral("attachment")).toObject();
            // The response echoes the same ref shape as the request: `attachmentId`
            // and `mediaType`. Reading `id`/`mimeType` here meant the fetched bytes
            // were filed under an empty key and never reached the row.
            const QString id = attachment.value(QStringLiteral("attachmentId")).toString();
            const QString mime = attachment.value(QStringLiteral("mediaType")).toString();
            const QString data = result.value(QStringLiteral("data")).toString();
            if (!id.isEmpty() && !data.isEmpty()) {
                m_attachmentUrls.insert(
                    id, QStringLiteral("data:%1;base64,%2")
                            .arg(mime.isEmpty() ? QStringLiteral("image/png") : mime, data));
                ++m_attachmentsRevision;
                emit attachmentsChanged();
            }
            return;
        }

        if (method == QLatin1String("session/updateQueue")) {
            // The queue is a projection, and nothing here re-read it after a
            // mutation — so a steered or retracted item stayed in the dock until
            // some later snapshot, which made the button look inert even when the
            // host had accepted it.
            //
            // The transcript's staged row is the other half: a removed item will
            // never be delivered, so its row must stop waiting; a steered one is
            // now aiming at the next step boundary. Neither used to be touched,
            // which is how a retracted message left a row saying "queued…" for the
            // rest of the session.
            reconcileQueueMutation();
            refreshProjections();
            return;
        }

        if (method == QLatin1String("session/projections")) {
            applyProjections(value.toObject());
            syncStagedPromptRows();
            // `applyProjections` confirms a pending preset when the answer
            // carries the permissions projection. If it is still pending here,
            // the answer carried nothing usable — say so rather than leave the
            // old preset looking current.
            if (!m_pendingPermissionPreset.isEmpty()) {
                const QString pending = m_pendingPermissionPreset;
                m_pendingPermissionPreset.clear();
                setAccessNotice(
                    QStringLiteral("the host accepted %1, but the access level came back "
                                   "empty; this may still show the previous one").arg(pending));
            }
            return;
        }

        if (method == QLatin1String("permissionPresets/catalog")) {
            const QJsonObject result = value.toObject();
            QVariantList presets;
            for (const QJsonValue &entry : result.value(QStringLiteral("options")).toArray()) {
                const QJsonObject option = entry.toObject();
                presets.append(QVariantMap{
                    {QStringLiteral("value"), option.value(QStringLiteral("value")).toString()},
                    {QStringLiteral("name"), option.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("description"),
                     option.value(QStringLiteral("description")).toString()},
                });
            }
            m_permissionPresets = presets;
            emit presetsChanged();
            return;
        }

        if (method == QLatin1String("session/search")) {
            const QJsonObject result = value.toObject();
            m_searchLoading = false;
            m_searchHasMore = result.value(QStringLiteral("hasMore")).toBool();

            // Join the title in from the session list: a hit reads as something you
            // recognise rather than as an opaque id.
            QHash<QString, QString> titles;
            for (const QJsonValue &entry : m_rawSessions) {
                const QJsonObject item = entry.toObject();
                titles.insert(item.value(QStringLiteral("sessionId")).toString(),
                              item.value(QStringLiteral("title")).toString());
            }

            QVariantList results;
            for (const QJsonValue &entry : result.value(QStringLiteral("items")).toArray()) {
                const QJsonObject item = entry.toObject();
                const QString id = item.value(QStringLiteral("sessionId")).toString();
                results.append(QVariantMap{
                    {QStringLiteral("sessionId"), id},
                    {QStringLiteral("title"), titles.value(id)},
                    {QStringLiteral("snippet"), item.value(QStringLiteral("snippet")).toString()},
                });
            }
            m_searchResults = results;
            emit searchChanged();
            return;
        }

        if (method == QLatin1String("session/list")) {
            // The wire shape is a page object, not a bare array:
            //   { value: { items: [ {sessionId, cwd, updatedAt, running, …}, … ] } }
            // A bare array is tolerated too, since nothing guarantees the host
            // will not simplify the projection later.
            const QJsonArray items = value.isArray()
                ? value.toArray()
                : value.toObject().value(QStringLiteral("items")).toArray();
            emit sessionsListed(items);
            applySessionList(items);
            return;
        }

        if (method == QLatin1String("session/page")) {
            applyEarlierPage(value.toObject());
            return;
        }

        if (method.startsWith(QLatin1String("goals/"))) {
            // A lifecycle verb's own answer says the mutation landed, and nothing
            // else pushes the new goal: goals are a PROJECTION, and the follow
            // stream sends projections only in its snapshot. Without this re-read
            // a cleared goal stayed on screen until the next message happened to
            // refresh — the owner's "cleared goals do not actually clear".
            refreshProjections();
            return;
        }

        if (method == QLatin1String("session/modelCatalog")) {
            applyModelCatalog(value.toObject());
            return;
        }

        if (method == QLatin1String("session/selectModel")) {
            // The host answers with the resolved route, which is authoritative:
            // it may differ from what was asked for (a fallback, a normalised
            // effort id), and the picker must show what will actually run.
            const QJsonObject selected = value.toObject()
                                             .value(QStringLiteral("selected")).toObject();
            const QString provider = selected.value(QStringLiteral("provider")).toString();
            const QString model = selected.value(QStringLiteral("model")).toString();
            if (provider.isEmpty() || model.isEmpty()) {
                // An answer this client cannot use. Do not clear the route to
                // empty (the picker would then fall back to the catalog default
                // and show a model the session is not on); say what arrived and
                // leave the last known route alone.
                setModelNotice(
                    QStringLiteral("the host answered without a model route; the picker "
                                   "still shows the previous one"));
                return;
            }
            m_provider = provider;
            m_model = model;
            m_reasoningEffort = selected.value(QStringLiteral("reasoningEffort")).toString();
            setModelNotice(QString());
            applyModelCatalog(m_catalog);
            return;
        }

        if (method == QLatin1String("workspaceFiles/list")) {
            applyDirectoryList(value.toObject());
            return;
        }

        if (method == QLatin1String("workspaceFiles/read")) {
            applyFileRead(value.toObject());
            return;
        }
    });
}

void DshClient::refreshSessions()
{
    listSessions();
}

void DshClient::listSessions()
{
    // One at a time. A session appearing or going re-lists, so a slow host
    // otherwise accumulates requests that all land together; the reply handler
    // clears this, whichever way the call ends.
    //
    // The event that arrives mid-list is not dropped, though: the list it would
    // have started was composed before that event, so its answer cannot carry the
    // session the event reported. One follow-up is remembered and started when
    // the current one ends — a bit, not a queue, so a burst coalesces into a
    // single re-read and two lists are never in flight at once.
    if (m_listInFlight) {
        m_listQueued = true;
        return;
    }
    m_listInFlight = true;
    // Announced before the call rather than after: on a slow host the wait IS the
    // state the user is looking at, and it begins here.
    setSessionsLoading(true);

    // Every forwarded statement the host has already made is accounted for by this
    // request — it is sent after them — so the answer may drop that part of the
    // overlay, while anything the host says from now on outranks the answer.
    m_listRequestEpoch = m_rosterEpoch;

    // The argument key is the host's own parameter name, `_request`.
    call(QStringLiteral("session/list"),
         QJsonObject{{QStringLiteral("_request"), QJsonObject{}}});

    // call() bails out before posting when it cannot resolve an authority or mint
    // a cookie. It reports that through rpcFailed, but no reply will ever arrive
    // to clear the flag, and a stuck flag would mean the rail never loads again.
    if (m_state == Failed) {
        m_listInFlight = false;
        m_listQueued = false;
        setSessionsLoading(false);
    }
}

// --- workspace files --------------------------------------------------------
//
// The tree is built here rather than in QML for one reason: lazy expansion is a
// cache, and a cache spread across a ListView's component instances is a cache
// that loses entries whenever a delegate is recycled. So the flow is
//    listDirectory()/toggleDirectory()  ->  workspaceFiles/list  ->  cache
//    cache + expanded set               ->  rebuildFileRows()    ->  fileEntries
// and the QML side is one ListView bound to `fileEntries` with nothing to
// remember. `fileEntries` is the flattened *visible* rows, exactly the shape
// SessionSidebar computes for sessions — parents before children, one `depth`
// per level — so the delegate is a plain row.

QString DshClient::workspaceScopeId() const
{
    // The scope IS the session id, verbatim — the exact string the session list
    // carries and `sessionId` exposes. It is not a normalised form: probed live,
    // `session-<uuid>` resolves while its bare uuid is answered
    // `gateway/lookup-not-found`, and an automation's full
    // `dsh-automation-session-<uuid>` resolves while its bare uuid does not. Each
    // session's workspace is keyed by the id the host stored for that session, so
    // rewriting it here would break every scope but the one that happened to be
    // stored unprefixed.
    return m_sessionId;
}

QString DshClient::normaliseWorkspacePath(const QString &path)
{
    QString normalised = path.trimmed();
    while (normalised.startsWith(QLatin1String("./")))
        normalised.remove(0, 2);
    if (normalised == QLatin1String("."))
        normalised.clear();
    // The host echoes "qml" for "qml/", so a trailing slash would give the same
    // directory two cache keys and defeat the change guard.
    while (normalised.endsWith(QLatin1Char('/')))
        normalised.chop(1);
    return normalised;
}

QString DshClient::joinWorkspacePath(const QString &directory, const QString &name)
{
    return directory.isEmpty() ? name : directory + QLatin1Char('/') + name;
}

void DshClient::listDirectory(const QString &path)
{
    const QString normalised = normaliseWorkspacePath(path);

    // The current directory is by definition unfolded, so navigating into one
    // shows its children without a second click.
    m_expandedDirectories.insert(normalised);

    if (normalised != m_filePath) {
        m_filePath = normalised;
        emit filePathChanged();
    }

    // The cache is deliberately not consulted: this is the explicit "show me
    // this directory now" path (a refresh, a navigation), so a re-listing is the
    // point. toggleDirectory() is the cached one.
    requestDirectory(normalised);
}

void DshClient::toggleDirectory(const QString &path)
{
    const QString normalised = normaliseWorkspacePath(path);
    if (normalised.isEmpty())
        return;   // the root is always shown; it has no caret to fold

    if (m_expandedDirectories.contains(normalised)) {
        m_expandedDirectories.remove(normalised);
        rebuildFileRows();
        return;
    }

    m_expandedDirectories.insert(normalised);
    if (m_directories.contains(normalised)) {
        rebuildFileRows();
        return;   // already fetched: folding and unfolding is free after this
    }
    requestDirectory(normalised);
}

void DshClient::requestDirectory(const QString &path)
{
    const QString scope = workspaceScopeId();
    if (scope.isEmpty()) {
        setFileError(QStringLiteral("no session yet — the workspace belongs to a session"));
        return;
    }

    m_lastDirectoryRequest = path;
    call(QStringLiteral("workspaceFiles/list"),
         QJsonObject{
             {QStringLiteral("workspaceFileScopeId"), scope},
             // `path` is required and cannot be empty: the root is spelled ".".
             {QStringLiteral("path"), path.isEmpty() ? QStringLiteral(".") : path},
         });
}

void DshClient::applyDirectoryList(const QJsonObject &value)
{
    // The host's own normalised path, which is what the cache is keyed by — not
    // the path that was asked for, so "./qml", "qml/" and "qml" cannot become
    // three entries for one directory.
    const QString path = value.value(QStringLiteral("path")).toString();

    QVariantList entries;
    const QJsonArray raw = value.value(QStringLiteral("entries")).toArray();
    entries.reserve(raw.size());
    for (const QJsonValue &entry : raw) {
        const QJsonObject object = entry.toObject();
        entries.append(QVariantMap{
            {QStringLiteral("name"), object.value(QStringLiteral("name")).toString()},
            {QStringLiteral("type"), object.value(QStringLiteral("type")).toString()},
            // Present on files, absent on directories; -1 is "the host did not
            // say", which the view renders as nothing rather than as 0 bytes.
            {QStringLiteral("size"), object.value(QStringLiteral("size")).toDouble(-1)},
        });
    }

    m_directories.insert(path, entries);
    setFileError(QString());
    rebuildFileRows();
}

void DshClient::applyFileRead(const QJsonObject &value)
{
    setFileError(QString());

    const QString text = value.value(QStringLiteral("text")).toString();
    // `eof` is the host saying the range reached the end of the file; false means
    // the 400-line head is all we saw. Either it, or a single line longer than
    // the cap, makes this a partial view.
    const bool complete = value.value(QStringLiteral("eof")).toBool(true);
    const bool capped = text.size() > FILE_TEXT_CHAR_CAP;

    m_fileText = capped ? text.left(FILE_TEXT_CHAR_CAP) : text;
    emit fileTextChanged();

    const int bytes = int(value.value(QStringLiteral("bytes")).toDouble());
    if (bytes != m_fileBytes) {
        m_fileBytes = bytes;
        emit fileBytesChanged();
    }

    const bool truncated = !complete || capped;
    if (truncated != m_fileTruncated) {
        m_fileTruncated = truncated;
        emit fileTruncatedChanged();
    }
}

void DshClient::appendFileRows(const QString &directory, int depth, QVariantList &out) const
{
    const auto cached = m_directories.constFind(directory);
    if (cached == m_directories.constEnd())
        return;   // not fetched yet, or the listing was refused: no rows to show

    for (const QVariant &entry : *cached) {
        const QVariantMap object = entry.toMap();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QString type = object.value(QStringLiteral("type")).toString();
        const QString path = joinWorkspacePath(directory, name);
        const bool isDirectory = type == QLatin1String("directory");
        const bool expanded = isDirectory && m_expandedDirectories.contains(path);

        out.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("type"), type},
            {QStringLiteral("size"), object.value(QStringLiteral("size"))},
            {QStringLiteral("path"), path},
            {QStringLiteral("depth"), depth},
            {QStringLiteral("expanded"), expanded},
        });

        // Recurse only into what is cached: an unfolded directory still being
        // fetched shows its plain row, and its children appear when the answer
        // lands and rebuilds this.
        if (expanded && m_directories.contains(path))
            appendFileRows(path, depth + 1, out);
    }
}

void DshClient::rebuildFileRows()
{
    QVariantList flattened;
    appendFileRows(QString(), 0, flattened);

    // Emit only on a real change. A listing that returns the same children (a
    // refresh, or a host that repeats an event) must not make the tree rebuild
    // itself, exactly as with the session list.
    if (flattened == m_fileEntries)
        return;

    m_fileEntries = flattened;
    emit fileEntriesChanged();
}

void DshClient::readFile(const QString &path)
{
    const QString normalised = normaliseWorkspacePath(path);
    const QString scope = workspaceScopeId();
    if (scope.isEmpty()) {
        setFileError(QStringLiteral("no session yet — the workspace belongs to a session"));
        return;
    }
    if (normalised.isEmpty())
        return;

    setFileError(QString());

    // Selection is adopted at once so the header names the file being opened;
    // the text itself is cleared, so a slow read shows an empty view rather than
    // the previous file's contents under the new file's name.
    if (normalised != m_fileSelectedPath) {
        m_fileSelectedPath = normalised;
        emit fileSelectedPathChanged();
    }
    if (!m_fileText.isEmpty()) {
        m_fileText.clear();
        emit fileTextChanged();
    }
    // The previous file's size and truncation belong to the previous file: a
    // failed read would otherwise label the new name with the old numbers.
    if (m_fileBytes != 0) {
        m_fileBytes = 0;
        emit fileBytesChanged();
    }
    if (m_fileTruncated) {
        m_fileTruncated = false;
        emit fileTruncatedChanged();
    }

    call(QStringLiteral("workspaceFiles/read"),
         QJsonObject{
             {QStringLiteral("workspaceFileScopeId"), scope},
             {QStringLiteral("path"), normalised},
             {QStringLiteral("range"),
              QJsonObject{
                  // A 1-based *line* offset. The host rejects 0
                  // ("offset must be a safe integer of at least 1"), and `limit`
                  // is a line count capped at 5000.
                  {QStringLiteral("offset"), 1},
                  {QStringLiteral("limit"), FILE_HEAD_LINES},
              }},
         });
}

void DshClient::goUpDirectory()
{
    if (m_filePath.isEmpty())
        return;   // already at the workspace root
    const int slash = m_filePath.lastIndexOf(QLatin1Char('/'));
    listDirectory(slash < 0 ? QString() : m_filePath.left(slash));
}

void DshClient::resetWorkspaceFiles()
{
    m_directories.clear();
    m_expandedDirectories.clear();
    m_lastDirectoryRequest.clear();

    const bool hadRows = !m_fileEntries.isEmpty();
    m_fileEntries.clear();
    if (hadRows)
        emit fileEntriesChanged();

    if (!m_filePath.isEmpty()) {
        m_filePath.clear();
        emit filePathChanged();
    }
    if (!m_fileSelectedPath.isEmpty()) {
        m_fileSelectedPath.clear();
        emit fileSelectedPathChanged();
    }
    if (!m_fileText.isEmpty()) {
        m_fileText.clear();
        emit fileTextChanged();
    }
    if (m_fileBytes != 0) {
        m_fileBytes = 0;
        emit fileBytesChanged();
    }
    if (m_fileTruncated) {
        m_fileTruncated = false;
        emit fileTruncatedChanged();
    }
    setFileError(QString());
}

void DshClient::setFileError(const QString &message)
{
    if (m_fileError == message)
        return;
    m_fileError = message;
    emit fileErrorChanged();
}

// --- following a session ----------------------------------------------------
//
// `session/follow` is the stream the web UI opens on a conversation. Its items
// are NOT ACP update objects: the host sends its own durable journal vocabulary
// (`assistant/message`, `tool/call`, `tool/result`, `step/start`, …), wrapped as
//
//   {type:"snapshot", header, cursor, records:[{type:"event", event:{…}}], hasMore, projections}
//   {type:"event", event:{type, seq, time, data}}
//   {type:"assistant-stream", frame:{type:"start"|"chunk"|"end", attemptId, revision, …}}
//
// The request argument is the host method's parameter name, `request` (this is
// the method that takes `request`, unlike `session/list`'s `_request`), whose
// `address` is a tagged union. `assistantStream` is `true` or absent — the host's
// schema is a literal `true`, and sending `false` is a boundary-validation
// failure rather than "off":
//
//   {request:{address:{kind:"session", sessionId}, assistantStream:true}}
//   {request:{address:{kind:"subagent", parentSessionId, childSessionId, mode}, assistantStream:true}}
//
// Verified against the live host: a subagent child addressed as `kind:"session"`
// is refused with `session/agent-busy` ("subagent Sessions require their durable
// parent address"), and the subagent variant with a mode the child does not have
// is refused with `subagent/unauthorized` ("subagent mode does not match the
// supplied address"). `mode:"unknown"` is accepted for either and lets the host
// resolve the child's real mode, which is `values.subagent.mode` on a
// `session/list` item.

void DshClient::followSession(const QString &sessionId)
{
    if (sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session to follow"));
        return;
    }

    // A PTY belongs to the session it was opened in. The id is minted locally and
    // otherwise survives everything, so switching sessions offered the host a
    // terminal minted for a different session. Close it against the session that
    // opened it, before the follow re-points m_sessionId — and note there is no
    // `terminal/list`, so a terminal abandoned here can never be found again.
    if (!m_terminalId.isEmpty() && sessionId != m_sessionId)
        closeTerminal();

    followAddress(QJsonObject{
                      {QStringLiteral("kind"), QStringLiteral("session")},
                      {QStringLiteral("sessionId"), sessionId},
                  },
                  sessionId);

}

void DshClient::followSubagent(const QString &parentSessionId, const QString &childSessionId)
{
    // "unknown" is the host's own "resolve this child's mode for me" value, so a
    // caller that does not know the mode still gets the child instead of the
    // `subagent/unauthorized` a wrong guess would earn.
    followSubagent(parentSessionId, childSessionId, QStringLiteral("unknown"));
}

void DshClient::followSubagent(const QString &parentSessionId, const QString &childSessionId,
                               const QString &mode)
{
    if (parentSessionId.isEmpty() || childSessionId.isEmpty()) {
        setStatus(QStringLiteral("a subagent address needs both a parent and a child id"));
        return;
    }

    followAddress(QJsonObject{
                      {QStringLiteral("kind"), QStringLiteral("subagent")},
                      {QStringLiteral("parentSessionId"), parentSessionId},
                      {QStringLiteral("childSessionId"), childSessionId},
                      {QStringLiteral("mode"),
                       mode.isEmpty() ? QStringLiteral("unknown") : mode},
                  },
                  childSessionId);
}

// The reset a follow does when it must replace the transcript rather than add to
// it: a session switch, an unfollow, and a reconnect whose snapshot came back
// with a rewritten surface or a hole wider than one page.
//
// This is the function that used to be `followAddress`'s inline reset, and it is
// what a reconnect no longer runs: clearing here and replaying the snapshot is
// exactly the rebuild that made the 2026-10-25 storm look like the session view
// refreshing. It remains for the cases where continuity would be a lie.
void DshClient::resetFollowTranscript()
{
    // A live attempt belongs to the log that is being dropped, and its rows are
    // about to go with it.
    discardProvisional();
    m_transcript->clear();
    // The prompt rows went with it, so their ids must go too. This is the whole
    // reason a delivery can never be swallowed: an id whose row no longer exists
    // must not match anything, or the snapshot's replay of the message is dropped
    // as a duplicate of a row that is not there any more.
    m_localPrompts.clear();
    m_stagedPromptRows.clear();
    // The compaction rows went with the transcript for the same reason: a marker
    // key that outlived its row would make a replayed checkpoint refresh a row
    // that is not there and skip the marker the new transcript needs.
    m_compactionRows.clear();
    m_compactionFacts.clear();
    // The change payloads went with the transcript too: a callId left behind
    // would let a replayed result in the new log keep a diff from the old one.
    m_diffPayloadCalls.clear();
    // The "waiting for approval" notices went with it as well. A retained
    // approval offered again for this address must draw its row into the
    // transcript that replaced the old one, not be treated as already drawn.
    for (auto it = m_pendingWaterfalls.begin(); it != m_pendingWaterfalls.end(); ++it)
        it.value().rowDrawn = false;
    // The trajectory ledger is a projection of one session's journal, so it is
    // dropped for exactly the same reason and repopulated by the same snapshot.
    resetTrajectory();
    m_contextUsed = 0;
    m_contextSize = 0;
    emit contextChanged();

    // A different log: the previous cursor means nothing here, and a UI offering
    // "load earlier" must lose the affordance until the new snapshot says
    // otherwise.
    resetEarlierHistory();

    // Nothing of the old log is on screen any more, so there is no seq a snapshot
    // may be cut against; the next snapshot is replayed whole.
    m_transcriptThroughSeq = -1;
}

void DshClient::followAddress(const QJsonObject &address, const QString &sessionId)
{
    if (m_state != Ready) {
        setStatus(QStringLiteral("not connected yet"));
        return;
    }

    // Following the session that is already followed is not a switch, but it used
    // to run the whole reset: clear the transcript, drop the trajectory ledger,
    // zero the context counters, cancel the live stream and open another one for
    // the same journal. The rail calls `followSession` on every row click —
    // including the row that is already open — so selecting the conversation you
    // are reading blanked it and rebuilt it from a fresh snapshot, and each pass
    // also appended a second "showing the most recent history only" notice.
    // Nothing about the journal changed, so there is nothing to re-read.
    //
    // The address is compared, not just the id: a subagent child and an ordinary
    // session are different addresses over the same id, and the host refuses the
    // wrong form, so a mode change must still go through.
    //
    // An empty `m_followStreamId` means the stream is gone — a drop, a stream
    // error, an ended stream. A drop of the *same* address is handled before this
    // function is reached: onConnected calls resumeFollowAddress(), which keeps
    // the transcript. This path is therefore always a replacement, and clearing
    // is right for it.
    if (!m_followStreamId.isEmpty() && address == m_followAddress)
        return;

    // Opening any Session is a choice of a real conversation, so a seat the
    // reader never used is abandoned — its target cannot survive the reader
    // moving somewhere else, and the composer now follows that Session's own
    // draft. A materialisation's own follow is exempt: there the seat is what is
    // becoming this session, and it is given up by finishMaterialisation instead.
    if (m_pendingSeat.active && !m_seatFollow) {
        m_heldPrompt = HeldPrompt{};
        setPendingSession(false);
    }

    // The job roster is fenced by the session, so a switch must not carry the
    // old conversation's work onto the new chip. Reopening the same session (a
    // dead follow stream) keeps it: the replacement stream's first frame is
    // whole-set, so it confirms or replaces that roster immediately.
    const bool switchedSession = sessionId != m_sessionId;

    // Only one followed session at a time. Two journals interleaved into one
    // transcript is unreadable, and a second stream would keep appending to it
    // long after the user moved on. Cancelling is enough; the stale streamId is
    // also dropped, so any item already in flight for it is ignored below.
    if (!m_followStreamId.isEmpty()) {
        cancelStream(m_followStreamId);
        m_followStreamId.clear();
    }

    // A different session is a different conversation: carrying the old rows over
    // would attribute one session's output to another. The snapshot of the new
    // session repopulates the transcript immediately. A reconnect that could not
    // be continued lands here too, and the same answer is right for it: the
    // replay is the only way to fill a hole.
    m_followContinuation = false;
    resetFollowTranscript();

    // A refusal belongs to the session it was said in. Carrying it across a
    // switch would blame the new conversation for the old one's failure.
    setAccessNotice(QString());
    setModelNotice(QString());
    m_pendingPermissionPreset.clear();

    m_followAddress = address;
    m_sessionId = sessionId;
    // The green mark says "finished while you were not looking". Following the
    // session is the reader looking at it, so the mark goes — whether the switch
    // is theirs, a restore, or a subagent opened from its parent's row.
    clearUnreadSession(sessionId);
    // An approval dialog belongs to the session that raised it. Moving away
    // closes it and leaves the request retained, so it is offered again when
    // its own session is followed — never granted from the transcript of
    // another. An unscoped request (no agentId: an older host, or a frame that
    // omitted it) keeps today's behaviour and stays up, because there is no
    // identity to prove it belongs elsewhere and hiding it is the worse failure.
    if (!m_offeredApprovalId.isEmpty()) {
        const auto offered = m_pendingWaterfalls.constFind(m_offeredApprovalId);
        const bool scopedElsewhere = offered == m_pendingWaterfalls.constEnd()
                                  || (!offered.value().agentId.isEmpty()
                                      && offered.value().agentId != m_sessionId);
        if (scopedElsewhere)
            releaseOfferedApproval();
    }
    // A question belongs to the session that asked it for exactly the same
    // reason, and a batch answered from another session's transcript would be
    // journalled into that other session. Moving away stops asking here; the
    // request stays pending and is offered again where it belongs.
    if (!m_offeredQuestionId.isEmpty()) {
        const auto offered = m_pendingWaterfalls.constFind(m_offeredQuestionId);
        const bool scopedElsewhere = offered == m_pendingWaterfalls.constEnd()
                                  || (!offered.value().agentId.isEmpty()
                                      && offered.value().agentId != m_sessionId);
        if (scopedElsewhere)
            releaseOfferedQuestion();
    }
    // No snapshot belongs to this address yet. A stream that ends before one
    // never populated the transcript, and that is the failure the reader has to
    // be told about rather than left with a blank pane and a status line.
    m_followSnapshotSeen = false;
    // The file tree is scoped to the session, so a different session is a
    // different workspace: carrying the old rows over would attribute one
    // host's files to another.
    resetWorkspaceFiles();
    if (switchedSession)
        clearJobs();
    emit sessionChanged();
    setBusy(false);
    // No snapshot belongs to this address yet, so nothing about its turn is
    // known. Sending in that gap is staged, not claimed (see m_turnOpen).
    setTurnOpen(true);
    setStatus(QStringLiteral("following %1…").arg(shortSessionId(sessionId)));

    // Remember where the reader actually is, so the next launch restores their
    // session rather than whichever one the host last saw activity in.
    {
        QSettings prefs;
        prefs.setValue(QStringLiteral("session/last"), sessionId);
    }

    // Every follow goes through here, including the one the window does at
    // startup — hooking `followSession` alone left the roster stream unopened,
    // which the log showed by listing only two streams.
    //
    // A fresh follow is a fresh stream, so the one automatic recovery below is
    // available again.
    m_jobStreamRecovered = false;
    openJobStream();
    openChangesStream();
    // The transcript was just cleared and stays empty until the snapshot arrives.
    // Without this the window shows a blank conversation and no reason for it.
    setOpeningSession(true);

    m_followStreamId = openStream(
        QStringLiteral("session/follow"),
        QJsonObject{{QStringLiteral("request"), QJsonObject{
            {QStringLiteral("address"), address},
            // Opt in to the process-local assistant frames. They are the only
            // liveness signal the stream carries; without this it delivers the
            // durable journal only, once each event has already been committed.
            {QStringLiteral("assistantStream"), true},
        }}});

    // The picker is host-wide, so it is worth having even before the snapshot
    // arrives — the catalog call needs no session parameter.
    refreshConfigOptions();
}

bool DshClient::resumeFollowAddress(const QJsonObject &address, const QString &sessionId)
{
    // Continuity is only honest for the same follow. A different address is a
    // different conversation; a session with no rows yet has nothing to continue.
    if (m_state != Ready || sessionId != m_sessionId || address != m_followAddress)
        return false;
    if (m_transcriptThroughSeq < 0 || m_transcript->rowCount() == 0)
        return false;

    // The old socket's stream id is dead; onDisconnected already cleared it, but
    // a resume reached any other way must not leave a stale id blocking openStream.
    if (!m_followStreamId.isEmpty()) {
        cancelStream(m_followStreamId);
        m_followStreamId.clear();
    }

    // The marker the snapshot branch reads. It is set before the request so a
    // snapshot cannot arrive unmarked.
    m_followContinuation = true;
    // There is no blank pane to wait for: the reader's transcript is still on
    // screen. Saying "opening" would make the view pin to the bottom again for a
    // snapshot that mostly adds nothing.
    setOpeningSession(false);
    // No snapshot belongs to the new stream yet, and a stream that ends before
    // one must say so — but it must not claim nothing was loaded (the rows from
    // the previous stream are still there). onStreamEnded() distinguishes the
    // two with m_followContinuation.
    m_followSnapshotSeen = false;

    // These died with the socket. The follow path is their only other opener, and
    // that path is being replaced by this one for a reconnect. The roster's
    // session has not changed, so it stays on screen until the first frame of
    // the replacement stream replaces it whole; a fresh stream restores the one
    // automatic recovery.
    m_jobStreamRecovered = false;
    openJobStream();
    openChangesStream();

    m_followStreamId = openStream(
        QStringLiteral("session/follow"),
        QJsonObject{{QStringLiteral("request"), QJsonObject{
            {QStringLiteral("address"), address},
            {QStringLiteral("assistantStream"), true},
        }}});

    qInfo() << "session/follow resumed after a mux drop: keeping"
            << m_transcript->rowCount() << "row(s) through seq" << m_transcriptThroughSeq
            << "- the snapshot will add only what was missed";
    return true;
}

void DshClient::unfollowSession()
{
    if (m_followStreamId.isEmpty() && m_sessionId.isEmpty())
        return;

    if (!m_followStreamId.isEmpty()) {
        cancelStream(m_followStreamId);
        m_followStreamId.clear();
    }
    // The roster is fenced by the session being closed. Leaving its stream open
    // would keep a dead conversation's jobs on the chip, and its popover would
    // offer to stop work that no longer belongs to anything on screen.
    if (!m_jobStreamId.isEmpty()) {
        cancelStream(m_jobStreamId);
        m_jobStreamId.clear();
    }
    m_jobStreamRecovered = false;
    clearJobs();
    setOpeningSession(false);

    // Nothing is followed, so no approval or question dialog can be scoped to
    // anything. The request stays pending and is offered again for whichever
    // session is followed next.
    releaseOfferedApproval();
    releaseOfferedQuestion();

    m_sessionId.clear();
    emit sessionChanged();
    m_followAddress = {};
    // Nothing is followed any more, so no snapshot of this address can arrive.
    m_followSnapshotSeen = false;
    resetEarlierHistory();
    // Nothing is followed, so there is no journal to project: an unbound ledger
    // would keep showing the session that was just closed.
    resetTrajectory();

    // The stream carried the turn state, so it is stale the moment it is gone.
    setBusy(false);
    // Nothing is followed, so there is no turn this client could be inside.
    setTurnOpen(false);
    m_followContinuation = false;
    // The transcript's own copy goes with it. Its rows stay as the frozen record
    // of the conversation, but a start time that outlived its stream would date
    // the next turn from the last one's beginning.
    m_transcript->noteTurn(-1, false, 0);
    // A live attempt can be neither replaced nor continued once nothing is
    // followed, so its rows are taken back rather than left saying `Deep diving`
    // over a conversation that is closed.
    discardProvisional();
    // Nothing is followed, so the staged rows' messages can no longer arrive
    // here. They stop saying "steering…" while the rows themselves stay as the
    // frozen record of the conversation they belonged to.
    withholdStagedPrompts();
    setStatus(QStringLiteral("unfollowed"));
}

void DshClient::onStreamItem(const QString &streamId, const QJsonObject &value)
{
    // Guard on emptiness as well as equality: an unrelated stream while nothing
    // is followed must not be mistaken for the follow stream.
    if (m_followStreamId.isEmpty() || streamId != m_followStreamId)
        return;
    applyFollowFrame(value);
}

void DshClient::onStreamError(const QString &streamId, const QString &code, const QString &message)
{
    // The roster is a stream with no natural end, so a refusal here is the same
    // class of failure as the shape bug that kept it empty: the chip would stay
    // frozen on whatever it last heard. Reopen once; a second failure in a row
    // is logged and left alone rather than retried in a loop.
    if (!m_jobStreamId.isEmpty() && streamId == m_jobStreamId) {
        qWarning() << "job/list stream failed:" << code << message;
        recoverJobStream();
        return;
    }

    if (m_followStreamId.isEmpty() || streamId != m_followStreamId)
        return;

    m_followStreamId.clear();
    setOpeningSession(false);
    setBusy(false);
    // Unknown again: a stream that failed may have failed inside a live turn.
    setTurnOpen(true);
    // A failed continuation is over; the rows it kept stay (they are still the
    // session's own history), and the next follow decides from scratch.
    m_followContinuation = false;
    // The frames that would have finished this attempt cannot arrive any more.
    // Its rows are taken back rather than left mid-answer; a durable message
    // that still lands renders from the journal.
    discardProvisional();
    // The stream that would have delivered them has failed. The rows stop
    // claiming to steer; nothing is forgotten, so a delivery that still arrives
    // (a reconnect, another client's follow) becomes the same row.
    withholdStagedPrompts();
    setStatus(QStringLiteral("follow failed: %1").arg(code));

    // Surfaced as a row, not just as a status line: the transcript is where a
    // user looks, and a stream that dies silently is indistinguishable from an
    // agent that simply stopped talking.
    m_transcript->append(QStringLiteral("error"),
                         QStringLiteral("%1: %2").arg(
                             code.isEmpty() ? QStringLiteral("stream failed") : code, message));
    // No snapshot is coming on this stream either. The approval's agent is still
    // blocked, so offer it against the transcript this session does have rather
    // than let the failure hide the decision. A question held for this session
    // gets the same treatment.
    offerPendingApproval();
    offerPendingQuestion();
}

void DshClient::onStreamEnded(const QString &streamId)
{
    // `job/list` has no natural end; the carrier closes it. A close while the
    // session is still followed would leave the chip on a stale roster forever,
    // so treat it like a failure and give the stream one reopening.
    if (!m_jobStreamId.isEmpty() && streamId == m_jobStreamId) {
        qWarning() << "job/list stream ended; reopening the roster once";
        recoverJobStream();
        return;
    }

    if (m_followStreamId.isEmpty() || streamId != m_followStreamId)
        return;

    m_followStreamId.clear();
    // A follow whose stream ends before any snapshot never populated the
    // transcript at all. That used to be the whole visible result: an empty pane,
    // an ordinary "stream ended" status line and no row — which is exactly how a
    // refused subagent restore read. The error path already adds a row; this is
    // the same failure with the host saying nothing about why.
    //
    // A continuation is the one case where that is not true: the previous
    // stream's rows are still on screen, so the failure is that they may be
    // behind, not that there is nothing to read.
    const bool wasContinuation = m_followContinuation;
    const bool neverLoaded = !m_followSnapshotSeen;
    m_followContinuation = false;
    setOpeningSession(false);
    setBusy(false);
    setTurnOpen(true);
    // The stream ended, so the attempt it was describing can never be settled
    // here: the rows go rather than being frozen mid-answer.
    discardProvisional();
    withholdStagedPrompts();
    setStatus(QStringLiteral("stream ended"));
    if (neverLoaded) {
        m_transcript->append(
            QStringLiteral("error"),
            wasContinuation
                ? QStringLiteral("the session stream ended before it caught up; the "
                                 "transcript may be missing what arrived during the drop")
                : QStringLiteral("the session stream ended before any history arrived; "
                                 "nothing was loaded"));
    }
    // The stream is gone, so it will never lay down a snapshot. A retained
    // approval for this session would otherwise wait for a history that is not
    // coming; the agent behind it is still blocked, so offer it now. A retained
    // question is in the same position.
    offerPendingApproval();
    offerPendingQuestion();
}

void DshClient::applyFollowFrame(const QJsonObject &value)
{
    const QString type = value.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("snapshot")) {
        // The opening window: a page of recent durable events, the projection
        // baseline, and (because we opted in) any assistant attempt in flight.
        const QJsonArray records = value.value(QStringLiteral("records")).toArray();

        // A snapshot after a drop is a continuation, not a replay. Its page
        // overlaps rows this client already holds, and those rows are exactly the
        // records at or below m_transcriptThroughSeq — so the page is a cursor:
        // everything above the cut is appended, everything at or below it is
        // skipped. Two things make that unsafe, and both fall back to the ordinary
        // rebuild (correctness before continuity):
        //
        //  * a record whose `surfaceOp` is not `append` — a compaction rewrote the
        //    visible surface, so the rows held no longer correspond to the records;
        //  * a hole — the page's first record above the cut is not the next seq,
        //    which means more was missed than one page can supply, and appending
        //    would silently drop the turns in between.
        bool continuation = m_followContinuation;
        m_followContinuation = false;
        if (continuation) {
            bool surfaceReplaced = false;
            qint64 firstAbove = -1;
            for (const QJsonValue &record : records) {
                const QJsonObject event =
                    record.toObject().value(QStringLiteral("event")).toObject();
                const qint64 seq = eventSeq(event);
                const QJsonValue surfaceOp = event.value(QStringLiteral("surfaceOp"));
                const bool appends = surfaceOp.isUndefined() || surfaceOp.isNull()
                                  || surfaceOp.toString() == QLatin1String("append");
                if (!appends)
                    surfaceReplaced = true;
                if (seq > m_transcriptThroughSeq && firstAbove < 0)
                    firstAbove = seq;
            }
            const bool hole = firstAbove >= 0 && firstAbove > m_transcriptThroughSeq + 1;
            if (surfaceReplaced || hole) {
                qWarning() << "session/follow snapshot cannot continue the transcript:"
                           << (surfaceReplaced
                                   ? QStringLiteral("a record rewrote the surface (surfaceOp != append)")
                                   : QStringLiteral("the page begins after a hole"))
                           << "| first record above the cut" << firstAbove
                           << "| held through seq" << m_transcriptThroughSeq
                           << "-> rebuilding from the snapshot";
                continuation = false;
                resetFollowTranscript();
            }
        }

        m_replayingSnapshot = true;
        for (const QJsonValue &record : records) {
            const QJsonObject event = record.toObject().value(QStringLiteral("event")).toObject();
            if (event.isEmpty())
                continue;
            // Already on screen: the stream that just dropped delivered it, and
            // re-applying it is what replayed the whole conversation on every
            // reconnect.
            if (continuation && eventSeq(event) >= 0
                && eventSeq(event) <= m_transcriptThroughSeq)
                continue;
            applySessionEvent(event);
            noteAppliedSeq(event);
        }
        m_replayingSnapshot = false;

        // The opening window is the first page, so it leaves the first cursor.
        // `throughSeq` is an *inclusive* cut, so the page before this one ends at
        // one below the oldest record held — which is what makes the next page
        // join on without repeating that record or leaving a hole above it.
        //
        // A continuation does not move the cursor: the rows held include records
        // older than this page, and moving it to this page's oldest would make
        // "load earlier" re-fetch rows that are already on screen.
        if (!continuation) {
            m_pageCursor = records.isEmpty() ? -1 : recordSeq(records.first().toObject()) - 1;
            m_hasEarlier = value.value(QStringLiteral("hasMore")).toBool();
            refreshHasEarlier();
        }

        // Projections last, so the host's own current figures win over the
        // per-request accounting replayed from the history above.
        applyProjections(value.value(QStringLiteral("projections")).toObject());

        const QString preset = value.value(QStringLiteral("header")).toObject()
                                   .value(QStringLiteral("agentPreset")).toString();
        if (!preset.isEmpty()) {
            const QString info = QStringLiteral("DSH %1").arg(preset);
            if (m_agentInfo != info) {
                m_agentInfo = info;
                emit agentInfoChanged();
            }
        }

        // An in-flight attempt in the baseline is the only way to learn that the
        // session was already working when we attached: the history above is
        // committed, and its turn boundaries say nothing about the live turn.
        const QJsonObject active = value.value(QStringLiteral("assistantStream")).toObject()
                                       .value(QStringLiteral("activeAttempt")).toObject();
        setBusy(!active.isEmpty());
        setStatus(active.isEmpty() ? QStringLiteral("ready") : QStringLiteral("thinking…"));
        // Attaching mid-answer has to show the text already written, not start
        // from the next token: the baseline's compact stream is the same delta
        // sequence the live frames carry, packed. Without this the row would hold
        // only the suffix until the durable message replaced it.
        if (!active.isEmpty())
            seedProvisionalFromBaseline(active);
        else
            discardProvisional();

        // …but an attempt is not a turn. The host clears `activeAttempt` when a
        // model call ends, so during a tool call in a live turn the snapshot says
        // "nothing is running". The page's own records end at the head, so the
        // newest turn boundary *in the page* is the current one: a `turn/start`
        // with no later `turn/end` means the turn is open. A page with messages
        // but no boundary at all starts and ends inside one turn (its `turn/start`
        // is older than the window), so that is open too; a blank page is not.
        //
        // This is the answer sendPrompt needs and must not guess: it was taken
        // from `activeAttempt` alone before, which is how a prompt queued into a
        // running turn was echoed as a delivered bubble after a re-follow.
        //
        // The same pass is what keeps the running line's clock across a re-follow.
        // Its start is the open turn's `turn/start` record — that event's own
        // `time`, which the model owns and every snapshot replays — so a
        // reconnect that rebuilds the transcript from scratch lands on the same
        // instant the old rows were counting from. A boundary older than the page
        // leaves the start at zero: the client then draws no number, because an
        // attempt's stamp or the wall clock would be a guess dressed as a fact.
        bool turnOpen = !active.isEmpty();
        bool sawBoundary = false;
        int boundaryTurn = active.value(QStringLiteral("turn")).toInt(-1);
        qint64 turnStart = 0;
        for (const QJsonValue &record : records) {
            const QJsonObject boundary = record.toObject().value(QStringLiteral("event")).toObject();
            const QString recordType = boundary.value(QStringLiteral("type")).toString();
            if (recordType == QLatin1String("turn/start")) {
                turnOpen = true;
                sawBoundary = true;
                boundaryTurn = boundary.value(QStringLiteral("data")).toObject()
                                   .value(QStringLiteral("turn")).toInt(boundaryTurn);
                turnStart = qint64(boundary.value(QStringLiteral("time")).toDouble(0));
            } else if (recordType == QLatin1String("turn/end")) {
                turnOpen = false;
                sawBoundary = true;
                // The boundary's own number, not the replayed rows': a page can
                // hold several turns and only this record says which one ended.
                boundaryTurn = boundary.value(QStringLiteral("data")).toObject()
                                   .value(QStringLiteral("turn")).toInt(boundaryTurn);
                turnStart = 0;
            }
        }
        if (!sawBoundary && !records.isEmpty())
            turnOpen = true;
        setTurnOpen(turnOpen);
        // A page whose open turn began before the window has no boundary to name
        // the turn, so the replayed rows' newest turn stands in for the number.
        // The start stays zero there, and the label stays a plain `Deep diving`.
        if (turnOpen && boundaryTurn < 0)
            boundaryTurn = m_transcriptTurn;
        // The transcript's copy of the turn goes the same way as this one. It is
        // told here rather than left to the replayed rows, because `turn/start`
        // is a boundary and produces no row of its own.
        m_transcript->noteTurn(boundaryTurn, turnOpen, turnStart);

        // `hasMore` says the window is a page, not the whole log. Older history
        // is reachable through loadEarlierHistory(), but its rows cannot be put
        // above this one, so the notice stays: presenting a truncated
        // conversation as the whole one is the failure worth a row.
        //
        // Not repeated on a continuation: the notice is already in the transcript
        // from the first snapshot, and a second one per reconnect is the noise the
        // storm produced.
        if (!continuation && value.value(QStringLiteral("hasMore")).toBool()) {
            m_transcript->append(QStringLiteral("notice"),
                                 QStringLiteral("showing the most recent history only"));
        }

        syncStagedPromptRows();

        // The snapshot is fully applied, and its rows are the burst a view has
        // to land at the bottom of. `openingSession` goes false last, not first,
        // so the one notification a view gets for "the wait is over" also means
        // "every row is here" — the snapshot is a single frame, and pinning to
        // its tail is one deferred pass rather than one per row.
        m_followSnapshotSeen = true;
        // The history is in place, so a retained approval for this session can
        // draw its row after it rather than above it, and open its dialog. A
        // retained question is offered at the same point. A session switch
        // reached the same point through this call.
        offerPendingApproval();
        offerPendingQuestion();
        setOpeningSession(false);
        return;
    }

    if (type == QLatin1String("event")) {
        const QJsonObject event = value.value(QStringLiteral("event")).toObject();
        const QString eventType = event.value(QStringLiteral("type")).toString();

        // A record the transcript already holds would be a second copy of it.
        // This should not happen — the stream resumes at the snapshot's head —
        // but a reconnect makes the overlap possible, and a duplicate message is
        // worse than one skipped frame. It also makes the live path obey the same
        // cut as the snapshot path.
        if (eventSeq(event) >= 0 && eventSeq(event) <= m_transcriptThroughSeq) {
            qInfo() << "session/follow event already rendered, skipping seq"
                    << eventSeq(event) << "type" << eventType;
            return;
        }

        // Turn boundaries are the one piece of journal state the UI needs live:
        // they are what makes the Send button become Stop. They are read here,
        // from live frames only — a `turn/start` replayed out of the opening
        // window belongs to a turn that has long since ended.
        if (eventType == QLatin1String("turn/start")) {
            setBusy(true);
            setTurnOpen(true);
            setStatus(QStringLiteral("working…"));
            // The one turn the fold must leave alone: it is still being written,
            // and collapsing work as it happens is worse than a long list.
            //
            // The boundary's own `time` is the running line's clock. It is the
            // host's instant, not this client's, so the number means the same
            // thing here as in the web client — and it does not move when a new
            // attempt starts or when a reconnect replays the journal.
            const int startTurn = event.value(QStringLiteral("data")).toObject()
                                       .value(QStringLiteral("turn")).toInt(-1);
            if (startTurn >= 0)
                m_transcript->noteTurn(startTurn, true,
                                       qint64(event.value(QStringLiteral("time")).toDouble(0)));
        } else if (eventType == QLatin1String("turn/end")) {
            setBusy(false);
            setTurnOpen(false);
            setStatus(QStringLiteral("ready"));
            // A turn that has ended cannot settle an attempt any more. Anything
            // still provisional here is a dropped stream's leftovers, and the
            // fold below is about to close over the turn's rows.
            discardProvisional();
            // Ended, so it folds — including the turn that was live until now —
            // and its clock stops with it rather than dating the next turn.
            m_transcript->noteTurn(m_transcriptTurn, false, 0);
            // A turn end is the host's own claim boundary: queued input is either
            // consumed (journaled as `user/message`, handled where it lands) or
            // dropped, and the inbox is no longer what this client last read. This
            // is the second of the two moments the dock can go stale, and it was
            // never refreshed here — which is why a delivered message's dock row
            // outlived its message.
            refreshProjections();
        }

        // A turn boundary or an ordinary durable record: either way it is now part
        // of the transcript, and its seq is the cut a later reconnect snapshot is
        // skipped against.
        noteAppliedSeq(event);
        applySessionEvent(event);
        return;
    }

    if (type == QLatin1String("assistant-stream")) {
        // Process-local presentation frames: the deltas of the attempt being
        // written. They are built into PROVISIONAL rows that the durable
        // `assistant/message` later replaces in place — see
        // reconcileProvisionalMessage(). Frames are never folded into the
        // transcript as history, which is what used to make every token appear
        // twice and is why they were ignored entirely before.
        //
        // The end of an attempt is not the end of the turn — a turn runs many
        // attempts — so `end` closes these rows, not the busy state. `turn/end`
        // is what ends a turn, and the busy/status a `start` implies are set by
        // applyAssistantFrame itself.
        const QJsonObject frame = value.value(QStringLiteral("frame")).toObject();
        // A frame for an attempt proves a turn is open even when the boundary
        // itself was older than the snapshot's window — and that is a fact about
        // the journal's turn state, which the attempt's own rows know nothing of.
        if (frame.value(QStringLiteral("type")).toString() == QLatin1String("start"))
            setTurnOpen(true);
        applyAssistantFrame(frame);
        return;
    }

    qInfo() << "unhandled session/follow item type:" << type;
}

// --- live assistant streaming ----------------------------------------------
//
// The rows below are the one place in this client where a row is created from
// something that is NOT a durable event. Everything here is therefore about
// getting rid of them again: replaced by the journal's own copy of the same
// text, or withdrawn. See the note in DshClient.h for the identity rule.

namespace {

/// One chunk with the timestamp the compact stream recorded for it.
struct TimedChunk {
    double time = 0;      ///< epoch ms, fractional in principle
    QJsonObject chunk;
};

/// Expand the compact `AssistantStreamRecord[]` the durable events (and the
/// reconnect baseline) carry into the delta sequence it was compacted from.
///
/// A faithful port of `expandAssistantStream` in @deepseek-ai/dsh-llm, including
/// the member count: a tool-call run expands to one member per delta even though
/// no text row uses it, because `nextIndex` counts EVERY chunk frame and a
/// skipped member would shift every position after it.
QList<TimedChunk> expandAssistantStream(const QJsonArray &records)
{
    QList<TimedChunk> chunks;

    for (const QJsonValue &value : records) {
        const QJsonObject record = value.toObject();
        const QString type = record.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("chunk")) {
            chunks.append(TimedChunk{record.value(QStringLiteral("time")).toDouble(0),
                                     record.value(QStringLiteral("chunk")).toObject()});
            continue;
        }

        const bool textRun = type == QLatin1String("text-chunks");
        const bool reasoningRun = type == QLatin1String("reasoning-chunks");
        const bool toolRun = type == QLatin1String("tool-call-chunks");
        if (!textRun && !reasoningRun && !toolRun)
            continue;   // an unknown record type has no positional meaning here

        const QJsonArray members = textRun || reasoningRun
            ? record.value(QStringLiteral("texts")).toArray()
            : record.value(QStringLiteral("args")).toArray();
        const QJsonArray deltas = record.value(QStringLiteral("dt")).toArray();
        const int blockIndex = record.value(QStringLiteral("index")).toInt();

        double time = record.value(QStringLiteral("time0")).toDouble(0);
        for (int i = 0; i < members.size(); ++i) {
            if (i > 0)
                time += deltas.at(i - 1).toDouble(0);

            QJsonObject chunk;
            if (textRun) {
                chunk = QJsonObject{{QStringLiteral("type"), QStringLiteral("text-delta")},
                                    {QStringLiteral("index"), blockIndex},
                                    {QStringLiteral("text"), members.at(i).toString()}};
            } else if (reasoningRun) {
                chunk = QJsonObject{{QStringLiteral("type"), QStringLiteral("reasoning-delta")},
                                    {QStringLiteral("index"), blockIndex},
                                    {QStringLiteral("text"), members.at(i).toString()}};
            } else {
                chunk = QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("tool-call-delta")},
                    {QStringLiteral("index"), blockIndex},
                    {QStringLiteral("id"), record.value(QStringLiteral("id"))},
                    {QStringLiteral("argumentsDelta"), members.at(i).toString()}};
                if (record.contains(QStringLiteral("name")))
                    chunk.insert(QStringLiteral("name"), record.value(QStringLiteral("name")));
            }
            chunks.append(TimedChunk{time, chunk});
        }
    }

    return chunks;
}

/// The visible text of one assistant message as the journal translation would
/// row it: consecutive reasoning/text blocks of the same kind merge, empty ones
/// and tool-call blocks drop out. Mirrors `appendOrExtend`'s merge, applied to
/// the block sequence rather than to whatever row happens to be at the tail.
QList<QPair<bool, QString>> messageRuns(const QJsonObject &data)
{
    QList<QPair<bool, QString>> runs;
    const QJsonArray content = data.value(QStringLiteral("message")).toObject()
                                   .value(QStringLiteral("content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        const QString blockType = block.value(QStringLiteral("type")).toString();
        const bool thought = blockType == QLatin1String("reasoning");
        if (!thought && blockType != QLatin1String("text"))
            continue;
        const QString text = block.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            continue;
        if (!runs.isEmpty() && runs.last().first == thought)
            runs.last().second += text;
        else
            runs.append({thought, text});
    }
    return runs;
}

}   // namespace

QString DshClient::provisionalKey(bool thought) const
{
    // The stream's own identity, as the brief says: the attemptId. The thought
    // row takes a suffix so the two rows of one attempt are addressable apart.
    return thought ? m_attempt.attemptId + QStringLiteral(":reason")
                   : m_attempt.attemptId;
}

void DshClient::drawProvisionalRows()
{
    // Two rows, thought first. A row can only be appended below what is already
    // held, and a reasoning model thinks before it answers — creating both here
    // is what lets the first delta land in the right row without a later
    // re-order. Until text arrives the thought row is empty and the delegate
    // gives it nothing; the agent row is the one that says `Deep diving …`.
    m_transcript->append(QStringLiteral("thought"), m_attempt.thoughtText,
                         QString(), QStringLiteral("streaming"), QString(),
                         provisionalKey(true), m_attempt.startTime, m_attempt.turn);
    m_transcript->append(QStringLiteral("agent"), m_attempt.agentText,
                         QString(), QStringLiteral("streaming"), QString(),
                         provisionalKey(false), m_attempt.startTime, m_attempt.turn);
    // No tail flag is raised here. Whether the journal's next chunk may extend
    // the row above is a property of that row — its kind, its status, its turn —
    // and is read from the model when the chunk arrives (see extendRow). A flag
    // could not be lowered again: these rows are withdrawn, not deleted, so the
    // model keeps them for the rest of the session and the flag would have
    // outlived the thing it described.
}

void DshClient::appendProvisionalChunk(bool thought, const QString &delta)
{
    if (delta.isEmpty())
        return;
    QString &held = thought ? m_attempt.thoughtText : m_attempt.agentText;
    held += delta;
    m_transcript->updateByKey(provisionalKey(thought), QStringLiteral("streaming"),
                             QString(), held);
}

void DshClient::seedProvisionalFromBaseline(const QJsonObject &active)
{
    // A reconnect is a replacement, never an addition: the opening window
    // replays from the top and any rows held are already duplicated by it.
    discardProvisional();

    m_attempt.attemptId = active.value(QStringLiteral("attemptId")).toString();
    if (m_attempt.attemptId.isEmpty())
        return;
    m_attempt.turn = active.value(QStringLiteral("turn")).toInt(-1);
    m_attempt.step = active.value(QStringLiteral("step")).toInt(-1);
    m_attempt.startedAfterSeq = static_cast<qint64>(
        active.value(QStringLiteral("startedAfterSeq")).toDouble(-1));
    m_attempt.nextIndex = active.value(QStringLiteral("nextIndex")).toInt(0);
    // The baseline names no start time. The first chunk does, and the elapsed
    // label is meant to count from the attempt's beginning, so the earliest
    // chunk time is the honest stamp — with the wall clock as the fallback for
    // an attempt that has produced nothing yet.
    m_attempt.startTime = QDateTime::currentMSecsSinceEpoch();

    const QList<TimedChunk> members =
        expandAssistantStream(active.value(QStringLiteral("stream")).toArray());
    for (int i = 0; i < members.size() && i < m_attempt.nextIndex; ++i) {
        const TimedChunk &member = members.at(i);
        const QString chunkType = member.chunk.value(QStringLiteral("type")).toString();
        if (chunkType == QLatin1String("text-delta"))
            m_attempt.agentText += member.chunk.value(QStringLiteral("text")).toString();
        else if (chunkType == QLatin1String("reasoning-delta"))
            m_attempt.thoughtText += member.chunk.value(QStringLiteral("text")).toString();
    }
    // The earliest member's time is when the attempt began; the loop above must
    // not leave the row dated to its last token.
    if (!members.isEmpty() && members.first().time > 0)
        m_attempt.startTime = static_cast<qint64>(members.first().time);

    drawProvisionalRows();
}

void DshClient::applyAssistantFrame(const QJsonObject &frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    const QString attemptId = frame.value(QStringLiteral("attemptId")).toString();

    if (type == QLatin1String("start")) {
        // A start while another attempt is open means this client's built-up
        // rows can no longer be trusted to be the ones the next message settles
        // (the web client treats it as a rebaseline for the same reason). Drop
        // them and begin clean; the old text, if any, arrives as a durable
        // message and renders from the journal.
        discardProvisional();
        m_attempt.attemptId = attemptId;
        m_attempt.turn = frame.value(QStringLiteral("turn")).toInt(-1);
        m_attempt.step = frame.value(QStringLiteral("step")).toInt(-1);
        m_attempt.startedAfterSeq = static_cast<qint64>(
            frame.value(QStringLiteral("startedAfterSeq")).toDouble(-1));
        m_attempt.startTime = QDateTime::currentMSecsSinceEpoch();
        // A start is the host saying a model call is running, whether or not the
        // frame names the attempt — so the button flips here, before the rows are
        // drawn, and the caller does not have to know it. The end of an attempt
        // is not the end of the turn, so nothing here unsets it: `turn/end` does.
        setBusy(true);
        setStatus(QStringLiteral("thinking…"));
        if (m_attempt.attemptId.isEmpty())
            return;
        drawProvisionalRows();
        return;
    }

    if (type == QLatin1String("chunk")) {
        if (!m_attempt.alive() || attemptId != m_attempt.attemptId)
            return;   // an attempt this client did not see start: the journal owns it
        const int index = frame.value(QStringLiteral("index")).toInt(-1);
        if (index != m_attempt.nextIndex) {
            // A hole: the rows held are not the prefix of the message that will
            // be committed, so they cannot be swapped for it. Drop them — the
            // journal's own copy renders whole when it arrives.
            discardProvisional();
            return;
        }
        ++m_attempt.nextIndex;
        const QJsonObject chunk = frame.value(QStringLiteral("chunk")).toObject();
        const QString chunkType = chunk.value(QStringLiteral("type")).toString();
        if (chunkType == QLatin1String("text-delta")) {
            appendProvisionalChunk(false, chunk.value(QStringLiteral("text")).toString());
        } else if (chunkType == QLatin1String("reasoning-delta")) {
            appendProvisionalChunk(true, chunk.value(QStringLiteral("text")).toString());
        }
        // block-start / block-end / tool-call-delta / usage / finish are not
        // visible text. block-end in particular repeats text the deltas already
        // carried, so reading it would double the row.
        return;
    }

    if (type == QLatin1String("end")) {
        if (!m_attempt.alive() || attemptId != m_attempt.attemptId)
            return;   // already settled by its message, or never ours
        const QJsonObject outcome = frame.value(QStringLiteral("outcome")).toObject();
        const QString settledType = outcome.value(QStringLiteral("eventType")).toString();
        if (outcome.value(QStringLiteral("kind")).toString() != QLatin1String("committed")
            || settledType != QLatin1String("assistant/message")) {
            // Abandoned, or committed as an `assistant/attempt` that carries no
            // message: nothing will replace these rows, so they go now.
            discardProvisional();
            return;
        }
        // The durable message normally arrives before this marker (the web
        // client stages it for exactly that reason), and claiming it already
        // reset the attempt. Getting here with one open means the settlement is
        // still to come: record the seq it named, and keep the rows until it.
        m_attempt.settledSeq = static_cast<qint64>(
            outcome.value(QStringLiteral("seq")).toDouble(-1));
        return;
    }
}

bool DshClient::reconcileProvisionalMessage(const QJsonObject &event,
                                            const QJsonObject &data)
{
    if (!m_attempt.alive())
        return false;
    // A page being translated belongs above the transcript, not in it: a record
    // replayed from it can never be the live attempt's settlement, and claiming
    // one would rewrite the live row with old text.
    if (m_earlierSink)
        return false;

    const qint64 seq = static_cast<qint64>(
        event.value(QStringLiteral("seq")).toDouble(-1));
    // The attempt began after this seq, so anything at or below it was already
    // journalled and cannot be its settlement.
    if (seq <= m_attempt.startedAfterSeq)
        return false;

    // `surfaceOp` is on the event, not in its data. A `replace` is a compaction
    // rewriting the surface, never a live attempt's answer — the same test the
    // web client's fold makes.
    const QJsonValue surfaceOp = event.value(QStringLiteral("surfaceOp"));
    const bool appends = surfaceOp.isUndefined() || surfaceOp.isNull()
                      || surfaceOp.toString() == QLatin1String("append");

    // The durable event carries no attemptId. Its identity is the (turn, step)
    // the `start` frame announced, and the host commits exactly one assistant
    // message per step — that pair is what makes the replacement exact.
    const bool sameStep = data.value(QStringLiteral("turn")).toInt(-2) == m_attempt.turn
                       && data.value(QStringLiteral("step")).toInt(-2) == m_attempt.step;

    if (!sameStep) {
        // A message newer than this attempt's start that is NOT its settlement:
        // the attempt can no longer be assumed to be the row the journal will
        // describe. Dropping the preview is the safe answer — keeping it is how
        // a missing or renamed `step` field would show the text twice.
        discardProvisional();
        return false;
    }

    if (!appends) {
        // A message for our step that is not an append is not our settlement.
        // Nothing else will be, so the rows go and the journal's copy stands.
        discardProvisional();
        return false;
    }
    if (m_attempt.settledSeq >= 0 && seq != m_attempt.settledSeq) {
        // `end` named a different event as this attempt's settlement.
        discardProvisional();
        return false;
    }

    const QList<QPair<bool, QString>> runs = messageRuns(data);
    const QString thoughtKey = provisionalKey(true);
    const QString agentKey = provisionalKey(false);

    // The exact swap: the two provisional rows are the same sequence of kinds
    // the journal produced, so each row's text is rewritten in place. The row
    // keeps its identity, its position, its start stamp and the view's scroll.
    bool replaced = false;
    if (runs.size() == 2 && runs.at(0).first && !runs.at(1).first) {
        m_transcript->updateByKey(thoughtKey, QStringLiteral("completed"),
                                  QString(), runs.at(0).second);
        replaced = m_transcript->updateByKey(agentKey, QStringLiteral("completed"),
                                             QString(), runs.at(1).second);
    } else if (runs.size() == 1 && !runs.at(0).first) {
        // An answer with no reasoning: the thought row never drew anything and
        // is retired; the agent row becomes the message.
        m_transcript->updateByKey(thoughtKey, QStringLiteral("withdrawn"));
        replaced = m_transcript->updateByKey(agentKey, QStringLiteral("completed"),
                                             QString(), runs.at(0).second);
    } else if (runs.size() == 1 && runs.at(0).first) {
        m_transcript->updateByKey(agentKey, QStringLiteral("withdrawn"));
        replaced = m_transcript->updateByKey(thoughtKey, QStringLiteral("completed"),
                                             QString(), runs.at(0).second);
    } else if (runs.isEmpty()) {
        // A message with no visible text at all (tool calls only).
        m_transcript->updateByKey(thoughtKey, QStringLiteral("withdrawn"));
        m_transcript->updateByKey(agentKey, QStringLiteral("withdrawn"));
        replaced = true;
    }

    if (!replaced) {
        // Anything else — alternating blocks, or a row that is no longer held
        // because the transcript was replaced — cannot be lined up row for row.
        // Hide both provisional rows and let the journal's own rows stand; a
        // withdrawn row draws nothing, so the text is there exactly once.
        m_transcript->updateByKey(thoughtKey, QStringLiteral("withdrawn"));
        m_transcript->updateByKey(agentKey, QStringLiteral("withdrawn"));
        const qint64 time = static_cast<qint64>(
            event.value(QStringLiteral("time")).toDouble(0));
        for (const QPair<bool, QString> &run : runs)
            appendRow(run.first ? QStringLiteral("thought") : QStringLiteral("agent"),
                      run.second, QString(), QString(), QString(), QString(), time,
                      m_attempt.turn);
    }

    // Settled: these rows now belong to the journal, so no later frame or
    // discard may withdraw them.
    m_attempt.reset();
    return true;
}

void DshClient::discardProvisional()
{
    if (!m_attempt.alive())
        return;
    // AcpTranscript has no delete. A row that must go keeps its place and is
    // told to draw nothing — the same instrument a withdrawn prompt uses — and
    // the delegate gives a withdrawn row no height, so it cannot linger as a gap.
    m_transcript->updateByKey(provisionalKey(true), QStringLiteral("withdrawn"));
    m_transcript->updateByKey(provisionalKey(false), QStringLiteral("withdrawn"));
    m_attempt.reset();
}

/// A compact unified-ish diff for an edit or write call, from the tool's own
/// arguments. The workspace watch cannot produce one: it reports a version token
/// and a path, never the previous text, so the arguments are the only place both
/// sides of a change exist.
///
/// Capped at 60 lines. A longer diff is not something to read inline, and the
/// file is one click away in the Files panel.
static QString diffFromToolArguments(const QString &name, const QJsonObject &arguments)
{
    QStringList lines;
    if (name == QLatin1String("edit")) {
        const QString oldText = arguments.value(QStringLiteral("old_string")).toString();
        const QString newText = arguments.value(QStringLiteral("new_string")).toString();
        if (oldText.isEmpty() && newText.isEmpty())
            return {};
        for (const QString &line : oldText.split(QLatin1Char('\n')))
            lines.append(QStringLiteral("- ") + line);
        for (const QString &line : newText.split(QLatin1Char('\n')))
            lines.append(QStringLiteral("+ ") + line);
    } else if (name == QLatin1String("write")) {
        const QString content = arguments.value(QStringLiteral("content")).toString();
        if (content.isEmpty())
            return {};
        for (const QString &line : content.split(QLatin1Char('\n')))
            lines.append(QStringLiteral("+ ") + line);
    } else {
        return {};
    }

    if (lines.size() > 60) {
        const int hidden = lines.size() - 60;
        lines = lines.mid(0, 60);
        lines.append(QStringLiteral("\u2026 %1 more line(s)").arg(hidden));
    }
    return lines.join(QLatin1Char('\n'));
}

/// The file a tool call is about, from the key that tool actually sends.
///
/// Measured against this box's journals (2026-10-25; `session-f5e254b5` alone,
/// and then every one of the 78 sessions in this workspace): `edit` (407 calls
/// in that session, 1 705 across the workspace), `write` (58 / 206), `read`
/// (129 / 1 037) and `read_image` (170 / 401) all send `file_path` —
/// `read_image` has no image-specific key. None of them sends `path`; the only
/// tool that does is `grep` (1 / 19), and no tool sends both. The diff row used
/// to read `path` for an edit, a key the edit tool never sends, so its title was
/// empty and the row collapsed to its body's first line: a bare "-".
///
/// One resolver rather than a `value(a, value(b, …))` at each call site: a
/// change row's file and every other tool's one-line summary both ask here, so
/// they cannot disagree about which file a call touched.
static QString toolFileArgument(const QJsonObject &arguments)
{
    static const QStringList keys{QStringLiteral("file_path"), QStringLiteral("path")};
    for (const QString &key : keys) {
        const QString value = arguments.value(key).toString().trimmed();
        if (!value.isEmpty())
            return value;
    }
    return {};
}

/// What an edit or a write changed, as the collapsed row's second half: the file,
/// then what happened to it. Empty for every other tool, whose own `toolSummary`
/// stands.
///
/// The web's copy is `Edited {name}` plus `+{count}`/`-{count}`. This row is a
/// tool row and already carries the tool's display name as its title, so the
/// shape (the subject, then the counts) is taken and the name is not repeated.
/// For an edit the counts are the lines of the replaced block — the same old and
/// new text the diff payload is built from, so the line and the payload agree.
/// For a write it is what was written: its line count and its size.
///
/// The path is deliberately NOT truncated here: the delegate elides it to the
/// row's width, and a `toolSummary` cap would cut the file name off a long path
/// before the row ever got a chance to show as much of it as fits.
static QString changeSummary(const QString &name, const QJsonObject &arguments)
{
    if (name != QLatin1String("edit") && name != QLatin1String("write"))
        return {};

    const QString file = toolFileArgument(arguments);
    if (name == QLatin1String("edit")) {
        const QString oldText = arguments.value(QStringLiteral("old_string")).toString();
        const QString newText = arguments.value(QStringLiteral("new_string")).toString();
        // Nothing to count: the file is still the fact the row has to carry.
        if (oldText.isEmpty() && newText.isEmpty())
            return file;
        const int removed = oldText.isEmpty() ? 0 : int(oldText.split(QLatin1Char('\n')).size());
        const int added = newText.isEmpty() ? 0 : int(newText.split(QLatin1Char('\n')).size());
        // U+2212, not a hyphen: a count reads as arithmetic, not as a diff line.
        QString counts = QStringLiteral("+%1 \u2212%2").arg(added).arg(removed);
        // `replace_all` changes every occurrence while the arguments (and so the
        // diff) carry only one block. The host's own result says so in words
        // ("All occurrences were successfully replaced"), and dropping that
        // result would leave the row claiming a single change: the fact rides
        // the line instead.
        if (arguments.value(QStringLiteral("replace_all")).toBool())
            counts += QStringLiteral(" \u00b7 all occurrences");
        return file.isEmpty() ? counts : file + QStringLiteral(" \u00b7 ") + counts;
    }

    const QString content = arguments.value(QStringLiteral("content")).toString();
    if (content.isEmpty())
        return file;
    // A trailing newline ends the last line rather than opening an empty one.
    const int lines = int(content.count(QLatin1Char('\n')))
                      + (content.endsWith(QLatin1Char('\n')) ? 0 : 1);
    QString change = QStringLiteral("wrote %1 line%2")
                         .arg(lines)
                         .arg(lines == 1 ? QString() : QStringLiteral("s"));
    const QString size = byteSizeText(int(content.toUtf8().size()));
    if (!size.isEmpty())
        change += QStringLiteral(" \u00b7 ") + size;
    return file.isEmpty() ? change : file + QStringLiteral(" \u00b7 ") + change;
}

void DshClient::applySessionEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const QJsonObject data = event.value(QStringLiteral("data")).toObject();

    // Which turn this event belongs to. Every journal event carries `turn`, and
    // the last one seen stands in for the few that do not. Read from the event
    // rather than from a counter so that a page of replayed history arrives
    // already numbered, and its turns fold exactly like the live ones.
    const int eventTurn = data.value(QStringLiteral("turn")).toInt(m_transcriptTurn);
    if (data.contains(QStringLiteral("turn")))
        m_transcriptTurn = eventTurn;

    // The trajectory is folded first, from the raw record. It is the same event
    // the translation below turns into ACP vocabulary, but the ledger needs the
    // fields that translation drops (callId, arguments, isError, source.kind),
    // so it reads the record rather than the translated update. Folding here
    // rather than in a second decoder is what keeps the two views from ever
    // disagreeing about which records a session has.
    applyTrajectoryEvent(event);

    // The translation below deliberately mirrors the host's own ACP bridge
    // (@deepseek-ai/dsh-acp's codec), because that bridge is the definition of
    // "the two transports render the same": a committed assistant message
    // becomes one thought chunk per reasoning block and one message chunk per
    // text block, a tool call becomes the generic ACP tool lifecycle, and
    // reported usage becomes a usage update. Journal event types the bridge does
    // not project (`step/*`, `request/*`, delivery receipts, …) are ignored here
    // for the same reason rather than logged, since they are numerous and never
    // transcript-visible.
    if (type == QLatin1String("assistant/message")) {
        // The settlement of a live attempt, if one is open. Checked BEFORE the
        // ordinary translation, because that translation would extend the tail —
        // and the tail is the provisional row this very message has to replace.
        // Claiming rewrites those rows in place with the journal's own text; the
        // two never coexist as visible copies.
        if (!reconcileProvisionalMessage(event, data)) {
            const QJsonArray content = data.value(QStringLiteral("message")).toObject()
                                           .value(QStringLiteral("content")).toArray();
            for (const QJsonValue &blockValue : content) {
                const QJsonObject block = blockValue.toObject();
                const QString blockType = block.value(QStringLiteral("type")).toString();
                if (blockType != QLatin1String("reasoning") && blockType != QLatin1String("text"))
                    continue;   // images and tool-call blocks are never message content

                const QString text = block.value(QStringLiteral("text")).toString();
                if (text.isEmpty())
                    continue;

                applySessionUpdate(QJsonObject{
                    {QStringLiteral("sessionUpdate"),
                     blockType == QLatin1String("reasoning")
                         ? QStringLiteral("agent_thought_chunk")
                         : QStringLiteral("agent_message_chunk")},
                    {QStringLiteral("content"), QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), text},
                    }},
                }, qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
            }
        }

        // Context occupancy, expressed as the same usage update the ACP path
        // delivers. The host's own definition of context pressure is prompt-side
        // traffic: input plus cache reads and writes, and no output. The window
        // itself only arrives with the projection baseline, so a usage record on
        // its own cannot fill the header readout.
        const QJsonObject usage = data.value(QStringLiteral("usage")).toObject();
        if (!usage.isEmpty() && m_contextSize > 0) {
            applySessionUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("usage_update")},
                {QStringLiteral("used"), usage.value(QStringLiteral("inputTokens")).toInt()
                     + usage.value(QStringLiteral("cacheReadTokens")).toInt()
                     + usage.value(QStringLiteral("cacheWriteTokens")).toInt()},
                {QStringLiteral("size"), m_contextSize},
            });
        }
        return;
    }

    if (type == QLatin1String("user/message")) {
        // A prompt this client put in the transcript is already there, and this
        // event is what happens to its row.
        //
        // While a turn runs the row is a delivery *state* — "steering…" or
        // "queued…" — because the host does not journal the message until it
        // delivers it. So this is the delivery: the row becomes the message in
        // place, with the host's own text. When the prompt started the turn, the
        // row already is the message and this copy is the same thing said twice.
        // Everything else — another client's prompt, an injected file-change
        // notice, a goal continuation — is news to this transcript.
        const QJsonObject source = data.value(QStringLiteral("source")).toObject();
        const QString rpcId = source.value(QStringLiteral("rpcId")).toString();
        bool reconciled = false;   // the durable text is already in a row

        const auto local = m_localPrompts.constFind(rpcId);
        const bool staged = local != m_localPrompts.cend()
                         && local.value() == LocalPromptStaged;
        // A page is older history: it can never carry a prompt that has not been
        // delivered yet, and its rows belong above the transcript, so a staged
        // entry is left alone there rather than addressed against the live rows.
        if (staged && !m_earlierSink) {
            // The model's one in-place instrument is status and text, not kind,
            // so the staged row was created as a user row that is merely not yet
            // delivered — the kind this event needs it to be.
            reconciled = m_transcript->updateByKey(rpcId, QStringLiteral("delivered"),
                                                  QString(), contentText(
                                                      data.value(QStringLiteral("content"))
                                                          .toArray()));
            // A false return means the row is gone (the transcript was replaced),
            // so the text is rendered below instead. A delivery is never
            // swallowed: that is the invariant this whole path exists for.
            m_localPrompts.remove(rpcId);
        } else if (!staged && local != m_localPrompts.cend()) {
            m_localPrompts.remove(rpcId);
            // The existence probe is also the decision: a row that is still held
            // and already carries the message makes this copy the duplicate the
            // echo exists to prevent. One that is gone — a transcript replaced
            // while the id was remembered — used to swallow the message here,
            // which is exactly how a prompt could disappear from the transcript.
            //
            // It does NOT return: an echoed prompt is one the RPC reply already
            // carried, so its rows must not be drawn twice — but the attachments
            // it was sent with are rows this transcript has never drawn at all,
            // and returning here dropped every attachment a person had just
            // attached. The text is skipped by `reconciled` below; the file and
            // image blocks of the same message are still appended from its own
            // content.
            if (m_transcript->updateByKey(rpcId, QString()))
                reconciled = true;
        }

        // Not every user-role message is a person.
        //
        // The journal projects the same `user/message` event for a typed prompt, a
        // goal continuation round, a background-job notice, a subagent settling and
        // the harness's "Time sampled …" blocks. `source.kind` is what tells them
        // apart — `user` is the only human one, and there is no separate `system`
        // kind to look for. Without this every one of them rendered as a bubble
        // misattributing text to the reader.
        //
        // `form` is a second, independent axis describing shape, not authorship.
        const QString sourceKind = source.value(QStringLiteral("kind")).toString();

        // Delivery is the moment a queued message stops being queued, and it is
        // the one moment the dock was never told about: its list is a snapshot of
        // the host's inbox, refreshed on a successful send and on a re-follow
        // only, so the row for a message that had already landed in the
        // transcript outlived it — and the next refresh then slid the *next*
        // pending message into the row the reader was looking at, which reads as
        // one row overwriting another.
        //
        // The rpcId is the tie and it is exact, so our own row is dropped here
        // with no round trip; the re-read is for whatever else the inbox holds
        // (another client's message, an item retracted by that client). Neither
        // is done for a page of earlier history, which cannot carry a live
        // delivery and would ask the host once per replayed row.
        if (!m_earlierSink && !m_replayingSnapshot
            && sourceKind == QLatin1String("user")) {
            dropDeliveredQueueItem(rpcId);
            refreshProjections();
        }

        // A GOAL's own rounds arrive as user-role rows too — a continuation round
        // carries a different `source.kind` — and the goal is a projection the
        // follow stream never pushes. Measured 2026-10-28: a goal created in the
        // session did not appear in the strip until the reader sent a message,
        // because a human message is the only row that triggered a re-read. So a
        // goal round re-reads as well; the same guards keep it out of snapshot
        // replay and out of history paging.
        if (!m_earlierSink && !m_replayingSnapshot
            && sourceKind != QLatin1String("user")) {
            refreshProjections();
        }

        if (!sourceKind.isEmpty() && sourceKind != QLatin1String("user")) {
            const QString form = source.value(QStringLiteral("form")).toString();
            const QString hostSummary = source.value(QStringLiteral("summary")).toString();

            // A compaction checkpoint is the durable marker: the record the host
            // keeps to say the surface was rewritten. Its payload is an
            // instruction envelope written for the model and never rendered —
            // what the reader gets is the title, the summary behind the
            // disclosure, and the counts the `compaction/summary` record
            // supplied. It replaces the old dim "context compacted" notice, and
            // it is the only row that makes a compaction durable: an automatic
            // one has no announcement at all.
            if (sourceKind == QLatin1String("compact-checkpoint")) {
                const QString identity = compactionIdentity(
                    source.value(QStringLiteral("compactionId")).toString(),
                    source.value(QStringLiteral("sourceCommandId")).toString());
                const CompactionFacts facts = identity.isEmpty()
                    ? CompactionFacts{}
                    : m_compactionFacts.value(identity);
                const QString preview =
                    compactionPreview(facts.summary, facts.items, facts.tokens);
                const QString markerKey = identity.isEmpty()
                    ? QStringLiteral("compaction-marker:seq:%1")
                          .arg(qint64(event.value(QStringLiteral("seq")).toDouble(-1)))
                    : compactionRowKey(identity, QStringLiteral("marker"));

                // The checkpoint retires the live announcement itself, so no
                // instant has both rows for one compaction and a compaction that
                // ended without a checkpoint never leaves one behind.
                if (!identity.isEmpty()) {
                    updateCompactionRow(compactionRowKey(identity, QStringLiteral("running")),
                                        QStringLiteral("withdrawn"), QString(), QString());
                }

                // A replay can reach this record again — a continuation snapshot
                // overlapping the held rows, or a page holding the checkpoint —
                // and a page can deliver it before its summary. Either way the
                // marker is refreshed in place, never doubled.
                if (!identity.isEmpty() && m_compactionRows.contains(identity)) {
                    updateCompactionRow(markerKey, QStringLiteral("completed"),
                                        facts.summary, preview);
                } else {
                    appendRow(QStringLiteral("compacted"), preview,
                              QStringLiteral("Context compacted"),
                              QStringLiteral("completed"), facts.summary, markerKey,
                              qint64(event.value(QStringLiteral("time")).toDouble(0)),
                              eventTurn);
                    if (!identity.isEmpty())
                        m_compactionRows.insert(identity, markerKey);
                }
                return;
            }

            QString body;
            for (const QJsonValue &blockValue : data.value(QStringLiteral("content")).toArray()) {
                const QJsonObject block = blockValue.toObject();
                if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                    body += block.value(QStringLiteral("text")).toString();
            }
            if (body.isEmpty())
                body = hostSummary;

            // The caption is the producer kind; the body is what expands, and
            // the host's own summary — when there is one — is the preview beside
            // the caption. Naming the producer is the difference between "the
            // agent said this" and "the harness noted it".
            //
            // The body goes in `text`, which is the role the transcript delegate
            // reads. It used to be passed as `key` — a tool-call address the
            // delegate cannot read at all — and when the host sent no summary it
            // was dropped outright: expanding a reminder re-drew
            // `<system-reminder>` and the instruction behind it was unreachable.
            appendRow(QStringLiteral("notice"), body, sourceKind, form, hostSummary,
                      QString(), qint64(event.value(QStringLiteral("time")).toDouble(0)),
                      eventTurn);
            return;
        }

        for (const QJsonValue &blockValue :
             data.value(QStringLiteral("content")).toArray()) {
            const QJsonObject block = blockValue.toObject();
            const QString blockType = block.value(QStringLiteral("type")).toString();

            // Attachments used to be dropped here without a word, and then every
            // one of them was an "image" row — including a FILE. A file block has
            // no picture behind it: its bytes are not an image, and
            // `session/attachment` resolves image references only ("Image is not
            // referenced by this session"). The row still reserved 264 px and asked
            // for it anyway, and because a refused `session/attachment` is in the
            // quiet failure set the refusal was silent — so a presented or uploaded
            // file was a blank box with a caption.
            //
            // `type` is what decides the row kind. Only an image is an image.
            if (blockType == QLatin1String("image")) {
                const QJsonObject attachment =
                    block.value(QStringLiteral("attachment")).toObject();
                // `attachmentId` and `mediaType`, not `id` and `mimeType`. The
                // wrong names silently produced an empty id, which made every
                // attachment row a caption with no image behind it — the whole
                // inbound path was dead and looked like it worked.
                const QString id =
                    attachment.value(QStringLiteral("attachmentId")).toString();
                QString label = attachment.value(QStringLiteral("name")).toString();
                if (label.isEmpty())
                    label = attachment.value(QStringLiteral("mediaType")).toString();
                if (label.isEmpty())
                    label = id;
                // An "image" row, not a notice: a delegate renders it, and it
                // must not fold into a turn's process rows.
                appendRow(QStringLiteral("image"),
                          QStringLiteral("%1 · %2").arg(blockType, label), QString(),
                          QString(), id, QString(),
                          qint64(event.value(QStringLiteral("time")).toDouble(0)),
                          eventTurn);
                continue;
            }

            if (blockType == QLatin1String("file")) {
                // A file row names the file and stops there: name, size, media
                // type. There is no bytes round trip to make — the attachment
                // store only serves images — and nothing here claims a preview it
                // cannot draw.
                const QJsonObject attachment =
                    block.value(QStringLiteral("attachment")).toObject();
                const QString name = attachment.value(QStringLiteral("name")).toString();
                // `bytes` is absent on some refs; -1 is `fileMetaLine`'s "the host
                // did not say", never 0.
                const int bytes =
                    attachment.contains(QStringLiteral("bytes"))
                        ? attachment.value(QStringLiteral("bytes")).toInt()
                        : -1;
                // `status` carries `attached`: this file belongs to the user's
                // own message, so the delegate draws it as a chip against the
                // bubble rather than as a full-width row of the transcript.
                //
                // A presented file is the other `file` row and carries
                // `openable`, the marker the Files reader needs. The two markers
                // come from disjoint producers — only `deliverables/presented`
                // writes `openable`, and only this branch, a human
                // `user/message` content block, writes `attached` — so no
                // presented file can acquire this one and start following a
                // bubble, and no message attachment can pass for a deliverable.
                appendRow(QStringLiteral("file"),
                          name.isEmpty() ? QStringLiteral("unnamed file") : name,
                          fileMetaLine(bytes,
                                       attachment.value(QStringLiteral("mediaType")).toString()),
                          QStringLiteral("attached"), QString(), QString(),
                          qint64(event.value(QStringLiteral("time")).toDouble(0)),
                          eventTurn);
                continue;
            }

            // A reconciled row already carries the text; only the attachment
            // rows this message may also carry are still missing.
            if (blockType != QLatin1String("text") || reconciled)
                continue;
            const QString text = block.value(QStringLiteral("text")).toString();
            if (text.isEmpty())
                continue;
            applySessionUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("user_message_chunk")},
                {QStringLiteral("content"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), text},
                }},
            });
        }
        return;
    }

    if (type == QLatin1String("tool/call")) {
        // The arguments are the row's content, and its identity: the matching
        // `tool/result` addresses this row by `callId` rather than appending a
        // second, empty one beneath it.
        const QString callId = data.value(QStringLiteral("callId")).toString();
        const QString name = data.value(QStringLiteral("name")).toString();

        // The host sends `arguments` as an encoded JSON *string*, not as an
        // object, so asking for an object directly yields nothing and the row
        // renders as its own name with no summary — which is exactly the bug this
        // is fixing. An object is still accepted in case the encoding changes.
        const QJsonValue raw = data.value(QStringLiteral("arguments"));
        QJsonObject arguments;
        if (raw.isObject())
            arguments = raw.toObject();
        else if (raw.isString())
            arguments = QJsonDocument::fromJson(raw.toString().toUtf8()).object();

        // One row per edit, not two. The change a call makes travels on the
        // call's OWN row: `changeSummary` puts the file and the counts on the
        // collapsed line, and the diff is the payload that opens under it, drawn
        // by the tool card.
        //
        // It used to be a second `notice` row, which is why an edit rendered as a
        // bare "-": that row read its title from `arguments["path"]`, a key `edit`
        // does not send (it sends `file_path`), so the notice had no caption and
        // showed only its body's first line. The `notice` path is still there for
        // real notices; only this producer is gone.
        //
        // `request` is the decoded arguments, which the card shows as IN. It is
        // the same object the summary was derived from, kept whole: a summary is
        // one line and cannot be the request.
        const QString diff = diffFromToolArguments(name, arguments);
        if (!diff.isEmpty() && !callId.isEmpty()) {
            // The diff is a payload this row already holds, so its successful
            // result must not replace it with the host's own confirmation ("The
            // file … has been updated successfully." — which the row's counts
            // already say). Remembered by callId because the result record names
            // only its call, not its tool.
            m_diffPayloadCalls.insert(callId);
        }

        appendRow(QStringLiteral("tool"), toolSummary(name, arguments), name,
                  QStringLiteral("in_progress"), diff, callId,
                  qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn,
                  toolRequestText(arguments));
        return;
    }

    if (type == QLatin1String("tool/result")) {
        const QJsonObject message = data.value(QStringLiteral("message")).toObject();
        const QString callId = message.value(QStringLiteral("toolCallId")).toString();
        const QString output = toolResultText(message);
        const QString status = message.value(QStringLiteral("isError")).toBool()
            ? QStringLiteral("failed")
            : QStringLiteral("completed");

        // The images the result carried, if any. `read_image` returns the picture
        // itself as a block beside its text envelope — "the image itself rides the
        // adjacent image block" — and reading only the text is why a read_image
        // row named the file and showed nothing. A FILE block is not a picture and
        // is named rather than drawn, for the same reason as on a user message.
        for (const QJsonValue &blockValue :
             message.value(QStringLiteral("content")).toArray()) {
            const QJsonObject block = blockValue.toObject();
            const QString blockType = block.value(QStringLiteral("type")).toString();
            const QJsonObject attachment = block.value(QStringLiteral("attachment")).toObject();

            if (blockType == QLatin1String("file")) {
                const QString name = attachment.value(QStringLiteral("name")).toString();
                const int bytes =
                    attachment.contains(QStringLiteral("bytes"))
                        ? attachment.value(QStringLiteral("bytes")).toInt()
                        : -1;
                appendRow(QStringLiteral("file"),
                          name.isEmpty() ? QStringLiteral("unnamed file") : name,
                          fileMetaLine(bytes,
                                       attachment.value(QStringLiteral("mediaType")).toString()),
                          QString(), QString(), QString(),
                          qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
                continue;
            }

            if (blockType != QLatin1String("image"))
                continue;
            const QString attachmentId =
                attachment.value(QStringLiteral("attachmentId")).toString();
            if (attachmentId.isEmpty())
                continue;
            const QString media = attachment.value(QStringLiteral("mediaType")).toString();
            appendRow(QStringLiteral("image"),
                      QStringLiteral("%1 · %2 × %3")
                          .arg(media.isEmpty() ? QStringLiteral("image") : media)
                          .arg(attachment.value(QStringLiteral("width")).toInt())
                          .arg(attachment.value(QStringLiteral("height")).toInt()),
                      QString(), QString(), attachmentId, QString(),
                      qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
        }

        // Onto the call's own row when this window holds it: the live transcript
        // first, then — while a page is being translated — the page's own staged
        // rows, which the transcript cannot be asked about yet.
        //
        // A page used to skip this and always route the result through the ACP
        // tool-call translation, which carries only a status: no name, no
        // arguments and no output. So a page holding both a call and its result
        // drew the call and then an empty second row, and a page that opened on a
        // result — its call above the cut, or shadowed by a compaction — lost the
        // output entirely.
        //
        // A change row keeps the diff its call already put in `detail`. The
        // result's own text is dropped for a SUCCESSFUL edit or write — it is
        // the host's "The file … has been updated successfully.", which says
        // less than the counts on the row and would replace the payload with a
        // sentence. A FAILED one is the opposite: the edit changed nothing, so
        // its diff would describe a change that never happened and the error
        // replaces it. The empty `detail` below is "leave what is there", the
        // same meaning AcpTranscript::updateByKey gives it.
        const bool isError = message.value(QStringLiteral("isError")).toBool();
        const QString settledDetail =
            (!isError && m_diffPayloadCalls.contains(callId)) ? QString() : output;
        const bool attached = m_earlierSink
            ? attachPageToolResult(callId, status, settledDetail)
            : m_transcript->updateByKey(callId, status, settledDetail);

        if (!attached) {
            // The call is not in this window. The result still has to be
            // readable, so it becomes a row that says it stands alone and keeps
            // its output in `detail` — the card's OUT pane. The alternative, a
            // nameless tool row with no payload, was a call invented out of a
            // result. `key` is the callId, so a replayed copy of the same result
            // lands on this row instead of opening a second one.
            appendRow(QStringLiteral("tool"),
                      QStringLiteral("call not in this window"),
                      QStringLiteral("tool result"), status, output, callId,
                      qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
        }
        return;
    }

    // `present` — the tool that declares files as deliverables — does not send a
    // file content block at all. Its own journal record is this one: the paths it
    // declared, with the description the caller gave. Nothing rendered it, so a
    // presented file existed in the host's log and nowhere in this window; the
    // tool's text result ("Presented /path") was the only trace, and a text line
    // is not a file.
    //
    // One `file` row per file, the same row an attached file gets, plus the one
    // thing a presented file has and an upload does not: a PATH this client can
    // open. `status` carries `openable`, which is what tells this row apart from
    // a message attachment's `attached` — those two are the only states a file
    // row has — and the delegate offers the Files panel's own reader for it. That
    // reader is
    // `workspaceFiles/read`; it refuses binaries with `workspace-file/not-text`,
    // a refusal the panel and the status line already surface, so a click on a
    // PDF says why rather than doing nothing.
    if (type == QLatin1String("deliverables/presented")) {
        const QJsonArray files = data.value(QStringLiteral("files")).toArray();
        for (const QJsonValue &value : files) {
            const QJsonObject file = value.toObject();
            const QString path = file.value(QStringLiteral("path")).toString().trimmed();
            if (path.isEmpty())
                continue;
            appendRow(QStringLiteral("file"), path,
                      file.value(QStringLiteral("description")).toString(),
                      QStringLiteral("openable"), QString(), QString(),
                      qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
        }
        return;
    }

    // ---- compaction ---------------------------------------------------------
    //
    // A compaction is silent for the 15-20 seconds it runs: `start`, then the
    // summary, then the checkpoint that rewrites the surface. The web and Android
    // announce only a compaction someone asked for — an automatic one is an
    // implementation detail of the turn it runs inside — and the durable marker
    // is the checkpoint, drawn in the user/message branch above where the old
    // transcript wrote a dim "context compacted" notice. The trajectory ledger
    // folds the same three records into cells of its own; this is the
    // transcript's view of them.
    if (type == QLatin1String("compaction/start")) {
        // Only a commanded compaction announces itself: a `/compact` is someone
        // waiting, while an automatic compaction happens inside a turn the reader
        // is already watching.
        const QString commandId = data.value(QStringLiteral("sourceCommandId")).toString();
        const QString identity = compactionIdentity(
            data.value(QStringLiteral("compactionId")).toString(), commandId);
        if (commandId.isEmpty() || identity.isEmpty())
            return;

        const QString runKey = compactionRowKey(identity, QStringLiteral("running"));
        // A marker for this compaction already stands (a page can hand the
        // checkpoint back before its start): an announcement would be a second
        // row for one compaction, and nothing would ever retire it.
        if (m_compactionRows.contains(identity) || hasCompactionRow(runKey))
            return;
        appendRow(QStringLiteral("compacted"), QStringLiteral("Compacting context\u2026"),
                  QString(), QStringLiteral("running"), QString(), runKey,
                  qint64(event.value(QStringLiteral("time")).toDouble(0)), eventTurn);
        return;
    }

    if (type == QLatin1String("compaction/summary")) {
        const QString identity = compactionIdentity(
            data.value(QStringLiteral("compactionId")).toString(),
            data.value(QStringLiteral("sourceCommandId")).toString());
        const QString summary = blocksOfType(data.value(QStringLiteral("summary")).toArray(),
                                             QStringLiteral("text"));

        // The counts are only ever what the record carried. `shadowedSeqs` is a
        // list of seqs on the live host; a bare number is accepted too so the
        // reader does not depend on one encoding.
        int items = -1;
        const QJsonValue shadowed = data.value(QStringLiteral("shadowedSeqs"));
        if (shadowed.isArray())
            items = int(shadowed.toArray().size());
        else if (shadowed.isDouble())
            items = shadowed.toInt();
        const int tokens = data.contains(QStringLiteral("shadowedTokenCount"))
            ? data.value(QStringLiteral("shadowedTokenCount")).toInt()
            : -1;

        // A summary never opens a row of its own: the marker is the checkpoint's.
        // It stores the facts and, when a page has already delivered the
        // checkpoint, refreshes the marker that checkpoint drew — instead of
        // leaving a durable row that says nothing.
        if (!identity.isEmpty()) {
            m_compactionFacts.insert(identity, CompactionFacts{summary, items, tokens});
            if (m_compactionRows.contains(identity)) {
                updateCompactionRow(compactionRowKey(identity, QStringLiteral("marker")),
                                    QStringLiteral("completed"), summary,
                                    compactionPreview(summary, items, tokens));
            }
        }
        return;
    }

    if (type == QLatin1String("compaction/end")) {
        const QString identity = compactionIdentity(
            data.value(QStringLiteral("compactionId")).toString(),
            data.value(QStringLiteral("sourceCommandId")).toString());
        // Retired either way: an aborted attempt must not leave a permanent
        // "Compacting context…" behind. The checkpoint, not this record, is what
        // makes a compaction durable, so an automatic one still leaves no trace
        // until its checkpoint arrives.
        if (!identity.isEmpty())
            updateCompactionRow(compactionRowKey(identity, QStringLiteral("running")),
                                QStringLiteral("withdrawn"), QString(), QString());
        return;
    }

    // Turn boundaries are handled by the caller, which knows whether it holds a
    // live frame or a replayed history record.
}

void DshClient::applySessionUpdate(const QJsonObject &update, qint64 time, int turn)
{
    // The single translation point from an ACP-vocabulary update to transcript
    // rows. Kept deliberately identical to AcpClient::applySessionUpdate: any
    // divergence here shows up as the same session rendering differently
    // depending on which transport is driving the window.
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();

    if (kind == QLatin1String("agent_message_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        extendRow(QStringLiteral("agent"), content.value(QStringLiteral("text")).toString(), time, turn);
        return;
    }

    if (kind == QLatin1String("agent_thought_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        extendRow(QStringLiteral("thought"), content.value(QStringLiteral("text")).toString(), time, turn);
        return;
    }

    if (kind == QLatin1String("user_message_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        extendRow(QStringLiteral("user"), content.value(QStringLiteral("text")).toString(), time, turn);
        return;
    }

    if (kind == QLatin1String("usage_update")) {
        // An earlier page carries the context pressure of its own moment in the
        // log. Folding that in would overwrite the live figures with a stale
        // reading, and the page is only being read for its rows.
        if (m_earlierSink)
            return;

        m_contextUsed = update.value(QStringLiteral("used")).toInt();
        m_contextSize = update.value(QStringLiteral("size")).toInt();
        emit contextChanged();
        // Deliberately no setStatus here. It used to write "context N / M" into
        // the status line on every pressure update, which is where the header's
        // status sits — so the one line that reports what the client is doing
        // ("thinking…", "connected to …", an upload failing) was overwritten by a
        // number the header already shows, and last updated at that.
        return;
    }

    if (kind == QLatin1String("tool_call") || kind == QLatin1String("tool_call_update")) {
        const QString title = update.value(QStringLiteral("title")).toString().isEmpty()
            ? update.value(QStringLiteral("kind")).toString()
            : update.value(QStringLiteral("title")).toString();
        const QString status = update.value(QStringLiteral("status")).toString();
        appendRow(QStringLiteral("tool"), QString(), title, status, QString(), QString(),
                  0, turn);
        return;
    }

    if (kind == QLatin1String("plan")) {
        appendRow(QStringLiteral("notice"), QStringLiteral("plan updated"));
        return;
    }

    // Unknown kinds are surfaced, not swallowed: a silent drop here would look
    // exactly like an agent that stopped talking.
    qInfo() << "unhandled session update kind:" << kind;
}

void DshClient::appendRow(const QString &kind, const QString &text, const QString &title,
                          const QString &status, const QString &detail, const QString &key,
                          qint64 time, int turn, const QString &request)
{

    if (m_earlierSink) {
        // The key and the turn travel with the staged row: a page is translated
        // on its own, so its result can only find its call in this list, and its
        // chunks can only be kept inside one turn by the turn stored here.
        m_pageRows.append(EarlierRow{kind, text, title, status, detail, request,
                                     key, turn});
        return;
    }
    m_transcript->append(kind, text, title, status, detail, key, time, turn, request);
}

bool DshClient::attachPageToolResult(const QString &callId, const QString &status,
                                     const QString &detail)
{
    if (callId.isEmpty())
        return false;
    // Backwards because a result follows its call, and the newest row with that
    // key is the one still waiting for it.
    for (int i = static_cast<int>(m_pageRows.size()) - 1; i >= 0; --i) {
        EarlierRow &row = m_pageRows[i];
        if (row.key != callId)
            continue;
        row.status = status;
        // An empty detail is "leave it", as in AcpTranscript::updateByKey: a
        // page holding a change call and its successful result keeps the diff
        // the call put there instead of overwriting it with the confirmation.
        if (!detail.isEmpty())
            row.detail = detail;
        return true;
    }
    return false;
}

bool DshClient::updateCompactionRow(const QString &key, const QString &status,
                                    const QString &detail, const QString &text)
{
    if (key.isEmpty())
        return false;
    // The page being translated first: it is assembled before its rows reach the
    // transcript, so a marker or an announcement that belongs to it can only be
    // completed here. A live row is still reachable below, which is what lets a
    // page's `compaction/summary` refresh a marker the checkpoint drew earlier.
    if (m_earlierSink) {
        for (int i = static_cast<int>(m_pageRows.size()) - 1; i >= 0; --i) {
            EarlierRow &row = m_pageRows[i];
            if (row.key != key)
                continue;
            if (!status.isEmpty())
                row.status = status;
            if (!detail.isEmpty())
                row.detail = detail;
            if (!text.isEmpty())
                row.text = text;
            return true;
        }
    }
    return m_transcript->updateByKey(key, status, detail, text);
}

bool DshClient::hasCompactionRow(const QString &key)
{
    if (key.isEmpty())
        return false;
    if (m_earlierSink) {
        for (int i = static_cast<int>(m_pageRows.size()) - 1; i >= 0; --i) {
            if (m_pageRows.at(i).key == key)
                return true;
        }
    }
    // An empty update is the model's existence probe: it finds the row and
    // changes nothing.
    return m_transcript->updateByKey(key, QString());
}

void DshClient::logPromptOutcome(const QString &method, const QString &requestId,
                                 const QString &outcome, const QString &action,
                                 const QString &detail)
{
    if (requestId.isEmpty())
        return;
    const auto entry = m_localPrompts.constFind(requestId);
    const QString row = entry == m_localPrompts.cend()
                            ? QStringLiteral("gone")
                            : (entry.value() == LocalPromptStaged
                                   ? QStringLiteral("staged")
                                   : QStringLiteral("echoed"));
    const QString line =
        QStringLiteral("%1 outcome: %2 | requestId %3 | local row %4, %5%6")
            .arg(method, outcome, requestId, row, action,
                 detail.isEmpty() ? QString() : QStringLiteral(" | %1").arg(detail));
    if (outcome == QLatin1String("succeeded"))
        qInfo().noquote() << line;
    else
        qWarning().noquote() << line;
}

void DshClient::abandonLocalPrompt(const QString &requestId)
{
    if (requestId.isEmpty())
        return;
    const auto entry = m_localPrompts.constFind(requestId);
    if (entry == m_localPrompts.cend())
        return;

    const bool staged = entry.value() == LocalPromptStaged;
    m_localPrompts.remove(requestId);

    // The model has no delete, so the row keeps its place and the delegate is
    // told to draw nothing: a state row left saying "steering…" for a message the
    // host refused would be the same lie in the other direction. An echoed row is
    // the message itself, and it stays standing with the error row beneath it —
    // that is what a refused send has always looked like.
    if (staged)
        m_transcript->updateByKey(requestId, QStringLiteral("withdrawn"));
}

void DshClient::withholdLocalPrompt(const QString &requestId)
{
    if (requestId.isEmpty())
        return;
    // Only a staged row claims anything about delivery. An echoed row already
    // holds the message, so hiding it would lose a prompt the host may well have
    // accepted — and forgetting its id would let the durable copy double it.
    if (m_localPrompts.value(requestId, LocalPromptEchoed) != LocalPromptStaged)
        return;
    m_transcript->updateByKey(requestId, QStringLiteral("held"));
}

void DshClient::withholdStagedPrompts()
{
    QStringList staged;
    for (auto it = m_localPrompts.cbegin(); it != m_localPrompts.cend(); ++it) {
        if (it.value() == LocalPromptStaged)
            staged += it.key();
    }
    for (const QString &requestId : staged)
        withholdLocalPrompt(requestId);
}

bool DshClient::dropDeliveredQueueItem(const QString &requestId)
{
    if (requestId.isEmpty() || m_queue.isEmpty())
        return false;

    QVariantList kept;
    kept.reserve(m_queue.size());
    bool dropped = false;
    for (const QVariant &entry : m_queue) {
        // `rpcId` and nothing else: the row's `id` is the host's MessageId, which
        // this client never minted and cannot know before the inbox is read.
        if (entry.toMap().value(QStringLiteral("rpcId")).toString() == requestId) {
            dropped = true;
            continue;
        }
        kept.append(entry);
    }
    if (!dropped)
        return false;

    m_queue = kept;
    emit queueChanged();
    return true;
}

void DshClient::syncStagedPromptRows()
{
    if (m_earlierSink || m_replayingSnapshot || m_sessionId.isEmpty())
        return;

    struct StagedPrompt {
        QString key;
        QString status;
        QString preview;
        QString fullText;
        bool own = false;
    };

    QList<StagedPrompt> wanted;
    QSet<QString> wantedKeys;
    for (const QVariant &value : m_queue) {
        const QVariantMap item = value.toMap();
        StagedPrompt prompt;
        prompt.key = item.value(QStringLiteral("rpcId")).toString();
        prompt.own = !prompt.key.isEmpty();
        if (prompt.key.isEmpty()) {
            const QString id = item.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;
            prompt.key = QStringLiteral("queue:") + id;
        }
        prompt.fullText = item.value(QStringLiteral("text")).toString();
        prompt.status = item.value(QStringLiteral("steering")).toBool()
                            ? QStringLiteral("steering")
                            : QStringLiteral("queued");
        prompt.preview = stagedPreview(prompt.fullText);
        wanted.append(prompt);
        wantedKeys.insert(prompt.key);
    }

    const int rows = m_transcript->rowCount();
    QList<int> staged;
    QSet<QString> delivered;
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_transcript->index(row, 0);
        const QString key = m_transcript->data(index, AcpTranscript::KeyRole).toString();
        if (key.isEmpty())
            continue;
        const QString status =
            m_transcript->data(index, AcpTranscript::StatusRole).toString();
        const bool isStaged = status == QLatin1String("queued")
                           || status == QLatin1String("steering")
                           || status == QLatin1String("held")
                           || status == QLatin1String("withdrawn");
        if (!isStaged) {
            m_stagedPromptRows.remove(key);
            if (wantedKeys.contains(key))
                delivered.insert(key);
            continue;
        }
        if (m_stagedPromptRows.contains(key) || wantedKeys.contains(key))
            staged.append(row);
    }

    QList<StagedPrompt> pending;
    for (const StagedPrompt &prompt : wanted) {
        if (!delivered.contains(prompt.key))
            pending.append(prompt);
    }

    bool settled = staged.size() == pending.size();
    for (int i = 0; settled && i < pending.size(); ++i) {
        const int row = staged.at(i);
        if (row != rows - static_cast<int>(pending.size()) + i) {
            settled = false;
            break;
        }
        const QModelIndex index = m_transcript->index(row, 0);
        settled = m_transcript->data(index, AcpTranscript::KeyRole).toString()
                      == pending.at(i).key
               && m_transcript->data(index, AcpTranscript::StatusRole).toString()
                      == pending.at(i).status
               && m_transcript->data(index, AcpTranscript::DetailRole).toString()
                      == pending.at(i).preview
               && m_transcript->data(index, AcpTranscript::FullTextRole).toString()
                      == pending.at(i).fullText;
    }

    if (settled) {
        for (const StagedPrompt &prompt : pending) {
            m_stagedPromptRows.insert(prompt.key);
            if (prompt.own)
                m_localPrompts.insert(prompt.key, LocalPromptStaged);
        }
        return;
    }

    for (int i = static_cast<int>(staged.size()) - 1; i >= 0; --i) {
        const QModelIndex index = m_transcript->index(staged.at(i), 0);
        const QString key = m_transcript->data(index, AcpTranscript::KeyRole).toString();
        if (m_localPrompts.value(key, LocalPromptEchoed) == LocalPromptStaged)
            m_localPrompts.remove(key);
        m_stagedPromptRows.remove(key);
        m_transcript->removeRow(staged.at(i));
    }

    for (const StagedPrompt &prompt : pending) {
        m_transcript->append(QStringLiteral("user"), QString(), QString(), prompt.status,
                             prompt.preview, prompt.key, 0, -1, QString(),
                             prompt.fullText);
        m_stagedPromptRows.insert(prompt.key);
        if (prompt.own)
            m_localPrompts.insert(prompt.key, LocalPromptStaged);
    }
}

void DshClient::reconcileQueueMutation()
{
    const QueueMutation mutation = m_queueMutation;
    m_queueMutation = QueueMutation{};

    if (mutation.itemId.isEmpty())
        return;

    // Which prompt this dock row stands for. The dock's own list is the only place
    // the host's item id and the client's rpcId are held together, and it still
    // holds the pre-mutation snapshot — the refresh runs after this.
    QString requestId;
    for (const QVariant &entry : m_queue) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("id")).toString() == mutation.itemId) {
            requestId = row.value(QStringLiteral("rpcId")).toString();
            break;
        }
    }
    if (requestId.isEmpty())
        return;   // not this client's message: nothing in the transcript to settle

    if (mutation.kind == QLatin1String("remove")) {
        // The host will never deliver it, so the row must stop waiting. Its id is
        // forgotten too, so a delivery that arrives anyway renders as the message
        // rather than being swallowed as a duplicate.
        abandonLocalPrompt(requestId);
    } else if (mutation.kind == QLatin1String("steer")) {
        // Accepted into the running turn: it is no longer waiting for the next one.
        m_transcript->updateByKey(requestId, QStringLiteral("steering"));
    } else if (mutation.kind == QLatin1String("edit")) {
        // The staged row's body stays empty — it is still not a message — and the
        // preview is the only text it shows.
        m_transcript->updateByKey(requestId, QString(), stagedPreview(mutation.text));
    }
}

/// One line describing what a tool call is doing, taken from its own arguments.
/// One line identifying a tool call, taken from its own arguments.
///
/// The host sends the tool's identifier and an opaque `arguments` object, and the
/// transcript used to render only the identifier: every Bash row read "Bash" with
/// nothing after it. The web UI shows the command, the path, the query — the thing
/// that makes a row identifiable at a glance — so the same fields are lifted here.
///
/// A shell call is identified by the command it RUNS. The order used to be
/// `description` first, which is why every bash row read as the note written about
/// the call — the same words the card's IN half also showed, so the row repeated
/// itself — and why a command with no note fell through to whatever else the
/// generic key list could find. The description is still here, but as the shell
/// summary's fallback rather than its first choice.
QString DshClient::toolSummary(const QString &name, const QJsonObject &arguments)
{
    if (arguments.isEmpty())
        return {};

    // An edit or a write is a CHANGE, not a request with a subject: its row has
    // to name the file and say what happened to it. Handled here rather than at
    // the call site so that a page, which reaches the transcript through the same
    // `tool/call` branch, gets the identical line.
    if (name == QLatin1String("edit") || name == QLatin1String("write"))
        return changeSummary(name, arguments);

    // A shell row: the command line, then the description if there is no command.
    // The card's IN half is the whole request; this line is the identification.
    static const QStringList shellKeys{
        QStringLiteral("command"), QStringLiteral("description"),
    };
    // Every other family keeps its own precedence: the path first (that is what
    // identifies a read), then the search keys, then the rest. `description` stays
    // first here because for a prose-carrying tool it is the summary, not a note
    // repeated from IN. The path is not in this list: `toolFileArgument` is the one
    // place that knows which key each tool sends, and a second list here is how the
    // edit diff came to read a key the edit tool does not send.
    static const QStringList otherKeys{
        QStringLiteral("description"),
        QStringLiteral("command"),     QStringLiteral("pattern"),   QStringLiteral("query"),
        QStringLiteral("url"),         QStringLiteral("prompt"),    QStringLiteral("name"),
    };

    QString value;
    if (isShellCall(name, arguments)) {
        for (const QString &key : shellKeys) {
            const QString candidate = arguments.value(key).toString().trimmed();
            if (!candidate.isEmpty()) {
                value = candidate;
                break;
            }
        }
    } else {
        const QString description = arguments.value(QStringLiteral("description")).toString().trimmed();
        if (!description.isEmpty())
            value = description;
        else
            value = toolFileArgument(arguments);
        if (value.isEmpty()) {
            for (const QString &key : otherKeys) {
                const QString candidate = arguments.value(key).toString().trimmed();
                if (!candidate.isEmpty()) {
                    value = candidate;
                    break;
                }
            }
        }
    }

    // A call whose request is an ARRAY rather than a string field. `present`
    // sends `files: [{path, description}]` and `web_search` sends `queries: [ … ]`,
    // and neither has a top-level string to lift — so these rows used to print the
    // object as compact JSON, braces and all, into a 26 px line.
    if (value.isEmpty()) {
        static const QStringList arrayKeys{
            QStringLiteral("files"), QStringLiteral("queries"),
        };
        for (const QString &key : arrayKeys) {
            const QJsonArray array = arguments.value(key).toArray();
            if (array.isEmpty())
                continue;
            const QJsonValue first = array.first();
            QString candidate;
            if (first.isString()) {
                candidate = first.toString().trimmed();
            } else {
                const QJsonObject object = first.toObject();
                candidate = toolFileArgument(object);
                if (candidate.isEmpty()) {
                    for (const QString &field : {QStringLiteral("name"), QStringLiteral("query"),
                                                 QStringLiteral("question"),
                                                 QStringLiteral("content")}) {
                        candidate = object.value(field).toString().trimmed();
                        if (!candidate.isEmpty())
                            break;
                    }
                }
            }
            if (candidate.isEmpty())
                continue;
            // The count is said, not implied: one path with no sign of the others
            // would read as the whole request. "(+2)" is the host's own array size.
            value = array.size() > 1 ? QStringLiteral("%1 (+%2)").arg(candidate).arg(array.size() - 1)
                                     : candidate;
            break;
        }
    }

    // Nothing recognisable in the arguments still has to say something rather than
    // render as a bare word. The whole request is honest, if ugly; IN is where it
    // is meant to be read.
    if (value.isEmpty())
        return QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));

    // One line only: the summary sits on a single 24 px row and is elided there,
    // so the first line is the honest thing to hand it — a newline in the middle
    // would otherwise make the row's height unpredictable.
    const int newline = value.indexOf(QLatin1Char('\n'));
    if (newline >= 0)
        value = value.left(newline);
    static constexpr int SummaryCap = 200;
    if (value.size() > SummaryCap)
        value = value.left(SummaryCap - 1) + QChar(0x2026);
    return value;
}

/// The printable text of a tool result, for the OUT pane.
/// Unwrap the small XML envelope some tools put around their result.
///
/// `<path>…</path><type>…</type><content>…</content>` is addressed to the model,
/// so the tags are noise in a window: a read row rendered as
/// "read_image · <path>/tmp/x.png</path>". Only text that actually starts with the
/// envelope is touched, so ordinary output containing the word "path" is safe.
static QString unwrapToolEnvelope(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(QLatin1String("<path>")))
        return text;

    static const QRegularExpression pathRe(QStringLiteral("<path>(.*?)</path>"),
                                           QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression contentRe(QStringLiteral("<content>\\s*(.*?)\\s*</content>"),
                                              QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch pathMatch = pathRe.match(trimmed);
    const QRegularExpressionMatch contentMatch = contentRe.match(trimmed);
    if (!pathMatch.hasMatch())
        return text;
    if (!contentMatch.hasMatch())
        return pathMatch.captured(1);
    return pathMatch.captured(1) + QLatin1Char('\n') + contentMatch.captured(1);
}

void DshClient::restartSelf()
{
    // The same argv, unchanged: whatever transport, QML path or flags this
    // process was started with are the ones the replacement should have. Only
    // the image is new.
    //
    // Everything here is checked and logged, because the first version was a
    // button that did nothing and said nothing: `startDetached` fails quietly
    // when it cannot exec, `quit()` is a request rather than a guarantee, and
    // between the two there was no way to tell which had happened.
    const QString exe = QCoreApplication::applicationFilePath();
    const QStringList args = QCoreApplication::arguments().mid(1);
    QString wd = QDir::currentPath();

    qInfo() << "restarting:" << exe << args << "from" << wd;

    // Keep a working directory that exists; a detached child inherits ours, and
    // a process started from a deleted directory cannot be trusted with anything
    // that resolves relative paths.
    if (wd.isEmpty() || !QFileInfo(wd).isDir())
        wd = QDir::homePath();

    if (!QProcess::startDetached(exe, args, wd)) {
        const QString why = QStringLiteral("could not restart: %1").arg(exe);
        qWarning() << why;
        setStatus(why);
        return;
    }

    setStatus(QStringLiteral("restarting…"));

    // `quit()` posts a deferred quit to the event loop. If anything below holds
    // the loop open it never runs, so give it a deadline and then leave hard —
    // the replacement is already running, and two clients on one window is worse
    // than an abrupt exit.
    QTimer::singleShot(1500, [] { std::exit(0); });
    QCoreApplication::quit();
}

void DshClient::refreshProjections()
{
    if (m_state != Ready || m_sessionId.isEmpty())
        return;
    call(QStringLiteral("session/projections"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), m_sessionId},
         }}});
}

void DshClient::reportRpcFailure(const QString &method, const QString &code,
                                const QString &message, const QString &commandLine)
{
    emit rpcFailed(method, code, message);

    // The task and plugin pages have their own notice line, and a failure there
    // is not a transcript event: the conversation did not change because a
    // catalog read timed out. Handled before the status-line/transcript path so
    // neither gets an error row for a page the reader may not even have open.
    if (method == QLatin1String("schedule/catalog")) {
        m_scheduleLoading = false;
        setScheduleNotice(QStringLiteral("Could not load tasks: %1").arg(message), true);
        return;
    }
    if (method == QLatin1String("schedule/update")) {
        m_scheduleSaving = false;
        setScheduleNotice(QStringLiteral("Could not save the task: %1").arg(message), true);
        emit scheduleChanged();
        return;
    }
    if (method == QLatin1String("schedule/delete")) {
        m_scheduleSaving = false;
        setScheduleNotice(QStringLiteral("Could not delete the task: %1").arg(message), true);
        emit scheduleChanged();
        return;
    }
    if (method == QLatin1String("schedule/history")) {
        m_scheduleHistoryLoading = false;
        setScheduleNotice(QStringLiteral("Could not load delivery records: %1").arg(message), true);
        emit scheduleHistoryChanged();
        return;
    }
    if (method == QLatin1String("pluginManager/listBundles")) {
        m_pluginsLoading = false;
        setPluginsNotice(QStringLiteral("Could not load the plugins: %1").arg(message), true);
        return;
    }
    if (method == QLatin1String("pluginManager/setBundleEnabled")) {
        const QString name = m_pluginChanging;
        m_pluginChanging.clear();
        rebuildPluginSections();
        setPluginsNotice(name.isEmpty()
                             ? QStringLiteral("Could not change the plugin: %1").arg(message)
                             : QStringLiteral("%1 was not changed: %2").arg(name, message),
                         true);
        emit pluginsChanged();
        return;
    }

    // A control's own surface first. These belong to chips on the status strip,
    // so they must be said even when the method is one the status line keeps
    // quiet — a click the host declined must not look like one that worked.
    if (method == QLatin1String("commands/execute")) {
        const QString trimmed = commandLine.trimmed();
        if (trimmed.section(QLatin1Char(' '), 0, 0) == QLatin1String("/permission")) {
            const QString word = trimmed.section(QLatin1Char(' '), 0, 0);
            const QString preset = trimmed.mid(word.length()).trimmed();
            m_pendingPermissionPreset.clear();
            // A timeout or a lost answer is not a refusal: the host may have run
            // the command and lost the reply. Say that rather than claim it was
            // not applied.
            const bool uncertain = code == QLatin1String("timeout")
                                   || code == QLatin1String("network");
            if (preset.isEmpty())
                setAccessNotice(message);
            else if (uncertain)
                setAccessNotice(QStringLiteral("%1: %2 — it may or may not have been applied")
                                    .arg(preset, message));
            else
                setAccessNotice(QStringLiteral("%1 was not applied: %2").arg(preset, message));
        }
    } else if (method == QLatin1String("session/selectModel")) {
        const bool uncertain = code == QLatin1String("timeout")
                               || code == QLatin1String("network");
        setModelNotice(uncertain
                           ? QStringLiteral("the model switch got no answer: %1").arg(message)
                           : QStringLiteral("the model did not change: %1").arg(message));
    } else if (method == QLatin1String("session/projections")
               && !m_pendingPermissionPreset.isEmpty()) {
        // The command succeeded but the strip cannot re-read what is now in
        // force. Say so rather than let the old preset look current: the chip is
        // host-driven and this client will not paint the accepted value itself.
        const QString pending = m_pendingPermissionPreset;
        m_pendingPermissionPreset.clear();
        setAccessNotice(
            QStringLiteral("the host accepted %1, but the access level could not be "
                           "re-read; this may still show the previous one").arg(pending));
    }

    // Polling is quiet; anything a person pressed is not.
    //
    // A refused action used to be a log line and nothing else, so a click that
    // the host declined looked exactly like a click that worked: steer on a turn
    // that had already ended is refused with `session/steer-unavailable`, the row
    // stayed put, and nothing said why.
    static const QSet<QString> quiet{
        QStringLiteral("session/list"),      QStringLiteral("session/page"),
        QStringLiteral("session/projections"), QStringLiteral("session/attachment"),
        QStringLiteral("job/list"),          QStringLiteral("settings/describe"),
        QStringLiteral("permissionPresets/catalog"),
        QStringLiteral("fileReferences/list"), QStringLiteral("workspaceFiles/changes"),
    };
    if (quiet.contains(method))
        return;

    const QString line = QStringLiteral("%1 failed: %2").arg(method, message);
    setStatus(line);
    m_transcript->append(QStringLiteral("error"), line);

    // The rail's own notice surface, for the three controls that create
    // something. The status line and the transcript error row belong to the
    // window; a person who pressed a + inside the rail is looking there, and a
    // failed create that only changed a strip at the foot of the window is the
    // silence this ends. Copy names what they tried, then the host's own words.
    if (method == QLatin1String("workspace/create"))
        emit railNotice(QStringLiteral("Could not add the workspace: %1").arg(message), true);
    else if (method == QLatin1String("directoryPicker/pick"))
        emit railNotice(QStringLiteral("Could not open the folder chooser: %1").arg(message), true);
    else if (method == QLatin1String("session/create")) {
        emit railNotice(QStringLiteral("Could not create the session: %1").arg(message), true);
        // The call was a seat's materialisation: the turn it was holding has not
        // been sent anywhere, and the seat stays so a retry needs no new press.
        // The text goes back to the composer rather than disappearing with no row
        // to show it was ever typed.
        if (m_seatCreateInFlight) {
            const HeldPrompt held = m_heldPrompt;
            m_heldPrompt = HeldPrompt{};
            m_seatCreateInFlight = false;
            m_seatFollow = false;
            if (held.active)
                emit pendingPromptDropped(held.text, message);
        }
    }
}

QString DshClient::toolResultText(const QJsonObject &message)
{
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    QStringList parts;
    for (const QJsonValue &block : content) {
        const QJsonObject b = block.toObject();
        const QString type = b.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text") || type == QLatin1String("content"))
            parts.append(unwrapToolEnvelope(b.value(QStringLiteral("text")).toString()));
        else if (!b.isEmpty())
            parts.append(QString::fromUtf8(QJsonDocument(b).toJson(QJsonDocument::Compact)));
    }
    if (parts.isEmpty() && !content.isEmpty())
        parts.append(QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact)));
    return parts.join(QLatin1Char('\n'));
}

void DshClient::extendRow(const QString &kind, const QString &text, qint64 time, int turn)
{
    if (m_earlierSink) {
        // The mirror of appendOrExtend, on a page instead of the transcript: a
        // page is translated oldest-first, so consecutive chunks of one kind are
        // one message and must merge into the row the first of them opened.
        //
        // The turn is part of the test, not just the kind. A page is cut at a
        // message boundary, and a message boundary can be a turn boundary: the
        // page would otherwise be able to join turn N's last text to turn N+1's
        // first, which is a row spanning two turns. A page with no turn numbers
        // at all is not a DSH page; separate rows are the honest fallback.
        const bool merge = !m_pageRows.isEmpty()
                        && m_pageRows.last().kind == kind
                        && turn >= 0 && m_pageRows.last().turn == turn;
        if (merge)
            m_pageRows.last().text += text;
        else
            m_pageRows.append(EarlierRow{kind, text, {}, {}, {}, {}, {}, turn});
        return;
    }

    // Two properties of the model decide whether the journal's text may join the
    // row above, and both are read from that row rather than remembered in a
    // flag. (1) A row must never span two turns: `appendOrExtend` merges by kind
    // alone, so turn N's tail and turn N+1's start would otherwise become one
    // message. (2) The journal's text must never be merged into a provisional
    // row — one that is still streaming will be replaced by the settlement, and
    // one already withdrawn draws nothing, so the text would vanish into it.
    // Appending instead puts the journal's copy on its own row; the provisional
    // rows are hidden, so no text is shown twice.
    const bool sameTurn = turn >= 0 && tailTurn() == turn;
    if (sameTurn && !tailIsProvisional())
        m_transcript->appendOrExtend(kind, text, time, turn);
    else
        m_transcript->append(kind, text, {}, {}, {}, {}, time, turn);
}

int DshClient::tailTurn() const
{
    const int rows = m_transcript->rowCount();
    if (rows <= 0)
        return -1;
    return m_transcript->data(m_transcript->index(rows - 1, 0),
                              AcpTranscript::TurnRole).toInt();
}

bool DshClient::tailIsProvisional() const
{
    const int rows = m_transcript->rowCount();
    if (rows <= 0)
        return false;

    const QModelIndex tail = m_transcript->index(rows - 1, 0);
    // Only the two rows drawProvisionalRows appends can be provisional. No
    // journal event produces an agent or thought row with a status, so this pair
    // cannot be true of a settled row: `reconcileProvisionalMessage` rewrites a
    // claimed row to "completed", which means it now belongs to the journal and
    // later chunks of the same turn may extend it again.
    const QString kind = m_transcript->data(tail, AcpTranscript::KindRole).toString();
    if (kind != QLatin1String("agent") && kind != QLatin1String("thought"))
        return false;
    const QString status = m_transcript->data(tail, AcpTranscript::StatusRole).toString();
    return status == QLatin1String("streaming") || status == QLatin1String("withdrawn");
}

void DshClient::applyProjections(const QJsonObject &projections)
{
    if (projections.isEmpty())
        return;

    // Both a snapshot's `projections` and an RPC's SessionProjectionsValue carry
    // the same {asOfSeq, values} shape, so one reader serves both.
    const QJsonObject values = projections.value(QStringLiteral("values")).toObject();
    const QJsonObject pressure = values.value(QStringLiteral("contextPressure")).toObject();

    int used = m_contextUsed;
    int size = m_contextSize;
    if (pressure.value(QStringLiteral("projectedTokens")).isDouble())
        used = pressure.value(QStringLiteral("projectedTokens")).toInt();
    else if (pressure.value(QStringLiteral("pressureTokens")).isDouble())
        used = pressure.value(QStringLiteral("pressureTokens")).toInt();
    if (pressure.value(QStringLiteral("contextWindow")).isDouble())
        size = pressure.value(QStringLiteral("contextWindow")).toInt();

    if (used != m_contextUsed || size != m_contextSize) {
        m_contextUsed = used;
        m_contextSize = size;
        emit contextChanged();
    }

    // The todo list and the goal ride this same snapshot and were being thrown
    // away. Read here, above the model-route block, because that block returns
    // early for a session nobody has re-routed — and a fresh session is exactly
    // the one whose plan you most want to see.
    QVariantList todos;
    const QJsonValue todoValue = values.value(QStringLiteral("todos"));
    if (todoValue.isArray()) {
        for (const QJsonValue &entry : todoValue.toArray()) {
            const QJsonObject item = entry.toObject();
            todos.append(QVariantMap{
                {QStringLiteral("content"), item.value(QStringLiteral("content")).toString()},
                {QStringLiteral("status"), item.value(QStringLiteral("status")).toString()},
            });
        }
    }
    if (todos != m_todos) {
        m_todos = todos;
        emit todosChanged();
    }

    // The projection wraps a snapshot: {goal:{…}, roundsStarted, …}. A session
    // with no goal has no snapshot, which is the empty map.
    QVariantMap goal;
    const QJsonObject goalProjection = values.value(QStringLiteral("goal")).toObject();
    const QJsonObject goalSnapshot = goalProjection.value(QStringLiteral("goal")).toObject();
    if (!goalSnapshot.isEmpty()) {
        goal.insert(QStringLiteral("objective"),
                    goalSnapshot.value(QStringLiteral("objective")).toString());
        goal.insert(QStringLiteral("phase"), goalSnapshot.value(QStringLiteral("phase")).toString());
        // The identity and revision are what a lifecycle change is addressed to:
        // every durable goal mutation is compare-and-set on this pair.
        goal.insert(QStringLiteral("id"), goalSnapshot.value(QStringLiteral("id")).toString());
        goal.insert(QStringLiteral("revision"),
                    goalSnapshot.value(QStringLiteral("revision")).toInt());
        goal.insert(QStringLiteral("roundsStarted"),
                    goalProjection.value(QStringLiteral("roundsStarted")).toInt());
        goal.insert(QStringLiteral("maxRounds"),
                    goalSnapshot.value(QStringLiteral("maxGoalRounds")).toInt());
    }
    if (goal != m_goal) {
        m_goal = goal;
        emit goalChanged();
    }

    // Input the agent has accepted but not yet consumed. Two buckets: `next-turn`
    // is the ordinary queue, `next-step` is a steer waiting for the boundary.
    // These used to be invisible — a queued message simply disappeared until the
    // next turn started.
    QVariantList queue;
    const QJsonObject inbox = values.value(QStringLiteral("inbox")).toObject();
    const QStringList buckets{QStringLiteral("next-turn"), QStringLiteral("next-step")};
    for (const QString &bucket : buckets) {
        const QJsonArray items = inbox.value(bucket).toArray();
        for (const QJsonValue &entry : items) {
            const QJsonObject message = entry.toObject();
            QString text;
            for (const QJsonValue &block : message.value(QStringLiteral("content")).toArray()) {
                const QJsonObject b = block.toObject();
                if (b.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                    text = b.value(QStringLiteral("text")).toString();
                    break;
                }
            }
            queue.append(QVariantMap{
                {QStringLiteral("id"), message.value(QStringLiteral("id")).toString()},
                // The client-minted prompt identity the host preserved on the
                // message, when there is one. This is the *only* thing that ties a
                // dock row to the transcript row this client put in for the same
                // prompt: `id` is the host's own MessageId, minted fresh by
                // `createUserMessage`, and shares no namespace with the rpcId the
                // client minted (`source.rpcId`, from the host's prompt admission
                // path). Without it a delivery cannot say which dock row it has
                // emptied, and a retracted item cannot say which staged row to
                // take back.
                {QStringLiteral("rpcId"),
                 message.value(QStringLiteral("source")).toObject()
                        .value(QStringLiteral("rpcId")).toString()},
                {QStringLiteral("text"), text},
                {QStringLiteral("steering"), bucket == QLatin1String("next-step")},
            });
        }
    }
    if (queue != m_queue) {
        m_queue = queue;
        emit queueChanged();
    }

    // The permission posture in force. Read-only here: it changes through the
    // `/permission` command, and this is what tells the menu which entry to mark.
    // A switch the host accepted is confirmed against this projection rather than
    // painted from the click, so the chip can never show a preset the session is
    // not actually in.
    const QJsonObject permissions = values.value(QStringLiteral("permissions")).toObject();
    const QString preset = permissions.value(QStringLiteral("currentValue")).toString();
    if (preset != m_permissionPreset) {
        m_permissionPreset = preset;
        emit presetsChanged();
    }
    if (!m_pendingPermissionPreset.isEmpty()) {
        const QString pending = m_pendingPermissionPreset;
        if (preset == pending) {
            m_pendingPermissionPreset.clear();
            setAccessNotice(QString());
        } else {
            m_pendingPermissionPreset.clear();
            setAccessNotice(
                preset.isEmpty()
                    ? QStringLiteral("the host accepted %1, but the session reports no "
                                     "access level").arg(pending)
                    : QStringLiteral("the host accepted %1, but this session is in %2")
                          .arg(pending, preset));
        }
    }

    // The durable model route. `next` is what the following request will use;
    // `lastUsed` is the fallback for a session nobody has re-routed.
    const QJsonObject selection = values.value(QStringLiteral("modelSelection")).toObject();
    QJsonObject route = selection.value(QStringLiteral("next")).toObject();
    if (route.isEmpty())
        route = selection.value(QStringLiteral("lastUsed")).toObject();
    if (route.isEmpty())
        return;

    const QString provider = route.value(QStringLiteral("provider")).toString();
    const QString model = route.value(QStringLiteral("model")).toString();
    const QString effort = route.value(QStringLiteral("reasoningEffort")).toString();
    if (provider == m_provider && model == m_model && effort == m_reasoningEffort)
        return;

    m_provider = provider;
    m_model = model;
    m_reasoningEffort = effort;

    // The route changed, so the picker's currentValue and the effort choices are
    // both stale. The catalog itself is unchanged, so no round trip is needed.
    // The host has now spoken about this session's route, so any notice about a
    // switch that did not take is superseded.
    setModelNotice(QString());
    applyModelCatalog(m_catalog);
}

// --- history paging ---------------------------------------------------------
//
// The follow snapshot opens on the most recent window only and says so with
// `hasMore`. Earlier history comes from `session/page`, which takes the *same*
// tagged address union and returns the same record shape:
//
//   args   {request:{address, throughSeq, maxMessages?}}
//   result {records:[{type:"event", event:{…}}], hasMore}
//
// `throughSeq` is an inclusive cut, and the host cuts a page at an append-surface
// message (or, for a merged message, at its earliest source seq), so a page never
// starts mid-message:
//
//   paginate: end = throughSeq + 1; walk back to the maxMessages'th message;
//             the slice is events[cut .. end); hasMore = cut > 0
//
// Two consequences the cursor below depends on, both verified against the live
// host: asking with the oldest record's own seq hands that record back again, and
// `maxMessages` defaults to 50 messages (not records). A value past the head is
// refused with `gateway/bad-request` "session page through seq N is past cursor
// M", and -1 yields an empty page — which is why the cursor is only trusted once
// a real window has supplied one.

bool DshClient::canLoadEarlier() const
{
    return m_hasEarlier && m_pageCursor >= 0;
}

void DshClient::resetEarlierHistory()
{
    m_pageCursor = -1;
    m_hasEarlier = false;
    m_loadingEarlier = false;
    m_pageSessionId.clear();
    m_earlierSink = false;
    m_pageRows.clear();
    m_earlierRows.clear();
    m_pageTrajectory = TrajectoryLedger{};
    refreshHasEarlier();
}

void DshClient::refreshHasEarlier()
{
    const bool available = canLoadEarlier();
    if (available == m_hasEarlierAdvertised)
        return;
    m_hasEarlierAdvertised = available;
    emit hasEarlierChanged();
}

void DshClient::loadEarlierHistory()
{
    if (m_sessionId.isEmpty() || m_followStreamId.isEmpty()) {
        setStatus(QStringLiteral("no session to page"));
        return;
    }

    // A second request would fetch the same page twice and stage every row of it
    // twice over.
    if (m_loadingEarlier)
        return;

    if (!canLoadEarlier()) {
        setStatus(QStringLiteral("no earlier history left"));
        return;
    }

    m_loadingEarlier = true;
    m_pageSessionId = m_sessionId;
    refreshHasEarlier();

    // `maxMessages` is left out on purpose: the host's own default is the window
    // the web UI pages with, and a client-side number would silently diverge from
    // it. One page per call keeps each response bounded and the UI responsive.
    setStatus(QStringLiteral("loading earlier history…"));
    call(QStringLiteral("session/page"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("address"), m_followAddress},
             {QStringLiteral("throughSeq"), double(m_pageCursor)},
         }}});
}

void DshClient::applyEarlierPage(const QJsonObject &value)
{
    m_loadingEarlier = false;
    refreshHasEarlier();

    // A page belongs to the session it was asked for. A follow in the meantime
    // makes the answer stale, and staging it would attribute one log's records to
    // the session now on screen.
    if (m_pageSessionId != m_sessionId)
        return;

    const QJsonArray records = value.value(QStringLiteral("records")).toArray();

    // Translate through the same translator the live stream uses, so a record
    // renders identically however it arrived; only the destination differs.
    m_pageRows.clear();
    m_pageTrajectory = TrajectoryLedger{};
    m_earlierSink = true;
    for (const QJsonValue &record : records) {
        const QJsonObject event = record.toObject().value(QStringLiteral("event")).toObject();
        if (!event.isEmpty())
            applySessionEvent(event);
    }
    m_earlierSink = false;

    // The ledger's page goes in front of it, then publishes once: the same
    // records, in the same order, on the other view of the same journal.
    mergeTrajectoryPage();
    rebuildTrajectory();

    const int added = int(m_pageRows.size());

    // A page is older than everything already staged, so it goes on top of it:
    // the staging list stays oldest-first, which is the order a prepend would
    // insert it in.
    m_earlierRows = m_pageRows + m_earlierRows;

    // Put them where they belong, above what is already displayed.
    //
    // The insertion is reversed on purpose. prepend() inserts at row 0, so
    // inserting the page oldest-first would leave it reversed; walking it
    // backwards lands the page's first row at the top of the page and the whole
    // page above the existing content, which is exactly the reading order. And
    // because the page is older than anything already shown, it belongs above it
    // without disturbing the rows the reader is looking at.
    for (int i = int(m_pageRows.size()) - 1; i >= 0; --i) {
        const EarlierRow &pageRow = m_pageRows.at(i);
        m_transcript->prepend(pageRow.kind, pageRow.text, pageRow.title,
                              pageRow.status, pageRow.detail, pageRow.request);
    }
    m_pageRows.clear();

    // The cursor moves to one below the oldest record the page returned, because
    // the cut is inclusive: asking again with the page's own first seq would
    // repeat that record.
    if (!records.isEmpty())
        m_pageCursor = recordSeq(records.first().toObject()) - 1;
    m_hasEarlier = value.value(QStringLiteral("hasMore")).toBool();
    refreshHasEarlier();

    qInfo() << "earlier history page:" << records.size() << "record(s) ->" << added
            << "row(s) prepended; transcript rows" << m_transcript->rowCount()
            << "trajectory cells" << m_trajectory.cells.size()
            << "canLoadEarlier" << canLoadEarlier();

    setStatus(QStringLiteral("%1 earlier row(s) loaded").arg(added));

    emit earlierHistoryLoaded(added, m_hasEarlier);
}

QVariantList DshClient::earlierHistoryRows() const
{
    QVariantList rows;
    rows.reserve(m_earlierRows.size());
    for (const EarlierRow &row : m_earlierRows) {
        rows.append(QVariantMap{
            {QStringLiteral("kind"), row.kind},
            {QStringLiteral("text"), row.text},
            {QStringLiteral("title"), row.title},
            {QStringLiteral("status"), row.status},
            {QStringLiteral("detail"), row.detail},
            {QStringLiteral("request"), row.request},
        });
    }
    return rows;
}

// --- trajectory -------------------------------------------------------------
//
// The session as the *model's* input/output ledger rather than as chat bubbles.
//
// There is no host endpoint behind this: `session/trajectory` and its friends
// answer HTTP 404 `not found` (verified), so the ledger is derived here from the
// same durable journal records the transcript already consumes. The vocabulary
// is the web's `packages/client/ui-trajectory` cell kinds, as dsh-android's
// `model/Trajectory.kt` documents them, folded from OUR journal:
//
//   web kind     our journal record                    gap
//   ---------    -----------------------------------   --------------------------
//   system       system/message                        none (see below)
//   user         user/message, source.kind == "user"   none
//   context      user/message with any other source,    the web also draws relay
//                agent/inbox/spliced, developer/       notices this journal has
//                message, todo/write                    no record for
//   compacted    compaction/start|summary|end          none
//   message      assistant/message                     none
//   tool         tool/call + tool/result               none
//   subtool      tool/call whose callId has a PTC      not exercised by this
//                parent link                            session (no such records)
//
// Three facts about that table are worth stating rather than burying:
//
//  * `system` DOES have a source. The brief for this view said the journal keeps
//    no system-prompt record, but it does: this session's own journal starts
//    with `system/message` at seq 7 carrying role "system" and the whole prompt
//    as a text block. Mapping the record that exists is not fabricating one, so
//    it is mapped; the row is honest evidence rather than parity theatre.
//  * `subtool` is implemented but unexercised here. The parent link is read from
//    `tool/ptc-dispatch-start` / `tool/ptc-dispatch` exactly as dsh-android's
//    `ToolCallParents` reads it (`subCallId` -> `parentCallId`), and this
//    session's journal contains none of those records, so no cell can be one.
//  * Everything else in the journal is deliberately NOT a cell: `turn/*` and
//    `step/*` are the turn/step envelopes the cells carry as fields; the request
//    header and context, usage, delivery receipts, approvals, commands, titles,
//    the subagent catalog, retries and seed markers are bookkeeping, not the
//    model's input or output; `compaction/prune` only records which old records
//    a compaction shadowed. The web has no cell for any of them either.

DshClient::TrajectoryLedger &DshClient::trajectorySink()
{
    return m_earlierSink ? m_pageTrajectory : m_trajectory;
}

int DshClient::appendTrajectoryCell(TrajectoryCell cell)
{
    TrajectoryLedger &ledger = trajectorySink();
    const int at = int(ledger.cells.size());
    ledger.byId.insert(cell.id, at);
    ledger.cells.append(cell);

    // A page is translated into its own sink and published when it is merged in
    // front, so only the live ledger's indices mean anything here.
    if (!m_earlierSink)
        m_trajectoryDirty.append(at);
    return at;
}

DshClient::TrajectoryCell &DshClient::editTrajectoryCell(int index)
{
    // Marking rather than publishing keeps one record's whole fold — an append
    // plus the completion of an older cell, say — to one publication.
    if (!m_earlierSink)
        m_trajectoryDirty.append(index);
    return trajectorySink().cells[index];
}

QVariantMap DshClient::flattenTrajectoryCell(const TrajectoryCell &cell) const
{
    return QVariantMap{
        {QStringLiteral("id"), cell.id},
        {QStringLiteral("kind"), cell.kind},
        {QStringLiteral("lane"), cell.lane},
        {QStringLiteral("status"), cell.status},
        {QStringLiteral("seq"), double(cell.seq)},
        {QStringLiteral("turn"), cell.turn},
        {QStringLiteral("step"), cell.step},
        {QStringLiteral("text"), cell.text},
        {QStringLiteral("summary"), cell.summary},
        {QStringLiteral("result"), cell.result},
        {QStringLiteral("resultPreview"), cell.resultPreview},
        {QStringLiteral("input"), cell.input},
        {QStringLiteral("output"), cell.output},
        {QStringLiteral("thinking"), cell.thinking},
        {QStringLiteral("toolName"), cell.toolName},
        {QStringLiteral("callId"), cell.callId},
    };
}

void DshClient::applyTrajectoryEvent(const QJsonObject &event)
{
    // Publication is gated on a real mutation rather than run unconditionally:
    // this journal is roughly half `turn/*`, `step/*` and delivery receipts,
    // none of which is a cell, and republishing anything for them would be pure
    // waste.
    if (!foldTrajectoryRecord(event))
        return;

    // While an earlier page is being translated the mutation landed in the page
    // sink, so the published ledger cannot have changed. applyEarlierPage()
    // republishes once, when it merges the page in front of the ledger.
    if (m_earlierSink)
        return;

    rebuildTrajectory();
}

bool DshClient::foldTrajectoryRecord(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const QJsonObject data = event.value(QStringLiteral("data")).toObject();
    const qint64 seq = qint64(event.value(QStringLiteral("seq")).toDouble(-1));

    // Turn and step boundaries are not cells; they are the envelopes a cell
    // carries. `turn/start` is where the prompt that opens the turn is placed,
    // and `step/start` is what a record without its own step inherits.
    if (type == QLatin1String("turn/start")) {
        m_trajectoryTurn = data.value(QStringLiteral("turn")).toInt(m_trajectoryTurn + 1);
        m_trajectoryStep = 0;
        return false;
    }
    if (type == QLatin1String("step/start")) {
        m_trajectoryTurn = data.value(QStringLiteral("turn")).toInt(m_trajectoryTurn);
        m_trajectoryStep = data.value(QStringLiteral("step")).toInt(m_trajectoryStep);
        return false;
    }

    // `turn/end` closes an open envelope but adds no cell, so it does not touch
    // the ledger. The open/closed state is the *view's* business, and it already
    // has `busy` for the newest turn.
    if (type == QLatin1String("turn/end") || type == QLatin1String("step/end"))
        return false;

    const int turn = data.value(QStringLiteral("turn")).toInt(m_trajectoryTurn);
    const int step = data.value(QStringLiteral("step")).toInt(m_trajectoryStep);

    if (type == QLatin1String("system/message")) {
        const QJsonArray content = data.value(QStringLiteral("message")).toObject()
                                       .value(QStringLiteral("content")).toArray();
        const QString text = contentText(content);
        // A compaction re-materialises the prompt as a surface *replacement*: the
        // shadowed copies return with empty content and only the surviving record
        // carries the text (verified in this session's own journal, where seq
        // 4878 and 4879 are empty `system-prompt` placements and seq 4880 is the
        // prompt itself). An empty record is therefore a tombstone, not an empty
        // prompt, and yields no cell.
        if (text.isEmpty())
            return false;

        TrajectoryCell cell;
        cell.id = QStringLiteral("system:%1").arg(seq);
        cell.kind = QStringLiteral("system");
        cell.lane = 0;
        cell.status = QStringLiteral("complete");
        cell.text = QStringLiteral("System prompt");
        cell.summary = trajectoryPreview(text);
        cell.output = trajectoryDetail(text);
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("user/message")) {
        const QJsonObject source = data.value(QStringLiteral("source")).toObject();
        const QString sourceKind = source.value(QStringLiteral("kind")).toString();
        const QString text = contentText(data.value(QStringLiteral("content")).toArray());
        if (text.isEmpty())
            return false;

        // `source.kind == "user"` is a human prompt; every other kind is the
        // host or a plugin putting words in the user's mouth (runtime context,
        // a skill catalogue, another agent's message, an approval notice). The
        // web separates exactly those two cases into `user` and `context`.
        //
        // This deliberately does NOT repeat the transcript's local-prompt
        // dedupe: the ledger is fed only by durable records, so a prompt appears
        // here once, when the host has committed it, which is precisely what a
        // ledger of the model's input should show.
        const bool human = sourceKind == QLatin1String("user");

        TrajectoryCell cell;
        cell.id = QStringLiteral("user:%1").arg(seq);
        cell.kind = human ? QStringLiteral("user") : QStringLiteral("context");
        cell.lane = 0;
        cell.status = QStringLiteral("complete");
        cell.text = human ? QString() : sourceKind;
        cell.summary = trajectoryPreview(text);
        cell.input = trajectoryDetail(text);
        cell.seq = seq;
        // A human prompt arrives *before* the turn it opens, so a record with no
        // turn of its own is tagged with the next turn rather than the one it
        // arrived after — dsh-android's "a human message opens the following
        // assistant turn" rule. Every other untagged record belongs to the turn
        // already running, so it looks back.
        cell.turn = human ? qMax(turn + 1, 1) : turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("agent/inbox/spliced")) {
        QStringList texts;
        for (const QJsonValue &value : data.value(QStringLiteral("inserted")).toArray()) {
            const QString text = contentText(value.toObject()
                                                 .value(QStringLiteral("content")).toArray());
            if (!text.isEmpty())
                texts += text;
        }
        const QString joined = texts.join(QStringLiteral("\n"));
        if (joined.isEmpty())
            return false;

        TrajectoryCell cell;
        cell.id = QStringLiteral("inbox:%1").arg(seq);
        cell.kind = QStringLiteral("context");
        cell.lane = 0;
        cell.status = QStringLiteral("complete");
        // `target` is the host's own word for where the splice landed — the
        // `next-turn` reminder the desktop client itself receives, for one.
        cell.text = QStringLiteral("Inbox · %1")
                        .arg(data.value(QStringLiteral("target")).toString());
        cell.summary = trajectoryPreview(joined);
        cell.input = trajectoryDetail(joined);
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("developer/message")) {
        const QJsonArray content = data.value(QStringLiteral("message")).toObject()
                                       .value(QStringLiteral("content")).toArray();
        const QString dump = contentDump(content);
        if (dump.isEmpty())
            return false;

        TrajectoryCell cell;
        cell.id = QStringLiteral("dev:%1").arg(seq);
        cell.kind = QStringLiteral("context");
        cell.lane = 0;
        cell.status = QStringLiteral("complete");
        // These blocks are instructions rather than prose (`tool-add`,
        // `tool-removal`, text), so the count is the honest one-line label and
        // the block dump is the detail.
        cell.text = QStringLiteral("Developer · %1 block(s)").arg(content.size());
        cell.summary = trajectoryPreview(dump);
        cell.input = trajectoryDetail(dump);
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("todo/write")) {
        const QJsonArray todos = data.value(QStringLiteral("todos")).toArray();
        if (todos.isEmpty())
            return false;

        QStringList lines;
        for (const QJsonValue &value : todos) {
            const QJsonObject todo = value.toObject();
            lines += QStringLiteral("[%1] %2")
                         .arg(todo.value(QStringLiteral("status")).toString(),
                              todo.value(QStringLiteral("content")).toString());
        }
        const QString joined = lines.join(QStringLiteral("\n"));

        TrajectoryCell cell;
        cell.id = QStringLiteral("todos:%1").arg(seq);
        // The web has no Trajectory node for `todo/write` — its chat target
        // draws the plan card instead — but the write is model-visible input, so
        // it is carried as a context record, the same honest alternative
        // dsh-android takes, rather than dropped from the ledger.
        cell.kind = QStringLiteral("context");
        cell.lane = 0;
        cell.status = QStringLiteral("complete");
        cell.text = QStringLiteral("Todos · %1").arg(todos.size());
        cell.summary = trajectoryPreview(lines.join(QStringLiteral("; ")));
        cell.input = trajectoryDetail(joined);
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("assistant/message")) {
        const QJsonObject message = data.value(QStringLiteral("message")).toObject();
        const QJsonArray content = message.value(QStringLiteral("content")).toArray();
        // No blocks at all is a compaction tombstone, not an empty answer; an
        // answer whose only blocks were tool calls still has its `tool-call`
        // blocks here and is labelled below.
        if (content.isEmpty())
            return false;

        const QString text = blocksOfType(content, QStringLiteral("text"));
        const QString reasoning = blocksOfType(content, QStringLiteral("reasoning"));

        TrajectoryCell cell;
        cell.id = QStringLiteral("message:%1").arg(seq);
        cell.kind = QStringLiteral("message");
        cell.lane = 1;
        cell.status = QStringLiteral("complete");
        // Web `isToolCallOnly`: an assistant step whose whole content was tool
        // calls is labelled rather than left blank, because the row is still the
        // model's output and an empty one reads as a rendering fault.
        if (text.isEmpty() && reasoning.isEmpty())
            cell.text = QStringLiteral("Tool call only");
        cell.summary = trajectoryPreview(text.isEmpty() ? reasoning : text);
        cell.thinking = trajectoryDetail(reasoning);
        cell.output = trajectoryDetail(text);
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("tool/call")) {
        const QString callId = data.value(QStringLiteral("callId")).toString();
        if (callId.isEmpty())
            return false;

        const QString name = data.value(QStringLiteral("name")).toString();
        const QString arguments = data.value(QStringLiteral("arguments")).toString();
        const QString parent = m_ptcParents.value(callId);

        TrajectoryCell cell;
        cell.id = QStringLiteral("call:%1").arg(callId);
        // A dispatched sub-call is the web's own `subtool` cell, not a `tool`
        // one; the ledger stays flat, the parent link only tags the row.
        cell.kind = parent.isEmpty() ? QStringLiteral("tool") : QStringLiteral("subtool");
        cell.lane = 2;
        // A call with no result yet is still running — the one status this
        // journal can express live, and the reason the transcript needs a
        // `in_progress` badge at all.
        cell.status = QStringLiteral("running");
        cell.text = name;
        cell.summary = trajectoryPreview(arguments);
        cell.input = trajectoryDetail(arguments);
        cell.toolName = name;
        cell.callId = callId;
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("tool/result")) {
        const QJsonObject message = data.value(QStringLiteral("message")).toObject();
        const QString callId = message.value(QStringLiteral("toolCallId")).toString();
        const bool isError = message.value(QStringLiteral("isError")).toBool();
        const QString output = contentDump(message.value(QStringLiteral("content")).toArray());

        TrajectoryLedger &ledger = trajectorySink();
        int at = ledger.indexOf(QStringLiteral("call:%1").arg(callId));

        if (at < 0) {
            // A result whose call is not held. The opening window can start
            // after the call, and a result the model produced must not vanish
            // just because the call above it is outside the page — so the result
            // opens its own cell, with the call's name unknown, rather than
            // being dropped.
            TrajectoryCell cell;
            cell.id = QStringLiteral("call:%1").arg(callId);
            cell.kind = QStringLiteral("tool");
            cell.lane = 2;
            cell.status = isError ? QStringLiteral("error") : QStringLiteral("complete");
            cell.text = QStringLiteral("tool result");
            cell.callId = callId;
            cell.seq = seq;
            cell.turn = turn;
            cell.step = step;
            at = appendTrajectoryCell(cell);
        }

        TrajectoryCell &cell = editTrajectoryCell(at);
        cell.status = isError ? QStringLiteral("error") : QStringLiteral("complete");
        cell.output = trajectoryDetail(output);
        // Web `summarizeResult`: a failure's contract is its code, a text result
        // is the preview, an empty one is the "No output" marker. This journal
        // carries no result code, so a failure's verdict is simply `error`.
        if (isError) {
            cell.result = QStringLiteral("error");
            cell.resultPreview = trajectoryPreview(output);
        } else if (output.isEmpty()) {
            cell.result = QStringLiteral("No output");
        } else {
            cell.resultPreview = trajectoryPreview(output);
        }
        return true;
    }

    if (type == QLatin1String("tool/ptc-dispatch-start")
        || type == QLatin1String("tool/ptc-dispatch")) {
        // No cell of its own: the record is the *link* that makes a dispatched
        // call a `subtool`, not a step in the ledger. Read exactly as
        // dsh-android's ToolCallParents reads it.
        const QString subCallId = data.value(QStringLiteral("subCallId")).toString();
        const QString parentCallId = data.value(QStringLiteral("parentCallId")).toString();
        if (subCallId.isEmpty() || parentCallId.isEmpty())
            return false;

        m_ptcParents.insert(subCallId, parentCallId);

        // The link may arrive after the call it tags, so an already-folded cell
        // is re-tagged rather than left as a plain tool. When the call is not
        // held yet the link is remembered and the call picks it up as it folds,
        // which is not a change to the ledger *now* — hence false.
        TrajectoryLedger &ledger = trajectorySink();
        const int at = ledger.indexOf(QStringLiteral("call:%1").arg(subCallId));
        if (at < 0 || ledger.cells.at(at).kind == QLatin1String("subtool"))
            return false;
        editTrajectoryCell(at).kind = QStringLiteral("subtool");
        return true;
    }

    if (type == QLatin1String("compaction/start")) {
        const QString compactionId = data.value(QStringLiteral("compactionId")).toString();
        const QString id = QStringLiteral("compact:%1")
                               .arg(compactionId.isEmpty() ? QString::number(seq) : compactionId);
        if (!compactionId.isEmpty())
            m_compactionCells.insert(compactionId, id);

        TrajectoryCell cell;
        cell.id = id;
        cell.kind = QStringLiteral("compacted");
        cell.lane = 1;
        // Compaction runs for seconds to minutes, so `start` without a summary
        // yet is genuinely running rather than a finished row.
        cell.status = QStringLiteral("running");
        cell.text = QStringLiteral("Compaction started");
        cell.seq = seq;
        cell.turn = turn;
        cell.step = step;
        appendTrajectoryCell(cell);
        return true;
    }

    if (type == QLatin1String("compaction/summary")) {
        const QString compactionId = data.value(QStringLiteral("compactionId")).toString();
        const QString summary = blocksOfType(data.value(QStringLiteral("summary")).toArray(),
                                            QStringLiteral("text"));
        if (summary.isEmpty())
            return false;

        TrajectoryLedger &ledger = trajectorySink();
        QString id = m_compactionCells.value(compactionId);
        int at = id.isEmpty() ? -1 : ledger.indexOf(id);
        if (at < 0) {
            // A summary whose `start` is outside the window: the same reasoning
            // as an orphaned tool result — the payload is real, so it gets a row.
            TrajectoryCell cell;
            cell.id = id.isEmpty() ? QStringLiteral("compact:%1").arg(seq) : id;
            cell.kind = QStringLiteral("compacted");
            cell.lane = 1;
            cell.seq = seq;
            cell.turn = turn;
            cell.step = step;
            if (!compactionId.isEmpty())
                m_compactionCells.insert(compactionId, cell.id);
            at = appendTrajectoryCell(cell);
        }

        TrajectoryCell &cell = editTrajectoryCell(at);
        // Web `layout.ts`: a complete compaction that *has* a summary shows the
        // summary and leaves `text` empty; only a summary-less one falls back to
        // the "Context compacted" marker copy.
        cell.status = QStringLiteral("complete");
        cell.text.clear();
        cell.summary = trajectoryPreview(summary);
        cell.output = trajectoryDetail(summary);
        return true;
    }

    if (type == QLatin1String("compaction/end")) {
        const QString compactionId = data.value(QStringLiteral("compactionId")).toString();
        const QString id = m_compactionCells.value(compactionId);
        TrajectoryLedger &ledger = trajectorySink();
        const int at = id.isEmpty() ? -1 : ledger.indexOf(id);
        if (at < 0)
            return false;

        TrajectoryCell &cell = editTrajectoryCell(at);
        if (cell.status == QLatin1String("complete"))
            return false;
        cell.status = QStringLiteral("complete");
        // An end with no summary in between is still a finished compaction; the
        // marker copy is what the web shows for exactly that case.
        if (cell.text.isEmpty() && cell.output.isEmpty())
            cell.text = QStringLiteral("Context compacted");
        return true;
    }

    // Everything else in the journal is bookkeeping, not a ledger cell:
    // request/header and request/context are the request's own configuration,
    // usage and the delivery receipts account for tokens, approvals and commands
    // are control-plane, titles and the subagent catalog are session metadata,
    // retries belong to an attempt the assistant message already represents, and
    // `compaction/prune` records only which old records a compaction shadowed.
    // None of them is the model's input or output, and the web has no cell for
    // any of them either.
    return false;
}

void DshClient::rebuildTrajectory()
{
    const QVector<TrajectoryCell> &cells = m_trajectory.cells;
    const int total = int(cells.size());

    // A prepended page renumbers every cell, so the projection cannot be patched
    // across it: flatten the whole ledger once. Everything else is a patch at a
    // known index — the stream appends at the end, and a tool result or a
    // compaction completes a cell that is already in the list.
    if (m_trajectoryReindexed || int(m_trajectoryList.size()) > total) {
        m_trajectoryReindexed = false;
        m_trajectoryDirty.clear();

        QVariantList flattened;
        flattened.reserve(total);
        for (const TrajectoryCell &cell : cells)
            flattened.append(flattenTrajectoryCell(cell));

        // Emit only on a real change, exactly as rebuildFileRows() and
        // applySessionList() do: a repeated record, or a page that translated to
        // the rows already held, must not make every bound view rebuild itself.
        if (flattened == m_trajectoryList)
            return;

        m_trajectoryList = flattened;
        emit trajectoryChanged();
        return;
    }

    bool changed = false;
    const int published = int(m_trajectoryList.size());

    // Completing a call rewrites an entry that is already published; compare it
    // first so a result that says nothing new does not wake every bound view.
    for (int i = 0; i < m_trajectoryDirty.size(); ++i) {
        const int index = m_trajectoryDirty.at(i);
        if (index < 0 || index >= total || index >= published)
            continue;
        const QVariantMap entry = flattenTrajectoryCell(cells.at(index));
        if (m_trajectoryList.at(index).toMap() == entry)
            continue;
        m_trajectoryList[index] = entry;
        changed = true;
    }
    m_trajectoryDirty.clear();

    // The common record appends one cell, and the published list is normally
    // already the ledger's prefix, so the growth is one entry and no more.
    if (total > published) {
        m_trajectoryList.reserve(total);
        for (int index = published; index < total; ++index) {
            m_trajectoryList.append(flattenTrajectoryCell(cells.at(index)));
            changed = true;
        }
    }

    if (changed)
        emit trajectoryChanged();
}

void DshClient::mergeTrajectoryPage()
{
    if (m_pageTrajectory.cells.isEmpty())
        return;

    // A page is strictly older than everything held — `throughSeq` is a cut
    // below the oldest record — so its cells go in front, in the order they were
    // translated, without any sorting. The id index is rebuilt rather than
    // adjusted because prepending shifts every index in it, and the projection
    // is marked for the same reason: every index it holds has moved.
    QVector<TrajectoryCell> merged = m_pageTrajectory.cells;
    merged += m_trajectory.cells;
    m_trajectory.cells = merged;
    m_pageTrajectory = TrajectoryLedger{};
    m_trajectoryReindexed = true;

    m_trajectory.byId.clear();
    m_trajectory.byId.reserve(m_trajectory.cells.size());
    for (int i = 0; i < m_trajectory.cells.size(); ++i)
        m_trajectory.byId.insert(m_trajectory.cells.at(i).id, i);
}

void DshClient::resetTrajectory()
{
    m_trajectory = TrajectoryLedger{};
    m_pageTrajectory = TrajectoryLedger{};
    m_ptcParents.clear();
    m_compactionCells.clear();
    m_trajectoryTurn = 0;
    m_trajectoryStep = 0;
    m_trajectoryDirty.clear();
    m_trajectoryReindexed = false;

    // A cleared ledger is a real change, so a view switches to its empty state
    // rather than showing the previous session's cells under the new id.
    if (m_trajectoryList.isEmpty())
        return;
    m_trajectoryList.clear();
    emit trajectoryChanged();
}

// --- model selection --------------------------------------------------------

void DshClient::refreshConfigOptions()
{
    // The host's parameter list for this method is empty, so `args` stays empty.
    call(QStringLiteral("session/modelCatalog"));
}

void DshClient::applyModelCatalog(const QJsonObject &catalog)
{
    if (!catalog.isEmpty())
        m_catalog = catalog;

    QVariantList options;

    if (!m_catalog.isEmpty()) {
        // A session that has never chosen a route follows the catalog default, so
        // the picker shows what would actually run rather than nothing selected.
        QString provider = m_provider;
        QString model = m_model;
        if (provider.isEmpty() || model.isEmpty()) {
            const QJsonObject fallback = m_catalog.value(QStringLiteral("default")).toObject();
            provider = fallback.value(QStringLiteral("provider")).toString();
            model = fallback.value(QStringLiteral("model")).toString();
        }

        // The catalog's provider/model nesting is flattened here so the QML
        // picker stays a flat list of {value,label,group,provider,description} —
        // the exact shape AcpClient::refreshConfigOptions produces.
        //
        // `provider` carries the catalog's own provider ID beside the display
        // name in `group`, because the picker sections the routes by provider
        // and both reference clients put the signed-in account and then the
        // official API first — a rank the host does not promise. Sorting by the
        // display name instead would be ordering captions by eye, and the two
        // providers that must lead are exactly the two whose names happen to
        // sort first, which would hide the mistake.
        QVariantList choices;
        for (const QJsonValue &groupValue : m_catalog.value(QStringLiteral("groups")).toArray()) {
            const QJsonObject group = groupValue.toObject();
            const QString groupName = group.value(QStringLiteral("name")).toString();
            const QString providerId = group.value(QStringLiteral("id")).toString();
            for (const QJsonValue &modelValue : group.value(QStringLiteral("models")).toArray()) {
                const QJsonObject entry = modelValue.toObject();
                choices.append(QVariantMap{
                    {QStringLiteral("value"),
                     modelChoiceValue(providerId, entry.value(QStringLiteral("id")).toString())},
                    {QStringLiteral("label"), entry.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("group"), groupName},
                    {QStringLiteral("provider"), providerId},
                    {QStringLiteral("description"),
                     entry.value(QStringLiteral("description")).toString()},
                });
            }
        }

        options.append(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("model")},
            {QStringLiteral("name"), QStringLiteral("Model")},
            {QStringLiteral("category"), QStringLiteral("model")},
            {QStringLiteral("currentValue"), modelChoiceValue(provider, model)},
            {QStringLiteral("choices"), choices},
        });

        // Reasoning effort is a per-model capability, so the option exists only
        // when the selected model declares efforts. A model with a declared
        // default does not get a "Provider default" entry, because that default is
        // already what the value resolves to.
        const QJsonObject reasoning = reasoningFor(m_catalog, provider, model);
        const QJsonArray efforts = reasoning.value(QStringLiteral("efforts")).toArray();
        if (!efforts.isEmpty()) {
            const QString declared = reasoning.value(QStringLiteral("defaultEffort")).toString();
            QVariantList effortChoices;
            if (declared.isEmpty()) {
                effortChoices.append(QVariantMap{
                    {QStringLiteral("value"), QString()},
                    {QStringLiteral("label"), QStringLiteral("Provider default")},
                    {QStringLiteral("group"), QString()},
                    {QStringLiteral("description"), QString()},
                });
            }
            for (const QJsonValue &effortValue : efforts) {
                const QJsonObject effort = effortValue.toObject();
                effortChoices.append(QVariantMap{
                    {QStringLiteral("value"), effort.value(QStringLiteral("id")).toString()},
                    {QStringLiteral("label"), effort.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("group"), QString()},
                    {QStringLiteral("description"),
                     effort.value(QStringLiteral("description")).toString()},
                });
            }

            options.append(QVariantMap{
                {QStringLiteral("id"), QStringLiteral("reasoning_effort")},
                {QStringLiteral("name"), QStringLiteral("Reasoning effort")},
                {QStringLiteral("category"), QStringLiteral("thought_level")},
                {QStringLiteral("currentValue"),
                 m_reasoningEffort.isEmpty() ? declared : m_reasoningEffort},
                {QStringLiteral("choices"), effortChoices},
            });
        }
    }

    m_configOptions = options;
    emit configOptionsChanged();
}

// --- prompts ----------------------------------------------------------------

void DshClient::sendPrompt(const QString &text, const QString &mode)
{
    if (text.trimmed().isEmpty())
        return;

    if (m_pendingSeat.active) {
        // The first real use of a deferred seat. Nothing has been created and
        // there is no id to prompt, so the turn is held here and the seat is
        // resolved against the roster as it is now; the prompt is delivered into
        // whatever that resolution becomes (finishMaterialisation).
        //
        // A second Enter while that is in flight is refused rather than queued:
        // there is one held turn, and the second is the reader's next message,
        // which the composer can still hold once the seat is gone.
        if (m_heldPrompt.active) {
            setStatus(QStringLiteral("still starting this session…"));
            return;
        }
        if (m_state != Ready) {
            // No socket, so no session can be made. The text goes back to the
            // composer rather than vanishing with no row to show for it.
            const QString reason = QStringLiteral("not connected yet");
            setStatus(reason);
            emit pendingPromptDropped(text, reason);
            return;
        }
        m_heldPrompt = HeldPrompt{true, text, mode};
        materialisePendingPrompt();
        return;
    }

    deliverPrompt(text, mode, false);
}

void DshClient::deliverPrompt(const QString &text, const QString &mode, bool knownIdle)
{
    if (m_sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session to prompt"));
        return;
    }
    if (text.trimmed().isEmpty())
        return;

    // A client-minted identity, exactly as ACP mints its own request ids. It is
    // what lets the row below be recognised in the journal later.
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // What the transcript says the moment Enter is pressed.
    //
    // A prompt that starts a turn is journaled as it is delivered, so the echo is
    // the same message said once — the durable copy is recognised by this id and
    // dropped. A queued or steered one is admitted to the agent's inbox and is
    // not journaled until it is actually delivered, which may be minutes later or
    // never. Silence there is not honest: the message was invisible from Enter
    // until delivery. A user *bubble* is not honest either — that was the
    // original lie, and the transcript had to take it back. So while a turn runs
    // the row is a delivery *state*: dim, no stamp, nothing to copy, and its text
    // is only filled in when the host's own copy arrives.
    // Whether a turn is running is what decides between the two rows below, and
    // `m_busy` is not that question — see m_turnOpen in the header. `m_busy` is
    // still read here for one narrower purpose: it is set the instant a prompt of
    // ours starts a turn, so a second Enter before the host's own `turn/start`
    // arrives is staged rather than echoed.
    //
    // The two modes are logged here and only here. The composer chose `mode` from
    // `busy` (and, once this property exists, `turnOpen`), the client sends it
    // unchanged, and nothing used to record the decision: a steer that came back
    // queued could not be attributed to the composer, to the host, or to a lost
    // socket. `sentMode` is exactly what goes on the wire.
    const QString sentMode = mode.isEmpty() ? QStringLiteral("queue") : mode;
    // `knownIdle` is the materialised seat's answer: the session it became was
    // just created, or is a blank the host already had, so a turn cannot already
    // be running in it. Without it the first prompt of a deferred seat would be
    // drawn as a *staged* row — a follow latches `turnOpen` to "unknown" — for a
    // turn the client knows it starts. The branch below is what sets `turnOpen`
    // for either answer, so nothing is written here.
    const bool startsTurn = knownIdle || (!m_turnOpen && !m_busy);

    // The staged input becomes `PromptContentPart`s here, before the prompt is
    // logged as sent, echoed, cleared or called. The host's union declares exactly
    // three arms and nothing else:
    //
    //   {type:'text',  text}
    //   {type:'image', mediaType, data, name?}
    //   {type:'file',  receiptId}
    //
    // The file arm is the *receipt id* minted by /api/session/uploadFileBinary
    // (`value.receiptId`), not the durable `value.file` ref: the ref under any
    // field name matches no arm, and the host answers `gateway/input-invalid`
    // naming `request`. A file entry that never got a receipt therefore cannot be
    // sent at all — it is refused here by name, before the call, instead of being
    // dropped or discovered at the boundary. A receipt is also scoped to the
    // Agent it was uploaded into, so an entry from another session is refused for
    // the same reason (the host would say `session/attachment-invalid`: "File was
    // not uploaded for this session").
    const bool subagentTarget =
        m_followAddress.value(QStringLiteral("kind")).toString() == QLatin1String("subagent");
    QJsonArray content{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), text},
    }};
    for (const QVariant &entry : m_pendingAttachments) {
        const QVariantMap staged = entry.toMap();
        const QString name = staged.value(QStringLiteral("name")).toString();
        const QString label = name.isEmpty() ? QStringLiteral("an unnamed attachment") : name;

        if (staged.value(QStringLiteral("kind")).toString() == QLatin1String("image")) {
            // The image arm carries the bytes inline; it has no receipt because
            // there is no upload to cite. Both fields are required by the union,
            // so a half-staged image is refused here rather than guessed at.
            const QString mediaType = staged.value(QStringLiteral("mediaType")).toString();
            const QString data = staged.value(QStringLiteral("data")).toString();
            if (mediaType.isEmpty() || data.isEmpty()) {
                setStatus(QStringLiteral("cannot send %1: the image has no staged bytes — "
                                         "remove it and attach it again").arg(label));
                qWarning() << "session/prompt refused: staged image" << label
                           << "has no media type or data";
                return;
            }
            QJsonObject part{
                {QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("mediaType"), mediaType},
                {QStringLiteral("data"), data},
            };
            if (!name.isEmpty())
                part.insert(QStringLiteral("name"), name);
            content.append(part);
            continue;
        }

        const QString receipt = staged.value(QStringLiteral("receipt")).toString();
        if (receipt.isEmpty()) {
            setStatus(QStringLiteral("cannot send %1: the upload did not produce a receipt — "
                                     "remove it and attach the file again").arg(label));
            qWarning() << "session/prompt refused: staged file" << label << "has no receipt";
            return;
        }
        const QString stagedIn = staged.value(QStringLiteral("session")).toString();
        if (!stagedIn.isEmpty() && stagedIn != m_sessionId) {
            setStatus(QStringLiteral("cannot send %1: it was attached to another session — "
                                     "remove it and attach it again here").arg(label));
            qWarning() << "session/prompt refused: staged file" << label
                       << "belongs to session" << stagedIn << "not" << m_sessionId;
            return;
        }
        if (subagentTarget) {
            // A child takes text and images only: `SubagentPromptRequest.content`
            // is the attachment package's union, which has no file arm at all.
            setStatus(QStringLiteral("cannot send %1: a subagent takes text and images, "
                                     "not file attachments").arg(label));
            qWarning() << "session/prompt refused: file" << label
                       << "cannot ride a subagent prompt";
            return;
        }
        content.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("file")},
            {QStringLiteral("receiptId"), receipt},
        });
    }

    qInfo() << "session/prompt: sent mode" << sentMode
            << "| requested mode" << (mode.isEmpty() ? QStringLiteral("(none)") : mode)
            << "| requestId" << requestId
            << "| session" << m_sessionId
            << "| busy" << m_busy << "turnOpen" << m_turnOpen
            << "startsTurn" << startsTurn
            << "| text chars" << text.size()
            << "attachments" << m_pendingAttachments.size()
            << "| local row"
            << (startsTurn
                    ? QStringLiteral("echoed")
                    : (sentMode == QLatin1String("steer") ? QStringLiteral("staged (steering)")
                                                          : QStringLiteral("staged (queued)")));
    if (startsTurn) {
        setTurnOpen(true);
        m_localPrompts.insert(requestId, LocalPromptEchoed);
        m_transcript->append(QStringLiteral("user"), text, QString(), QString(),
                             QString(), requestId);
    } else {
        m_localPrompts.insert(requestId, LocalPromptStaged);
        // `detail` is the bounded preview the row draws, and `fullText` is what
        // the reader actually typed. The row is a delivery state, so its visible
        // text has to stay one short line; but the model is the only place the
        // whole prompt then exists until the host delivers it (or never), and a
        // Copy that could only recover 140 characters would be losing the
        // reader's text. The extra argument is empty on every other row.
        m_transcript->append(QStringLiteral("user"), QString(), QString(),
                             sentMode == QLatin1String("steer") ? QStringLiteral("steering")
                                                                : QStringLiteral("queued"),
                             stagedPreview(text), requestId, 0, -1, QString(), text);
    }

    // A subagent is not prompted as a session. The address this transcript is
    // following says which it is, so the composer needs no separate mode: the
    // same Send goes to whichever conversation is on screen.
    if (subagentTarget) {
        const QString parent = m_followAddress.value(QStringLiteral("parentSessionId")).toString();
        const QString child = m_followAddress.value(QStringLiteral("childSessionId")).toString();
        if (parent.isEmpty() || child.isEmpty()) {
            // Nothing is sent, so the row that was just put in the transcript
            // must not be left behind claiming to wait for a delivery — and the
            // staged input stays staged, because the reader is about to resend it.
            abandonLocalPrompt(requestId);
            setStatus(QStringLiteral("a subagent address needs both a parent and a child"));
            return;
        }
        // The parts are already copies inside `content`; only now is the staging
        // list done with them. Clearing before a refusal above would have thrown
        // away input that was never sent.
        clearPendingAttachments();
        call(QStringLiteral("subagents/prompt"),
             QJsonObject{{QStringLiteral("request"), QJsonObject{
                 {QStringLiteral("requestId"), requestId},
                 {QStringLiteral("parentSessionId"), parent},
                 {QStringLiteral("childSessionId"), child},
                 // The discriminator the browser control address also carries;
                 // only a continuable child accepts a human message at all.
                 {QStringLiteral("mode"), QStringLiteral("continuable")},
                 // `delivery`, not `mode`: the subagent vocabulary names queueing
                 // and steering differently from the session one.
                 {QStringLiteral("delivery"),
                  sentMode == QLatin1String("steer") ? QStringLiteral("steer")
                                                     : QStringLiteral("queue")},
                 {QStringLiteral("content"), content},
             }}}, requestId);
        qInfo() << "subagents/prompt sent: requestId" << requestId
                << "delivery" << (sentMode == QLatin1String("steer")
                                      ? QStringLiteral("steer") : QStringLiteral("queue"))
                << "parent" << parent << "child" << child;
        setBusy(true);
        setStatus(QStringLiteral("thinking…"));
        refreshSessions();
        return;
    }

    // The message is about to leave: the parts are copies inside `content`, so
    // the staging list can go. A queue and a steer take the same content, receipt
    // ids included — the host binds each receipt to this requestId and retires it
    // only when the message is delivered or its queue occurrence is removed.
    clearPendingAttachments();
    call(QStringLiteral("session/prompt"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("requestId"), requestId},
             {QStringLiteral("sessionId"), m_sessionId},
             // `queue` admits the message while a turn is already running, which
             // is what the host's own composer does by default; `steer` delivers at
             // the next step boundary. Neither cancels anything — `session/cancel`
             // is the only interrupter. Both are passed through as given: an
             // unknown mode is refused by the host's boundary validation naming
             // the field, which is a clearer failure than silently queueing.
             {QStringLiteral("mode"), sentMode},
             {QStringLiteral("content"), content},
         }}}, requestId);

    // Say which one happened. A queue and a steer look identical at the moment of
    // pressing Enter — both leave the composer empty — and the difference only
    // shows up later, as a message that arrives mid-turn or one that waits.
    if (startsTurn) {
        setBusy(true);
        setStatus(QStringLiteral("thinking…"));
    } else if (sentMode == QLatin1String("steer")) {
        setStatus(QStringLiteral("steering · delivered at the next step boundary"));
    } else {
        setStatus(QStringLiteral("queued · delivered when this turn ends"));
    }
}

void DshClient::cancel()
{
    if (m_sessionId.isEmpty())
        return;

    // Stop ends the wait a staged prompt was in. The host keeps its inbox across
    // a cancel, so the message may still be delivered — the row stops claiming to
    // steer, and the delivery turns the same row into the message if it comes.
    withholdStagedPrompts();

    // Stop was pressed, so the in-flight attempt's rows go now. The host may
    // still finalize the delivered prefix as an `interrupted` assistant message;
    // that arrives as an ordinary journal event and renders from the journal,
    // which is why the provisional rows must not wait for it.
    discardProvisional();

    // Stop means the same thing on screen whether the conversation is a session
    // or a subagent, but the host spells it differently: a child is interrupted
    // through its parent, and only a continuable child can be interrupted at all.
    if (m_followAddress.value(QStringLiteral("kind")).toString()
        == QLatin1String("subagent")) {
        call(QStringLiteral("subagents/interruptByParent"),
             QJsonObject{
                 {QStringLiteral("childSessionId"),
                  m_followAddress.value(QStringLiteral("childSessionId")).toString()},
                 {QStringLiteral("parentSessionId"),
                  m_followAddress.value(QStringLiteral("parentSessionId")).toString()},
                 {QStringLiteral("mode"), QStringLiteral("continuable")},
             });
        setStatus(QStringLiteral("stopping the subagent…"));
        return;
    }

    call(QStringLiteral("session/cancel"),
         QJsonObject{{QStringLiteral("request"),
                      QJsonObject{{QStringLiteral("sessionId"), m_sessionId}}}});
    setStatus(QStringLiteral("cancelling…"));
}

void DshClient::setConfigOption(const QString &configId, const QString &value)
{
    if (m_sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session yet"));
        setModelNotice(QStringLiteral("no session to change the model for"));
        return;
    }

    QString provider = m_provider;
    QString model = m_model;
    QString effort = m_reasoningEffort;

    if (configId == QLatin1String("model")) {
        // A model choice is the opaque JSON pair the catalog advertised, so the
        // same picker value works for either transport. This is the round trip:
        // `applyModelCatalog` writes `[provider, model]` and this reads it back,
        // so a bare `provider/model` string from a future catalog would be
        // refused here — visibly, on the strip, not with a silent no-op.
        const QJsonArray route = QJsonDocument::fromJson(value.toUtf8()).array();
        if (route.size() != 2
            || route.at(0).toString().isEmpty() || route.at(1).toString().isEmpty()) {
            const QString message =
                QStringLiteral("unrecognised model choice: %1").arg(value);
            setStatus(message);
            setModelNotice(message);
            return;
        }
        provider = route.at(0).toString();
        model = route.at(1).toString();

        // Effort is a per-model capability. Carrying an effort the new model does
        // not declare would make the host refuse the whole selection, so drop it
        // and let the host apply that model's own default; the reply then reports
        // the effort actually resolved.
        if (!effort.isEmpty()) {
            bool supported = false;
            const QJsonArray efforts = reasoningFor(m_catalog, provider, model)
                                           .value(QStringLiteral("efforts")).toArray();
            for (const QJsonValue &entry : efforts) {
                if (entry.toObject().value(QStringLiteral("id")).toString() == effort) {
                    supported = true;
                    break;
                }
            }
            if (!supported)
                effort.clear();
        }
    } else if (configId == QLatin1String("reasoning_effort")) {
        // The empty string is the advertised "no explicit effort" value, which
        // the host expresses by leaving the field out entirely.
        effort = value;
    } else {
        const QString message =
            QStringLiteral("unknown session config option: %1").arg(configId);
        setStatus(message);
        setModelNotice(message);
        return;
    }

    // The route is only known once the session's projection baseline has been
    // read. A selection made from the host-wide default before then would send an
    // empty provider and be rejected at the far end.
    if (provider.isEmpty() || model.isEmpty()) {
        const QString message = QStringLiteral("no model route for this session yet");
        setStatus(message);
        setModelNotice(message);
        return;
    }

    QJsonObject request{
        {QStringLiteral("sessionId"), m_sessionId},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("model"), model},
    };
    if (!effort.isEmpty())
        request.insert(QStringLiteral("reasoningEffort"), effort);

    // A fresh attempt replaces the last failure's sentence; the chip keeps
    // showing the old route until the host's answer says what is now in force.
    setModelNotice(QString());
    call(QStringLiteral("session/selectModel"),
         QJsonObject{{QStringLiteral("request"), request}});
}

void DshClient::createSession()
{
    if (m_state != Ready) {
        setStatus(QStringLiteral("not connected yet"));
        return;
    }

    // The seat resolves its target the way the old request did: the current
    // session's Workspace, else its directory. Nothing is sent here — this is
    // where `session/create` used to run and litter the roster with a row nothing
    // can remove.
    const QString workspaceId = m_workspaceOfSession.value(m_sessionId);
    // The synthetic automations bucket is not a Workspace and must never be sent
    // as one.
    if (!workspaceId.isEmpty() && workspaceId != QString::fromLatin1(kAutomationsGroup)) {
        beginPendingSession(workspaceId, QString());
        return;
    }

    QString cwd;
    if (!m_sessionId.isEmpty()) {
        for (const QJsonValue &entry : m_rawSessions) {
            const QJsonObject item = entry.toObject();
            if (item.value(QStringLiteral("sessionId")).toString() != m_sessionId)
                continue;
            cwd = item.value(QStringLiteral("cwd")).toString();
            break;
        }
    }
    // Neither a Workspace nor a directory: the host's own default, which is what
    // an empty `session/create` request asked for and is still what the seat
    // materialises into. No target is invented here.
    beginPendingSession(QString(), cwd);
}

void DshClient::createSessionInWorkspace(const QString &workspaceId)
{
    if (m_state != Ready) {
        emit railNotice(QStringLiteral("Cannot create a session: not connected yet."), true);
        setStatus(QStringLiteral("not connected yet"));
        return;
    }
    const QString id = workspaceId.trimmed();
    // A header's key is either a real Workspace id, "" for Ungrouped, or the
    // synthetic automations bucket. Only the first is a Workspace the host will
    // accept here; the rail does not offer the control on the other two, and this
    // refuses them anyway rather than recording a seat that could only fail.
    if (id.isEmpty() || id == QString::fromLatin1(kAutomationsGroup)) {
        emit railNotice(QStringLiteral("Cannot create a session here: that group is not a workspace."),
                        true);
        setStatus(QStringLiteral("not a workspace"));
        return;
    }

    // The host resolves the directory from the Workspace, so the seat carries
    // only that id and the one `session/create` at first send sends only it.
    beginPendingSession(id, QString());
}

void DshClient::beginPendingSession(const QString &workspaceId, const QString &cwd)
{
    // A second press while a seat is already held retargets it and changes
    // nothing else: the reader is still composing the same unsent session, and
    // the text in the field must survive a press of a different +.
    if (m_pendingSeat.active) {
        const bool moved = m_pendingSeat.workspaceId != workspaceId
                        || m_pendingSeat.cwd != cwd;
        m_pendingSeat.workspaceId = workspaceId;
        m_pendingSeat.cwd = cwd;
        if (moved)
            emit pendingSessionChanged();
        setStatus(pendingSeatStatus());
        return;
    }

    // Opening any Session abandons a seat, so this is not a follow: the followed
    // session is given up first and its view is cleared. `unfollowSession` keeps
    // the old transcript as a frozen record on purpose; a new-session screen has
    // no session to file those rows under, so they go too.
    //
    // Everything else scoped to the session being left goes with it. A follow did
    // this as a side effect of opening the next session; a seat follows nothing,
    // so the screen would otherwise keep the previous session's shell, file tree,
    // queue, todos and change list. The terminal is closed first, while the id it
    // was minted against is still the followed one. The queue matters most: its
    // own controls guard on `sessionId`, so they would be dead buttons over
    // another conversation's messages.
    if (!m_terminalId.isEmpty())
        closeTerminal();
    resetWorkspaceFiles();
    if (!m_queue.isEmpty()) {
        m_queue.clear();
        emit queueChanged();
    }
    if (!m_todos.isEmpty()) {
        m_todos.clear();
        emit todosChanged();
    }
    clearChangedFiles();

    unfollowSession();
    resetFollowTranscript();
    setOpeningSession(false);

    // The access posture and the last refusal belonged to the session just given
    // up. This seat has neither, and a chip still naming a preset would be a
    // claim about a session that does not exist.
    if (!m_permissionPreset.isEmpty()) {
        m_permissionPreset.clear();
        emit presetsChanged();
    }
    setAccessNotice(QString());
    setModelNotice(QString());

    m_pendingSeat.workspaceId = workspaceId;
    m_pendingSeat.cwd = cwd;
    setPendingSession(true);
    // The seat is where the reader is now. Without this, the next roster answer
    // would run restoreLastSession() against an empty sessionId and reopen the
    // conversation they just left.
    m_restoreAttempted = true;
    setStatus(pendingSeatStatus());
}

QString DshClient::pendingSeatStatus() const
{
    const QString where = m_pendingSeat.workspaceId.isEmpty()
                              ? QString()
                              : workspaceLabel(m_pendingSeat.workspaceId);
    if (!where.isEmpty())
        return QStringLiteral("new session in %1 — type a message to start it").arg(where);
    if (!m_pendingSeat.cwd.isEmpty()) {
        return QStringLiteral("new session in %1 — type a message to start it")
            .arg(m_pendingSeat.cwd.section(QLatin1Char('/'), -1));
    }
    return QStringLiteral("new session — type a message to start it");
}

QString DshClient::workspacePath(const QString &workspaceId) const
{
    if (workspaceId.isEmpty())
        return QString();
    for (const QVariant &entry : m_workspaces) {
        const QVariantMap workspace = entry.toMap();
        if (workspace.value(QStringLiteral("workspaceId")).toString() == workspaceId)
            return workspace.value(QStringLiteral("path")).toString();
    }
    return QString();
}

QString DshClient::workspaceLabel(const QString &workspaceId) const
{
    if (workspaceId.isEmpty())
        return QString();
    for (const QVariant &entry : m_workspaces) {
        const QVariantMap workspace = entry.toMap();
        if (workspace.value(QStringLiteral("workspaceId")).toString() != workspaceId)
            continue;
        const QString title = workspace.value(QStringLiteral("title")).toString();
        if (!title.isEmpty())
            return title;
        const QString path = workspace.value(QStringLiteral("path")).toString();
        return path.isEmpty() ? QString() : path.section(QLatin1Char('/'), -1);
    }
    return QString();
}

DshClient::SessionPlan DshClient::planSessionTarget(const QString &workspaceId,
                                                    const QString &cwd) const
{
    // The roster as it is NOW. `m_sessions` already has the forwarded
    // `api-session/*` statements folded over the last `session/list`, so a row
    // the host announced a second ago is in it and a row that was archived since
    // is flagged as such.
    //
    // dsh-android's `plan` counts a candidate only when it is blank, not a
    // subagent child and not archived; a Session that already has a turn in it is
    // not a blank whatever the host's `blank` flag says, so `running` is kept for
    // the caller rather than folded into the filter.
    QVector<QVariantMap> blanks;
    blanks.reserve(m_sessions.size());
    for (const QVariant &entry : m_sessions) {
        const QVariantMap row = entry.toMap();
        if (!row.value(QStringLiteral("blank")).toBool())
            continue;
        if (row.value(QStringLiteral("archived")).toBool())
            continue;
        if (row.value(QStringLiteral("origin")).toString() == QLatin1String("subagent"))
            continue;
        blanks.append(row);
    }

    auto idle = [](const QVariantMap &row) {
        return !row.value(QStringLiteral("running")).toBool();
    };

    SessionPlan plan;
    if (!workspaceId.isEmpty()) {
        // 1. A blank the Workspace already accounts is the session this intent
        //    means. Membership alone is the test, because the host only accounts a
        //    Session to a Workspace whose directory matches, and re-deriving that
        //    from the roster's `cwd` would miss a member spelled differently —
        //    which reads as "no blank there" and creates a duplicate.
        for (const QVariantMap &row : blanks) {
            if (row.value(QStringLiteral("workspaceId")).toString() != workspaceId)
                continue;
            plan.kind = SessionPlanKind::Reuse;
            plan.sessionId = row.value(QStringLiteral("sessionId")).toString();
            plan.workspaceId = workspaceId;
            plan.idle = idle(row);
            return plan;
        }
        // 2. A blank rooted here but not accounted yet is adopted rather than
        //    duplicated. Exact string equality is deliberate: the host compares
        //    `header.cwd` to the Workspace path byte for byte.
        const QString path = workspacePath(workspaceId);
        if (!path.isEmpty()) {
            for (const QVariantMap &row : blanks) {
                if (row.value(QStringLiteral("cwd")).toString() != path)
                    continue;
                plan.kind = SessionPlanKind::Adopt;
                plan.sessionId = row.value(QStringLiteral("sessionId")).toString();
                plan.workspaceId = workspaceId;
                plan.idle = idle(row);
                return plan;
            }
        }
        // 3. Nothing fits: the one branch that adds a session.
        plan.kind = SessionPlanKind::Create;
        plan.workspaceId = workspaceId;
        return plan;
    }

    if (!cwd.isEmpty()) {
        // A blank at exactly this directory is the session the user means,
        // whether or not some Workspace accounts it: opening it puts them in the
        // right directory either way.
        for (const QVariantMap &row : blanks) {
            if (row.value(QStringLiteral("cwd")).toString() != cwd)
                continue;
            plan.kind = SessionPlanKind::Reuse;
            plan.sessionId = row.value(QStringLiteral("sessionId")).toString();
            plan.idle = idle(row);
            return plan;
        }
        plan.kind = SessionPlanKind::Create;
        plan.cwd = cwd;
        return plan;
    }

    // No target at all: the host's own default. Still `Create` without a request
    // field, exactly as the eager call sent it.
    plan.kind = SessionPlanKind::Create;
    return plan;
}

void DshClient::materialisePendingPrompt()
{
    if (!m_pendingSeat.active || !m_heldPrompt.active)
        return;

    const SessionPlan plan = planSessionTarget(m_pendingSeat.workspaceId,
                                               m_pendingSeat.cwd);
    if (plan.kind == SessionPlanKind::Reuse) {
        // A blank the host has at the target is already what the reader meant:
        // open it and issue no `session/create` at all. The seat is given up
        // *after* the follow, so adoptDraft() in the window can see the field's
        // text was typed on the seat and belongs to this session.
        m_seatFollow = true;
        followSession(plan.sessionId);
        m_seatFollow = false;
        finishMaterialisation(plan.sessionId, plan.idle);
        return;
    }

    QJsonObject request;
    if (plan.kind == SessionPlanKind::Adopt) {
        // Both ids: the host adopts the blank into the Workspace and adds no
        // session. Idempotent, which is what makes a second press safe.
        request.insert(QStringLiteral("sessionId"), plan.sessionId);
        request.insert(QStringLiteral("workspaceId"), plan.workspaceId);
    } else if (!plan.workspaceId.isEmpty()) {
        // One or the other, never both: the host resolves the directory from the
        // Workspace and refuses a request carrying both.
        request.insert(QStringLiteral("workspaceId"), plan.workspaceId);
    } else if (!plan.cwd.isEmpty()) {
        request.insert(QStringLiteral("cwd"), plan.cwd);
    }

    // The reply is what knows the id; the held turn is delivered there.
    m_seatCreateInFlight = true;
    setStatus(plan.kind == SessionPlanKind::Adopt
                  ? QStringLiteral("starting the session…")
                  : QStringLiteral("creating a session…"));
    call(QStringLiteral("session/create"),
         QJsonObject{{QStringLiteral("request"), request}});
}

void DshClient::finishMaterialisation(const QString &sessionId, bool idle)
{
    const HeldPrompt held = m_heldPrompt;
    m_heldPrompt = HeldPrompt{};
    m_seatCreateInFlight = false;
    m_seatFollow = false;

    if (m_sessionId.isEmpty() || sessionId.isEmpty()) {
        // The follow did not take — a socket that went away between the press and
        // the call. The seat is still an unsent new session, so it is kept and a
        // retry needs no new press; the turn goes back to the composer rather than
        // being lost with no row to show for it.
        const QString reason = QStringLiteral("the session could not be opened");
        setStatus(reason);
        emit pendingPromptDropped(held.text, reason);
        return;
    }

    // The seat has done its job. Cleared before the prompt is delivered so a
    // second send during the call is an ordinary one to the new session.
    setPendingSession(false);

    // The session was just made, or is a blank the host already had: there is no
    // turn it could be queued behind, so the first prompt starts one.
    deliverPrompt(held.text, held.mode, idle);
}

void DshClient::createWorkspaceFromPicker()
{
    if (m_state != Ready) {
        emit railNotice(QStringLiteral("Cannot add a workspace: not connected yet."), true);
        setStatus(QStringLiteral("not connected yet"));
        return;
    }
    if (m_workspacePickInFlight)
        return;   // the host's chooser is already open for this control

    // Nothing is announced before the picker answers. The host opens its own
    // modal chooser, which is the feedback — and a cancel (a null answer) must
    // leave no status line, no error row and no notice behind, so the first
    // thing this says is either the path it was given or the create it started.
    m_workspacePickInFlight = true;
    call(QStringLiteral("directoryPicker/pick"));
}

void DshClient::createWorkspaceAt(const QString &path)
{
    if (path.isEmpty())
        return;
    setStatus(QStringLiteral("adding a workspace…"));
    // Only `path` goes on the wire: `WorkspaceCreateRequest` is that one field.
    // The reply's `created` says whether the registry already had it, and the
    // Workspace itself is never appended here — the follow stream's upsert is
    // what puts it on screen, so the rail's list stays the host's registry.
    call(QStringLiteral("workspace/create"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("path"), path},
         }}});
}

void DshClient::renameSession(const QString &title, const QString &sessionId)
{
    // Empty id means the followed session: the rail's app menu no longer names
    // one, and the row menu names its own, so both are served by one signature.
    const QString id = sessionId.isEmpty() ? m_sessionId : sessionId;
    const QString trimmed = title.trimmed();
    if (id.isEmpty() || trimmed.isEmpty())
        return;

    // The host owns the title: it trims, rejects the empty, and answers with what
    // it stored. The stream's `upsert` then carries it back, so nothing here has
    // to guess at the result.
    call(QStringLiteral("session/rename"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), id},
             {QStringLiteral("title"), trimmed},
         }}});
}

void DshClient::archiveSession(const QString &sessionId)
{
    const QString id = sessionId.isEmpty() ? m_sessionId : sessionId;
    if (id.isEmpty())
        return;
    // No `stopActivity`: a running session is refused with `workspace/session-active`
    // rather than quietly killed. Archiving is meant to tidy a list, not to
    // interrupt work, and the refusal is the honest answer. A row menu that
    // offers this on a live child is not pre-filtered for the same reason: the
    // refusal is the answer, and the client shows failed calls.
    call(QStringLiteral("workspace/archiveSession"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), id},
         }}});
}

void DshClient::unarchiveSession(const QString &sessionId)
{
    const QString id = sessionId.isEmpty() ? m_sessionId : sessionId;
    if (id.isEmpty())
        return;
    call(QStringLiteral("workspace/unarchiveSession"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), id},
         }}});
}

void DshClient::pinSession(const QString &sessionId)
{
    const QString id = sessionId.isEmpty() ? m_sessionId : sessionId;
    if (id.isEmpty())
        return;
    call(QStringLiteral("workspace/pinSession"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), id},
         }}});
}

void DshClient::unpinSession(const QString &sessionId)
{
    const QString id = sessionId.isEmpty() ? m_sessionId : sessionId;
    if (id.isEmpty())
        return;
    call(QStringLiteral("workspace/unpinSession"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), id},
         }}});
}

void DshClient::updateQueueItem(const QString &itemId, const QString &kind, const QString &text)
{
    if (m_sessionId.isEmpty() || itemId.isEmpty())
        return;

    QJsonObject action{{QStringLiteral("kind"), kind}};
    if (kind == QLatin1String("edit")) {
        // An edit is a text-only replacement, and the host rejects an empty one.
        if (text.trimmed().isEmpty())
            return;
        action.insert(QStringLiteral("content"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), text}},
        });
    } else if (kind != QLatin1String("remove") && kind != QLatin1String("steer")) {
        return;   // not a QueueAction; better to do nothing than to invent one
    }

    call(QStringLiteral("session/updateQueue"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), m_sessionId},
             {QStringLiteral("itemId"), itemId},
             {QStringLiteral("action"), action},
         }}});
    // Kept until the answer, because the answer is only `{accepted:true}` and the
    // transcript row needs the item, the action and (for an edit) the new text.
    m_queueMutation = QueueMutation{itemId, kind, text};
}

void DshClient::refreshPermissionPresets()
{
    if (m_state != Ready)
        return;
    call(QStringLiteral("permissionPresets/catalog"), QJsonObject{});
}

void DshClient::setPermissionPreset(const QString &value)
{
    const QString preset = value.trimmed();
    if (preset.isEmpty())
        return;

    // A fresh attempt replaces the last failure's sentence. Nothing about the
    // preset shown is changed here: the chip keeps reading the host's projection,
    // and the command's answer plus a refreshed projection are what move it.
    setAccessNotice(QString());
    m_pendingPermissionPreset.clear();

    // The current-session control is the host's own `/permission <preset>`
    // command (the web's popup picker drives the same line), not a
    // `settings/mutate` on `permission.defaultPreset` — that setting is the
    // default for *future* sessions and would leave this one in exactly the
    // state the owner complained about. `runCommand` validates the preset
    // against the catalog before a line is posted.
    runCommand(QStringLiteral("/permission %1").arg(preset));
}

void DshClient::handleCommandReply(const QString &line, const QJsonValue &value)
{
    const QString trimmed = line.trimmed();
    const QString word = trimmed.section(QLatin1Char(' '), 0, 0);
    const QString argument = trimmed.mid(word.length()).trimmed();
    const bool permission = word == QLatin1String("/permission");

    // `commands/execute` answers `null` when the line resolves to no command at
    // all: the host does not error, so this used to be indistinguishable from
    // success and said nothing. It is a refusal for the person who typed it.
    if (value.isNull() || value.isUndefined()) {
        const QString message =
            QStringLiteral("the host does not know %1").arg(word);
        if (permission) {
            setAccessNotice(message);
        } else {
            setStatus(message);
            m_transcript->append(QStringLiteral("error"), message);
        }
        return;
    }

    const QJsonObject result = value.toObject()
                                   .value(QStringLiteral("result")).toObject();
    const QString kind = result.value(QStringLiteral("kind")).toString();
    const QString text = result.value(QStringLiteral("text")).toString();

    if (kind == QLatin1String("error")) {
        const QString message = text.isEmpty()
                                    ? QStringLiteral("%1 failed").arg(word) : text;
        if (permission) {
            setAccessNotice(
                argument.isEmpty()
                    ? message
                    : QStringLiteral("%1 was not applied: %2").arg(argument, message));
        } else {
            // A `/compact` or other command that ran and refused (attachments,
            // a busy turn) is a refusal like any other: say it, do not swallow it.
            setStatus(message);
            m_transcript->append(QStringLiteral("error"), message);
        }
        return;
    }

    if (kind != QLatin1String("success")) {
        const QString message =
            QStringLiteral("%1 answered without a result").arg(word);
        if (permission)
            setAccessNotice(message);
        else {
            setStatus(message);
            m_transcript->append(QStringLiteral("error"), message);
        }
        return;
    }

    if (!permission) {
        // `/compact` succeeded. Its own effect is a transcript event and its own
        // notice arrives through the journal; a success must not be turned into
        // an error here, and it needs no extra word.
        return;
    }

    if (argument.isEmpty()) {
        // The bare `/permission` query: the host's answer is the current preset
        // and the available names. It changes nothing.
        setStatus(text.isEmpty() ? QStringLiteral("permission") : text);
        return;
    }

    // The host accepted the line. Do not paint the new preset here: the chip is
    // the host's projection, so wait for it and confirm the two agree.
    m_pendingPermissionPreset = argument;
    setStatus(text.isEmpty() ? QStringLiteral("switching access…") : text);
    refreshProjections();
}

void DshClient::clearJobs()
{
    if (m_jobs.isEmpty())
        return;
    m_jobs.clear();
    emit jobsChanged();
}

void DshClient::refreshJobs()
{
    openJobStream();
}

void DshClient::recoverJobStream()
{
    if (!m_jobStreamId.isEmpty()) {
        cancelStream(m_jobStreamId);
        m_jobStreamId.clear();
    }
    if (m_jobStreamRecovered || m_state != Ready || m_sessionId.isEmpty()
            || m_socket == nullptr
            || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    // Set before the reopening: a host that refuses again must not be retried in
    // a loop. A frame that arrives resets it (handleJobRows), as does a follow.
    m_jobStreamRecovered = true;
    qWarning() << "job/list closed while followed; reopening the roster once for"
               << m_sessionId;
    openJobStream();
}

void DshClient::openJobStream()
{
    if (!m_jobStreamId.isEmpty()) {
        cancelStream(m_jobStreamId);
        m_jobStreamId.clear();
    }
    if (m_state != Ready || m_sessionId.isEmpty())
        return;

    // The host declares ONE parameter, named `request`:
    //
    //   @deepseek-ai/dsh-api-job-controller/lib/typert.host.js:154-172
    //     method: 'list', mode: 'stream',
    //     parameters: [{ name: 'request', wire: 'request', source: 'json',
    //                    typeSymbol: '#JobListRequest', … }]
    //
    // and JobListRequest is exactly `{ readonly sessionId: SessionId }`
    // (same file, line 253). DshClient.h's own opening note states the
    // rule: argument keys are the host method parameter names. The gateway
    // enforces it in dsh-api-gateway/lib/types/index.js:1128-1153, which derives
    // the accepted keys from `parameter.wire` and refuses a mismatch with
    // `gateway/arguments-invalid`. A flat `{sessionId}` therefore failed at the
    // boundary with "missing request", and no roster frame was ever sent — which
    // is why the chip never appeared. The session goes INSIDE `request`.
    m_jobStreamId = openStream(QStringLiteral("job/list"),
                               QJsonObject{{QStringLiteral("request"), QJsonObject{
                                   {QStringLiteral("sessionId"), m_sessionId},
                               }}});
}

void DshClient::handleJobRows(const QJsonObject &value)
{
    if (value.value(QStringLiteral("type")).toString() != QLatin1String("rows"))
        return;

    // A whole-set frame: replace, never merge. These are the host's JobView
    // fields exactly as declared at typert.host.js:260-262 (JobStatus at :257),
    // so detail/progress and the timestamps are carried, not invented.
    QVariantList jobs;
    for (const QJsonValue &entry : value.value(QStringLiteral("jobs")).toArray()) {
        const QJsonObject job = entry.toObject();
        jobs.append(QVariantMap{
            {QStringLiteral("id"), job.value(QStringLiteral("id")).toString()},
            {QStringLiteral("kind"), job.value(QStringLiteral("kind")).toString()},
            {QStringLiteral("label"), job.value(QStringLiteral("label")).toString()},
            {QStringLiteral("status"), job.value(QStringLiteral("status")).toString()},
            {QStringLiteral("progress"), job.value(QStringLiteral("progress")).toString()},
            {QStringLiteral("detail"), job.value(QStringLiteral("detail")).toString()},
            {QStringLiteral("startedAt"), job.value(QStringLiteral("startedAt")).toDouble()},
            {QStringLiteral("finishedAt"), job.value(QStringLiteral("finishedAt")).toDouble()},
        });
    }
    // A frame proves the stream is alive, so the one automatic reopening it is
    // entitled to becomes available again if the carrier closes it later.
    m_jobStreamRecovered = false;
    if (jobs != m_jobs) {
        m_jobs = jobs;
        emit jobsChanged();
    }
}

void DshClient::clearChangedFiles()
{
    if (m_changedFiles.isEmpty())
        return;
    m_changedFiles.clear();
    emit changedFilesChanged();
}

void DshClient::openChangesStream()
{
    if (!m_changesStreamId.isEmpty()) {
        cancelStream(m_changesStreamId);
        m_changesStreamId.clear();
    }
    clearChangedFiles();
    if (m_state != Ready || m_sessionId.isEmpty())
        return;

    // The watch is fenced by an explicit scope, so the client has to name the
    // workspace root itself. It comes from the session list's `cwd`, which is the
    // same field the host derived the root from when it created the session.
    QString root;
    for (const QJsonValue &entry : m_rawSessions) {
        const QJsonObject item = entry.toObject();
        if (item.value(QStringLiteral("sessionId")).toString() != m_sessionId)
            continue;
        root = item.value(QStringLiteral("cwd")).toString();
        break;
    }
    if (root.isEmpty())
        return;

    m_changesStreamId = openStream(
        QStringLiteral("workspaceFiles/changes"),
        QJsonObject{{QStringLiteral("workspaceFileScope"), QJsonObject{
                         {QStringLiteral("sessionId"), m_sessionId},
                         {QStringLiteral("workspaceRoot"), root},
                     }},
                     {QStringLiteral("path"), QString()}});
}

void DshClient::handleChangeFrame(const QJsonObject &value)
{
    if (value.value(QStringLiteral("kind")).toString() != QLatin1String("change"))
        return;
    const QString path =
        value.value(QStringLiteral("change")).toObject().value(QStringLiteral("absolutePath"))
            .toString();
    if (path.isEmpty() || m_changedFiles.contains(path))
        return;
    m_changedFiles.append(path);
    emit changedFilesChanged();
}

void DshClient::listFileReferences(const QString &query)
{
    if (query.isEmpty() || m_sessionId.isEmpty()) {
        if (!m_fileCandidates.isEmpty()) {
            m_fileCandidates.clear();
            emit fileCandidatesChanged();
        }
        return;
    }

    call(QStringLiteral("fileReferences/list"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("query"), query}});
}

QString DshClient::busyEnter() const
{
    return Prefs::busyEnter();
}

void DshClient::setBusyEnter(const QString &mode)
{
    if (mode == Prefs::busyEnter())
        return;
    Prefs::setBusyEnter(mode);
    emit busyEnterChanged();
}

QStringList DshClient::railExpandedSessions() const
{
    return RailState::expanded();
}

void DshClient::setRailExpandedSessions(const QStringList &entries)
{
    RailState::setExpanded(entries);
}

QStringList DshClient::railCollapsed() const
{
    return RailState::collapsed();
}

void DshClient::setRailCollapsed(const QStringList &ids)
{
    RailState::setCollapsed(ids);
}

void DshClient::goalAction(const QString &action)
{
    static const QStringList verbs{QStringLiteral("pause"), QStringLiteral("resume"),
                                   QStringLiteral("clear"), QStringLiteral("complete")};
    if (!verbs.contains(action))
        return;

    const QString id = m_goal.value(QStringLiteral("id")).toString();
    const int revision = m_goal.value(QStringLiteral("revision")).toInt();
    if (m_sessionId.isEmpty() || id.isEmpty() || revision <= 0) {
        setStatus(QStringLiteral("no goal to act on"));
        return;
    }

    // `agent` is the session id on this API, as it is for commands and the terminal.
    call(QStringLiteral("goals/%1").arg(action),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("ref"), QJsonObject{
                         {QStringLiteral("id"), id},
                         {QStringLiteral("revision"), revision},
                     }}});
    setStatus(QStringLiteral("%1…").arg(action));
}

void DshClient::editGoal(const QString &objective, int maxGoalRounds)
{
    const QString id = m_goal.value(QStringLiteral("id")).toString();
    const int revision = m_goal.value(QStringLiteral("revision")).toInt();
    if (m_sessionId.isEmpty() || id.isEmpty() || revision <= 0) {
        setStatus(QStringLiteral("no goal to act on"));
        return;
    }

    QJsonObject request;
    if (!objective.isEmpty())
        request.insert(QStringLiteral("objective"), objective);
    if (maxGoalRounds > 0)
        request.insert(QStringLiteral("maxGoalRounds"), maxGoalRounds);
    if (request.isEmpty()) {
        setStatus(QStringLiteral("no goal change to save"));
        return;
    }

    call(QStringLiteral("goals/edit"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("ref"), QJsonObject{
                         {QStringLiteral("id"), id},
                         {QStringLiteral("revision"), revision},
                     }},
                     {QStringLiteral("request"), request}});
    setStatus(QStringLiteral("editing goal…"));
}

void DshClient::forkSession(const QString &sessionId)
{
    const QString from = sessionId.isEmpty() ? m_sessionId : sessionId;
    if (from.isEmpty())
        return;
    call(QStringLiteral("session/fork"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), from},
         }}});
    setStatus(QStringLiteral("forking…"));
}

void DshClient::killJob(const QString &jobId)
{
    if (jobId.isEmpty())
        return;
    // The kill half of the same descriptor (typert.host.js:130-153): its one
    // parameter is also `request`, and JobKillRequest is
    //   { readonly sessionId: SessionId; readonly jobId: JobId }
    // (line 241). Both fields are required, so the shape this used to send
    // was refused twice over: no `sessionId` inside the envelope, where the wire
    // type requires one. The roster marks the change; this call does not guess.
    call(QStringLiteral("job/kill"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), m_sessionId},
             {QStringLiteral("jobId"), jobId},
         }}});
    setStatus(QStringLiteral("stopping %1…").arg(jobId));
}

void DshClient::clearPendingAttachments()
{
    if (m_pendingAttachments.isEmpty())
        return;
    m_pendingAttachments.clear();
    emit pendingAttachmentsChanged();
}

void DshClient::uploadAttachment(const QUrl &fileUrl)
{
    if (m_sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session to attach to"));
        return;
    }

    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("could not read %1").arg(QFileInfo(path).fileName()));
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.isEmpty()) {
        setStatus(QStringLiteral("%1 is empty").arg(QFileInfo(path).fileName()));
        return;
    }

    uploadBytes(QFileInfo(path).fileName(), bytes);
}

void DshClient::pasteAttachment()
{
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) {
        setStatus(QStringLiteral("no image on the clipboard"));
        return;
    }

    // Encoded to bytes in memory rather than written to a file first: the prompt
    // carries the bytes inline, and a temp file would be one more thing to clean
    // up and one more path that can be wrong.
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    if (encoded.isEmpty()) {
        setStatus(QStringLiteral("could not encode the clipboard image"));
        return;
    }
    uploadBytes(QStringLiteral("pasted.png"), encoded);
}

void DshClient::uploadBytes(const QString &name, const QByteArray &bytes)
{
    if (m_sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session to attach to"));
        return;
    }

    // An image never takes the upload route. `PromptContentPart` declares two
    // attachment arms, and the image one carries the bytes inline as canonical
    // base64 — exactly what the web composer does. Uploading it and citing a
    // receipt would send the *file* arm instead, and the model would get a name it
    // has to open rather than a picture it can see.
    const QString mediaType = promptImageMediaType(bytes);
    if (!mediaType.isEmpty()) {
        m_pendingAttachments.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("kind"), QStringLiteral("image")},
            {QStringLiteral("mediaType"), mediaType},
            {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
        });
        emit pendingAttachmentsChanged();
        setStatus(QStringLiteral("%1 attached").arg(name));
        return;
    }

    // The host takes the *dimensions* of the upload from query parameters and the
    // bytes from the body; there is no JSON envelope on this route.
    QNetworkRequest request = makeRequest(QStringLiteral("/api/session/uploadFileBinary"));
    QUrl url = request.url();
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("sessionId"), m_sessionId);
    query.addQueryItem(QStringLiteral("name"), name);
    url.setQuery(query);
    request.setUrl(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));

    // The Agent scope the receipt will be minted in, captured before the reply: a
    // reader who switches sessions while the upload is in flight must not have the
    // answer filed under the new one, where the receipt could never be spent.
    const QString stagedIn = m_sessionId;

    setStatus(QStringLiteral("uploading %1…").arg(name));
    QNetworkReply *reply = m_network->post(request, bytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply, name, stagedIn]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            // A failure the user caused is worth a sentence, not a silent nothing.
            setStatus(QStringLiteral("upload failed: %1").arg(reply->errorString()));
            return;
        }

        const QJsonObject result =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject value = result.value(QStringLiteral("value")).toObject();
        const QJsonObject ref = value.value(QStringLiteral("file")).toObject();
        const QString receipt = value.value(QStringLiteral("receiptId")).toString();
        if (!result.value(QStringLiteral("ok")).toBool() || ref.isEmpty() || receipt.isEmpty()) {
            // `FileUploadValue` is `{receiptId, file}` and BOTH are load-bearing:
            // the prompt's file arm is a receipt id, so a stored file without one
            // cannot be sent at all. Say that here rather than stage an entry the
            // host would refuse as `gateway/input-invalid`.
            const QString detail = result.value(QStringLiteral("error"))
                                       .toObject()
                                       .value(QStringLiteral("message"))
                                       .toString();
            if (detail.isEmpty()) {
                setStatus(QStringLiteral("upload answered without a usable receipt: %1 was not staged")
                              .arg(name));
            } else {
                setStatus(QStringLiteral("upload rejected: %1").arg(detail));
            }
            return;
        }

        m_pendingAttachments.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("kind"), QStringLiteral("file")},
            {QStringLiteral("session"), stagedIn},
            {QStringLiteral("ref"), ref.toVariantMap()},
            {QStringLiteral("receipt"), receipt},
        });
        emit pendingAttachmentsChanged();
        setStatus(QStringLiteral("%1 attached").arg(name));
    });
}

void DshClient::setSetting(const QString &ns, const QString &key,
                          const QVariant &value, int revision)
{
    if (ns.isEmpty() || key.isEmpty())
        return;

    // The key is a dotted path from the flattened listing, so a nested write is
    // the same operation as a top-level one with a longer array. A key that
    // genuinely contains a dot would be misread as nesting; none of the shipped
    // namespaces do, and the failure is a refused write rather than a wrong one.
    QJsonArray path;
    for (const QString &segment : key.split(QLatin1Char('.')))
        path.append(segment);

    call(QStringLiteral("settings/mutate"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("ns"), ns},
             {QStringLiteral("ops"), QJsonArray{QJsonObject{
                 {QStringLiteral("op"), QStringLiteral("set")},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("value"), QJsonValue::fromVariant(value)},
             }}},
             {QStringLiteral("expectedRevision"), revision},
         }}});

    setStatus(QStringLiteral("saving %1…").arg(key));
}

void DshClient::refreshSettings()
{
    if (m_state != Ready)
        return;
    call(QStringLiteral("settings/describe"), QJsonObject{});
}

// --- Automation tasks (schedule/*) ------------------------------------------
//
// The wire surface is exactly five unary methods — `schedule/catalog`, `list`,
// `history`, `delete` and `update` (see the installed package's
// `dsh-schedule/lib/typert.remote-client.d.ts`). There is no create, no
// enable/disable and no run-now on it: creation lives in the model tool
// `schedule_create`, and an inactive task is one that has already fired or whose
// session was archived. The page therefore lists, edits name/instruction,
// deletes and reads delivery history, and does not invent the missing verbs.

namespace {

/// `09:00:00.000` from a daily/weekly record as the clock a person reads.
QString scheduleClock(const QString &stored)
{
    const QString text = stored.trimmed();
    return text.size() > 5 ? text.left(5) : text;
}

/// A duration in the unit a person would use for it.
QString scheduleSpan(qint64 seconds)
{
    if (seconds <= 0)
        return QStringLiteral("0 seconds");
    const auto unit = [](qint64 count, const QString &name) {
        return QStringLiteral("%1 %2%3").arg(QString::number(count), name,
                                             count == 1 ? QString() : QStringLiteral("s"));
    };
    if (seconds % 86400 == 0)
        return unit(seconds / 86400, QStringLiteral("day"));
    if (seconds % 3600 == 0)
        return unit(seconds / 3600, QStringLiteral("hour"));
    if (seconds % 60 == 0)
        return unit(seconds / 60, QStringLiteral("minute"));
    return unit(seconds, QStringLiteral("second"));
}

/// ISO weekdays in display order, Monday first.
const char *const kWeekdayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

/// What the rule does, in one line, in the web page's own vocabulary.
QString scheduleRuleText(const QJsonObject &record)
{
    const QString kind = record.value(QStringLiteral("kind")).toString();
    const QString zone = record.value(QStringLiteral("timeZone")).toString();
    const QString suffix = zone.isEmpty() ? QString()
                                          : QStringLiteral(" (%1)").arg(zone);
    if (kind == QLatin1String("after")) {
        return QStringLiteral("Once, %1 after creation")
            .arg(scheduleSpan(qint64(record.value(QStringLiteral("afterSeconds")).toDouble())));
    }
    if (kind == QLatin1String("at"))
        return QStringLiteral("Once");
    if (kind == QLatin1String("every")) {
        return QStringLiteral("Every %1")
            .arg(scheduleSpan(qint64(record.value(QStringLiteral("everySeconds")).toDouble())));
    }
    if (kind == QLatin1String("daily")) {
        return QStringLiteral("Every day at %1%2")
            .arg(scheduleClock(record.value(QStringLiteral("time")).toString()), suffix);
    }
    if (kind == QLatin1String("weekly")) {
        QStringList days;
        const QJsonArray weekdays = record.value(QStringLiteral("weekdays")).toArray();
        for (const QJsonValue &day : weekdays) {
            const int index = day.toInt() - 1;
            if (index >= 0 && index < 7)
                days.append(QString::fromLatin1(kWeekdayNames[index]));
        }
        return QStringLiteral("Weekly on %1 at %2%3")
            .arg(days.join(QStringLiteral(", ")),
                 scheduleClock(record.value(QStringLiteral("time")).toString()), suffix);
    }
    if (kind == QLatin1String("cron")) {
        return QStringLiteral("Cron %1%2")
            .arg(record.value(QStringLiteral("expression")).toString(), suffix);
    }
    return kind.isEmpty() ? QStringLiteral("Reminder") : kind;
}

/// Local display text from the manifest's `LocalizedText`: a plain string, or a
/// table whose English entry wins and whose first string is the fallback.
QString localisedText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (!value.isObject())
        return QString();
    const QJsonObject text = value.toObject();
    const QString english = text.value(QStringLiteral("en")).toString();
    if (!english.isEmpty())
        return english;
    for (auto it = text.constBegin(); it != text.constEnd(); ++it) {
        if (it.value().isString() && !it.value().toString().isEmpty())
            return it.value().toString();
    }
    return QString();
}

/// The sentence a bundle's read-only reason becomes. An unknown reason still
/// says it cannot be switched, without inventing a cause.
QString bundleReadOnlyText(const QString &reason)
{
    if (reason == QLatin1String("management-required")) {
        return QStringLiteral("This plugin bundle is managed by the deployment's "
                              "configuration and cannot be switched here.");
    }
    if (reason == QLatin1String("unaddressable")) {
        return QStringLiteral("The host cannot address this plugin bundle from here, "
                              "so it cannot be switched.");
    }
    return QStringLiteral("This plugin bundle cannot be switched from here.");
}

/// One bundle row: the facts the Plugins page draws and the state of its switch.
QVariantMap bundleRow(const QJsonObject &bundle, const QString &changing)
{
    const QString name = bundle.value(QStringLiteral("name")).toString().trimmed();
    const QJsonObject meta = bundle.value(QStringLiteral("meta")).toObject();
    const QString title = localisedText(meta.value(QStringLiteral("title"))).trimmed();
    QString description = localisedText(meta.value(QStringLiteral("description"))).trimmed();
    if (description.isEmpty())
        description = bundle.value(QStringLiteral("description")).toString().trimmed();

    const QJsonObject error = bundle.value(QStringLiteral("error")).toObject();
    QString errorText = localisedText(error.value(QStringLiteral("diagnostic"))).trimmed();
    if (errorText.isEmpty())
        errorText = localisedText(meta.value(QStringLiteral("error"))).trimmed();
    const QString errorCode = error.value(QStringLiteral("code")).toString().trimmed();
    if (errorText.isEmpty())
        errorText = errorCode;

    const QString readOnlyReason =
        bundle.value(QStringLiteral("readOnlyReason")).toString().trimmed();
    const bool enabled = bundle.value(QStringLiteral("enabled")).toBool();
    const bool hasError = bundle.contains(QStringLiteral("error"));
    const bool busy = !changing.isEmpty();

    QString status = QStringLiteral("Disabled");
    if (hasError)
        status = QStringLiteral("Problem");
    else if (enabled)
        status = QStringLiteral("Enabled");

    return QVariantMap{
        {QStringLiteral("name"), name},
        {QStringLiteral("title"), title.isEmpty() ? name : title},
        {QStringLiteral("version"), bundle.value(QStringLiteral("version")).toString()},
        {QStringLiteral("description"), description},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("installed"), bundle.value(QStringLiteral("installed")).toBool()},
        {QStringLiteral("optional"), bundle.value(QStringLiteral("optional")).toBool()},
        {QStringLiteral("removable"), bundle.value(QStringLiteral("removable")).toBool()},
        {QStringLiteral("status"), status},
        {QStringLiteral("hasError"), hasError},
        {QStringLiteral("errorSummary"), errorText},
        {QStringLiteral("readOnly"), !readOnlyReason.isEmpty()},
        {QStringLiteral("readOnlyText"),
         readOnlyReason.isEmpty() ? QString() : bundleReadOnlyText(readOnlyReason)},
        {QStringLiteral("changing"), busy && changing == name},
        {QStringLiteral("switchable"), readOnlyReason.isEmpty() && !busy && !name.isEmpty()},
    };
}

} // namespace

void DshClient::refreshScheduleTasks()
{
    if (m_state != Ready) {
        m_scheduleLoading = false;
        setScheduleNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    if (m_scheduleLoading)
        return;
    m_scheduleLoading = true;
    emit scheduleChanged();
    // `schedule/catalog` declares no parameters, so the descriptor requires an
    // empty args object rather than an omitted one.
    call(QStringLiteral("schedule/catalog"), QJsonObject{});
}

void DshClient::applyScheduleCatalog(const QJsonArray &items)
{
    // Two lists rather than the host's single order: the catalog is sorted by
    // `scheduledAt`, so a one-shot that fired days ago (now inactive) would
    // head the page and the live tasks would start below the fold. Active
    // first, each list keeping the host's order.
    QVariantList activeRows;
    QVariantList inactiveRows;
    QHash<QString, QJsonObject> records;
    QHash<QString, QString> sessions;
    // The rail's mark, folded here rather than in the row builder because this is
    // the one answer that names every Session's tasks. Only `active` counts: a
    // task the host has ended will never fire again and is no reason to say a
    // Session is on a timer. A row naming no Session marks nothing.
    QHash<QString, int> taskCounts;
    const QDateTime now = QDateTime::currentDateTimeUtc();

    for (const QJsonValue &entry : items) {
        const QJsonObject record = entry.toObject();
        const QString id = record.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;

        // The expected record is the ScheduleRecord alone: `sessionId`,
        // `status` and `lastDelivery` are catalog decorations, and the strict
        // decoder rejects an object that carries them.
        QJsonObject raw{
            {QStringLiteral("id"), id},
            {QStringLiteral("kind"), record.value(QStringLiteral("kind"))},
            {QStringLiteral("title"), record.value(QStringLiteral("title"))},
            {QStringLiteral("prompt"), record.value(QStringLiteral("prompt"))},
            {QStringLiteral("scheduledAt"), record.value(QStringLiteral("scheduledAt"))},
        };
        const QString kind = raw.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("after")) {
            raw.insert(QStringLiteral("afterSeconds"),
                       record.value(QStringLiteral("afterSeconds")));
        } else if (kind == QLatin1String("every")) {
            raw.insert(QStringLiteral("everySeconds"),
                       record.value(QStringLiteral("everySeconds")));
        } else if (kind == QLatin1String("daily") || kind == QLatin1String("weekly")) {
            raw.insert(QStringLiteral("time"), record.value(QStringLiteral("time")));
            raw.insert(QStringLiteral("timeZone"), record.value(QStringLiteral("timeZone")));
            if (kind == QLatin1String("weekly"))
                raw.insert(QStringLiteral("weekdays"), record.value(QStringLiteral("weekdays")));
        } else if (kind == QLatin1String("cron")) {
            raw.insert(QStringLiteral("expression"), record.value(QStringLiteral("expression")));
            raw.insert(QStringLiteral("timeZone"), record.value(QStringLiteral("timeZone")));
        }
        records.insert(id, raw);
        const QString sessionId = record.value(QStringLiteral("sessionId")).toString();
        sessions.insert(id, sessionId);

        const QString status = record.value(QStringLiteral("status")).toString();
        const QString scheduledAt = record.value(QStringLiteral("scheduledAt")).toString();
        const QDateTime target = QDateTime::fromString(scheduledAt, Qt::ISODate);
        const QString sessionTitle =
            sessionListItem(sessionId).value(QStringLiteral("title")).toString();

        const QVariantMap row{
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), record.value(QStringLiteral("title")).toString()},
            {QStringLiteral("prompt"), record.value(QStringLiteral("prompt")).toString()},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("rule"), scheduleRuleText(record)},
            {QStringLiteral("scheduledAt"), scheduledAt},
            {QStringLiteral("nextRun"), scheduleInstant(scheduledAt)},
            {QStringLiteral("due"), status == QLatin1String("active") && target.isValid()
                                        && target <= now},
            {QStringLiteral("status"), status},
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("sessionTitle"), sessionTitle},
            {QStringLiteral("lastDeliveredAt"),
             scheduleInstant(record.value(QStringLiteral("lastDelivery"))
                                 .toObject()
                                 .value(QStringLiteral("deliveredAt"))
                                 .toString())},
        };
        if (status == QLatin1String("active")) {
            activeRows.append(row);
            if (!sessionId.isEmpty())
                ++taskCounts[sessionId];
        } else {
            inactiveRows.append(row);
        }
    }

    QVariantList rows = activeRows;
    rows.append(inactiveRows);

    m_scheduleRecords = records;
    m_scheduleSessions = sessions;
    m_scheduleTasks = rows;
    m_scheduleLoading = false;
    m_scheduleLoaded = true;

    // The roster's own copy of the count, replaced only by a successful read: a
    // failed one never reaches here, so a transient host hiccup cannot read as
    // "every task vanished" and blank every mark. A rebuild only when a count
    // actually moved — the page's own binds do not need the roster re-emitted.
    if (taskCounts != m_scheduleCounts) {
        m_scheduleCounts = taskCounts;
        rebuildSessionRows();
    }

    // A deleted task takes its delivery records with it, so a page still showing
    // them would be showing records the host no longer has.
    if (!m_scheduleHistoryFor.isEmpty() && !records.contains(m_scheduleHistoryFor)) {
        m_scheduleHistoryFor.clear();
        m_scheduleHistory.clear();
        m_scheduleHistoryLoading = false;
        emit scheduleHistoryChanged();
    }

    emit scheduleChanged();
}

QJsonObject DshClient::scheduleExpectedRecord(const QString &id) const
{
    return m_scheduleRecords.value(id);
}

void DshClient::setScheduleNotice(const QString &notice, bool isError)
{
    if (m_scheduleNotice == notice && m_scheduleNoticeError == isError)
        return;
    m_scheduleNotice = notice;
    m_scheduleNoticeError = isError;
    emit scheduleChanged();
}

void DshClient::saveScheduleTask(const QString &id, const QString &title,
                                 const QString &prompt)
{
    if (m_state != Ready) {
        setScheduleNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    const QJsonObject expected = scheduleExpectedRecord(id);
    if (expected.isEmpty()) {
        setScheduleNotice(QStringLiteral("That task is no longer in the catalog."), true);
        refreshScheduleTasks();
        return;
    }

    const QString newTitle = title.trimmed();
    if (newTitle.isEmpty()) {
        setScheduleNotice(QStringLiteral("A task name is required."), true);
        return;
    }
    const QString newPrompt = prompt.trimmed();
    const QJsonObject request{
        {QStringLiteral("sessionId"), m_scheduleSessions.value(id)},
        {QStringLiteral("id"), id},
        {QStringLiteral("expected"), expected},
    };
    QJsonObject withContent = request;
    if (newTitle != expected.value(QStringLiteral("title")).toString())
        withContent.insert(QStringLiteral("title"), newTitle);
    if (!newPrompt.isEmpty()
        && newPrompt != expected.value(QStringLiteral("prompt")).toString()) {
        withContent.insert(QStringLiteral("prompt"), newPrompt);
    }
    if (!withContent.contains(QStringLiteral("title"))
        && !withContent.contains(QStringLiteral("prompt"))) {
        setScheduleNotice(QStringLiteral("Nothing changed."), false);
        return;
    }

    m_scheduleSaving = true;
    emit scheduleChanged();
    call(QStringLiteral("schedule/update"),
         QJsonObject{{QStringLiteral("request"), withContent}});
}

void DshClient::deleteScheduleTask(const QString &id)
{
    if (m_state != Ready) {
        setScheduleNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    const QString sessionId = m_scheduleSessions.value(id);
    if (sessionId.isEmpty()) {
        setScheduleNotice(QStringLiteral("That task is no longer in the catalog."), true);
        refreshScheduleTasks();
        return;
    }
    m_scheduleSaving = true;
    emit scheduleChanged();
    call(QStringLiteral("schedule/delete"),
         QJsonObject{{QStringLiteral("request"),
                      QJsonObject{{QStringLiteral("sessionId"), sessionId},
                                  {QStringLiteral("id"), id}}}});
}

void DshClient::loadScheduleHistory(const QString &id)
{
    if (m_state != Ready) {
        setScheduleNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    const QString sessionId = m_scheduleSessions.value(id);
    if (sessionId.isEmpty()) {
        setScheduleNotice(QStringLiteral("That task is no longer in the catalog."), true);
        refreshScheduleTasks();
        return;
    }
    m_scheduleHistoryFor = id;
    m_scheduleHistory.clear();
    m_scheduleHistoryLoading = true;
    emit scheduleHistoryChanged();
    // `limit` is required and bounded 1..100; 50 is one page the page can show
    // without a cursor, which this client does not drive.
    call(QStringLiteral("schedule/history"),
         QJsonObject{{QStringLiteral("request"),
                      QJsonObject{{QStringLiteral("sessionId"), sessionId},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("limit"), 50}}}});
}

// --- Plugins (pluginManager/*) ---------------------------------------------

void DshClient::refreshPluginInventory()
{
    if (m_state != Ready) {
        m_pluginsLoading = false;
        setPluginsNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    if (m_pluginsLoading)
        return;
    m_pluginsLoading = true;
    emit pluginsChanged();
    call(QStringLiteral("pluginManager/listBundles"), QJsonObject{});
}

void DshClient::setBundleEnabled(const QString &name, bool enabled)
{
    const QString bundle = name.trimmed();
    if (bundle.isEmpty())
        return;
    if (m_state != Ready) {
        setPluginsNotice(QStringLiteral("Not connected to the host."), true);
        return;
    }
    if (!m_pluginChanging.isEmpty())
        return;
    m_pluginChanging = bundle;
    rebuildPluginSections();
    emit pluginsChanged();
    call(QStringLiteral("pluginManager/setBundleEnabled"),
         QJsonObject{{QStringLiteral("name"), bundle},
                     {QStringLiteral("enabled"), enabled}});
}

void DshClient::setPluginsNotice(const QString &notice, bool isError)
{
    if (m_pluginsNotice == notice && m_pluginsNoticeError == isError)
        return;
    m_pluginsNotice = notice;
    m_pluginsNoticeError = isError;
    emit pluginsChanged();
}

void DshClient::applyPluginBundles(const QJsonArray &bundles)
{
    m_pluginBundles = bundles;
    m_pluginsLoading = false;
    m_pluginsLoaded = true;
    m_pluginChanging.clear();
    rebuildPluginSections();
    emit pluginsChanged();
}

void DshClient::rebuildPluginSections()
{
    QVariantList officialRows;
    QVariantList installedRows;
    for (const QJsonValue &entry : m_pluginBundles) {
        const QJsonObject bundle = entry.toObject();
        const QString name = bundle.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        const bool installed = bundle.value(QStringLiteral("installed")).toBool();
        const bool optional = bundle.value(QStringLiteral("optional")).toBool();
        const bool hasError = bundle.contains(QStringLiteral("error"));
        if (!installed && !optional && !hasError)
            continue;
        if (optional && !installed)
            officialRows.append(bundleRow(bundle, m_pluginChanging));
        else
            installedRows.append(bundleRow(bundle, m_pluginChanging));
    }

    QVariantList sections;
    if (!officialRows.isEmpty()) {
        sections.append(QVariantMap{
            {QStringLiteral("title"),
             QStringLiteral("Official (%1)").arg(officialRows.size())},
            {QStringLiteral("note"),
             QStringLiteral("Bundles this installation ships that the profile has not selected.")},
            {QStringLiteral("rows"), officialRows},
        });
    }
    if (!installedRows.isEmpty()) {
        sections.append(QVariantMap{
            {QStringLiteral("title"),
             QStringLiteral("Installed (%1)").arg(installedRows.size())},
            {QStringLiteral("note"),
             QStringLiteral("The bundles this profile selects and loads.")},
            {QStringLiteral("rows"), installedRows},
        });
    }
    m_pluginInventory = sections;
}

void DshClient::applyBundleChange(const QJsonValue &value)
{
    const QJsonObject answer = value.toObject();
    const QString target = answer.value(QStringLiteral("target")).toString().trimmed();
    const QString name = target.isEmpty() ? m_pluginChanging : target;
    const QString application = answer.value(QStringLiteral("application")).toString();
    const QJsonObject error = answer.value(QStringLiteral("error")).toObject();
    QString failure = localisedText(error.value(QStringLiteral("diagnostic"))).trimmed();
    if (failure.isEmpty())
        failure = error.value(QStringLiteral("code")).toString().trimmed();
    const bool enabled = answer.value(QStringLiteral("enabled")).toBool();

    m_pluginChanging.clear();
    rebuildPluginSections();

    if (application == QLatin1String("failed")) {
        setPluginsNotice(failure.isEmpty()
                             ? QStringLiteral("The host could not change this bundle.")
                             : QStringLiteral("%1 was not changed: %2").arg(name, failure),
                         true);
    } else if (application == QLatin1String("cancelled")) {
        setPluginsNotice(failure.isEmpty()
                             ? QStringLiteral("The host cancelled the change.")
                             : QStringLiteral("%1 was not changed: %2").arg(name, failure),
                         true);
    } else if (application == QLatin1String("restart-required")) {
        setPluginsNotice(QStringLiteral("%1 will be %2 when the host next starts.")
                             .arg(name, enabled ? QStringLiteral("enabled")
                                                : QStringLiteral("disabled")),
                         false);
    } else if (application == QLatin1String("overridden")) {
        setPluginsNotice(QStringLiteral("%1 was saved, but a higher-priority layer decides "
                                        "whether it runs.").arg(name),
                         false);
    } else {
        setPluginsNotice(QString(), false);
    }
    emit pluginsChanged();
    if (application != QLatin1String("failed") && application != QLatin1String("cancelled"))
        refreshPluginInventory();
}

QString DshClient::attachmentUrl(const QString &id) const
{
    return m_attachmentUrls.value(id);
}

void DshClient::loadAttachment(const QString &id)
{
    if (id.isEmpty() || m_attachmentUrls.contains(id) || m_sessionId.isEmpty())
        return;

    // A placeholder stops a recycled delegate asking again before the answer
    // lands; the value is replaced when the bytes arrive.
    m_attachmentUrls.insert(id, QString());
    call(QStringLiteral("session/attachment"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("sessionId"), m_sessionId},
             {QStringLiteral("attachmentId"), id},
         }}});
}

QStringList DshClient::slashCommands() const
{
    return SlashCommands::names();
}

void DshClient::runCommand(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;
    if (m_sessionId.isEmpty()) {
        if (!m_pendingSeat.active)
            return;
        // A seat has no session for `commands/execute` to run in, and there is no
        // transcript to put a row in (the seat's is empty by design). Say it in
        // the strip: a command that silently did nothing is the failure this
        // client has already been accused of. The line stays in the composer, so
        // nothing the reader typed is spent on the refusal.
        setStatus(QStringLiteral("%1 needs a session — send a message to start one")
                      .arg(trimmed.section(QLatin1Char(' '), 0, 0)));
        return;
    }

    // A command the client does not know is refused here, visibly, rather than
    // posted. The host would refuse it too, but that answer costs a round trip
    // and arrives after the composer has already taken the line out of the
    // draft; saying it at once, with the names that do work, is the useful
    // answer. This is also why the check reads SlashCommands rather than
    // whatever the host happens to accept: this client's vocabulary is the set
    // it can drive.
    if (!SlashCommands::isKnown(trimmed)) {
        const QString message =
            QStringLiteral("%1 is not a command here — try %2")
                .arg(trimmed.section(QLatin1Char(' '), 0, 0),
                     SlashCommands::names().join(QStringLiteral(", ")));
        setStatus(message);
        m_transcript->append(QStringLiteral("error"), message);
        return;
    }

    // `agent` is the session id on this API — the terminal namespace already
    // passes m_sessionId as agentId, and it is the same identity here.
    //
    // `submittedAttachments` is part of the descriptor's strict argument list even
    // though a typed command has none, and the host rejects the call outright
    // without it: "args field does not match the descriptor: missing
    // submittedAttachments". Empty is the documented value for a plain
    // invocation, so an empty array is what a command carries; the client's
    // staged attachments belong to a prompt, and a `/command` is not one.
    //
    // A `/permission` line is validated first: the catalog is the host's own list
    // of switch targets, so a word it does not advertise is refused here with
    // that list rather than spent on a round trip. When the catalog has not
    // arrived yet the word is still posted, because the host is the authority on
    // what it accepts; its answer is now surfaced rather than dropped.
    if (trimmed.section(QLatin1Char(' '), 0, 0) == QLatin1String("/permission")) {
        const QString word = trimmed.section(QLatin1Char(' '), 0, 0);
        const QString preset = trimmed.mid(word.length()).trimmed();
        if (!preset.isEmpty() && !m_permissionPresets.isEmpty()) {
            QStringList values;
            bool known = false;
            for (const QVariant &entry : m_permissionPresets) {
                const QVariantMap option = entry.toMap();
                const QString optionValue = option.value(QStringLiteral("value")).toString();
                if (optionValue.isEmpty())
                    continue;
                values.append(optionValue);
                if (optionValue == preset)
                    known = true;
            }
            if (!known) {
                setAccessNotice(QStringLiteral("no access preset called %1 — try %2")
                                    .arg(preset, values.join(QStringLiteral(", "))));
                return;
            }
        }
        setAccessNotice(QString());
        m_pendingPermissionPreset.clear();
    }

    call(QStringLiteral("commands/execute"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("line"), trimmed},
                     {QStringLiteral("submittedAttachments"), QJsonArray()}},
         QString(), trimmed);
}

void DshClient::searchSessions(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed == m_searchQuery)
        return;

    m_searchQuery = trimmed;

    // One character matches nearly every session, and a host with no index would
    // keep refusing: both stop here rather than spending a round trip to say so.
    if (trimmed.length() < 2 || m_searchUnavailable) {
        m_searchLoading = false;
        m_searchHasMore = false;
        m_searchResults.clear();
        emit searchChanged();
        return;
    }

    m_searchLoading = true;
    emit searchChanged();
    call(QStringLiteral("session/search"),
         QJsonObject{{QStringLiteral("request"), QJsonObject{
             {QStringLiteral("query"), trimmed},
         }}});
}

void DshClient::respondToPermission(const QString &requestId, bool allow)
{
    // The UI speaks one boolean for both transports; the host's approval waterfall
    // wants a vocabulary word. Mapping here keeps the QML transport-agnostic: the
    // same dialog drives ACP's allow/reject and this transport's
    // allowed-once/rejected without needing to know which is in play.
    const QString value = allow ? QStringLiteral("allowed-once") : QStringLiteral("rejected");

    const auto pending = m_pendingWaterfalls.constFind(requestId);
    if (pending == m_pendingWaterfalls.constEnd()) {
        // Answering an event the host never raised, or already withdrew, is an
        // error on the wire — and would report success for something that did not
        // happen. Say so rather than pretend. The prompt, if it is still up, is
        // closed; no transcript row is written, because the row belongs to a
        // session this client can no longer name.
        if (m_offeredApprovalId == requestId) {
            m_offeredApprovalId.clear();
            emit permissionWithdrawn(requestId);
            QTimer::singleShot(0, this, &DshClient::offerPendingApproval);
        }
        setStatus(QStringLiteral("that approval is no longer pending"));
        qWarning() << "no pending waterfall for" << requestId;
        return;
    }

    if (pending.value().event != QLatin1String("approval/request")) {
        // The vocabulary above is the approval one. Answering a question with it
        // would answer the wrong waterfall in the wrong dialect.
        setStatus(QStringLiteral("that waterfall is not an approval"));
        qWarning() << "respondToPermission called for non-approval waterfall" << requestId
                   << pending.value().event;
        return;
    }

    // The answer row belongs to the session that asked. An approval must only be
    // answerable while its own session is followed, so a dialog left over from a
    // session the reader moved away from can never write "approval granted" into
    // the transcript of the one they moved to. Leaving it pending rather than
    // answering also keeps the decision with the transcript that can show it.
    if (!pending.value().agentId.isEmpty() && pending.value().agentId != m_sessionId) {
        // It should never be offered here (offerPendingApproval() filters), but a
        // prompt must not outlive its right to be answered either way.
        if (m_offeredApprovalId == requestId) {
            m_offeredApprovalId.clear();
            emit permissionWithdrawn(requestId);
            QTimer::singleShot(0, this, &DshClient::offerPendingApproval);
        }
        setStatus(QStringLiteral("that approval belongs to another session"));
        qWarning() << "refusing to answer approval" << requestId << "raised by session"
                   << pending.value().agentId << "while following" << m_sessionId;
        return;
    }

    m_transcript->append(QStringLiteral("notice"),
                         allow ? QStringLiteral("approval granted")
                               : QStringLiteral("approval rejected"));
    // The prompt is closed and the next retained approval for this session is
    // offered by postWaterfallOutcome(), which is the one place every answer
    // passes through.
    answerWaterfall(requestId, value);
}

// --- terminal ----------------------------------------------------------------

void DshClient::openTerminal(int cols, int rows)
{
    if (m_state != Ready || m_sessionId.isEmpty()) {
        setStatus(QStringLiteral("no session to open a terminal in"));
        return;
    }

    // Re-attaching to an open terminal is the same call: the id is ours, so
    // create adopts the shell the host still holds instead of starting another.
    if (m_terminalId.isEmpty()) {
        // Caller-minted, and required. Omitting it is rejected as an opaque
        // boundary-validation error that does not name the field.
        //
        // The id is remembered per session before a new one is minted, so a
        // relaunched client reattaches to the shell the host retained instead of
        // abandoning it. There is no `terminal/list` to discover one, so if the id
        // is not written down here the PTY is unreachable for good.
        QSettings prefs;
        const QString stored =
            prefs.value(QStringLiteral("terminal/%1").arg(m_sessionId)).toString();
        if (!stored.isEmpty()) {
            m_terminalId = stored;
            qInfo() << "reattaching to retained terminal" << m_terminalId;
        } else {
            m_terminalId = QStringLiteral("term-%1")
                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));
            prefs.setValue(QStringLiteral("terminal/%1").arg(m_sessionId), m_terminalId);
        }
    }
    m_terminalCols = qMax(1, cols);
    m_terminalRows = qMax(1, rows);
    if (m_attachmentId.isEmpty())
        m_attachmentId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);

    call(QStringLiteral("terminal/create"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("request"), QJsonObject{
                        {QStringLiteral("id"), m_terminalId},
                        {QStringLiteral("cols"), m_terminalCols},
                        {QStringLiteral("rows"), m_terminalRows},
                     }}});

    // Ask the host to keep it alive when this client goes away, which is what
    // makes the remembered id worth anything on the next launch.
    call(QStringLiteral("terminal/retain"),
         QJsonObject{{QStringLiteral("sessionId"), m_sessionId},
                     {QStringLiteral("id"), m_terminalId}});
}

void DshClient::attachTerminal()
{
    // Open the follow stream and keep it: writing later only works while this
    // attachment is live and being serviced.
    if (!m_terminalStreamId.isEmpty())
        cancelStream(m_terminalStreamId);

    // The descriptor is EXACT: adding cols/rows is rejected with
    // gateway/arguments-invalid "unexpected "cols", "rows"". The grid size is
    // set by terminal/create and terminal/resize, not by the attach.
    m_terminalStreamId = openStream(
        QStringLiteral("terminal/follow"),
        QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                    {QStringLiteral("id"), m_terminalId},
                    {QStringLiteral("attachmentId"), m_attachmentId}});
    qInfo() << "terminal follow opened" << m_terminalStreamId
            << "attachment" << m_attachmentId;
}

void DshClient::writeTerminal(const QByteArray &data)
{
    if (m_terminalId.isEmpty() || data.isEmpty())
        return;

    call(QStringLiteral("terminal/write"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("id"), m_terminalId},
                     {QStringLiteral("attachmentId"), m_attachmentId},
                     {QStringLiteral("data"), QString::fromUtf8(data)}});
}

void DshClient::resizeTerminal(int cols, int rows)
{
    cols = qMax(1, cols);
    rows = qMax(1, rows);
    if (m_terminalId.isEmpty() || (cols == m_terminalCols && rows == m_terminalRows))
        return;

    m_terminalCols = cols;
    m_terminalRows = rows;
    emit terminalSizeChanged();

    // The PTY has to agree with the emulator or a full-screen program draws to
    // the wrong grid. Resize is best-effort: a host that refuses it leaves the
    // panel usable, just misfitted, so it is not treated as a failure.
    call(QStringLiteral("terminal/resize"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("id"), m_terminalId},
                     {QStringLiteral("attachmentId"), m_attachmentId},
                     {QStringLiteral("cols"), cols},
                     {QStringLiteral("rows"), rows}});
}

void DshClient::closeTerminal()
{
    if (m_terminalId.isEmpty())
        return;

    if (!m_terminalStreamId.isEmpty()) {
        cancelStream(m_terminalStreamId);
        m_terminalStreamId.clear();
    }

    call(QStringLiteral("terminal/close"),
         QJsonObject{{QStringLiteral("agentId"), m_sessionId},
                     {QStringLiteral("id"), m_terminalId}});

    {
        // Forget it: leaving the id stored would have the next open ask to
        // reattach to a terminal this client just closed.
        QSettings prefs;
        prefs.remove(QStringLiteral("terminal/%1").arg(m_sessionId));
    }
    m_terminalId.clear();
    m_terminalState.clear();
    emit terminalChanged();
}

void DshClient::handleTerminalItem(const QJsonObject &value)
{
    const QString type = value.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("snapshot")) {
        const QJsonObject info = value.value(QStringLiteral("info")).toObject();
        // `controllerId` says which attachment owns input. Ours must match, or
        // typing is refused with terminal/control-unavailable.
        const QString controller = info.value(QStringLiteral("controllerId")).toString();
        m_terminalState = info.value(QStringLiteral("state")).toString();
        qInfo() << "terminal snapshot: state" << m_terminalState
                << "controller" << controller
                << (controller == m_attachmentId ? "(ours - writable)" : "(NOT ours - read-only)");

        // A bounded screen: repaint from it rather than assuming a blank grid,
        // because this may be a re-attach into a terminal that already has output.
        const QString screen = value.value(QStringLiteral("screen")).toString();
        if (!screen.isEmpty())
            emit terminalOutput(screen.toUtf8());

        emit terminalChanged();
        emit terminalState(m_terminalState,
                           info.value(QStringLiteral("exitCode")).toInt(-1));
        return;
    }

    if (type == QLatin1String("output")) {
        // Raw PTY bytes, passed through untouched: the emulator consumes bytes,
        // not lines, and an escape sequence may be split across two frames.
        emit terminalOutput(value.value(QStringLiteral("data")).toString().toUtf8());
        return;
    }

    if (type == QLatin1String("state")) {
        const QJsonObject info = value.value(QStringLiteral("info")).toObject();
        m_terminalState = info.value(QStringLiteral("state")).toString();
        emit terminalChanged();
        emit terminalState(m_terminalState,
                           info.value(QStringLiteral("exitCode")).toInt(-1));
        return;
    }
}

// --- forwarded events and waterfalls ----------------------------------------

void DshClient::openEventStream()
{
    if (!m_eventStreamId.isEmpty())
        return;   // already subscribed; a second open is a duplicate-stream error

    // The endpoint is `$events`, and like every stream payload its arguments live
    // under `args` — empty here, because the stream carries whatever the host
    // forwards to this client.
    m_eventStreamId = openStream(QStringLiteral("$events"), QJsonObject{});
    qInfo() << "opened $events stream" << m_eventStreamId;
}

// --- the workspace registry -------------------------------------------------

void DshClient::openWorkspaceStream()
{
    if (!m_workspaceStreamId.isEmpty())
        return;   // already subscribed; a second open is a duplicate-stream error

    // No arguments: `workspace/follow` describes the whole registry, and every
    // generation opens with exactly one `baseline` frame.
    m_workspaceStreamId = openStream(QStringLiteral("workspace/follow"), QJsonObject{});
    qInfo() << "opened workspace/follow stream" << m_workspaceStreamId;
}

void DshClient::handleWorkspaceItem(const QJsonObject &value)
{
    const QString type = value.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("baseline")) {
        const QJsonObject baseline = value.value(QStringLiteral("value")).toObject();

        QVariantList workspaces;
        m_workspaceOfSession.clear();
        const QJsonArray items = baseline.value(QStringLiteral("items")).toArray();
        workspaces.reserve(items.size());
        for (const QJsonValue &entry : items) {
            const QJsonObject w = entry.toObject();
            const QString workspaceId = w.value(QStringLiteral("workspaceId")).toString();
            if (workspaceId.isEmpty())
                continue;

            // Membership is the host's, not ours. A Session belongs to a Workspace
            // because the registry says so — which is how an archived-then-restored
            // Session returns to the right group. Grouping by `cwd` equality is a
            // guess that splits one real Workspace the moment two share a
            // directory, and there are two such pairs on this box.
            QStringList sessionIds;
            const QJsonArray ids = w.value(QStringLiteral("sessionIds")).toArray();
            sessionIds.reserve(ids.size());
            for (const QJsonValue &id : ids) {
                const QString sessionId = id.toString();
                if (sessionId.isEmpty())
                    continue;
                sessionIds.append(sessionId);
                m_workspaceOfSession.insert(sessionId, workspaceId);
            }

            workspaces.append(QVariantMap{
                {QStringLiteral("workspaceId"), workspaceId},
                {QStringLiteral("path"), w.value(QStringLiteral("path")).toString()},
                {QStringLiteral("title"), w.value(QStringLiteral("title")).toString()},
                {QStringLiteral("sessionIds"), sessionIds},
            });
        }

        // The synthetic group, appended last and always present. Last because a
        // run's session is history the moment it ends, and always present because
        // the view drops it when it has no members — so an empty "Automations"
        // header cannot appear, and the ordering rule is decided in one place
        // rather than re-decided frame by frame.
        workspaces.append(QVariantMap{
            {QStringLiteral("workspaceId"), QString::fromLatin1(kAutomationsGroup)},
            {QStringLiteral("path"), QString()},
            {QStringLiteral("title"), QStringLiteral("Automations")},
            {QStringLiteral("sessionIds"), QStringList{}},
            {QStringLiteral("virtual"), true},
        });

        m_archivedSessionIds.clear();
        for (const QJsonValue &id : baseline.value(QStringLiteral("archivedSessionIds")).toArray()) {
            const QString sessionId = id.toString();
            if (!sessionId.isEmpty())
                m_archivedSessionIds.insert(sessionId);
        }

        m_pinnedSessionIds.clear();
        for (const QJsonValue &id : baseline.value(QStringLiteral("pinnedSessionIds")).toArray()) {
            const QString sessionId = id.toString();
            if (!sessionId.isEmpty())
                m_pinnedSessionIds.insert(sessionId);
        }

        const bool changed = (workspaces != m_workspaces);
        m_workspaces = workspaces;
        if (changed)
            emit workspacesChanged();

        qInfo() << "workspace baseline:" << m_workspaces.size() << "workspace(s),"
                << m_archivedSessionIds.size() << "archived,"
                << m_pinnedSessionIds.size() << "pinned session(s)";

        // The archived set decides what the rail may show, so the rows are rebuilt
        // now rather than waiting for the next `session/list` to happen by.
        rebuildSessionRows();
        return;
    }

    if (type == QLatin1String("upsert")) {
        const QJsonObject w = value.value(QStringLiteral("workspace")).toObject();
        const QString workspaceId = w.value(QStringLiteral("workspaceId")).toString();
        if (workspaceId.isEmpty())
            return;

        QStringList sessionIds;
        for (const QJsonValue &id : w.value(QStringLiteral("sessionIds")).toArray())
            sessionIds.append(id.toString());

        // Drop this Workspace's old mapping first: a Session that has left it must
        // not keep pointing back at it.
        for (auto it = m_workspaceOfSession.begin(); it != m_workspaceOfSession.end(); ) {
            if (it.value() == workspaceId)
                it = m_workspaceOfSession.erase(it);
            else
                ++it;
        }
        for (const QString &sessionId : sessionIds)
            m_workspaceOfSession.insert(sessionId, workspaceId);

        const QVariantMap entry{
            {QStringLiteral("workspaceId"), workspaceId},
            {QStringLiteral("path"), w.value(QStringLiteral("path")).toString()},
            {QStringLiteral("title"), w.value(QStringLiteral("title")).toString()},
            {QStringLiteral("sessionIds"), sessionIds},
        };

        bool replaced = false;
        for (int i = 0; i < m_workspaces.size(); ++i) {
            if (m_workspaces.at(i).toMap().value(QStringLiteral("workspaceId")).toString() == workspaceId) {
                m_workspaces[i] = entry;   // an update keeps its place in the order
                replaced = true;
                break;
            }
        }
        if (!replaced)
            m_workspaces.append(entry);

        emit workspacesChanged();
        rebuildSessionRows();
        return;
    }

    if (type == QLatin1String("remove")) {
        const QString workspaceId = value.value(QStringLiteral("workspaceId")).toString();
        for (int i = m_workspaces.size() - 1; i >= 0; --i) {
            if (m_workspaces.at(i).toMap().value(QStringLiteral("workspaceId")).toString() == workspaceId)
                m_workspaces.removeAt(i);
        }
        for (auto it = m_workspaceOfSession.begin(); it != m_workspaceOfSession.end(); ) {
            if (it.value() == workspaceId)
                it = m_workspaceOfSession.erase(it);
            else
                ++it;
        }
        emit workspacesChanged();
        rebuildSessionRows();
        return;
    }

    if (type == QLatin1String("order")) {
        // Adopted verbatim rather than re-sorted: the host owns this order and the
        // user arranges it, which is exactly what the web UI does with it.
        QHash<QString, QVariantMap> byId;
        QVariantList virtualEntries;
        for (const QVariant &entry : m_workspaces) {
            const QVariantMap map = entry.toMap();
            // The synthetic group is not the host's to order, so it steps aside for
            // the comparison and is put back at the end. Without this an order
            // frame would silently drop it and the automation runs would fall into
            // Ungrouped.
            if (map.value(QStringLiteral("virtual")).toBool()) {
                virtualEntries.append(map);
                continue;
            }
            byId.insert(map.value(QStringLiteral("workspaceId")).toString(), map);
        }

        QVariantList ordered;
        for (const QJsonValue &id : value.value(QStringLiteral("workspaceIds")).toArray()) {
            const auto it = byId.constFind(id.toString());
            if (it != byId.constEnd())
                ordered.append(it.value());
        }
        // A partial order is not an order; keeping the previous one is safer than
        // dropping the Workspaces it failed to name.
        if (ordered.size() != byId.size())
            return;

        for (const QVariant &entry : virtualEntries)
            ordered.append(entry);

        m_workspaces = ordered;
        emit workspacesChanged();
        return;
    }

    if (type == QLatin1String("archived")) {
        m_archivedSessionIds.clear();
        for (const QJsonValue &id : value.value(QStringLiteral("archivedSessionIds")).toArray()) {
            const QString sessionId = id.toString();
            if (!sessionId.isEmpty())
                m_archivedSessionIds.insert(sessionId);
        }
        qInfo() << "archived set is now" << m_archivedSessionIds.size() << "session(s)";
        rebuildSessionRows();
        return;
    }

    if (type == QLatin1String("pinned")) {
        m_pinnedSessionIds.clear();
        for (const QJsonValue &id : value.value(QStringLiteral("pinnedSessionIds")).toArray()) {
            const QString sessionId = id.toString();
            if (!sessionId.isEmpty())
                m_pinnedSessionIds.insert(sessionId);
        }
        qInfo() << "pinned set is now" << m_pinnedSessionIds.size() << "session(s)";
        rebuildSessionRows();
        return;
    }

    qInfo() << "workspace/follow: unhandled frame" << type;
}

void DshClient::handleEventStreamItem(const QJsonObject &value)
{
    const QString type = value.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("ready")) {
        // Every answer must quote this clientId, so nothing can be answered
        // before it arrives.
        m_eventClientId = value.value(QStringLiteral("clientId")).toString();
        qInfo() << "$events ready; clientId" << m_eventClientId;
        return;
    }

    if (type == QLatin1String("waterfall")) {
        handleWaterfall(value);
        return;
    }

    if (type == QLatin1String("cancel")) {
        // The host withdrew it — usually because another client answered first.
        // Dropping it is required: answering a settled waterfall is an error, and
        // the agent is no longer waiting.
        //
        // Dropping it silently is not enough. The dialog asking the question is in
        // the view, not in this map, so the withdrawal has to be announced or the
        // prompt outlives the decision it was asking for — and then offers to
        // answer something the host will refuse.
        const QString eventId = value.value(QStringLiteral("eventId")).toString();
        const auto pending = m_pendingWaterfalls.find(eventId);
        if (pending == m_pendingWaterfalls.end())
            return;

        const QString event = pending.value().event;
        m_pendingWaterfalls.erase(pending);
        qInfo() << "waterfall withdrawn by the host:" << eventId << event;
        // The row it coloured is not waiting any more.
        refreshSessionMarks();

        if (event == QLatin1String("approval/request")) {
            // Only the approval that is actually offered has a dialog to close.
            // A queued one, or one belonging to a session the reader has moved
            // away from, was never shown — announcing it would close whatever
            // prompt is legitimately open instead.
            if (eventId != m_offeredApprovalId)
                return;
            m_offeredApprovalId.clear();
            emit permissionWithdrawn(eventId);
            // Its dialog is gone; the next retained approval for this session,
            // if any, takes its turn — oldest first, never stacked.
            offerPendingApproval();
            return;
        }

        // The question dialog matches on the eventId, so this closes exactly the
        // question that was withdrawn. Like an approval, only the question that
        // is actually offered has a dialog to close; a queued one, or one
        // belonging to another session, was never shown.
        if (event == QLatin1String("user-questions/request")) {
            if (eventId != m_offeredQuestionId)
                return;
            m_offeredQuestionId.clear();
            emit permissionWithdrawn(eventId);
            // Its dialog is gone; the next retained question for this session,
            // if any, takes its turn — oldest first, never stacked.
            offerPendingQuestion();
            return;
        }

        emit permissionWithdrawn(eventId);
        return;
    }

    if (type == QLatin1String("emit")) {
        // The ordinary forwarded-event channel: the host telling this client that
        // something changed elsewhere — another session appeared, went busy or
        // ended. `args` is the Cordis emit argument array, verified live:
        //   api-session/activity -> ["session-625db3d0-…", 1791141117324]
        const QString event = value.value(QStringLiteral("event")).toString();
        const QJsonArray args = value.value(QStringLiteral("args")).toArray();
        emit hostEvent(event, args);

        // These are exactly the events that can change the roster, and they fire
        // for sessions this client is not driving — which is the point.
        //
        // What they actually carry, read from the host's own session controller
        // (`@deepseek-ai/dsh-api-session-controller/lib/index.js`): `added` is
        // `ApiSessionList.summaryFor(session)` — the very object a `session/list`
        // item is, sessionId/updatedAt/running/blank/cwd/parentSessionId/origin
        // and the `projections.values` the rail reads its title from
        // (`dsh-session-title` registers `title` as a projection); `removed` is a
        // bare session id, and the record it names stays on disk, so the next
        // list still returns it as a cold row; `status` is (sessionId, running).
        // So the payload is the summary, not a projection of one, and applying it
        // here cannot drift from what the list will say.
        //
        // Applied immediately rather than waiting for the list, because that list
        // reads the whole corpus — ~2200 sessions, measured at ~5 s and, while
        // the host's `ctx.sessionQuery` stalls, at minutes — and a session the
        // host has just announced must not be behind it.
        if (event == QLatin1String("api-session/added")) {
            adoptSessionSummary(args.at(0).toObject());
            // A real change of membership, so a list is still wanted to reconcile
            // it — the row above is provisional until one that postdates this
            // event lands. Only worth re-reading if anything consumes the list.
            if (!m_sessions.isEmpty())
                listSessions();
            return;
        }
        if (event == QLatin1String("api-session/removed")) {
            // The session's live agent ended; the row itself stays, exactly as the
            // next list's cold summary for it would say.
            patchSessionFlags(args.at(0).toString(),
                              QJsonObject{{QStringLiteral("running"), false},
                                          {QStringLiteral("agentAvailable"), false}});
            if (!m_sessions.isEmpty())
                listSessions();
            return;
        }
        if (event == QLatin1String("api-session/status")) {
            // The one field this event changes is `running`, and it states it
            // outright, so it is applied here and the roster is not re-read for it.
            patchSessionFlags(args.at(0).toString(),
                              QJsonObject{{QStringLiteral("running"), args.at(1).toBool()}});
            // A read is still started when nothing is in flight — it is what
            // refreshes other Sessions' titles and projections, which have no
            // event of their own — but a status that arrives DURING a read is
            // deliberately not queued: a list takes minutes here, status flaps
            // happen by the minute, and a queued status re-read would chain one
            // full-corpus read after another. That load is what makes a list slow
            // in the first place, so this is the one event that never queues.
            if (!m_listInFlight && !m_sessions.isEmpty())
                listSessions();
            return;
        }

        // Both namespaces announce their durable writes on this channel rather
        // than on a stream of their own (`schedule/changed` after a create, a
        // delivery, an edit or a delete; `plugin-manager/changed` after a
        // composition change). Re-read only after a page has asked once, so a
        // session that never opens either page pays nothing for them.
        if (event == QLatin1String("schedule/changed") && m_scheduleLoaded)
            refreshScheduleTasks();
        if (event == QLatin1String("plugin-manager/changed") && m_pluginsLoaded)
            refreshPluginInventory();
        return;
    }
}

void DshClient::applySessionList(const QJsonArray &items)
{
    // Kept raw rather than flattened on the spot: the archived set and the
    // Workspace mapping can each arrive after the list does, and either one
    // changes which rows may be shown. Holding the items means neither needs a
    // second round trip.
    m_rawSessions = items;

    // This answer was composed after the request went out, so it accounts for
    // every forwarded statement the host had already made at that point: those
    // overlay entries are spent and go. Anything newer arrived after the request
    // and may have missed the answer, so it stays and is drawn over the list
    // until a list that postdates it lands. The list itself is stored verbatim
    // and is never edited here, which is what keeps the host authoritative
    // wherever it is the newer answer.
    for (auto entry = m_sessionOverlay.begin(); entry != m_sessionOverlay.end(); ) {
        if (entry->epoch <= m_listRequestEpoch)
            entry = m_sessionOverlay.erase(entry);
        else
            ++entry;
    }

    rebuildSessionRows();

    // The rail announces the Sessions a schedule delivers into, and that count is
    // not on the list: it is folded from `schedule/catalog` (see
    // applyScheduleCatalog). So the first roster answer is where the catalog is
    // asked for, once — a rail that waited for the Automations page to be opened
    // would be back to rows that say nothing about their schedule. After this,
    // `schedule/changed` keeps it current and `onConnected` re-reads it on a fresh
    // socket. A host without the Schedule bundle answers with an error, the map
    // stays empty, and nothing marks anything.
    if (!m_scheduleAsked && m_state == Ready) {
        m_scheduleAsked = true;
        refreshScheduleTasks();
    }

    // Nothing else picks a session at startup: `followSession` is called only
    // from the rail, so the window opened on an empty transcript on every launch.
    // A held seat is a deliberate empty id, not a cold start: restoring over it
    // would replace the reader's own new-session screen with an old conversation.
    if (m_sessionId.isEmpty() && !m_restoreAttempted && !m_pendingSeat.active) {
        restoreLastSession();
    } else if (!m_sessionId.isEmpty()) {
        // A session is already open, so there is nothing to restore — and now
        // there never will be: a later `unfollowSession` must not make the next
        // roster refresh reopen a session the reader deliberately closed.
        m_restoreAttempted = true;
    }
}

/// The host stores one session id in two spellings: `session-<uuid>` in a
/// header, bare in places in a `session/list` item and in the archived and
/// pinned sets. Comparisons here go through the bare form for that reason — the
/// same rule rebuildSessionRows() applies when it tests those two sets.
static QString bareSessionId(const QString &sessionId)
{
    return sessionId.startsWith(QLatin1String("session-")) ? sessionId.mid(8) : sessionId;
}

void DshClient::adoptSessionSummary(const QJsonObject &summary)
{
    const QString wanted =
        bareSessionId(summary.value(QStringLiteral("sessionId")).toString());
    if (wanted.isEmpty())
        return;

    // Kept as the host sent it and built into a row by rebuildSessionRows(), the
    // same code every listed item goes through, so no field is interpreted here
    // that the list path does not also interpret: the archived and pinned sets,
    // the Workspace mapping and the subagent catalog are all applied there.
    //
    // Nothing is ordered here either. The rail sorts by `updatedAt` itself
    // (SessionSidebar's `rows`), and this payload carries the host's own
    // `updatedAt`, so the row lands exactly where the next list will put it.
    //
    // `added` also fires when an agent appears in, or goes from, a Session that
    // already exists; it re-publishes that Session's summary with the live
    // `running`/`agentAvailable` flags. Replacing the stored summary is therefore
    // right, and any flag stated BEFORE it must go, or the older statement would
    // be laid on top of the newer one.
    HostSessionOverlay &entry = m_sessionOverlay[wanted];
    entry.summary = summary;
    for (auto field = entry.flags.begin(); field != entry.flags.end(); ) {
        if (summary.contains(field.key()))
            field = entry.flags.erase(field);
        else
            ++field;
    }
    entry.epoch = ++m_rosterEpoch;
    rebuildSessionRows();

    // The one line that makes "it appeared without waiting for a list" checkable
    // in the client log: the roster's size at the moment the emit was applied,
    // with no `session/list` answer behind it.
    qInfo() << "api-session/added applied from the emit:" << shortSessionId(wanted)
            << "roster now" << m_sessions.size() << "row(s)";
}

void DshClient::patchSessionFlags(const QString &sessionId, const QJsonObject &flags)
{
    const QString wanted = bareSessionId(sessionId);
    if (wanted.isEmpty() || flags.isEmpty())
        return;

    // Only the fields the emit actually spoke about, and only when they disagree
    // with the newest statement already held: the host re-sends `added` for the
    // same Session, so a status flap must not make every bound view rebuild for
    // an identical roster.
    //
    // A Session no statement has mentioned before gets its entry from this one
    // only if the flag says something (see below) — a `status` for a Session the
    // client does not hold would otherwise leave empty entries behind.
    auto found = m_sessionOverlay.find(wanted);
    const bool held = found != m_sessionOverlay.end();
    HostSessionOverlay scratch;
    HostSessionOverlay &entry = held ? found.value() : scratch;
    bool changed = false;
    for (auto field = flags.constBegin(); field != flags.constEnd(); ++field) {
        const QJsonValue current = entry.flags.contains(field.key())
            ? entry.flags.value(field.key())
            : entry.summary.value(field.key());
        if (current == field.value())
            continue;
        entry.flags.insert(field.key(), field.value());
        changed = true;
    }
    if (!changed)
        return;
    entry.epoch = ++m_rosterEpoch;
    if (!held)
        m_sessionOverlay.insert(wanted, entry);
    rebuildSessionRows();
}

QJsonObject DshClient::sessionListItem(const QString &sessionId) const
{
    const QString wanted = bareSessionId(sessionId);
    if (wanted.isEmpty())
        return {};

    QJsonObject item;
    for (const QJsonValue &entry : m_rawSessions) {
        const QJsonObject candidate = entry.toObject();
        if (bareSessionId(candidate.value(QStringLiteral("sessionId")).toString()) == wanted) {
            item = candidate;
            break;
        }
    }

    // A statement the host forwarded after the current list was asked for is
    // newer than that list by construction (applySessionList prunes everything
    // older), so it is laid over the list's own entry — and when the list has
    // never heard of this Session, the summary IS its entry. Same rule the row
    // build applies; one caller must not see a different Session from another.
    const auto overlay = m_sessionOverlay.constFind(wanted);
    if (overlay == m_sessionOverlay.constEnd())
        return item;
    if (item.isEmpty())
        item = overlay->summary;
    for (auto field = overlay->summary.constBegin(); field != overlay->summary.constEnd(); ++field)
        item.insert(field.key(), field.value());
    for (auto field = overlay->flags.constBegin(); field != overlay->flags.constEnd(); ++field)
        item.insert(field.key(), field.value());
    return item;
}

bool DshClient::isArchivedSession(const QString &sessionId) const
{
    return m_archivedSessionIds.contains(sessionId)
        || m_archivedSessionIds.contains(bareSessionId(sessionId));
}

bool DshClient::subagentParentOf(const QString &sessionId, QString *parentId) const
{
    if (parentId)
        parentId->clear();

    const QJsonObject item = sessionListItem(sessionId);
    if (item.isEmpty())
        return false;

    // The host's own discriminator, not the parent link. `validateAddress`
    // refuses a plain session address only when `header.origin == "subagent"`,
    // and a `session/fork` result is created with `meta.parentSession` but never
    // `meta.origin` — so it has a parent and is still an ordinary Session. Using
    // the parent link alone would send every restored fork to the tagged form,
    // which the host answers with `subagent/unauthorized`.
    if (item.value(QStringLiteral("origin")).toString() != QLatin1String("subagent"))
        return false;

    // This is `header.parentSession`, the exact value the host compares against
    // the address it is given, so it is passed through verbatim. Empty is a real
    // answer: the child exists but cannot be addressed.
    if (parentId)
        *parentId = item.value(QStringLiteral("parentSessionId")).toString();
    return true;
}

bool DshClient::followResolvedSession(const QString &sessionId)
{
    QString parentId;
    if (subagentParentOf(sessionId, &parentId)) {
        if (parentId.isEmpty()) {
            qWarning() << "session" << sessionId
                       << "is a subagent child but the roster names no parent;"
                       << "the host will not accept any address for it";
            return false;
        }
        // The host refuses the plain address for a child (`session/agent-busy`:
        // "subagent Sessions require their durable parent address") and expects
        // the tagged form — the same one the rail sends for a child row. The
        // two-argument overload asks the host to resolve the child's mode, which
        // is the safe half of that address.
        qInfo() << "following subagent child" << shortSessionId(sessionId)
                << "under parent" << shortSessionId(parentId);
        followSubagent(parentId, sessionId);
        return true;
    }

    followSession(sessionId);
    return true;
}

QString DshClient::mostRecentTopLevelSessionId() const
{
    QString newestId;
    double newestAt = 0;
    bool haveOrder = false;

    for (const QJsonValue &entry : m_rawSessions) {
        const QJsonObject item = entry.toObject();
        const QString id = item.value(QStringLiteral("sessionId")).toString();
        if (id.isEmpty() || isArchivedSession(id))
            continue;
        // The host's own word for a subagent, so the fallback cannot itself be a
        // session the host refuses to open.
        if (item.value(QStringLiteral("origin")).toString() == QLatin1String("subagent"))
            continue;

        // The host lists newest first, but the fallback is stated as "most
        // recent" rather than "first", so it survives a change in that order.
        // `updatedAt` is a millisecond stamp; a missing one loses to any real
        // one and the first such row still counts when none has a stamp.
        const double updated = item.value(QStringLiteral("updatedAt")).toDouble();
        if (!haveOrder || updated > newestAt) {
            newestId = id;
            newestAt = updated;
            haveOrder = true;
        }
    }
    return newestId;
}

void DshClient::restoreLastSession()
{
    m_restoreAttempted = true;

    QSettings prefs;
    const QString remembered = prefs.value(QStringLiteral("session/last")).toString();

    // What to tell the reader when the remembered session could not be opened.
    // The row is appended *after* the fallback's follow, because a follow clears
    // the transcript: appending first would erase the very explanation.
    QString explanation;

    if (!remembered.isEmpty() && !isArchivedSession(remembered)
        && !sessionListItem(remembered).isEmpty()) {
        QString parentId;
        if (subagentParentOf(remembered, &parentId) && parentId.isEmpty()) {
            // A child with no parent on its roster row has no address the host
            // will accept, so retrying the restore would fail every time. Fall
            // through to another session: a failed start is worse than a
            // different session.
            qWarning() << "remembered session" << remembered
                       << "is a subagent child whose parent is unknown;"
                       << "opening a top-level session instead";
            explanation = QStringLiteral("could not reopen the last session (%1): "
                                         "it is a subagent child and its parent is unknown")
                              .arg(shortSessionId(remembered));
        } else if (followResolvedSession(remembered)) {
            return;
        }
    }

    const QString fallback = mostRecentTopLevelSessionId();
    if (fallback.isEmpty()) {
        // Nothing to open at all. Say why rather than leaving the pane blank
        // with the empty state's "ready — ask something".
        m_transcript->append(QStringLiteral("notice"),
                             explanation.isEmpty()
                                 ? QStringLiteral("no session could be reopened: there is no "
                                                  "top-level session that is not archived")
                                 : explanation + QStringLiteral("; and there is no top-level "
                                                                "session to open instead"));
        return;
    }

    followResolvedSession(fallback);
    if (!explanation.isEmpty()) {
        m_transcript->append(QStringLiteral("notice"),
                             explanation + QStringLiteral("; opened %1 instead")
                                               .arg(shortSessionId(fallback)));
    }
}

void DshClient::rebuildSessionRows()
{
    // The host's list, verbatim, plus what the forwarded `api-session/*` emits have
    // said since that list was asked for. The overlay only ever holds statements
    // newer than `m_rawSessions` — applySessionList prunes everything the answer
    // already accounts for — so a row can never be staler than the roster beside
    // it, and a list that postdates a statement still wins outright.
    //
    // A row added this way is built by exactly the code below, from the host's own
    // `SessionSummary`, so it gets the same archived/pinned flags, the same
    // Workspace grouping and the same subagent catalog lookup as any other row.
    QJsonArray items = m_rawSessions;
    if (!m_sessionOverlay.isEmpty()) {
        QSet<QString> listed;
        listed.reserve(items.size());
        for (int i = 0; i < items.size(); ++i) {
            QJsonObject item = items.at(i).toObject();
            const QString key =
                bareSessionId(item.value(QStringLiteral("sessionId")).toString());
            listed.insert(key);
            const auto overlay = m_sessionOverlay.constFind(key);
            if (overlay == m_sessionOverlay.constEnd())
                continue;
            for (auto field = overlay->summary.constBegin();
                 field != overlay->summary.constEnd(); ++field)
                item.insert(field.key(), field.value());
            for (auto field = overlay->flags.constBegin();
                 field != overlay->flags.constEnd(); ++field)
                item.insert(field.key(), field.value());
            items.replace(i, item);
        }
        // A Session no list has caught up with yet: the `added` summary is the only
        // description of it there is, and it is a row until a list that postdates
        // it says otherwise.
        for (auto overlay = m_sessionOverlay.constBegin();
             overlay != m_sessionOverlay.constEnd(); ++overlay) {
            if (listed.contains(overlay.key()) || overlay->summary.isEmpty())
                continue;
            QJsonObject item = overlay->summary;
            for (auto field = overlay->flags.constBegin();
                 field != overlay->flags.constEnd(); ++field)
                item.insert(field.key(), field.value());
            items.append(item);
        }
    }

    QVariantList flattened;
    flattened.reserve(items.size());

    // First pass: a child's readable label lives in its PARENT's
    // `projections.values.subagentCatalog`, not in the child's own entry — whose
    // stored title is the opening prompt ("You are a subagent…"), which is useless
    // as a row label. Harvesting it here means a view gets one flat list where
    // every row already knows its own name and parent, rather than being handed
    // two projections to join itself.
    QHash<QString, QString> labelsByChild;
    QHash<QString, QString> modesByChild;

    auto normalise = [](const QString &id) {
        return id.startsWith(QLatin1String("session-")) ? id.mid(8) : id;
    };

    for (const QJsonValue &entry : items) {
        const QJsonObject item = entry.toObject();
        const QJsonArray entries =
            item.value(QStringLiteral("projections")).toObject()
                .value(QStringLiteral("values")).toObject()
                .value(QStringLiteral("subagentCatalog")).toArray();
        for (const QJsonValue &child : entries) {
            const QJsonObject c = child.toObject();
            const QString id = c.value(QStringLiteral("id")).toString();
            const QString label = c.value(QStringLiteral("label")).toString();
            if (!id.isEmpty() && !label.isEmpty()) {
                labelsByChild.insert(id, label);
                labelsByChild.insert(normalise(id), label);
            }
            // One-shot or continuable. It decides whether a child can be sent
            // to at all, so the composer gate needs it — and the catalog was
            // already being read for the label, so this costs a hash insert.
            const QString mode = c.value(QStringLiteral("mode")).toString();
            if (!id.isEmpty() && !mode.isEmpty()) {
                modesByChild.insert(id, mode);
                modesByChild.insert(normalise(id), mode);
            }
        }
    }

    for (const QJsonValue &entry : items) {
        const QJsonObject item = entry.toObject();
        const QJsonObject values =
            item.value(QStringLiteral("projections")).toObject().value(QStringLiteral("values")).toObject();
        const QString id = item.value(QStringLiteral("sessionId")).toString();
        const QString parent = item.value(QStringLiteral("parentSessionId")).toString();

        // Flagged rather than dropped: whether archived Sessions are listed at all
        // is a view choice, and a filter applied down here would make "show
        // archived" impossible without another round trip. The host stores these
        // ids in both forms (`session-<uuid>` and bare), so both are tested.
        const bool archived = m_archivedSessionIds.contains(id)
                           || m_archivedSessionIds.contains(normalise(id));
        // Pinned the same way, and for the same reason: an archived Session is
        // never shown as pinned, so the flag is carried separately from the set.
        const bool pinned = !archived
                         && (m_pinnedSessionIds.contains(id)
                             || m_pinnedSessionIds.contains(normalise(id)));

        flattened.append(QVariantMap{
            {QStringLiteral("sessionId"), id},
            // The title lives under projections.values, not at the top level.
            {QStringLiteral("title"), values.value(QStringLiteral("title")).toString()},
            // Parentage and the readable child label, so a tree view needs no
            // second source. `origin` is kept because it is the host's own way of
            // saying "this is a subagent", rather than inferring it from a parent.
            {QStringLiteral("parentSessionId"), parent},
            {QStringLiteral("origin"), item.value(QStringLiteral("origin")).toString()},
            {QStringLiteral("label"), labelsByChild.value(id, labelsByChild.value(normalise(id)))},
            {QStringLiteral("subagentMode"),
             modesByChild.value(id, modesByChild.value(normalise(id)))},
            {QStringLiteral("cwd"), item.value(QStringLiteral("cwd")).toString()},
            // Which group this row belongs to, decided by the host's registry
            // rather than by matching directories. Empty until the workspace
            // baseline lands, or for a Session the registry does not account for.
            //
            // Automation runs are the exception, and are lifted out of whatever
            // Workspace files them: a scheduled run is not a conversation someone
            // started in that directory, and dozens of them turn a Workspace into a
            // log. The prefix is the automation plugin's own convention — it is not
            // a host feature, `origin` says nothing about it, and the plugin's
            // authoritative store is behind a bridge a native client cannot read —
            // so the name is the only thing available to key on. Nothing is hidden:
            // the rows move to their own group.
            {QStringLiteral("workspaceId"),
             id.startsWith(QLatin1String(kAutomationPrefix))
                 ? QString::fromLatin1(kAutomationsGroup)
                 : m_workspaceOfSession.value(id)},
            {QStringLiteral("updatedAt"), item.value(QStringLiteral("updatedAt")).toDouble()},
            {QStringLiteral("running"), item.value(QStringLiteral("running")).toBool()},
            // A question the agent is blocked on, taken from the list's own
            // projection block. It is the only pending-interaction state the host
            // publishes per Session: an approval is a waterfall, not a projection,
            // so a session waiting on one cannot be seen from the list and this
            // says nothing rather than guessing.
            {QStringLiteral("waiting"),
             values.value(QStringLiteral("userQuestions"))
                       .toObject()
                       .value(QStringLiteral("active"))
                       .toArray()
                       .size()
                 > 0},
            {QStringLiteral("blank"), item.value(QStringLiteral("blank")).toBool()},
            {QStringLiteral("archived"), archived},
            {QStringLiteral("pinned"), pinned},
            // How many active scheduled tasks deliver into this Session, for the
            // rail's clock mark. Not read from `item` at all: the host's summary
            // carries no schedule field, so the number is folded from the one
            // `schedule/catalog` answer (see applyScheduleCatalog), which is what
            // the web UI and the Android client do too. Zero covers both "hosts
            // no task" and "no catalog read yet", which marks nothing either way.
            // Both id spellings are tried for the same reason the archived and
            // pinned sets are: the host hands them out in both forms.
            {QStringLiteral("scheduledTaskCount"),
             m_scheduleCounts.value(id, m_scheduleCounts.value(normalise(id), 0))},
        });
    }

    // The green mark's one input is the roster's own `running` bit, read before
    // the list is compared: a turn that stopped is a real change even when every
    // other field of the row is identical.
    //
    // "Finished while you were not looking" is the fact both reference clients
    // draw — Android's `completedSessionIds` and the web's green `done` dot — and
    // it is a *transition*, not a timestamp: `updatedAt` moves on a rename too,
    // so a row that merely got a new title must not claim unread messages.
    //
    // The followed session is never marked: the reader is looking at it, and its
    // transcript is on screen.
    QSet<QString> nowRunning;
    QSet<QString> present;
    nowRunning.reserve(flattened.size());
    present.reserve(flattened.size());
    const QString followed = bareSessionId(m_sessionId);
    for (const QVariant &entry : flattened) {
        const QVariantMap row = entry.toMap();
        const QString id = row.value(QStringLiteral("sessionId")).toString();
        if (id.isEmpty())
            continue;
        present.insert(id);
        present.insert(bareSessionId(id));
        if (row.value(QStringLiteral("running")).toBool())
            nowRunning.insert(id);
    }
    bool unreadMoved = false;
    for (const QString &id : m_runningSessions) {
        if (nowRunning.contains(id))
            continue;
        if (!present.contains(id)) {
            // The session is gone from the roster. Its mark goes with it; a row
            // that cannot be opened has nothing to be read.
            unreadMoved = m_unreadSessions.remove(id) || unreadMoved;
            continue;
        }
        if (bareSessionId(id) == followed)
            continue;
        if (!m_unreadSessions.contains(id)) {
            m_unreadSessions.insert(id);
            // A subagent's id and its bare form are the same conversation; the
            // row carries whichever the summary used, so both are stored.
            m_unreadSessions.insert(bareSessionId(id));
            unreadMoved = true;
        }
    }
    // A session the host removed outright leaves no transition behind, so the
    // set is pruned against what the roster still names.
    const QSet<QString> known = m_unreadSessions;
    for (const QString &id : known) {
        if (!present.contains(id)) {
            m_unreadSessions.remove(id);
            unreadMoved = true;
        }
    }
    m_runningSessions = nowRunning;
    if (unreadMoved)
        emit sessionMarksChanged();

    // Emit only on a real change. `api-session/status` and `api-session/activity`
    // fire repeatedly for whichever session is busy — including the one running
    // this very turn — and re-emitting for an identical list makes every bound
    // view rebuild itself for nothing.
    if (flattened == m_sessions)
        return;

    m_sessions = flattened;
    emit sessionsChanged();
}

QVariantList DshClient::unreadSessionIds() const
{
    QStringList ids = m_unreadSessions.values();
    ids.sort();
    QVariantList out;
    out.reserve(ids.size());
    for (const QString &id : ids)
        out.append(id);
    return out;
}

void DshClient::refreshSessionMarks()
{
    // The amber mark is the pending waterfall map, which is the only client-side
    // record that covers an approval as well as a question. A waterfall with no
    // `agentId` belongs to no row this rail can draw, so it adds no mark rather
    // than colouring the followed session as a guess.
    QSet<QString> sessions;
    for (auto it = m_pendingWaterfalls.constBegin(); it != m_pendingWaterfalls.constEnd(); ++it) {
        const PendingWaterfall &held = it.value();
        if (held.event != QLatin1String("approval/request")
            && held.event != QLatin1String("user-questions/request"))
            continue;
        if (held.agentId.isEmpty())
            continue;
        sessions.insert(held.agentId);
        sessions.insert(bareSessionId(held.agentId));
    }

    QStringList ids = sessions.values();
    ids.sort();
    QVariantList refreshed;
    refreshed.reserve(ids.size());
    for (const QString &id : ids)
        refreshed.append(id);

    if (refreshed == m_pendingInteractionSessions)
        return;

    m_pendingInteractionSessions = refreshed;
    emit sessionMarksChanged();
}

void DshClient::clearUnreadSession(const QString &sessionId)
{
    const QString bare = bareSessionId(sessionId);
    bool moved = false;
    // A copy to iterate: removing from the set under its own iterator is exactly
    // the kind of unknown a set does not have an order to make safe.
    const QSet<QString> held = m_unreadSessions;
    for (const QString &id : held) {
        if (bareSessionId(id) != bare)
            continue;
        m_unreadSessions.remove(id);
        moved = true;
    }
    if (moved)
        emit sessionMarksChanged();
}

void DshClient::handleWaterfall(const QJsonObject &frame)
{
    const QString eventId = frame.value(QStringLiteral("eventId")).toString();
    const QString event = frame.value(QStringLiteral("event")).toString();
    const QJsonObject request = frame.value(QStringLiteral("request")).toObject();
    const QString agentId = frame.value(QStringLiteral("agentId")).toString();

    if (eventId.isEmpty())
        return;

    if (m_eventClientId.isEmpty()) {
        // Cannot be answered without the clientId. Say so loudly: from the agent's
        // side this is indistinguishable from a client that simply never replied.
        qWarning() << "waterfall" << event << "arrived before $events ready; "
                      "it cannot be answered and the agent will stall";
        return;
    }

    // A client that reconnected is handed every waterfall still pending on the
    // host. It is already retained (and possibly already on screen), so keep its
    // arrival place instead of re-announcing it as a second row and a second
    // dialog. A re-delivery while nothing is offered may still be this session's
    // turn to see it.
    if (m_pendingWaterfalls.contains(eventId)) {
        qInfo() << "waterfall still pending, re-delivered:" << event << eventId;
        if (event == QLatin1String("approval/request"))
            offerPendingApproval();
        else if (event == QLatin1String("user-questions/request"))
            offerPendingQuestion();
        return;
    }

    PendingWaterfall pending;
    pending.event = event;
    pending.agentId = agentId;
    pending.request = request;
    pending.seq = m_nextWaterfallSeq++;
    m_pendingWaterfalls.insert(eventId, pending);
    // The row for the session that raised it goes amber now, whether or not that
    // session is the one being followed: the mark is how a reader notices a
    // question waiting in a conversation they are not looking at.
    refreshSessionMarks();

    if (event == QLatin1String("approval/request")) {
        // `request` carries {toolName, callId?, reason?}; the answer vocabulary is
        // allowed-once | rejected, and the tool blocks until it arrives.
        //
        // The approval is scoped to the session that raised it: its row and its
        // dialog happen only while that session is followed. Otherwise it stays
        // retained in the map above — the agent is still blocked, so dropping it
        // would stall the agent with no trace — and is offered when the reader
        // follows that session (see offerPendingApproval()).
        offerPendingApproval();
        return;
    }

    if (event == QLatin1String("user-questions/request")) {
        // The agent's `ask_user_question` call, blocked until this is answered.
        // `request` carries `{questions: [{id, question, detail?, header?,
        // options?: [{label, description?}], multiSelect?, intent?}]}`, and plan
        // review arrives as one of these with `intent.kind == "plan-review"` — so
        // handling questions is also what makes plan review possible at all.
        //
        // Scoped exactly like an approval: the notice row and the dialog happen
        // only while the session that raised the question is followed. Otherwise
        // it stays retained in the map above — the agent is blocked until it is
        // answered, so dropping it would stall the agent with no trace — and is
        // offered when the reader follows that session (see
        // offerPendingQuestion()). The batch under one eventId is the unit: the
        // dialog answers the whole array in one go.
        const QJsonArray questions = request.value(QStringLiteral("questions")).toArray();
        if (questions.isEmpty()) {
            // Nothing to render. Delegating is better than blocking the agent on a
            // card with no questions in it.
            qWarning() << "user-questions/request carried no questions; delegating";
            answerWaterfall(eventId, QString());
            return;
        }
        offerPendingQuestion();
        return;
    }

    // Anything else — and whatever the host adds later — is delegated back down
    // the host's own chain. This matters: the host's `noAnswerer` rejects an
    // unanswered request, so delegating keeps an unrecognised waterfall from
    // failing the turn outright.
    qInfo() << "delegating waterfall" << event << eventId;
    m_transcript->append(QStringLiteral("notice"),
                         QStringLiteral("handled by the host: %1").arg(event));
    emit waterfallDelegated(eventId, event);
    answerWaterfall(eventId, QString());   // empty value == {"kind":"next"}
}

bool DshClient::waterfallInFollowedSession(const PendingWaterfall &pending) const
{
    // The identity is proved by the host's own sources. A waterfall's `agentId`
    // is `source.context.agentId` (`@deepseek-ai/dsh-api-gateway`, startRemoteEvent),
    // and the Agent registry refuses to enter an Agent whose id differs from its
    // Session's: `agent id "…" does not match session id "…"`
    // (`@deepseek-ai/dsh-agent`, AgentRegistry.enter). So for an ordinary session
    // the field is exactly `m_sessionId`; for a subagent child it is the child's
    // own session id, which is what `followSubagent` stores in `m_sessionId`.
    //
    // An empty field is an older host, or a frame that omitted it. It falls back
    // to today's behaviour — shown for whichever session is followed — because an
    // approval or question nobody sees leaves its agent blocked forever.
    return pending.agentId.isEmpty() || pending.agentId == m_sessionId;
}

void DshClient::offerPendingApproval()
{
    // A follow is still laying down its history. Offering now would put the
    // notice above that history and the snapshot would then append below it;
    // the snapshot path calls this again once the rows are in place, and so does
    // a stream that ends before a snapshot, so neither case hides the approval.
    if (!m_followSnapshotSeen && !m_followStreamId.isEmpty())
        return;

    // One at a time: a second dialog must never replace the one a reader is
    // deciding on, and "oldest first" means the queue is drained in order.
    //
    // The dialog may still be up while its row went with a transcript that was
    // rebuilt underneath it (a re-follow of the same address after a stream
    // end, or a continuation that could not be continued). Draw the row again
    // at the tail rather than leave a prompt asking about something the
    // transcript no longer mentions.
    if (!m_offeredApprovalId.isEmpty()) {
        if (m_pendingWaterfalls.contains(m_offeredApprovalId)) {
            drawApprovalNotice(m_offeredApprovalId);
            return;
        }
        // Defensive: every removal path clears the offer, but a dangling id here
        // would block every later approval forever. Close the stale prompt and
        // carry on with the queue.
        const QString stale = m_offeredApprovalId;
        m_offeredApprovalId.clear();
        qWarning() << "offered approval vanished from the pending map:" << stale;
        emit permissionWithdrawn(stale);
    }

    QString oldestId;
    PendingWaterfall oldest;
    bool found = false;
    for (auto it = m_pendingWaterfalls.constBegin(); it != m_pendingWaterfalls.constEnd(); ++it) {
        const PendingWaterfall &candidate = it.value();
        if (candidate.event != QLatin1String("approval/request"))
            continue;
        if (!waterfallInFollowedSession(candidate))
            continue;
        if (found && candidate.seq >= oldest.seq)
            continue;
        oldestId = it.key();
        oldest = candidate;
        found = true;
    }
    if (!found)
        return;

    const QString reason = oldest.request.value(QStringLiteral("reason")).toString();

    m_offeredApprovalId = oldestId;
    // The row is drawn where the approval is surfaced, not where it arrived.
    drawApprovalNotice(oldestId);

    qInfo() << "offering approval" << oldestId << "raised by"
            << (oldest.agentId.isEmpty() ? QStringLiteral("an unidentified session")
                                         : oldest.agentId)
            << "while following" << m_sessionId;
    emit permissionRequested(oldestId, oldest.request.value(QStringLiteral("toolName")).toString(),
                             reason);
}

void DshClient::drawApprovalNotice(const QString &eventId)
{
    auto pending = m_pendingWaterfalls.find(eventId);
    if (pending == m_pendingWaterfalls.end() || pending.value().rowDrawn)
        return;

    // The notice belongs in the transcript that is on screen now, and only ever
    // for the session being followed (offerPendingApproval() has already applied
    // that filter). resetFollowTranscript() cleared this flag when it cleared the
    // rows, so a rebuild draws the notice again rather than leaving the dialog
    // over history that does not mention it.
    const QString toolName =
        pending.value().request.value(QStringLiteral("toolName")).toString();
    // Marked before the row is appended: the append emits model signals, and no
    // handler downstream may re-enter and draw the same notice twice.
    pending.value().rowDrawn = true;
    m_transcript->append(QStringLiteral("notice"),
                         QStringLiteral("the agent is waiting for approval: %1")
                             .arg(toolName.isEmpty() ? QStringLiteral("a tool") : toolName));
}

void DshClient::releaseOfferedApproval()
{
    if (m_offeredApprovalId.isEmpty())
        return;

    // The request is not withdrawn — it is still pending and still blocking its
    // agent. Only the prompt stops being asked here, and permissionWithdrawn is
    // the UI's one "stop asking" signal (its handler matches on the eventId, so
    // it closes exactly this dialog and no other).
    const QString eventId = m_offeredApprovalId;
    m_offeredApprovalId.clear();
    qInfo() << "stopped offering approval" << eventId << "- retained for its own session";
    emit permissionWithdrawn(eventId);
}

void DshClient::offerPendingQuestion()
{
    // Same rule as an approval: a follow is still laying down its history, so
    // the notice would land above it. The snapshot path — and a stream that ends
    // before one — calls this again, so neither case hides the question.
    if (!m_followSnapshotSeen && !m_followStreamId.isEmpty())
        return;

    // One at a time, and "one" is one user-questions/request: the dialog answers
    // the whole array it carries under that eventId together, so the batch is
    // never split across prompts. The dialog may still be up while its row went
    // with a transcript rebuilt underneath it, so draw the row again rather than
    // leave a prompt over history that does not mention it.
    if (!m_offeredQuestionId.isEmpty()) {
        if (m_pendingWaterfalls.contains(m_offeredQuestionId)) {
            drawQuestionNotice(m_offeredQuestionId);
            return;
        }
        // Defensive: every removal path clears the offer, but a dangling id here
        // would block every later question forever.
        const QString stale = m_offeredQuestionId;
        m_offeredQuestionId.clear();
        qWarning() << "offered question vanished from the pending map:" << stale;
        emit permissionWithdrawn(stale);
    }

    QString oldestId;
    PendingWaterfall oldest;
    bool found = false;
    for (auto it = m_pendingWaterfalls.constBegin(); it != m_pendingWaterfalls.constEnd(); ++it) {
        const PendingWaterfall &candidate = it.value();
        if (candidate.event != QLatin1String("user-questions/request"))
            continue;
        if (!waterfallInFollowedSession(candidate))
            continue;
        if (found && candidate.seq >= oldest.seq)
            continue;
        oldestId = it.key();
        oldest = candidate;
        found = true;
    }
    if (!found)
        return;

    m_offeredQuestionId = oldestId;
    // The row is drawn where the question is surfaced, not where it arrived.
    drawQuestionNotice(oldestId);

    qInfo() << "offering question" << oldestId << "raised by"
            << (oldest.agentId.isEmpty() ? QStringLiteral("an unidentified session")
                                         : oldest.agentId)
            << "while following" << m_sessionId;
    emit questionsRequested(
        oldestId,
        oldest.request.value(QStringLiteral("questions")).toArray().toVariantList());
}

void DshClient::drawQuestionNotice(const QString &eventId)
{
    auto pending = m_pendingWaterfalls.find(eventId);
    if (pending == m_pendingWaterfalls.end() || pending.value().rowDrawn)
        return;

    // The notice belongs in the transcript that is on screen now, and only ever
    // for the session being followed (offerPendingQuestion() has already applied
    // that filter). resetFollowTranscript() cleared this flag when it cleared the
    // rows, so a rebuild draws the notice again rather than leaving the dialog
    // over history that does not mention it.
    //
    // Marked before the row is appended: the append emits model signals, and no
    // handler downstream may re-enter and draw the same notice twice.
    pending.value().rowDrawn = true;
    m_transcript->append(QStringLiteral("notice"),
                         QStringLiteral("the agent is asking a question"));
}

void DshClient::releaseOfferedQuestion()
{
    if (m_offeredQuestionId.isEmpty())
        return;

    // The request is not withdrawn — it is still pending and still blocking its
    // agent. Only the prompt stops being asked here; permissionWithdrawn is the
    // UI's one "stop asking" signal (its handler matches on the eventId, so it
    // closes exactly this question and no other).
    const QString eventId = m_offeredQuestionId;
    m_offeredQuestionId.clear();
    qInfo() << "stopped offering question" << eventId << "- retained for its own session";
    emit permissionWithdrawn(eventId);
}

void DshClient::answerWaterfall(const QString &eventId, const QString &value)
{
    QJsonObject outcome;
    if (value.isEmpty()) {
        // Delegation: hand the event to the next listener in the host's chain.
        outcome.insert(QStringLiteral("kind"), QStringLiteral("next"));
    } else {
        outcome.insert(QStringLiteral("kind"), QStringLiteral("result"));
        outcome.insert(QStringLiteral("value"), value);
    }
    postWaterfallOutcome(eventId, outcome);
}

void DshClient::answerQuestions(const QString &eventId, const QVariantList &answers)
{
    const auto pending = m_pendingWaterfalls.constFind(eventId);
    if (pending == m_pendingWaterfalls.constEnd()) {
        // Answering an event the host never raised, or already withdrew, would
        // report success for something that did not happen. Say so, and close
        // the prompt if it is somehow still up; no transcript row is written,
        // because the question belongs to a session this client cannot name.
        if (m_offeredQuestionId == eventId) {
            m_offeredQuestionId.clear();
            emit permissionWithdrawn(eventId);
            QTimer::singleShot(0, this, &DshClient::offerPendingQuestion);
        }
        setStatus(QStringLiteral("that question is no longer pending"));
        qWarning() << "no pending waterfall for question" << eventId;
        return;
    }

    if (pending.value().event != QLatin1String("user-questions/request")) {
        // This path speaks the question vocabulary. Answering an approval with
        // it would answer the wrong waterfall in the wrong dialect.
        setStatus(QStringLiteral("that waterfall is not a question"));
        qWarning() << "answerQuestions called for non-question waterfall" << eventId
                   << pending.value().event;
        return;
    }

    // The answer belongs to the session that asked. A question raised by another
    // session must not be answered from this transcript: the host journals the
    // answer into the asking agent's session, so answering here would write into
    // a conversation the reader is not looking at. Leave it pending rather than
    // answering, so the decision stays with the session that can show it.
    if (!pending.value().agentId.isEmpty() && pending.value().agentId != m_sessionId) {
        // It should never be offered here (offerPendingQuestion() filters), but a
        // prompt must not outlive its right to be answered either way.
        if (m_offeredQuestionId == eventId) {
            m_offeredQuestionId.clear();
            emit permissionWithdrawn(eventId);
            QTimer::singleShot(0, this, &DshClient::offerPendingQuestion);
        }
        setStatus(QStringLiteral("that question belongs to another session"));
        qWarning() << "refusing to answer question" << eventId << "raised by session"
                   << pending.value().agentId << "while following" << m_sessionId;
        return;
    }

    QJsonArray items;
    for (const QVariant &entry : answers) {
        const QVariantMap map = entry.toMap();
        QJsonArray selected;
        for (const QVariant &s : map.value(QStringLiteral("selected")).toList())
            selected.append(s.toString());
        QJsonObject item{
            {QStringLiteral("id"), map.value(QStringLiteral("id")).toString()},
            {QStringLiteral("selected"), selected},
        };
        const QString custom = map.value(QStringLiteral("custom")).toString().trimmed();
        if (!custom.isEmpty())
            item.insert(QStringLiteral("custom"), custom);
        items.append(item);
    }

    // The result value is an object here, not a string: the host's answer type is
    // `{answers: [{id, selected: string[], custom?: string}]}`, which is why this
    // cannot go through answerWaterfall's string form.
    postWaterfallOutcome(eventId, QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("result")},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("answers"), items}}},
    });
}

void DshClient::postWaterfallOutcome(const QString &eventId, const QJsonObject &outcome)
{
    if (eventId.isEmpty())
        return;

    if (m_eventClientId.isEmpty()) {
        qWarning() << "cannot answer" << eventId << "without a clientId";
        return;
    }

    const QJsonObject payload{
        {QStringLiteral("clientId"), m_eventClientId},
        {QStringLiteral("eventId"), eventId},
        {QStringLiteral("outcome"), outcome},
    };

    // A waterfall is answered by a *unary* call, not on the mux, even though it
    // arrived on the mux. The response is a normal envelope and its value is
    // undefined, so there is nothing to read back — only failure matters.
    QJsonObject envelope{
        {QStringLiteral("type"), QStringLiteral("client-request")},
        {QStringLiteral("rpcId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("method"), QStringLiteral("$events/result")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("args"), payload}}},
    };

    QNetworkReply *reply = m_network->post(
        makeRequest(QStringLiteral("/api/$events/result")),
        QJsonDocument(envelope).toJson(QJsonDocument::Compact));

    m_pendingWaterfalls.remove(eventId);
    // Answered here, so its session is no longer parked on a person — on any row,
    // not only the one whose card took the answer.
    refreshSessionMarks();

    // An answered approval is no longer offered. The signal closes exactly the
    // dialog that asked; the next one is offered on the next turn of the event
    // loop, because the QML button that called us closes its dialog after this
    // call returns and would otherwise shut the new prompt instead.
    if (m_offeredApprovalId == eventId) {
        m_offeredApprovalId.clear();
        emit permissionWithdrawn(eventId);
        QTimer::singleShot(0, this, &DshClient::offerPendingApproval);
    }

    // The same for an answered (or delegated) question: its dialog must close
    // and the next retained question for this session gets its turn. Deferred
    // for the same reason — the QML button that called us closes the old dialog
    // after this returns, and a synchronous offer would be shut instead.
    if (m_offeredQuestionId == eventId) {
        m_offeredQuestionId.clear();
        emit permissionWithdrawn(eventId);
        QTimer::singleShot(0, this, &DshClient::offerPendingQuestion);
    }

    // A failure row belongs to the transcript the answer was given from. If the
    // reader has followed another session by the time the reply lands, the
    // status line carries it instead of writing into a conversation that never
    // asked.
    const QString answeredIn = m_sessionId;
    connect(reply, &QNetworkReply::finished,
            this, [this, reply, eventId, outcome, answeredIn]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            // The agent stays blocked, so this is a real failure, not a cosmetic one.
            const QString message =
                QStringLiteral("could not answer %1 (%2)")
                    .arg(eventId.isEmpty() ? QStringLiteral("a waterfall") : eventId,
                         reply->errorString());
            if (m_sessionId == answeredIn)
                m_transcript->append(QStringLiteral("error"), message);
            else
                setStatus(message);
            qWarning() << "failed to answer waterfall" << eventId << reply->errorString();
            return;
        }
        qInfo() << "answered waterfall" << eventId
                << outcome.value(QStringLiteral("kind")).toString();
    });
}

void DshClient::delegateWaterfall(const QString &eventId)
{
    answerWaterfall(eventId, QString());
}

QVariantList DshClient::pendingWaterfalls() const
{
    QVariantList out;
    for (auto it = m_pendingWaterfalls.constBegin(); it != m_pendingWaterfalls.constEnd(); ++it) {
        out.append(QVariantMap{
            {QStringLiteral("eventId"), it.key()},
            {QStringLiteral("event"), it.value().event},
            {QStringLiteral("agentId"), it.value().agentId},
            {QStringLiteral("request"), it.value().request.toVariantMap()},
        });
    }
    return out;
}

// --- logical streams --------------------------------------------------------

QString DshClient::openStream(const QString &endpoint, const QJsonObject &args)
{
    const QString streamId = QStringLiteral("s%1").arg(m_nextStreamId++);
    m_streamEndpoints.insert(streamId, endpoint);
    // Logged here rather than at each call site. Two rounds were spent inferring
    // which streams were open from the handful of sites that happened to say so,
    // and both inferences were wrong — a boot log listing two streams said
    // nothing about the four that might be open.
    qInfo() << "opened stream" << streamId << endpoint
            << QJsonDocument(args).toJson(QJsonDocument::Compact);

    sendJson(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("open")},
        {QStringLiteral("streamId"), streamId},
        {QStringLiteral("endpoint"), endpoint},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("args"), args}}},
    });
    return streamId;
}

void DshClient::cancelStream(const QString &streamId)
{
    if (!m_streamEndpoints.contains(streamId))
        return;
    m_streamEndpoints.remove(streamId);
    sendJson(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("cancel")},
        {QStringLiteral("streamId"), streamId},
    });
}

void DshClient::onTextMessageReceived(const QString &message)
{
    // The last proof that this client's event loop was running and its socket was
    // healthy. A disconnect measures the age of this; a large gap means the loop
    // was starved, which is what the host's heartbeat terminates us for.
    if (m_lastFrameAt.isValid())
        m_lastFrameAt.restart();
    else
        m_lastFrameAt.start();

    const QJsonObject frame = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = frame.value(QStringLiteral("type")).toString();
    const QString streamId = frame.value(QStringLiteral("streamId")).toString();

    if (type == QLatin1String("item")) {
        const QJsonObject value = frame.value(QStringLiteral("value")).toObject();

        // Items on the event stream are the forwarded Cordis vocabulary, not a
        // session's frame vocabulary, so they are dispatched separately rather
        // than routed through the session stream consumer.
        if (!m_eventStreamId.isEmpty() && streamId == m_eventStreamId) {
            handleEventStreamItem(value);
            return;
        }

        // The Workspace registry has its own small vocabulary as well — a baseline
        // and five kinds of increment — so it is dispatched rather than fed to the
        // session-stream consumer.
        if (!m_workspaceStreamId.isEmpty() && streamId == m_workspaceStreamId) {
            handleWorkspaceItem(value);
            return;
        }

        // The terminal stream has its own small vocabulary, so it is dispatched
        // rather than fed to the session-stream consumer.
        if (!m_changesStreamId.isEmpty() && streamId == m_changesStreamId) {
            handleChangeFrame(value);
            return;
        }

        if (!m_jobStreamId.isEmpty() && streamId == m_jobStreamId) {
            handleJobRows(value);
            return;
        }

        if (!m_terminalStreamId.isEmpty() && streamId == m_terminalStreamId) {
            handleTerminalItem(value);
            return;
        }

        emit streamItem(streamId, value);
        return;
    }
    if (type == QLatin1String("error")) {
        const QJsonObject error = frame.value(QStringLiteral("error")).toObject();
        emit streamError(streamId,
                         error.value(QStringLiteral("code")).toString(),
                         error.value(QStringLiteral("message")).toString());
        return;
    }
    if (type == QLatin1String("end")) {
        m_streamEndpoints.remove(streamId);
        emit streamEnded(streamId);
        return;
    }

    qWarning() << "unknown mux frame type:" << type;
}
