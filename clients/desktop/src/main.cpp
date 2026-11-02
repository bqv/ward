// dsh-native — a native desktop client for a local DSH host.
//
// One transport is started, chosen at LAUNCH with --transport:
//
//   Dsh  the host's own /api surface over WebSocket (/api/remote.mux), with the
//        client minting its own signed session cookie from the secret in the
//        harness home. This is the default, and the route to terminal, panel and
//        trajectory parity, because it is the same surface the web UI uses.
//   Acp  JSON-RPC over stdio to a child `dsh --profile acp`. No auth, no socket.
//        Limited to chat and tools — ACP is deliberately automation-only, and
//        this is the headless prompt path's transport.
//
// Nothing switches transports at runtime, and nothing falls back to another one
// by itself: the header's "acp" button opens the ACP sessions as a PANE over the
// window (a second, independent AcpClient with its own child), and the "web"
// button opens the web UI the same way.
//
// Either way there is no web engine, no browser, and no V8 in *this* process.

#include "AcpClient.h"
#include "ClientFacade.h"
#include "DshClient.h"
#include "WardClient.h"
#include "HeadlessClient.h"
#include "Shell.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QStyleFactory>
#include "Theme.h"
#include "Markdown.h"

#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

// Point the cursor loader at the theme that ships with this client.
//
// Why this exists at all: no cursor theme is selected on this box. There is no
// /usr/share/icons/default, no ~/.icons and no desktop settings daemon, so the
// loader resolves the theme name "default", finds no such directory, and every
// pointer falls through to the X core cursor FONT — the 1980s bitmap arrows.
// That is the "crappy default" this replaces; Adwaita was never actually in use.
//
// Why the environment and not a settings call: on the xcb platform Qt 6 asks
// libxcb-cursor for each shape by name, and that library reads XCURSOR_PATH and
// the `Xcursor.theme` X RESOURCE — but not the XCURSOR_THEME environment
// variable (libxcb-cursor 0.1.6 contains no such string; measured). So
// qputenv("XCURSOR_THEME", ...) would silently do nothing. What it does honour
// is the freedesktop indirection a distro ships: a directory in the path named
// `default` whose index.theme says `Inherits=<real theme>`. Our tree therefore
// carries themes/default/index.theme -> Inherits=dsh.
//
// Must run before QGuiApplication is constructed: the platform plugin reads the
// cursor path while it is being created, and setting it afterwards changes
// nothing.
void selectCursorTheme()
{
    // /proc/self/exe rather than QCoreApplication::applicationDirPath(), which
    // wants an application instance — and there cannot be one yet. This client
    // is Linux-only (libvterm, /proc-based host IPC), so the procfs shortcut is
    // not a portability compromise.
    const QString exe = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
    const QDir exeDir = exe.isEmpty() ? QDir::current() : QFileInfo(exe).absoluteDir();

    // The same "most specific wins" order as the QML tree just below, and for
    // the same reason: a compile-time path would leak the build directory into
    // the installed binary.
    const QStringList candidates{
        exeDir.filePath(QStringLiteral("themes")),                                  // relocatable install
        exeDir.filePath(QStringLiteral("../assets/cursors/themes")),                // in-tree: ./build/dsh-native
        QStringLiteral(DSH_NATIVE_DATA_SUBDIR "/themes"),                           // packaged
    };

    QString themeRoot;
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("default/index.theme")))) {
            themeRoot = QDir(candidate).absolutePath();
            break;
        }
    }
    if (themeRoot.isEmpty())
        return; // no theme shipped: leave the environment alone rather than half-break it

    // Ours first, so the client's own artwork wins over anything the desktop
    // installs later; everything after it is the fallback chain our theme's
    // `Inherits=Adwaita` resolves through for shapes it does not draw.
    QByteArray path = QFile::encodeName(themeRoot);
    const QByteArray ambient = qgetenv("XCURSOR_PATH");
    path += ':';
    path += ambient.isEmpty() ? QByteArray("/usr/local/share/icons:/usr/share/icons") : ambient;
    qputenv("XCURSOR_PATH", path);

    // XCURSOR_SIZE is deliberately left alone. The theme ships 22/24/32/48/64
    // rasters — including 22, which is what this display's DPI resolves to — so
    // whatever size the desktop asks for is drawn natively instead of rescaled.
    // Pinning a number here would override the owner's own HiDPI choice, and
    // there is no settings daemon on this box to put it back.
}

