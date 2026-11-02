/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "WardClient.h"

#include "AcpClient.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSocketNotifier>
#include <QTimer>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "core/util.h"
#include "proto/codec.h"
#include "proto/frame.h"
}

namespace {

QVariant cborToVariant(cbor_item_t *item)
{
    if (item == nullptr)
        return {};
    if (cbor_isa_uint(item))
        return QVariant::fromValue<qulonglong>(cbor_get_uint64(item));
    if (cbor_isa_negint(item))
        return QVariant::fromValue<qlonglong>(-1 - qlonglong(cbor_get_int(item)));
    if (cbor_is_bool(item))
        return cbor_get_bool(item);
    if (cbor_isa_string(item))
        return QString::fromUtf8(reinterpret_cast<const char *>(cbor_string_handle(item)),
                                 int(cbor_string_length(item)));
    if (cbor_isa_bytestring(item))
        return QByteArray(reinterpret_cast<const char *>(cbor_bytestring_handle(item)),
                          int(cbor_bytestring_length(item)));
    if (cbor_isa_array(item)) {
        QVariantList list;
        for (size_t i = 0; i < cbor_array_size(item); ++i)
            list.append(cborToVariant(cbor_array_handle(item)[i]));
        return list;
    }
    if (cbor_isa_map(item)) {
        QVariantMap map;
        struct cbor_pair *pairs = cbor_map_handle(item);
        for (size_t i = 0; i < cbor_map_size(item); ++i) {
            const QVariant key = cborToVariant(pairs[i].key);
            map.insert(key.toString(), cborToVariant(pairs[i].value));
        }
        return map;
    }
    return {};
}

cbor_item_t *variantToCbor(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        return wd_bool(value.toBool());
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return wd_uint(value.toULongLong());
    case QMetaType::QByteArray: {
        const QByteArray bytes = value.toByteArray();
        return cbor_build_bytestring(
            reinterpret_cast<const unsigned char *>(bytes.constData()),
            size_t(bytes.size()));
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        const QVariantList list = value.toList();
        cbor_item_t *array = wd_array(uint64_t(list.size()));
        for (const QVariant &entry : list) {
            cbor_item_t *item = variantToCbor(entry);
            if (item == nullptr)
                item = wd_text("");
            const bool pushed = cbor_array_push(array, item);
            (void)pushed;
            cbor_decref(&item);
        }
        return array;
    }
    case QMetaType::QVariantMap: {
        const QVariantMap map = value.toMap();
        cbor_item_t *out = wd_map(uint64_t(map.size()));
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            cbor_item_t *item = variantToCbor(it.value());
            wd_put(out, it.key().toUtf8().constData(), item != nullptr ? item : wd_text(""));
        }
        return out;
    }
    default:
        return wd_text(value.toString().toUtf8().constData());
    }
}

int writeAll(int fd, const void *data, size_t len)
{
    const char *p = static_cast<const char *>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {fd, POLLOUT, 0};
                if (poll(&pfd, 1, 2000) <= 0)
                    return -1;
                continue;
            }
            return -1;
        }
        off += size_t(n);
    }
    return 0;
}

int connectUnix(const QString &path, QString *error)
{
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const QByteArray bytes = path.toUtf8();
    if (bytes.size() >= int(sizeof(addr.sun_path))) {
        *error = QStringLiteral("socket path too long");
        return -1;
    }
    std::strncpy(addr.sun_path, bytes.constData(), sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = QString::fromLocal8Bit(strerror(errno));
        return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        *error = QStringLiteral("no daemon at %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        close(fd);
        return -1;
    }
    return fd;
}

int connectTcp(const QString &target, QString *error)
{
    const int colon = target.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) {
        *error = QStringLiteral("tcp target must be host:port");
        return -1;
    }
    const QByteArray host = target.left(colon).toUtf8();
    const int port = target.mid(colon + 1).toInt();
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.constData(), &addr.sin_addr) != 1) {
        *error = QStringLiteral("tcp host must be an IPv4 literal");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = QString::fromLocal8Bit(strerror(errno));
        return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        *error = QStringLiteral("no daemon at %1: %2").arg(target, QString::fromLocal8Bit(strerror(errno)));
        close(fd);
        return -1;
    }
    return fd;
}

int connectTarget(const QString &path, QString *error)
{
    if (path.startsWith(QLatin1String("tcp://")))
        return connectTcp(path.mid(6), error);
    return connectUnix(path, error);
}

bool readExact(int fd, void *data, size_t len)
{
    char *p = static_cast<char *>(data);
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 5000) <= 0)
            return false;
        ssize_t n = read(fd, p + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

QString defaultPath()
{
    const QByteArray env = qgetenv("WARD_SOCKET");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    char *path = wd_default_socket_path();
    if (path == nullptr)
        return {};
    const QString out = QString::fromLocal8Bit(path);
    free(path);
    return out;
}

} // namespace

WardClient::WardClient(const QString &socketPath, QObject *parent)
    : QObject(parent)
    , m_socketPath(socketPath.isEmpty() ? defaultPath() : socketPath)
    , m_transcript(new AcpTranscript(this))
{
    m_status = QStringLiteral("idle");
    m_busyEnter = QStringLiteral("queue");
    m_railExpanded = QSettings().value(QStringLiteral("ward/railExpanded")).toStringList();
    m_railCollapsed = QSettings().value(QStringLiteral("ward/railCollapsed")).toStringList();
}

WardClient::~WardClient()
{
    if (m_fd >= 0)
        close(m_fd);
}

void WardClient::setState(State state, const QString &status)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}

