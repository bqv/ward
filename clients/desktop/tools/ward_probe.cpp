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
    QCommandLineOption terminalOption(QStringList{QStringLiteral("terminal")},
                                      QStringLiteral("open a terminal and write a marker through it"));
    QCommandLineOption readOption(QStringList{QStringLiteral("read")},
                                  QStringLiteral("read one file through session.file_read"),
                                  QStringLiteral("path"));
    QCommandLineOption blobOption(QStringList{QStringLiteral("blob")},
                                  QStringLiteral("read a byte range through session.file_read_blob"),
                                  QStringLiteral("path"));
    QCommandLineOption blobOffsetOption(QStringList{QStringLiteral("blob-offset")},
                                        QStringLiteral("byte offset for --blob"),
                                        QStringLiteral("n"), QStringLiteral("0"));
    QCommandLineOption blobLenOption(QStringList{QStringLiteral("blob-len")},
                                     QStringLiteral("byte count for --blob"),
                                     QStringLiteral("n"), QStringLiteral("4096"));
    QCommandLineOption listOption(QStringList{QStringLiteral("list")},
                                  QStringLiteral("list one directory through session.file_list"),
                                  QStringLiteral("path"));
    QCommandLineOption presetOption(QStringList{QStringLiteral("preset")},
                                    QStringLiteral("select a policy preset and print the current one"),
                                    QStringLiteral("id"));
    QCommandLineOption confirmDangerOption(QStringList{QStringLiteral("confirm-danger")},
                                           QStringLiteral("answer a dangerous preset warning automatically"));
    parser.addOption(socketOption);
    parser.addOption(cwdOption);
    parser.addOption(promptOption);
    parser.addOption(sessionOption);
    parser.addOption(approveOption);
    parser.addOption(terminalOption);
    parser.addOption(readOption);
    parser.addOption(blobOption);
    parser.addOption(blobOffsetOption);
    parser.addOption(blobLenOption);
    parser.addOption(listOption);
    parser.addOption(presetOption);
    parser.addOption(confirmDangerOption);
    parser.process(app);

    WardClient client(parser.value(socketOption));
    QString text;
    bool prompted = false;
    bool failed = false;
    bool terminalOpened = false;
    bool terminalWritten = false;
    bool readStarted = false;
    bool blobStarted = false;
    bool listStarted = false;
    QString readResult;
    QByteArray blobResult;
    QStringList listResult;
    QString presetResult;
    bool presetStarted = false;
    QByteArray terminalText;
    const bool approve = parser.isSet(approveOption);
    const bool terminal = parser.isSet(terminalOption);
    const bool read = parser.isSet(readOption);
    const bool blob = parser.isSet(blobOption);
    const bool list = parser.isSet(listOption);
    const bool preset = parser.isSet(presetOption);
    const bool confirmDanger = parser.isSet(confirmDangerOption);

    QObject::connect(&client, &WardClient::lastErrorChanged, &app, [&]() {
        if (read || blob || list || preset)
            return;
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
        if (kind == QLatin1String("replay.end") && !parser.isSet(promptOption)
                && !terminal && !read && !blob && !list && !preset
                && !parser.isSet(sessionOption))
            app.quit();
    });
    QObject::connect(&client, &WardClient::dangerousPresetRequested, &app,
                     [&](const QString &id, const QString &warning) {
        QTextStream(stderr) << "dangerous preset " << id << ": " << warning << "\n";
        if (confirmDanger)
            client.confirmPermissionPreset(id);
    });
    QObject::connect(&client, &WardClient::permissionPresetChanged, &app, [&]() {
        if (!preset || client.permissionPreset().isEmpty())
            return;
        presetResult = client.permissionPreset();
        app.quit();
    });
    QObject::connect(&client, &WardClient::accessNoticeChanged, &app, [&]() {
        if (!preset || client.accessNotice().isEmpty())
            return;
        presetResult = QStringLiteral("error: ") + client.accessNotice();
        app.quit();
    });
    QObject::connect(&client, &WardClient::fileEntriesChanged, &app, [&]() {
        if (!list || client.fileEntries().isEmpty())
            return;
        for (const QVariant &entry : client.fileEntries()) {
            const QVariantMap row = entry.toMap();
            listResult.append(row.value(QStringLiteral("type")).toString()
                              + QLatin1Char(' ')
                              + row.value(QStringLiteral("name")).toString());
        }
        app.quit();
    });
    QObject::connect(&client, &WardClient::fileErrorChanged, &app, [&]() {
        if (!list || client.fileError().isEmpty())
            return;
        listResult.append(QStringLiteral("error: ") + client.fileError());
        app.quit();
    });
    QObject::connect(&client, &WardClient::blobRead, &app,
                     [&](const QString &path, qint64 offset, const QByteArray &data,
                         const QString &error) {
        if (!blob)
            return;
        Q_UNUSED(path);
        Q_UNUSED(offset);
        blobResult = error.isEmpty() ? data : (QStringLiteral("error: ") + error).toUtf8();
        app.quit();
    });
    QObject::connect(&client, &WardClient::fileTextChanged, &app, [&]() {
        if (!read || client.fileText().isEmpty())
            return;
        readResult = client.fileText();
        app.quit();
    });
    QObject::connect(&client, &WardClient::fileErrorChanged, &app, [&]() {
        if (!read || client.fileError().isEmpty())
            return;
        readResult = QStringLiteral("error: ") + client.fileError();
        app.quit();
    });
    QObject::connect(&client, &WardClient::terminalChanged, &app, [&]() {
        if (!terminal || !client.terminalOpen() || terminalWritten)
            return;
        terminalWritten = true;
        client.writeTerminal("echo ward-terminal-ok\n");
    });
    QObject::connect(&client, &WardClient::terminalOutput, &app,
                     [&](const QByteArray &data) {
        terminalText.append(data);
        if (terminalText.contains("ward-terminal-ok")) {
            client.closeTerminal();
            app.quit();
        }
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
        if (preset) {
            prompted = true;
            presetStarted = true;
            client.setPermissionPreset(parser.value(presetOption));
            return;
        }
        if (terminal || read || blob || list) {
            prompted = true;
            client.createSessionNow();
            return;
        }
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
        prompted = true;
        client.sendPrompt(parser.value(promptOption));
    });
    QObject::connect(&client, &WardClient::sessionsLoadingChanged, &app, [&]() {
        if (client.state() == WardClient::Ready && !parser.isSet(promptOption)
                && !terminal && !read && !blob && !list && !preset
                && !parser.isSet(sessionOption) && !client.sessionsLoading())
            app.quit();
    });
    QObject::connect(&client, &WardClient::sessionChanged, &app, [&]() {
        if (client.sessionId().isEmpty())
            return;
        if (terminal && !terminalOpened) {
            terminalOpened = true;
            QTextStream(stderr) << "session " << client.sessionId() << "\n";
            client.openTerminal(80, 24);
            return;
        }
        if (read && !readStarted) {
            readStarted = true;
            QTextStream(stderr) << "session " << client.sessionId() << "\n";
            client.readFile(parser.value(readOption));
            return;
        }
        if (blob && !blobStarted) {
            blobStarted = true;
            QTextStream(stderr) << "session " << client.sessionId() << "\n";
            client.readBlob(parser.value(blobOption),
                            parser.value(blobOffsetOption).toLongLong(),
                            parser.value(blobLenOption).toLongLong());
            return;
        }
        if (list && !listStarted) {
            listStarted = true;
            QTextStream(stderr) << "session " << client.sessionId() << "\n";
            client.listDirectory(parser.value(listOption));
            return;
        }
        if (prompted || !parser.isSet(promptOption))
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
    if (terminal) {
        out << QString::fromUtf8(terminalText);
    } else if (read) {
        out << readResult;
        if (!readResult.isEmpty() && !readResult.endsWith(QLatin1Char('\n')))
            out << '\n';
    } else if (blob) {
        fwrite(blobResult.constData(), 1, size_t(blobResult.size()), stdout);
        fflush(stdout);
    } else if (list) {
        out << listResult.join(QLatin1Char('\n'));
        if (!listResult.isEmpty())
            out << '\n';
    } else if (preset) {
        out << presetResult;
        if (!presetResult.isEmpty())
            out << '\n';
    } else {
        out << text;
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
            out << '\n';
    }
    return failed ? 1 : 0;
}