/// Where the client's own diagnostic stream goes.
///
/// dsh-native is normally started from a desktop entry, and a menu launch has no
/// terminal: every qInfo/qWarning it writes is discarded. Three failures in one
/// evening each cost a round trip for exactly that reason — the client knew what
/// was wrong (including the rail store's own "could not write rail state" line,
/// AcpCompat.cpp's reportStoreFailure) and there was nowhere to read it.
///
/// So the same message stream is written here, to
/// `$XDG_DATA_HOME/dsh-native/client.log` (`~/.local/share/dsh-native/...`), and
/// still forwarded to stderr so a terminal run reads exactly as before.
///
/// The file is capped at 2 MiB and rotated once (client.log.1), so the pair can
/// never exceed 4 MiB however long the client runs. Rotation happens at startup
/// when the previous run already filled the file, and again whenever a write
/// crosses the cap — a run that is left open for days still cannot grow without
/// bound. Losing the oldest block is the intended trade: the useful part of a
/// client log is the seconds before something went wrong.
///
/// Installed from main() after the application name and organisation are set:
/// those two are what resolve the settings store, and a handler must exist before
/// the parser, the transport or the QML can say anything.
class ClientLog
{
public:
    static constexpr qint64 Cap = 2 * 1024 * 1024;

    static void install()
    {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (dir.isEmpty())
            dir = QDir::homePath() + QStringLiteral("/.local/share");
        dir += QStringLiteral("/ward-gui");
        QDir().mkpath(dir);

        {
            QMutexLocker locker(&mutex());
            path() = dir + QStringLiteral("/client.log");
            previous() = path() + QStringLiteral(".1");

            // A file left at the cap by a previous run is moved aside before this
            // one appends to it, so the newest lines are never the ones lost.
            if (QFileInfo(path()).size() > Cap)
                rotateLocked();
            else
                openLocked();
        }

        qInstallMessageHandler(&ClientLog::write);
        // Logged only after the lock is released: the handler takes the same
        // mutex, so a line written while holding it deadlocks the process in its
        // first second — which is exactly what the first version of this did.
        qInfo() << "client log:" << (file() ? file()->fileName() : QStringLiteral("(unavailable)"));
    }

private:
    // File-scope statics through accessors: a function-local static keeps the
    // initialisation order obvious and avoids a global QFile constructed before
    // QCoreApplication.
    static QFile *&file()
    {
        static QFile *instance = nullptr;
        return instance;
    }
    static QString &path()
    {
        static QString instance;
        return instance;
    }
    static QString &previous()
    {
        static QString instance;
        return instance;
    }
    static QMutex &mutex()
    {
        static QMutex instance;
        return instance;
    }

    static void openLocked()
    {
        auto *handle = new QFile(path());
        if (!handle->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            // Say it on stderr only: this is the one failure the log cannot report.
            fprintf(stderr, "ward-gui: cannot open client log %s: %s\n",
                    qPrintable(path()), qPrintable(handle->errorString()));
            delete handle;
            return;
        }
        file() = handle;
    }

    /// Move the current file aside, keep the older one, and start a fresh file.
    /// Called with the mutex held.
    static void rotateLocked()
    {
        if (file() != nullptr) {
            file()->close();
            delete file();
            file() = nullptr;
        }
        QFile::remove(previous());
        if (QFileInfo::exists(path()))
            QFile::rename(path(), previous());
        openLocked();
    }

