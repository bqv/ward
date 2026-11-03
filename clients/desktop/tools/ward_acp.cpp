/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "WardClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <unistd.h>

namespace {

void writeJson(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    fwrite(line.constData(), 1, size_t(line.size()), stdout);
    fflush(stdout);
}

QJsonObject response(const QJsonValue &id, const QJsonObject &result) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("result"), result}};
}

QJsonObject errorResponse(const QJsonValue &id, int code, const QString &message) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("error"), QJsonObject{
                            {QStringLiteral("code"), code},
                            {QStringLiteral("message"), message}}}};
}

QString idKey(const QJsonValue &id) {
    return id.isString() ? id.toString() : QString::number(id.toInt());
}

} // namespace

class AcpServer : public QObject
{
    Q_OBJECT

public:
    explicit AcpServer(const QString &socket, QObject *parent = nullptr)
        : QObject(parent)
        , m_client(socket)
    {
        connect(&m_client, &WardClient::stateChanged, this, [this]() {
            if (m_client.state() == WardClient::Failed)
                fprintf(stderr, "ward-acp: %s\n", qPrintable(m_client.lastError()));
        });
        connect(&m_client, &WardClient::sessionChanged, this, &AcpServer::sessionChanged);
        connect(&m_client, &WardClient::eventReceived, this, &AcpServer::event);
        connect(&m_client, &WardClient::permissionRequested, this,
                &AcpServer::permissionRequested);
        QTimer::singleShot(0, &m_client, [this]() { m_client.connectToDaemon(); });
        m_stdin = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(m_stdin, &QSocketNotifier::activated, this, &AcpServer::readStdin);
    }

private slots:
    void readStdin() {
        char buffer[4096];
        ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got <= 0) {
            QCoreApplication::quit();
            return;
        }
        m_input.append(buffer, int(got));
        for (;;) {
            const int newline = m_input.indexOf('\n');
            if (newline < 0)
                break;
            const QByteArray line = m_input.left(newline).trimmed();
            m_input.remove(0, newline + 1);
            if (!line.isEmpty())
                handleLine(line);
        }
    }

    void sessionChanged() {
        if (m_pendingCreate.isUndefined())
            return;
        const QJsonValue id = m_pendingCreate;
        m_pendingCreate = QJsonValue::Undefined;
        m_session = m_client.sessionId();
        writeJson(response(id, QJsonObject{
            {QStringLiteral("sessionId"), m_session},
            {QStringLiteral("configOptions"), QJsonArray()}}));
    }

    void event(const QString &kind, const QVariantMap &payload) {
        if (kind == QLatin1String("session.prompted")) {
            notifyUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("user_message_chunk")},
                {QStringLiteral("content"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), payload.value(QStringLiteral("text")).toString()}}}});
        } else if (kind == QLatin1String("text.delta")) {
            notifyUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("agent_message_chunk")},
                {QStringLiteral("content"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), payload.value(QStringLiteral("text")).toString()}}}});
        } else if (kind == QLatin1String("tool.call")) {
            const QString tool = payload.value(QStringLiteral("tool")).toString();
            m_lastTool = QStringLiteral("call-%1")
                             .arg(payload.value(QStringLiteral("call")).toULongLong());
            notifyUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("tool_call")},
                {QStringLiteral("toolCallId"), m_lastTool},
                {QStringLiteral("title"), tool},
                {QStringLiteral("kind"), QStringLiteral("other")},
                {QStringLiteral("status"), QStringLiteral("pending")}});
        } else if (kind == QLatin1String("tool.result")) {
            notifyUpdate(QJsonObject{
                {QStringLiteral("sessionUpdate"), QStringLiteral("tool_call_update")},
                {QStringLiteral("toolCallId"), m_lastTool},
                {QStringLiteral("status"), QStringLiteral("completed")}});
        } else if (kind == QLatin1String("turn.end")) {
            if (!m_promptId.isUndefined()) {
                const QJsonValue id = m_promptId;
                m_promptId = QJsonValue::Undefined;
                writeJson(response(id, QJsonObject{
                    {QStringLiteral("stopReason"), QStringLiteral("end_turn")}}));
            }
        }
    }

    void permissionRequested(const QString &requestId, const QString &tool,
                             const QString &detail) {
        const QString acpId = QStringLiteral("perm-%1").arg(++m_nextPermission);
        m_permissionRequests.insert(acpId, requestId);
        writeJson(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), acpId},
            {QStringLiteral("method"), QStringLiteral("session/request_permission")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("sessionId"), m_session},
                {QStringLiteral("toolCall"), QJsonObject{
                    {QStringLiteral("toolCallId"), m_lastTool.isEmpty()
                         ? QStringLiteral("tool") : m_lastTool},
                    {QStringLiteral("title"), tool},
                    {QStringLiteral("kind"), QStringLiteral("other")},
                    {QStringLiteral("status"), QStringLiteral("pending")}}},
                {QStringLiteral("options"), QJsonArray{
                    QJsonObject{{QStringLiteral("optionId"), QStringLiteral("allow")},
                                {QStringLiteral("name"), QStringLiteral("Allow once")},
                                {QStringLiteral("kind"), QStringLiteral("allow_once")}},
                    QJsonObject{{QStringLiteral("optionId"), QStringLiteral("deny")},
                                {QStringLiteral("name"), QStringLiteral("Reject")},
                                {QStringLiteral("kind"), QStringLiteral("reject_once")}}}}}}});
        Q_UNUSED(detail);
    }