void WardClient::fail(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
    setState(Failed, message);
    if (m_notifier != nullptr) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

void WardClient::sendFrame(uint16_t type, uint64_t sub, uint64_t seq, cbor_item_t *body)
{
    size_t len = 0;
    uint8_t *bytes = wd_encode(body, &len);
    if (bytes == nullptr) {
        fail(QStringLiteral("cannot encode a frame"));
        return;
    }
    uint8_t header[WD_FRAME_HEADER];
    wd_frame_header(header, type, sub, seq, uint32_t(len));
    if (writeAll(m_fd, header, sizeof(header)) != 0 || writeAll(m_fd, bytes, len) != 0)
        fail(QStringLiteral("the daemon closed the connection"));
    free(bytes);
}

void WardClient::sendRequest(const QString &method, const QVariantMap &params)
{
    if (m_fd < 0)
        return;
    const quint64 id = m_nextId++;
    Pending pending{method};
    m_pending.insert(id, pending);
    cbor_item_t *body = wd_request(id, method.toUtf8().constData(), variantToCbor(params));
    sendFrame(WD_FRAME_REQUEST, 0, 0, body);
}

void WardClient::sendRequest(const QString &method)
{
    sendRequest(method, {});
}

void WardClient::connectToDaemon()
{
    if (m_socketPath.isEmpty()) {
        fail(QStringLiteral("no socket path; set WARD_SOCKET or pass --socket"));
        return;
    }
    setState(Connecting, QStringLiteral("connecting to %1").arg(m_socketPath));
    QString error;
    m_fd = connectTarget(m_socketPath, &error);
    if (m_fd < 0) {
        fail(error);
        return;
    }

    cbor_item_t *hello = wd_hello();
    size_t len = 0;
    uint8_t *bytes = wd_encode(hello, &len);
    if (bytes == nullptr) {
        fail(QStringLiteral("cannot encode hello"));
        return;
    }
    uint8_t header[WD_FRAME_HEADER];
    wd_frame_header(header, WD_FRAME_HELLO, 0, 0, uint32_t(len));
    const bool sent = writeAll(m_fd, header, sizeof(header)) == 0
                   && writeAll(m_fd, bytes, len) == 0;
    free(bytes);
    if (!sent) {
        fail(QStringLiteral("the daemon did not accept the handshake"));
        return;
    }

    if (!readExact(m_fd, header, sizeof(header))) {
        fail(QStringLiteral("no welcome from the daemon"));
        return;
    }
    const uint32_t bodyLen = uint32_t(header[0]) | (uint32_t(header[1]) << 8)
                           | (uint32_t(header[2]) << 16) | (uint32_t(header[3]) << 24);
    const uint16_t type = uint16_t(header[4]) | (uint16_t(header[5]) << 8);
    if (type != WD_FRAME_WELCOME || bodyLen == 0 || bodyLen > WD_MAX_BODY) {
        fail(QStringLiteral("the daemon refused the handshake"));
        return;
    }
    QByteArray welcome(int(bodyLen), Qt::Uninitialized);
    if (!readExact(m_fd, welcome.data(), size_t(bodyLen))) {
        fail(QStringLiteral("the welcome frame was truncated"));
        return;
    }
    cbor_item_t *root = wd_decode(reinterpret_cast<const uint8_t *>(welcome.constData()),
                                  size_t(welcome.size()));
    unsigned version = 0;
    uint64_t uid = 0;
    const bool ok = root != nullptr && wd_read_welcome(root, &version, &uid)
                 && version == WD_PROTOCOL_VERSION;
    wd_free(root);
    if (!ok) {
        fail(QStringLiteral("protocol version mismatch"));
        return;
    }
    m_handshaken = true;

    const int flags = fcntl(m_fd, F_GETFL, 0);
    fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &WardClient::onReadable);

    setState(Ready, QStringLiteral("ready"));
    m_sessionsLoading = true;
    emit sessionsLoadingChanged();
    sendRequest(QStringLiteral("session.list"), {{QStringLiteral("limit"), 1000}});
    refreshConfigOptions();
}

