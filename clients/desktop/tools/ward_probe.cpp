/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "WardClient.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption socketOption(QStringList{QStringLiteral("socket")},
                                    QStringLiteral("ward socket path or tcp://host:port"),
                                    QStringLiteral("path"));
    QCommandLineOption cwdOption(QStringList{QStringLiteral("cwd")},
                                 QStringLiteral("working directory for a new session"),
                                 QStringLiteral("dir"), QDir::currentPath());
    QCommandLineOption promptOption(QStringList{QStringLiteral("prompt")},
                                    QStringLiteral("prompt to send once a session exists"),
                                    QStringLiteral("text"));
    QCommandLineOption sessionOption(QStringList{QStringLiteral("session")},
                                     QStringLiteral("follow an existing session and replay it"),
                                     QStringLiteral("id"));
    QCommandLineOption approveOption(QStringList{QStringLiteral("approve")},
                                     QStringLiteral("allow every approval the turn asks for"));
    parser.addOption(socketOption);
    parser.addOption(cwdOption);
    parser.addOption(promptOption);
    parser.addOption(sessionOption);
    parser.addOption(approveOption);
    parser.process(app);

    WardClient client(parser.value(socketOption));
    QString text;
    bool prompted = false;
    bool failed = false;
    const bool approve = parser.isSet(approveOption);

    QObject::connect(&client, &WardClient::lastErrorChanged, &app, [&]() {
        if (!client.lastError().isEmpty()) {
            QTextStream(stderr) << "probe: " << client.lastError() << "\n";
            failed = true;
        }
    });
    QObject::connect(&client, &WardClient::eventReceived, &app,
                     [&](const QString &kind, const QVariantMap &payload) {
        QTextStream(stderr) << "event " << kind << "\n";
        if (kind == QLatin1String("text.delta"))
            text += payload.value(QStringLiteral("text")).toString();
        if (kind == QLatin1String("turn.end"))
            app.quit();
        if (kind == QLatin1String("replay.end") && !parser.isSet(promptOption))
            app.quit();
    });
    QObject::connect(&client, &WardClient::permissionRequested, &app,
                     [&](const QString &requestId, const QString &tool,
                         const QString &detail) {
        QTextStream(stderr) << "approval " << requestId << " tool=" << tool
                            << " reason=" << detail << "\n";
        if (approve)
            client.respondToPermission(requestId, true);
    });
    QObject::connect(&client, &WardClient::stateChanged, &app, [&]() {
        if (client.state() == WardClient::Failed) {
            failed = true;
            app.quit();
            return;
        }
        if (client.state() != WardClient::Ready || prompted)
            return;
        if (parser.isSet(sessionOption)) {
            prompted = true;
            client.followSession(parser.value(sessionOption));
            return;
        }
        if (!parser.isSet(promptOption)) {
            if (!client.sessionsLoading())
                app.quit();
            return;
        }
        client.createSession();
    });
    QObject::connect(&client, &WardClient::sessionsLoadingChanged, &app, [&]() {
        if (client.state() == WardClient::Ready && !parser.isSet(promptOption)
                && !client.sessionsLoading())
            app.quit();
    });
    QObject::connect(&client, &WardClient::sessionChanged, &app, [&]() {
        if (prompted || client.sessionId().isEmpty() || !parser.isSet(promptOption))
            return;
        prompted = true;
        QTextStream(stderr) << "session " << client.sessionId() << "\n";
        client.sendPrompt(parser.value(promptOption));
    });

    QTimer::singleShot(0, &client, &WardClient::connectToDaemon);
    QTimer::singleShot(30000, &app, [&]() {
        QTextStream(stderr) << "probe: timed out\n";
        failed = true;
        app.quit();
    });
    app.exec();

    QTextStream out(stdout);
    out << text;
    if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
        out << '\n';
    return failed ? 1 : 0;
}