    static void write(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        const char *level = "info";
        switch (type) {
        case QtDebugMsg:    level = "debug";    break;
        case QtInfoMsg:     level = "info";     break;
        case QtWarningMsg:  level = "warning";  break;
        case QtCriticalMsg: level = "critical"; break;
        case QtFatalMsg:    level = "fatal";    break;
        }

        QString line = QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        line += QStringLiteral(" [");
        line += QLatin1String(level);
        line += QLatin1Char(']');
        if (context.category != nullptr && qstrcmp(context.category, "default") != 0) {
            line += QLatin1Char(' ');
            line += QLatin1String(context.category);
        }
        line += QStringLiteral(": ");
        line += message;
        // The caller's own file:line is what turns "could not save" into a place
        // to look, and it is the half the default handler leaves out.
        if (context.file != nullptr) {
            line += QStringLiteral(" (");
            line += QLatin1String(context.file);
            line += QLatin1Char(':');
            line += QString::number(context.line);
            line += QLatin1Char(')');
        }
        line += QLatin1Char('\n');

        const QByteArray bytes = line.toUtf8();
        fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
        fflush(stderr);

        {
            QMutexLocker locker(&mutex());
            if (file() != nullptr) {
                if (file()->write(bytes) != bytes.size()) {
                    // A full disk must not turn into a stream of recursion errors.
                    file()->close();
                    delete file();
                    file() = nullptr;
                } else {
                    file()->flush();
                    if (file()->size() > Cap)
                        rotateLocked();
                }
            }
        }

        // qFatal still has to die: replacing the handler took that with it.
        if (type == QtFatalMsg)
            abort();
    }
};