void WardClient::onReadable()
{
    for (;;) {
        char buffer[65536];
        const ssize_t n = read(m_fd, buffer, sizeof(buffer));
        if (n > 0) {
            m_in.append(buffer, int(n));
            continue;
        }
        if (n == 0) {
            fail(QStringLiteral("the daemon closed the connection"));
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        fail(QString::fromLocal8Bit(strerror(errno)));
        return;
    }

    while (m_in.size() >= int(WD_FRAME_HEADER)) {
        wd_frame frame;
        size_t used = 0;
        const enum wd_frame_status status = wd_frame_parse(
            reinterpret_cast<const uint8_t *>(m_in.constData()), size_t(m_in.size()),
            &frame, &used);
        if (status == WD_FRAME_PARTIAL)
            break;
        if (status == WD_FRAME_INVALID) {
            fail(QStringLiteral("the daemon sent an invalid frame"));
            return;
        }
        const QByteArray body(reinterpret_cast<const char *>(frame.body), int(frame.len));
        const uint16_t type = frame.type;
        const uint64_t sub = frame.sub;
        const uint64_t seq = frame.seq;
        m_in.remove(0, int(used));
        handleFrame(type, sub, seq, body);
        if (m_fd < 0)
            return;
    }
}

void WardClient::handleFrame(uint16_t type, uint64_t sub, uint64_t seq,
                             const QByteArray &body)
{
    cbor_item_t *root = wd_decode(reinterpret_cast<const uint8_t *>(body.constData()),
                                  size_t(body.size()));
    if (root == nullptr) {
        fail(QStringLiteral("the daemon sent a body that does not decode"));
        return;
    }
    if (type == WD_FRAME_RESPONSE) {
        uint64_t id = 0;
        bool ok = false;
        cbor_item_t *value = nullptr;
        if (!wd_read_response(root, &id, &ok, &value)) {
            wd_free(root);
            fail(QStringLiteral("the daemon sent a malformed response"));
            return;
        }
        const Pending pending = m_pending.take(id);
        const QVariant decoded = cborToVariant(value);
        QVariantMap error;
        if (!ok) {
            error = decoded.toMap();
            m_lastError = error.value(QStringLiteral("message")).toString();
            emit lastErrorChanged();
        }
        wd_free(root);
        handleResponse(pending.method, ok, decoded, error);
        return;
    }
    if (type == WD_FRAME_EVENT) {
        char *kind = nullptr;
        cbor_item_t *payload = nullptr;
        if (wd_read_event(root, &kind, &payload)) {
            const QString eventKind = QString::fromUtf8(kind);
            const QVariantMap decoded = cborToVariant(payload).toMap();
            free(kind);
            wd_free(root);
            handleEvent(sub, seq, eventKind, decoded);
            return;
        }
        wd_free(root);
        return;
    }
    wd_free(root);
}

void WardClient::handleResponse(const QString &method, bool ok, const QVariant &value,
                                const QVariantMap &error)
{
    if (!ok) {
        const QString name = error.value(QStringLiteral("name")).toString();
        const QString message = error.value(QStringLiteral("message")).toString();
        const QString text = name.isEmpty() ? message : QStringLiteral("%1: %2").arg(name, message);
        m_lastError = text;
        emit lastErrorChanged();
        setState(Ready, text);
        if (method == QLatin1String("session.list")) {
            m_sessionsLoading = false;
            emit sessionsLoadingChanged();
        }
        if (method == QLatin1String("session.resume"))
            m_openingSession = false, emit openingSessionChanged();
        if (method == QLatin1String("session.prompt")) {
            m_busy = false;
            emit busyChanged();
            m_turnOpen = false;
            emit turnOpenChanged();
        }
        if (method == QLatin1String("session.file_read")) {
            m_fileError = text;
            m_fileText.clear();
            m_fileBytes = 0;
            m_fileTruncated = false;
            emit fileErrorChanged();
            emit fileTextChanged();
            emit fileBytesChanged();
        }
        if (method == QLatin1String("session.file_read_blob"))
            emit blobRead(m_blobPath, m_blobOffset, QByteArray(), text);
        if (method == QLatin1String("session.file_list")) {
            m_fileError = text;
            m_fileEntries.clear();
            emit fileEntriesChanged();
            emit fileErrorChanged();
        }
        return;
    }

    const QVariantMap result = value.toMap();
    if (method == QLatin1String("session.list")) {
        m_sessionsLoading = false;
        emit sessionsLoadingChanged();
        setSessions(result.value(QStringLiteral("sessions")).toList());
    } else if (method == QLatin1String("session.create")) {
        const QVariantMap session = result.value(QStringLiteral("session")).toMap();
        applySession(session);
        const QString id = session.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            m_sessionId = id;
            subscribe(id);
            emit sessionChanged();
        }
    } else if (method == QLatin1String("session.resume")) {
        const QVariantMap session = result.value(QStringLiteral("session")).toMap();
        applySession(session);
        m_sub = result.value(QStringLiteral("sub")).toULongLong();
        m_lastSeq = result.value(QStringLiteral("last_seq")).toULongLong();
        m_subscribedSession = session.value(QStringLiteral("id")).toString();
        m_subscribing.clear();
        if (!m_subscribedSession.isEmpty())
            m_subBySession.insert(m_subscribedSession, m_sub);
        m_sessionId = m_subscribedSession;
        emit sessionChanged();
        if (!m_queuedPrompt.isEmpty()) {
            const QString text = m_queuedPrompt;
            m_queuedPrompt.clear();
            doPrompt(text);
        }
    } else if (method == QLatin1String("session.prompt")) {
        const QVariantMap session = result.value(QStringLiteral("session")).toMap();
        applySession(session);
        const QVariantMap approval = result.value(QStringLiteral("approval")).toMap();
        if (!approval.isEmpty()) {
            m_busy = false;
            emit busyChanged();
            setState(Ready, QStringLiteral("waiting for approval"));
        } else if (!m_turnOpen) {
            m_busy = false;
            emit busyChanged();
        }
    } else if (method == QLatin1String("model.list")) {
        const QVariantList models = result.value(QStringLiteral("models")).toList();
        QVariantList choices;
        QString current;
        for (const QVariant &entry : models) {
            const QVariantMap model = entry.toMap();
            const QString id = model.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;
            choices.append(QVariantMap{{QStringLiteral("value"), id},
                                       {QStringLiteral("label"), id}});
            if (current.isEmpty())
                current = id;
        }
        if (!choices.isEmpty()) {
            m_configOptions = QVariantList{QVariantMap{
                {QStringLiteral("id"), QStringLiteral("model")},
                {QStringLiteral("name"), QStringLiteral("Model")},
                {QStringLiteral("currentValue"), current},
                {QStringLiteral("choices"), choices}}};
            m_contextSize = 32768;
            emit contextSizeChanged();
            emit configOptionsChanged();
        }
    } else if (method == QLatin1String("session.compact")) {
        setState(Ready, QStringLiteral("compacted"));
        subscribe(m_sessionId);
    } else if (method == QLatin1String("session.rename")
               || method == QLatin1String("session.pin")
               || method == QLatin1String("session.archive")) {
        const QVariantMap session = result.value(QStringLiteral("session")).toMap();
        applySession(session);
    } else if (method == QLatin1String("job.list")) {
        m_jobs = result.value(QStringLiteral("jobs")).toList();
        emit jobsChanged();
    } else if (method == QLatin1String("schedule.list")) {
        m_scheduleTasks = result.value(QStringLiteral("schedules")).toList();
        m_scheduleLoading = false;
        emit scheduleChanged();
    } else if (method == QLatin1String("goal.set")) {
        m_goal = result.value(QStringLiteral("goal")).toMap();
        emit goalChanged();
    } else if (method == QLatin1String("session.file_list")) {
        const QVariantList entries = result.value(QStringLiteral("entries")).toList();
        QVariantList rows;
        for (const QVariant &entry : entries) {
            const QVariantMap item = entry.toMap();
            const QString name = item.value(QStringLiteral("name")).toString();
            if (name.isEmpty())
                continue;
            const QString kind = item.value(QStringLiteral("kind")).toString();
            const QString path = m_filePath.isEmpty()
                                     ? name
                                     : m_filePath + QLatin1Char('/') + name;
            rows.append(QVariantMap{
                {QStringLiteral("name"), name},
                {QStringLiteral("path"), path},
                {QStringLiteral("type"), kind == QLatin1String("dir")
                                              ? QStringLiteral("directory")
                                              : QStringLiteral("file")},
                {QStringLiteral("depth"), 0},
                {QStringLiteral("expanded"), false},
                {QStringLiteral("size"), item.value(QStringLiteral("size"))}});
        }
        m_fileEntries = rows;
        m_fileTruncated = result.value(QStringLiteral("truncated")).toBool();
        m_fileError.clear();
        emit fileEntriesChanged();
        emit fileTruncatedChanged();
        emit fileErrorChanged();
    } else if (method == QLatin1String("session.file_read_blob")) {
        emit blobRead(result.value(QStringLiteral("path")).toString(),
                      qint64(result.value(QStringLiteral("offset")).toULongLong()),
                      result.value(QStringLiteral("data")).toByteArray(), QString());
    } else if (method == QLatin1String("session.file_read")) {
        m_fileSelectedPath = result.value(QStringLiteral("path")).toString();
        m_fileText = result.value(QStringLiteral("text")).toString();
        m_fileBytes = int(result.value(QStringLiteral("size")).toULongLong());
        m_fileTruncated = false;
        m_fileError.clear();
        emit fileSelectedPathChanged();
        emit fileTextChanged();
        emit fileBytesChanged();
        emit fileTruncatedChanged();
        emit fileErrorChanged();
    } else if (method == QLatin1String("approval.respond")) {
        if (!m_pendingApproval.isEmpty()) {
            emit permissionWithdrawn(m_pendingApproval);
            m_pendingApproval.clear();
        }
        m_busy = true;
        emit busyChanged();
        setState(Ready, QStringLiteral("thinking…"));
    } else if (method == QLatin1String("terminal.open")) {
        m_terminalId = result.value(QStringLiteral("terminal")).toString();
        m_terminalOpen = !m_terminalId.isEmpty();
        if (m_terminalOpen) {
            emit terminalState(QStringLiteral("running"), 0);
            setState(Ready, QStringLiteral("terminal open"));
        }
        emit terminalChanged();
    } else if (method == QLatin1String("terminal.write")) {
        if (!result.value(QStringLiteral("written")).toBool())
            emit railNotice(QStringLiteral("the terminal refused those bytes"), true);
    } else if (method == QLatin1String("terminal.resize")) {
        if (!result.value(QStringLiteral("resized")).toBool())
            emit railNotice(QStringLiteral("the terminal refused the new size"), true);
    } else if (method == QLatin1String("terminal.close")) {
        m_terminalOpen = false;
        m_terminalId.clear();
        emit terminalState(QStringLiteral("closed"), 0);
        emit terminalChanged();
    }
}