private:
    void notifyUpdate(const QJsonObject &update) {
        if (m_session.isEmpty())
            return;
        writeJson(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("method"), QStringLiteral("session/update")},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("sessionId"), m_session},
                {QStringLiteral("update"), update}}}});
    }

    void handleLine(const QByteArray &line) {
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            writeJson(errorResponse(QJsonValue::Null, -32700, QStringLiteral("parse error")));
            return;
        }
        const QJsonObject message = document.object();
        const QJsonValue id = message.value(QStringLiteral("id"));
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method.isEmpty()) {
            handleClientResponse(id, message.value(QStringLiteral("result")).toObject(),
                                 message.contains(QStringLiteral("error")));
            return;
        }
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        if (method == QLatin1String("initialize")) {
            writeJson(response(id, QJsonObject{
                {QStringLiteral("protocolVersion"), 1},
                {QStringLiteral("agentCapabilities"), QJsonObject{
                    {QStringLiteral("loadSession"), false},
                    {QStringLiteral("promptCapabilities"), QJsonObject{
                        {QStringLiteral("image"), false},
                        {QStringLiteral("audio"), false},
                        {QStringLiteral("embeddedContext"), false}}}}}}));
        } else if (method == QLatin1String("session/new")) {
            const QString cwd = params.value(QStringLiteral("cwd")).toString();
            if (!cwd.isEmpty() && QDir::isAbsolutePath(cwd))
                QDir::setCurrent(cwd);
            m_pendingCreate = id;
            m_client.createSessionNow();
        } else if (method == QLatin1String("session/prompt")) {
            m_session = params.value(QStringLiteral("sessionId")).toString();
            QString text;
            for (const QJsonValue &part : params.value(QStringLiteral("prompt")).toArray()) {
                if (part.toObject().value(QStringLiteral("type")).toString()
                        == QLatin1String("text"))
                    text += part.toObject().value(QStringLiteral("text")).toString();
            }
            if (m_session.isEmpty() || text.isEmpty()) {
                writeJson(errorResponse(id, -32602, QStringLiteral("sessionId and prompt are required")));
                return;
            }
            m_promptId = id;
            if (m_client.sessionId() != m_session)
                m_client.followSession(m_session);
            m_client.sendPrompt(text);
        } else if (method == QLatin1String("session/cancel")) {
            m_client.cancel();
        } else if (method == QLatin1String("session/resume")) {
            m_session = params.value(QStringLiteral("sessionId")).toString();
            m_client.followSession(m_session);
            writeJson(response(id, QJsonObject{
                {QStringLiteral("sessionId"), m_session},
                {QStringLiteral("configOptions"), QJsonArray()}}));
        } else {
            writeJson(errorResponse(id, -32601, QStringLiteral("method not found")));
        }
    }

    void handleClientResponse(const QJsonValue &id, const QJsonObject &result, bool error) {
        const QString key = idKey(id);
        if (!m_permissionRequests.contains(key))
            return;
        const QString requestId = m_permissionRequests.take(key);
        if (error) {
            m_client.respondToPermission(requestId, false);
            return;
        }
        const QString option = result.value(QStringLiteral("outcome")).toObject()
                                   .value(QStringLiteral("optionId")).toString();
        m_client.respondToPermission(requestId, option == QLatin1String("allow"));
    }

    WardClient m_client;
    QSocketNotifier *m_stdin = nullptr;
    QByteArray m_input;
    QJsonValue m_pendingCreate = QJsonValue::Undefined;
    QJsonValue m_promptId = QJsonValue::Undefined;
    QString m_session;
    QString m_lastTool;
    int m_nextPermission = 0;
    QHash<QString, QString> m_permissionRequests;
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QString socket;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket = argv[++i];
    }
    if (socket.isEmpty())
        socket = QString::fromLocal8Bit(qgetenv("WARD_SOCKET"));
    AcpServer server(socket);
    return app.exec();
}

#include "ward_acp.moc"