/// Start the transport selected at launch, and wire the diagnostics that belong
/// to the launch rather than to the window.
///
/// The shell calls this once its UI exists, because the first status message has
/// to be visible somewhere, and because the host rules (no fallback, no silent
/// second transport) are properties of the process, not of the shell. There is
/// deliberately no ACP fallback here: the mux IS the transport, and the window
/// says so when it is not there. `--transport acp` is still honoured at launch;
/// it is an instruction, not a runtime decision.
void startTransport(DshClient &dsh, AcpClient &acp, WardClient &ward, bool useDsh,
                    bool useWard)
{
    if (useWard) {
        QTimer::singleShot(0, &ward, [&ward]() { ward.connectToDaemon(); });
    } else if (useDsh) {
        QTimer::singleShot(0, &dsh, &DshClient::connectToHost);
        QObject::connect(&dsh, &DshClient::stateChanged, &dsh, [&dsh]() {
            if (dsh.state() == DshClient::Ready)
                dsh.listSessions();
        });
        QObject::connect(&dsh, &DshClient::sessionsListed, &dsh, [](const QJsonArray &sessions) {
            qInfo() << "session/list returned" << sessions.size() << "session(s)";
        });
        QObject::connect(&dsh, &DshClient::rpcFailed, &dsh,
                         [](const QString &method, const QString &code, const QString &message) {
                             qWarning() << "rpc" << method << "failed:" << code << message;
                         });
    } else {
        QTimer::singleShot(0, &acp, &AcpClient::start);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    selectCursorTheme();
    // QApplication, not QGuiApplication: the shell is a QWidget top-level and
    // its retained panes are QQuickWidgets, and there is one application object
    // per process.
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ward-gui"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("dsh"));

    // Before anything can log: the application name above is what resolves the
    // log's location, and the parser, the transport and the QML all log. A menu
    // launch has no terminal, so without the file every one of those lines is
    // discarded. It comes after selectCursorTheme() and before the parser, and it
    // does not touch the cursor environment.
    ClientLog::install();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native desktop client for ward"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption programOption(
        QStringList{QStringLiteral("program")},
        QStringLiteral("Program used to start the ACP server."),
        QStringLiteral("path"));
    QCommandLineOption workspaceOption(
        QStringList{QStringLiteral("workspace")},
        QStringLiteral("Workspace the session is created in."),
        QStringLiteral("dir"));
    QCommandLineOption promptOption(
        QStringList{QStringLiteral("prompt")},
        QStringLiteral("Send this prompt once the session is ready, then exit when it settles."),
        QStringLiteral("text"));
    QCommandLineOption transportOption(
        QStringList{QStringLiteral("transport")},
        QStringLiteral("Which transport to start AT LAUNCH: 'dsh' (the running host's"
                       " API over WebSocket), 'acp' (a stdio child — the headless"
                       " --prompt path), or 'ward' (a ward daemon over its binary"
                       " protocol on a Unix socket or loopback TCP; the default)."
                       " Nothing switches to another one at runtime."),
        QStringLiteral("name"), QStringLiteral("ward"));
    QCommandLineOption socketOption(
        QStringList{QStringLiteral("socket")},
        QStringLiteral("ward daemon socket: a Unix path or tcp://host:port"
                       " (default $WARD_SOCKET, then ward's default socket path)."),
        QStringLiteral("path"));
    QCommandLineOption webUrlOption(
        QStringList{QStringLiteral("web-url")},
        QStringLiteral("Origin of the web UI shown by the trapdoor"
                       " (default http://127.0.0.1:8080/, also DSH_NATIVE_WEB_URL)."),
        QStringLiteral("url"));
    parser.addOption(programOption);
    parser.addOption(workspaceOption);
    parser.addOption(promptOption);
    parser.addOption(transportOption);
    parser.addOption(socketOption);
    parser.addOption(webUrlOption);
    parser.process(app);

    const QString transport = parser.value(transportOption).toLower();
    if (transport != QLatin1String("acp") && transport != QLatin1String("dsh")
            && transport != QLatin1String("ward"))
        parser.showHelp(2);

    // The web UI's origin: the trapdoor's subject. It is LOCAL, and it stays
    // local: this client talks to the host on the machine it runs on, so the
    // trapdoor is 127.0.0.1:8080 and never one of the public fronts — the nginx
    // door on zeta, or the gate's LAN address — which exist for other people's
    // browsers, not for this window. Overridable only for a host that really is
    // elsewhere: --web-url or DSH_NATIVE_WEB_URL.
    QUrl webUrl(QStringLiteral("http://127.0.0.1:8080/"));
    {
        const QByteArray configured = qgetenv("DSH_NATIVE_WEB_URL");
        if (!configured.isEmpty())
            webUrl = QUrl(QString::fromUtf8(configured));
        if (parser.isSet(webUrlOption))
            webUrl = QUrl(parser.value(webUrlOption));
        const QString scheme = webUrl.scheme().toLower();
        if (!webUrl.isValid() || webUrl.host().isEmpty()
                || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
            qWarning() << "unusable web URL" << webUrl.toString()
                       << "- using http://127.0.0.1:8080/";
            webUrl = QUrl(QStringLiteral("http://127.0.0.1:8080/"));
        }
    }

    const bool useDsh = (transport == QLatin1String("dsh"));
    const bool useWard = (transport == QLatin1String("ward"));

    // --- the transports, as QML singletons -----------------------------
    //
    // Singleton *instance* registration, not QML-declared types: the objects are
    // owned by C++ and outlive every QML object, which is what lets the session
    // and transcript survive a pane being closed and reopened. Both are
    // registered so a transport can be chosen at launch without a rebuild —
    // registration is not starting.
    //
    //   Acp  — stdio child, `dsh --profile acp`. No auth. Chat and tools only.
    //          Started only by `--transport acp`, the headless prompt path.
    //   Dsh  — the host's own /api surface over WebSocket, with a self-minted
    //          session cookie. The route to full UI parity, and the default.
    AcpClient acp;
    if (parser.isSet(programOption))
        acp.setLaunchCommand(parser.value(programOption), {QStringLiteral("--profile"), QStringLiteral("acp")});
    if (parser.isSet(workspaceOption))
        acp.setWorkspace(parser.value(workspaceOption));

    DshClient dsh;

    // Ward speaks a length-prefixed CBOR protocol over a Unix socket or
    // loopback TCP. It is the transport this fork exists for, and it is the
    // default; the DSH transports remain selectable so the shell can still be
    // compared against the host it was written for.
    WardClient ward(parser.isSet(socketOption) ? parser.value(socketOption) : QString());

    // The headless pane's child. Not a transport and not a QML singleton type:
    // it is one one-shot child owned by one pane, so it is published as a
    // context property beside `dshTransport` and reached by name only from the
    // pane. Declared before the engine so it outlives every QML object that
    // binds to it; its destructor reaps a child that is still running.
    HeadlessClient headless;
    // The ACP pane's client: a SECOND, independent instance — not the transport
    // above.
    //
    // One ACP child runs several sessions, and the pane draws a bar over them,
    // so the transcript is no longer a property of the client but one
    // `AcpTranscript` per session. That is still why two conversations on
    // separate children need two instances: reusing the window's instance would
    // put the pane's sessions into the window's client, and closing the pane
    // would stop the child behind the window.
    //
    // This is NOT the case the "never start both transports against the same
    // host" rule is about, and the rule's wording now says so. That rule is
    // about two clients opening sessions on the same DSH host. An ACP child is
    // its own `dsh --profile acp` process, talking to no host at all, and it is
    // started only when a person presses the header's ACP button.
    AcpClient acpPane;
    if (parser.isSet(programOption)) {
        acpPane.setLaunchCommand(parser.value(programOption),
                                 {QStringLiteral("--profile"), QStringLiteral("acp")});
    }
    if (parser.isSet(workspaceOption))
        acpPane.setWorkspace(parser.value(workspaceOption));

    // ---- the one shell ------------------------------------------------------
    //
    // One window: the Widgets shell. There is no QML engine and no scene graph
    // in this process.
    QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    if (fusion == nullptr)
        qWarning() << "the Fusion Widgets style is not available;"
                   << "the platform style is being used instead";
    else
        QApplication::setStyle(fusion);
    QApplication::setPalette(Theme::palette());

    QObject *transportObject = useWard ? static_cast<QObject *>(&ward)
                              : useDsh ? static_cast<QObject *>(&dsh)
                                       : static_cast<QObject *>(&acp);
    ClientFacade facade(transportObject);

    ShellPanes panes;
    panes.webUrl = webUrl;
    panes.acpClient = &acpPane;
    panes.headlessClient = &headless;

    Shell shell(&facade, panes);
    shell.show();

    startTransport(dsh, acp, ward, useDsh, useWard);

    // Scriptable path: wait for the session, send one prompt, then leave once
    // the turn settles. This is what makes the client testable without a
    // display — the same binary, the same shell and the same AcpClient run the
    // real handshake and the real turn.
    //
    // ACP only: the WebSocket transport has no prompt path yet, and silently
    // ignoring the flag would look like a hang.
    if (parser.isSet(promptOption) && !useDsh) {
        const QString text = parser.value(promptOption);

        // Guard against a false settle: createSession() reports not-busy before
        // the prompt is sent, so only a busy->idle transition *after* we have
        // actually prompted may end the process.
        auto prompted = std::make_shared<bool>(false);

        QObject::connect(&acp, &AcpClient::sessionChanged, &acp, [&acp, text, prompted]() {
            *prompted = true;
            acp.sendPrompt(text);
        });

        QObject::connect(&acp, &AcpClient::busyChanged, &acp, [&acp, &app, prompted]() {
            if (!acp.busy() && *prompted) {
                QTimer::singleShot(150, &app, [&acp, &app]() {
                    // The transcript is the actual evidence that the turn
                    // produced text, so print it rather than only the exit code.
                    printf("%s", qPrintable(acp.transcriptAsText()));
                    fflush(stdout);
                    app.exit(0);
                });
            }
        });

        // A wedged turn must not hang the process forever.
        QTimer::singleShot(180000, &app, [&app]() { app.exit(4); });
    } else if (parser.isSet(promptOption)) {
        qWarning() << "--prompt is only implemented for --transport acp; ignoring it";
    }

    return app.exec();
}