void WardClient::handleEvent(uint64_t sub, uint64_t seq, const QString &kind,
                             const QVariantMap &payload)
{
    Q_UNUSED(seq);
    emit eventReceived(kind, payload);

    if (kind == QLatin1String("session.updated")) {
        applySession(payload.value(QStringLiteral("session")).toMap());
    } else if (kind == QLatin1String("session.prompted")) {
        appendRow(QStringLiteral("user"), payload.value(QStringLiteral("text")).toString());
    } else if (kind == QLatin1String("text.delta")) {
        m_transcript->appendOrExtend(QStringLiteral("agent"),
                                     payload.value(QStringLiteral("text")).toString(),
                                     QDateTime::currentMSecsSinceEpoch(), m_turn);
    } else if (kind == QLatin1String("turn.start")) {
        m_turn = int(payload.value(QStringLiteral("turn")).toULongLong());
        m_turnOpen = true;
        emit turnOpenChanged();
        m_transcript->noteTurn(m_turn, true, QDateTime::currentMSecsSinceEpoch());
    } else if (kind == QLatin1String("turn.end")) {
        m_transcript->noteTurn(m_turn, false);
        m_turnOpen = false;
        m_busy = false;
        emit turnOpenChanged();
        emit busyChanged();
        m_openingSession = false;
        emit openingSessionChanged();
        setState(Ready, QStringLiteral("ready"));
    } else if (kind == QLatin1String("tool.call")) {
        const QString tool = payload.value(QStringLiteral("tool")).toString();
        const QString call = QString::number(payload.value(QStringLiteral("call")).toULongLong());
        appendRow(QStringLiteral("tool"), tool, tool, QStringLiteral("pending"), tool, call,
                  m_turn);
        m_trajectory.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("tool")},
                                        {QStringLiteral("title"), tool},
                                        {QStringLiteral("text"), tool}});
        emit trajectoryChanged();
    } else if (kind == QLatin1String("tool.result")) {
        const QString tool = payload.value(QStringLiteral("tool")).toString();
        appendRow(QStringLiteral("tool"), tool, tool, QStringLiteral("completed"), tool, {},
                  m_turn);
    } else if (kind == QLatin1String("approval.requested")) {
        const QString requestId = QStringLiteral("%1:%2")
            .arg(payload.value(QStringLiteral("call")).toULongLong())
            .arg(payload.value(QStringLiteral("epoch")).toULongLong());
        m_pendingApproval = requestId;
        if (!m_pendingInteractionSessions.contains(m_sessionId))
            m_pendingInteractionSessions.append(m_sessionId);
        emit sessionMarksChanged();
        m_busy = false;
        emit busyChanged();
        setState(Ready, QStringLiteral("waiting for approval"));
        emit permissionRequested(requestId, QStringLiteral("tool"),
                                 payload.value(QStringLiteral("reason")).toString());
    } else if (kind == QLatin1String("approval.resolved")) {
        const QString requestId = QStringLiteral("%1:%2")
            .arg(payload.value(QStringLiteral("call")).toULongLong())
            .arg(payload.value(QStringLiteral("epoch")).toULongLong());
        emit permissionWithdrawn(requestId);
        m_pendingApproval.clear();
        m_pendingInteractionSessions.removeAll(m_sessionId);
        emit sessionMarksChanged();
    } else if (kind == QLatin1String("error")) {
        const QString message = payload.value(QStringLiteral("message")).toString();
        m_lastError = message;
        emit lastErrorChanged();
        appendRow(QStringLiteral("error"),
                  QStringLiteral("%1: %2")
                      .arg(payload.value(QStringLiteral("name")).toString(), message));
    } else if (kind == QLatin1String("session.compacted")) {
        appendRow(QStringLiteral("notice"), QStringLiteral("session compacted"));
        m_transcript->clear();
        m_trajectory.clear();
        emit trajectoryChanged();
    } else if (kind == QLatin1String("goal.set")) {
        m_goal = QVariantMap{{QStringLiteral("objective"),
                              payload.value(QStringLiteral("text")).toString()}};
        emit goalChanged();
    } else if (kind == QLatin1String("goal.cleared")) {
        m_goal.clear();
        emit goalChanged();
    } else if (kind == QLatin1String("turn.timings")) {
        const int prompt = int(payload.value(QStringLiteral("prompt_n")).toULongLong());
        const int predicted = int(payload.value(QStringLiteral("predicted_n")).toULongLong());
        if (prompt + predicted > 0) {
            m_contextUsed = prompt + predicted;
            emit contextChanged();
        }
    } else if (kind == QLatin1String("job.updated")) {
        m_jobs = QVariantList{payload};
        emit jobsChanged();
    } else if (kind == QLatin1String("subagent.updated")) {
        const QVariantMap updated = payload;
        if (!updated.isEmpty())
            appendRow(QStringLiteral("notice"),
                      QStringLiteral("subagent %1").arg(updated.value(QStringLiteral("state")).toString()));
    } else if (kind == QLatin1String("replay.end")) {
        m_openingSession = false;
        emit openingSessionChanged();
        if (!m_busy)
            setState(Ready, QStringLiteral("ready"));
    } else if (kind == QLatin1String("terminal.output")) {
        const QString handle = payload.value(QStringLiteral("terminal")).toString();
        if (!handle.isEmpty() && handle == m_terminalId) {
            if (payload.value(QStringLiteral("eof")).toBool()) {
                m_terminalOpen = false;
                m_terminalId.clear();
                emit terminalState(QStringLiteral("exited"), -1);
                emit terminalChanged();
            } else {
                emit terminalOutput(payload.value(QStringLiteral("data")).toString().toUtf8());
            }
        }
    } else if (kind == QLatin1String("session.modules")
               || kind == QLatin1String("session.upgraded")) {
        appendRow(QStringLiteral("notice"), kind);
    }
    Q_UNUSED(sub);
}

void WardClient::applySession(const QVariantMap &session)
{
    if (session.isEmpty())
        return;
    const QString id = session.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    QVariantMap row;
    row.insert(QStringLiteral("sessionId"), id);
    const QString name = session.value(QStringLiteral("name")).toString();
    row.insert(QStringLiteral("title"), name.isEmpty() ? QStringLiteral("Untitled") : name);
    row.insert(QStringLiteral("cwd"), session.value(QStringLiteral("cwd")).toString());
    row.insert(QStringLiteral("model"), session.value(QStringLiteral("model")).toString());
    row.insert(QStringLiteral("updatedAt"), session.value(QStringLiteral("created")));
    row.insert(QStringLiteral("pinned"), session.value(QStringLiteral("pinned")));
    row.insert(QStringLiteral("archived"), session.value(QStringLiteral("archived")));
    row.insert(QStringLiteral("parentSessionId"), QString());
    row.insert(QStringLiteral("running"), m_busy && id == m_sessionId);
    row.insert(QStringLiteral("waiting"), !m_pendingApproval.isEmpty() && id == m_sessionId);

    bool replaced = false;
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions.at(i).toMap().value(QStringLiteral("sessionId")).toString() == id) {
            m_sessions[i] = row;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        m_sessions.prepend(row);
    emit sessionsChanged();

    if (id == m_sessionId) {
        const QString model = row.value(QStringLiteral("model")).toString();
        if (!model.isEmpty())
            m_modelNotice = model;
        emit modelNoticeChanged();
    }
}

void WardClient::setSessions(const QVariantList &sessions)
{
    QVariantList rows;
    for (const QVariant &entry : sessions) {
        const QVariantMap session = entry.toMap();
        const QString id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QString name = session.value(QStringLiteral("name")).toString();
        rows.append(QVariantMap{
            {QStringLiteral("sessionId"), id},
            {QStringLiteral("title"), name.isEmpty() ? QStringLiteral("Untitled") : name},
            {QStringLiteral("cwd"), session.value(QStringLiteral("cwd"))},
            {QStringLiteral("model"), session.value(QStringLiteral("model"))},
            {QStringLiteral("updatedAt"), session.value(QStringLiteral("created"))},
            {QStringLiteral("pinned"), session.value(QStringLiteral("pinned"))},
            {QStringLiteral("archived"), session.value(QStringLiteral("archived"))},
            {QStringLiteral("parentSessionId"), QString()},
            {QStringLiteral("running"), false},
            {QStringLiteral("waiting"), false}});
    }
    m_sessions = rows;
    emit sessionsChanged();
}

void WardClient::subscribe(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;
    if (m_subscribing == sessionId)
        return;
    if (!m_terminalId.isEmpty())
        closeTerminal();
    m_subscribing = sessionId;
    clearTranscript();
    m_openingSession = true;
    emit openingSessionChanged();
    m_subscribedSession.clear();
    m_sub = 0;
    sendRequest(QStringLiteral("session.resume"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("from"), 0}});
}

void WardClient::createSession()
{
    m_pendingSession = false;
    emit pendingSessionChanged();
    sendRequest(QStringLiteral("session.create"),
                {{QStringLiteral("cwd"), QDir::currentPath()}});
}

void WardClient::createSessionInWorkspace(const QString &workspaceId)
{
    Q_UNUSED(workspaceId);
    createSession();
}

void WardClient::followSession(const QString &sessionId)
{
    if (sessionId.isEmpty())
        return;
    m_sessionId = sessionId;
    subscribe(sessionId);
    emit sessionChanged();
    m_pendingSession = false;
    emit pendingSessionChanged();
    if (m_subscribing != sessionId)
        subscribe(sessionId);
}

void WardClient::sendPrompt(const QString &text)
{
    sendPrompt(text, QString());
}

void WardClient::sendPrompt(const QString &text, const QString &mode)
{
    Q_UNUSED(mode);
    if (text.isEmpty())
        return;
    if (m_sessionId.isEmpty()) {
        m_queuedPrompt = text;
        createSession();
        return;
    }
    if (m_sub == 0 || m_subscribedSession != m_sessionId
            || m_subscribing == m_sessionId) {
        m_queuedPrompt = text;
        if (m_subscribing != m_sessionId)
            subscribe(m_sessionId);
        return;
    }
    doPrompt(text);
}

void WardClient::doPrompt(const QString &text)
{
    m_busy = true;
    m_turnOpen = true;
    emit busyChanged();
    emit turnOpenChanged();
    setState(Ready, QStringLiteral("thinking…"));
    sendRequest(QStringLiteral("session.prompt"),
                {{QStringLiteral("id"), m_sessionId}, {QStringLiteral("text"), text}});
}

void WardClient::cancel()
{
    if (m_sessionId.isEmpty())
        return;
    sendRequest(QStringLiteral("session.cancel"), {{QStringLiteral("id"), m_sessionId}});
    m_busy = false;
    emit busyChanged();
    setState(Ready, QStringLiteral("cancelled"));
}

QStringList WardClient::slashCommands() const
{
    return {QStringLiteral("/help"), QStringLiteral("/rules"), QStringLiteral("/skills"),
            QStringLiteral("/read"), QStringLiteral("/write"), QStringLiteral("/grep"),
            QStringLiteral("/search"), QStringLiteral("/mcp"), QStringLiteral("/recall"),
            QStringLiteral("/py"), QStringLiteral("/plan"), QStringLiteral("/compact")};
}

void WardClient::runCommand(const QString &line)
{
    if (line.startsWith(QLatin1String("/compact"))) {
        if (!m_sessionId.isEmpty())
            sendRequest(QStringLiteral("session.compact"),
                        {{QStringLiteral("id"), m_sessionId}});
        return;
    }
    if (line.startsWith(QLatin1String("/permission"))) {
        m_accessNotice = QStringLiteral("ward has no permission presets; its policy is server-side");
        emit accessNoticeChanged();
        return;
    }
    sendPrompt(line);
}

void WardClient::respondToPermission(const QString &requestId, bool allow)
{
    const QStringList parts = requestId.split(QLatin1Char(':'));
    if (parts.size() != 2 || m_sessionId.isEmpty())
        return;
    sendRequest(QStringLiteral("approval.respond"),
                {{QStringLiteral("session"), m_sessionId},
                 {QStringLiteral("call"), parts.at(0).toULongLong()},
                 {QStringLiteral("epoch"), parts.at(1).toULongLong()},
                 {QStringLiteral("allow"), allow}});
}

void WardClient::answerQuestions(const QString &eventId, const QVariantList &answers)
{
    Q_UNUSED(eventId);
    Q_UNUSED(answers);
}

void WardClient::loadEarlierHistory()
{
    m_loadingEarlier = false;
    emit hasEarlierChanged();
}

void WardClient::refreshConfigOptions()
{
    if (m_fd >= 0)
        sendRequest(QStringLiteral("model.list"));
}

void WardClient::refreshPermissionPresets()
{
    m_permissionPresets.clear();
    m_accessNotice = QStringLiteral("ward decides policy in its policy module; no presets");
    emit permissionPresetsChanged();
    emit accessNoticeChanged();
}

void WardClient::refreshModelCatalog()
{
    refreshConfigOptions();
}

void WardClient::setPermissionPreset(const QString &value)
{
    Q_UNUSED(value);
    refreshPermissionPresets();
}

void WardClient::setConfigOption(const QString &configId, const QString &value)
{
    if (configId == QLatin1String("model") && !m_sessionId.isEmpty())
        sendRequest(QStringLiteral("session.set_model"),
                    {{QStringLiteral("id"), m_sessionId},
                     {QStringLiteral("model"), value}});
}

void WardClient::pinSession(const QString &sessionId)
{
    sendRequest(QStringLiteral("session.pin"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("pinned"), true}});
}

void WardClient::unpinSession(const QString &sessionId)
{
    sendRequest(QStringLiteral("session.pin"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("pinned"), false}});
}

void WardClient::archiveSession(const QString &sessionId)
{
    sendRequest(QStringLiteral("session.archive"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("archived"), true}});
}

void WardClient::unarchiveSession(const QString &sessionId)
{
    sendRequest(QStringLiteral("session.archive"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("archived"), false}});
}

void WardClient::renameSession(const QString &name, const QString &sessionId)
{
    sendRequest(QStringLiteral("session.rename"),
                {{QStringLiteral("id"), sessionId}, {QStringLiteral("name"), name}});
}

void WardClient::renameSession(const QString &name)
{
    if (!m_sessionId.isEmpty())
        renameSession(name, m_sessionId);
}

void WardClient::restartSelf()
{
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    if (m_notifier != nullptr) {
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    m_pending.clear();
    connectToDaemon();
}

void WardClient::goalAction(const QString &action)
{
    if (m_sessionId.isEmpty())
        return;
    if (action == QLatin1String("clear")) {
        sendRequest(QStringLiteral("goal.clear"), {{QStringLiteral("session"), m_sessionId}});
        return;
    }
    sendRequest(QStringLiteral("goal.set"),
                {{QStringLiteral("session"), m_sessionId}, {QStringLiteral("text"), action}});
}

void WardClient::killJob(const QString &jobId)
{
    sendRequest(QStringLiteral("job.kill"), {{QStringLiteral("job"), jobId}});
}

void WardClient::refreshScheduleTasks()
{
    m_scheduleLoading = true;
    emit scheduleChanged();
    sendRequest(QStringLiteral("schedule.list"));
}

void WardClient::refreshPluginInventory()
{
    m_pluginsLoading = false;
    m_pluginsNotice = QStringLiteral("ward has no plugin bundles");
    emit pluginsChanged();
}

void WardClient::setRailExpandedSessions(const QStringList &entries)
{
    m_railExpanded = entries;
    QSettings().setValue(QStringLiteral("ward/railExpanded"), entries);
}

void WardClient::setRailCollapsed(const QStringList &entries)
{
    m_railCollapsed = entries;
    QSettings().setValue(QStringLiteral("ward/railCollapsed"), entries);
}

void WardClient::setBusyEnter(const QString &mode)
{
    if (m_busyEnter == mode)
        return;
    m_busyEnter = mode;
    QSettings().setValue(QStringLiteral("ward/busyEnter"), mode);
    emit busyEnterChanged();
}

void WardClient::loadAttachment(const QString &path)
{
    Q_UNUSED(path);
}

QString WardClient::attachmentUrl(const QString &ref) const
{
    return ref;
}

void WardClient::deleteScheduleTask(const QString &id)
{
    sendRequest(QStringLiteral("schedule.delete"), {{QStringLiteral("schedule"), id}});
}

void WardClient::saveScheduleTask(const QString &id, const QString &title,
                                  const QString &prompt)
{
    Q_UNUSED(title);
    if (!m_sessionId.isEmpty())
        sendRequest(QStringLiteral("schedule.set"),
                    {{QStringLiteral("session"), m_sessionId},
                     {QStringLiteral("cron"), QStringLiteral("@every 3600000")},
                     {QStringLiteral("text"), prompt}});
    Q_UNUSED(id);
}


void WardClient::listFileReferences(const QString &prefix)
{
    Q_UNUSED(prefix);
}

void WardClient::listDirectory(const QString &path)
{
    if (m_sessionId.isEmpty()) {
        m_fileError = QStringLiteral("no session to list files in");
        emit fileErrorChanged();
        return;
    }
    m_filePath = path;
    m_fileError.clear();
    m_fileEntries.clear();
    m_fileSelectedPath.clear();
    emit filePathChanged();
    emit fileEntriesChanged();
    emit fileSelectedPathChanged();
    emit fileErrorChanged();
    sendRequest(QStringLiteral("session.file_list"),
                {{QStringLiteral("id"), m_sessionId},
                 {QStringLiteral("path"), path}});
}

void WardClient::toggleDirectory(const QString &path)
{
    listDirectory(path);
}

void WardClient::readBlob(const QString &path, qint64 offset, qint64 len)
{
    if (m_sessionId.isEmpty()) {
        emit blobRead(path, offset, QByteArray(), QStringLiteral("no session to read a file in"));
        return;
    }
    if (path.isEmpty() || len <= 0) {
        emit blobRead(path, offset, QByteArray(), QStringLiteral("path and len are required"));
        return;
    }
    m_blobPath = path;
    m_blobOffset = offset;
    m_blobLen = len;
    sendRequest(QStringLiteral("session.file_read_blob"),
                {{QStringLiteral("id"), m_sessionId},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("offset"), offset},
                 {QStringLiteral("len"), len}});
}

void WardClient::readFile(const QString &path)
{
    if (m_sessionId.isEmpty()) {
        m_fileError = QStringLiteral("no session to read a file in");
        emit fileErrorChanged();
        return;
    }
    if (path.isEmpty())
        return;
    m_fileSelectedPath = path;
    m_fileError.clear();
    m_fileText.clear();
    m_fileBytes = 0;
    m_fileTruncated = false;
    emit fileSelectedPathChanged();
    emit fileErrorChanged();
    emit fileTextChanged();
    emit fileBytesChanged();
    sendRequest(QStringLiteral("session.file_read"),
                {{QStringLiteral("id"), m_sessionId},
                 {QStringLiteral("path"), path}});
}

void WardClient::goUpDirectory()
{
    if (m_filePath.isEmpty())
        return;
    const int slash = m_filePath.lastIndexOf(QLatin1Char('/'));
    listDirectory(slash < 0 ? QString() : m_filePath.left(slash));
}

void WardClient::clearChangedFiles()
{
    if (m_changedFiles.isEmpty())
        return;
    m_changedFiles.clear();
    emit changedFilesChanged();
}

void WardClient::clearPendingAttachments()
{
    if (m_pendingAttachments.isEmpty())
        return;
    m_pendingAttachments.clear();
    emit pendingAttachmentsChanged();
}

void WardClient::pasteAttachment()
{
    emit railNotice(QStringLiteral("ward has no attachment staging"), true);
}

void WardClient::uploadAttachment(const QUrl &url)
{
    Q_UNUSED(url);
    emit railNotice(QStringLiteral("ward has no attachment staging"), true);
}

void WardClient::updateQueueItem(const QString &itemId, const QString &kind)
{
    Q_UNUSED(itemId);
    Q_UNUSED(kind);
}

void WardClient::editGoal(const QString &objective, int maxGoalRounds)
{
    Q_UNUSED(maxGoalRounds);
    goalAction(objective);
}

void WardClient::loadScheduleHistory(const QString &id)
{
    Q_UNUSED(id);
    m_scheduleHistory.clear();
    emit scheduleHistoryChanged();
}

void WardClient::setBundleEnabled(const QString &name, bool enabled)
{
    Q_UNUSED(name);
    Q_UNUSED(enabled);
    emit railNotice(QStringLiteral("ward has no plugin bundles"), true);
}

void WardClient::replay(uint64_t sub, uint64_t from)
{
    if (m_fd < 0)
        return;
    cbor_item_t *control = wd_map(2);
    wd_put(control, "replay", wd_uint(sub));
    wd_put(control, "from", wd_uint(from));
    sendFrame(WD_FRAME_CONTROL, 0, 0, control);
}

void WardClient::openTerminal(int cols, int rows)
{
    if (m_sessionId.isEmpty()) {
        emit railNotice(QStringLiteral("no session to open a terminal in"), true);
        return;
    }
    if (!m_terminalId.isEmpty())
        return;
    m_terminalCols = qMax(1, cols);
    m_terminalRows = qMax(1, rows);
    emit terminalSizeChanged();
    sendRequest(QStringLiteral("terminal.open"),
                {{QStringLiteral("id"), m_sessionId},
                 {QStringLiteral("cols"), m_terminalCols},
                 {QStringLiteral("rows"), m_terminalRows}});
}

void WardClient::writeTerminal(const QByteArray &data)
{
    if (m_terminalId.isEmpty() || data.isEmpty())
        return;
    sendRequest(QStringLiteral("terminal.write"),
                {{QStringLiteral("terminal"), m_terminalId},
                 {QStringLiteral("data"), QString::fromUtf8(data)}});
}

void WardClient::resizeTerminal(int cols, int rows)
{
    cols = qMax(1, cols);
    rows = qMax(1, rows);
    if (m_terminalId.isEmpty() || (cols == m_terminalCols && rows == m_terminalRows))
        return;
    m_terminalCols = cols;
    m_terminalRows = rows;
    emit terminalSizeChanged();
    sendRequest(QStringLiteral("terminal.resize"),
                {{QStringLiteral("terminal"), m_terminalId},
                 {QStringLiteral("cols"), cols},
                 {QStringLiteral("rows"), rows}});
}

void WardClient::closeTerminal()
{
    if (m_terminalId.isEmpty())
        return;
    const QString handle = m_terminalId;
    m_terminalOpen = false;
    m_terminalId.clear();
    emit terminalState(QStringLiteral("closed"), 0);
    emit terminalChanged();
    sendRequest(QStringLiteral("terminal.close"),
                {{QStringLiteral("terminal"), handle}});
}

void WardClient::wardCall(const QString &method, const QVariantMap &params)
{
    sendRequest(method, params);
}

QStringList WardClient::protocolMethods() const
{
    return {QStringLiteral("session.create"), QStringLiteral("session.list"),
            QStringLiteral("session.get"), QStringLiteral("session.resume"),
            QStringLiteral("session.file_read"), QStringLiteral("session.file_read_blob"),
            QStringLiteral("session.file_list"),
            QStringLiteral("session.fork"),
            QStringLiteral("session.rename"),
            QStringLiteral("session.archive"), QStringLiteral("session.pin"),
            QStringLiteral("session.search"), QStringLiteral("session.prompt"),
            QStringLiteral("session.cancel"), QStringLiteral("session.compact"),
            QStringLiteral("session.checkpoint"), QStringLiteral("session.undo"),
            QStringLiteral("session.set_model"), QStringLiteral("session.set_system"),
            QStringLiteral("session.upgrade"), QStringLiteral("prompt.preview"),
            QStringLiteral("session.undo"), QStringLiteral("session.search"),
            QStringLiteral("session.checkpoint"),
            QStringLiteral("model.list"), QStringLiteral("model.select"),
            QStringLiteral("model.probe"), QStringLiteral("approval.respond"),
            QStringLiteral("subagent.prompt"), QStringLiteral("subagent.interrupt"),
            QStringLiteral("job.list"), QStringLiteral("job.kill"),
            QStringLiteral("schedule.list"), QStringLiteral("schedule.set"),
            QStringLiteral("schedule.delete"), QStringLiteral("goal.set"),
            QStringLiteral("goal.clear"), QStringLiteral("terminal.open"),
            QStringLiteral("terminal.write"), QStringLiteral("terminal.resize"),
            QStringLiteral("terminal.close"), QStringLiteral("attachment.add"),
            QStringLiteral("attachment.list"), QStringLiteral("module.list"),
            QStringLiteral("module.reload"), QStringLiteral("stats.session"),
            QStringLiteral("stats.global"), QStringLiteral("device.pair"),
            QStringLiteral("device.enrol"), QStringLiteral("device.list"),
            QStringLiteral("device.revoke")};
}

void WardClient::appendRow(const QString &kind, const QString &text, const QString &title,
                           const QString &status, const QString &detail, const QString &key,
                           int turn)
{
    m_transcript->append(kind, text, title, status, detail, key,
                         QDateTime::currentMSecsSinceEpoch(), turn);
}

void WardClient::clearTranscript()
{
    m_transcript->clear();
    m_trajectory.clear();
    emit trajectoryChanged();
    m_turn = -1;
}
