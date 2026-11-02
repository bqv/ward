// The part of the compat surface that does real work: rail state and self-restart.

#include "AcpCompat.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QStringList>

// Nothing to clear when nothing can fill the list. AcpClient overrides this: it
// derives changedFiles from the tool calls that name a path, so the dock's × has
// to be able to empty it there.
void AcpCompat::clearChangedFiles()
{
}

// ACP has no request that changes a goal, and a transport that cannot act must
// still answer. AcpClient overrides this to say so in the status line.
void AcpCompat::goalAction(const QString &)
{
}

// The rail's Workspace controls are DSH-only. ACP carries no Workspace registry
// to create one in and no directory chooser to pick one with, so there is
// nothing here to do and no model to change. What there is, is an answer: a
// press that vanishes is the failure this client keeps fixing, so both say so
// through the same notice the rail draws for every other message.
void AcpCompat::createWorkspaceFromPicker()
{
    emit railNotice(QStringLiteral("The ACP transport cannot add a workspace: it has no workspace list."),
                    true);
}

void AcpCompat::createSessionInWorkspace(const QString &)
{
    emit railNotice(QStringLiteral("The ACP transport cannot create a session in a workspace: it has no workspaces."),
                    true);
}

void AcpCompat::restartSelf()
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QStringList args = QCoreApplication::arguments().mid(1);
    // No status line here: `setStatus` belongs to the subclass and this base
    // cannot reach it — so the outcome goes to the log instead of vanishing.
    const QString wd = QDir::currentPath();
    const bool started = QProcess::startDetached(exe, args, wd.isEmpty() ? QDir::homePath() : wd);
    qInfo() << "restarting:" << exe << args << "started:" << started;
    if (!started) {
        qWarning() << "restart failed:" << exe;
        return;
    }
    QTimer::singleShot(1500, [] { std::exit(0); });
    QCoreApplication::quit();
}

namespace Prefs {

QString busyEnter()
{
    const QString stored = QSettings().value(QStringLiteral("composer/busyEnter")).toString();
    // Steering is the sharper behaviour, so it is never the default: queueing is
    // what a mistyped Enter should do.
    return stored == QLatin1String("steer") ? QStringLiteral("steer") : QStringLiteral("queue");
}

void setBusyEnter(const QString &mode)
{
    QSettings().setValue(QStringLiteral("composer/busyEnter"),
                         mode == QLatin1String("steer") ? QStringLiteral("steer")
                                                        : QStringLiteral("queue"));
}

}   // namespace Prefs

namespace SlashCommands {

QStringList names()
{
    // The two the client drives. `/permission` carries a value
    // (`/permission read-only`), so a command's word is only its first token —
    // see isKnown, and the same split in the composer. It stays in this table
    // because it is the host's own current-session switch: the permission-presets
    // plugin registers it, and it is the same line the web's popup picker sends.
    // The access chip and the composer both go through `runCommand`, so both are
    // validated against the catalog and both surface the host's answer.
    return {QStringLiteral("/compact"), QStringLiteral("/permission")};
}

bool isKnown(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || !trimmed.startsWith(QLatin1Char('/')))
        return false;
    const QString word = trimmed.section(QLatin1Char(' '), 0, 0);
    return names().contains(word);
}

}   // namespace SlashCommands

QString AcpCompat::busyEnter() const
{
    return Prefs::busyEnter();
}

void AcpCompat::setBusyEnter(const QString &mode)
{
    if (mode == Prefs::busyEnter())
        return;
    Prefs::setBusyEnter(mode);
    emit busyEnterChanged();
}

namespace RailState {

namespace {

// Let this rail write fall back to writing the file in place.
//
// `setAtomicSyncRequired(false)` reaches QSettings' QSaveFile as
// `setDirectWriteFallback(true)`, and that fallback applies to exactly one case:
// the temporary file beside the target **cannot be created**. It does not apply
// when the temporary file is created and the final step fails.
//
// That distinction is the whole history of this bug. The policy of the day denied
// `create` in ~/.config, so the temp file could not be made and this fallback did
// the work; the policy now grants `create` (and getattr/ioctl/lock/map/open/read/
// rename/setattr/unlink/write) but not `link`, so the temp file *is* created and
// QSettings' atomic save dies at `linkat` — measured, and reported by the audit
// log as `avc: denied { link } for dsh_native_t … tclass=file` against a
// `xdg_config_t` temp file. The fallback therefore buys nothing under the current
// policy and this call is not the reason the rail does not persist; it is kept
// only because it is still the right behaviour if the policy ever denies `create`
// again.
//
// Scoped to this store on purpose. The rest of the client's settings (window
// geometry, session/last, terminal ids) are still written atomically and fail
// under this policy — that half is the policy's to fix, not this file's. A rail
// write flushes the settings file's shared cache, so keys another object put
// there ride along with it, but nothing may rely on that accident.
void openStore(QSettings &store)
{
    store.setAtomicSyncRequired(false);
}

// Say it once per run when the store cannot be read or written.
//
// QSettings reports failure only through status(), and every failure mode that
// matters here — a read-only config directory, a full disk, an SELinux denial of
// the "<file>.lock" it creates before writing — still returns a perfectly
// plausible empty list and stores nothing. That silence is what made "rail state
// does not persist" survive three rounds of fixes to code that was already
// right, so a failure is now named out loud: the file, and why.
//
// Once per run, not once per click: while the store is unwritable every caret
// click fails identically, and a wall of the same line is how a log stops being
// read. The first one is the diagnosis.
void reportStoreFailure(const QSettings &store, const char *what)
{
    // A freshly constructed QSettings has read nothing, so its status is NoError
    // whatever the file says — every caller here passes a fresh object precisely
    // so the status describes the file rather than a shared cache, and without
    // this read the report could never fire at all. `allKeys()` is a read.
    (void)store.allKeys();
    if (store.status() == QSettings::NoError)
        return;
    static bool alreadySaid = false;
    if (alreadySaid)
        return;
    alreadySaid = true;
    qWarning() << "[rail] could not" << what << "rail state in" << store.fileName()
               << "-" << (store.status() == QSettings::AccessError
                              ? QStringLiteral("access denied (permissions, or an"
                                               " SELinux policy that forbids creating"
                                               " the settings lock/temp file)")
                              : QStringLiteral("the file is malformed"))
               << "- the rail will not remember it after a restart";
}

/// Say what one settings write actually did, on disk.
///
/// QSettings cannot answer that by itself. `status()` belongs to the file's
/// *shared* cache: a store opened after another object wrote the same file
/// reports that object's leftover state whether or not this write landed, which
/// is why every caller below opens a second `QSettings` (and why that second
/// object has to be made to read — see reportStoreFailure). So the answer is
/// taken from the file itself: the key's presence on disk, and the file's own
/// size and mtime.
///
/// This is the line that was missing for three rounds of "rail state does not
/// persist": without it the client could not say whether the code path ran, the
/// store was unwritable, or the write was refused by something outside the
/// process. It is one line per write, and a rail write is a human action.
///
/// `entries` is the rail state this read or write was about — what came back at
/// restore, or what was handed to the store. It is printed because the previous
/// version of this line could say the key existed on disk while saying nothing
/// about its value, so "restored nothing" and "restored the wrong thing" read
/// identically. `status-after` is a store constructed *now*, made to read, so it
/// reports the file rather than whatever the shared cache was left holding; the
/// operating object's own status is `status`.
void reportStoreState(const QSettings &store, const QString &key, const QStringList &entries,
                      const char *what)
{
    const QString path = store.fileName();
    const QFileInfo fileInfo(path);
    const QFileInfo dirInfo(fileInfo.absolutePath());

    QFile file(path);
    const bool readable = file.open(QIODevice::ReadOnly);
    const QByteArray onDisk = readable ? file.readAll() : QByteArray();
    // The group is the file's `[section]` and the key is written without its
    // prefix ("rail/expandedSessions" appears as `expandedSessions`), so the leaf
    // is the name to look for. For these keys — a list of session ids — the value
    // has no escaping that could hide the name.
    const QByteArray leaf = key.section(QLatin1Char('/'), -1).toUtf8();
    const bool landed = onDisk.contains(leaf + '=');

    // A store constructed here has read nothing yet, so its status is NoError
    // whatever the file says. `allKeys()` is the read that makes the status
    // describe the file — the same thing reportStoreFailure does, on a throwing-
    // away object, because the caller's own store is this one.
    QSettings fresh;
    (void)fresh.allKeys();

    qInfo().nospace() << "[settings] " << what << " " << key
                      << " -> " << path
                      << " status=" << int(store.status())
                      << " status-after=" << int(fresh.status())
                      << " file=" << (readable ? "readable" : "absent")
                      << " size=" << fileInfo.size()
                      << " key-on-disk=" << (landed ? "yes" : "NO")
                      << " dir-writable=" << (dirInfo.isWritable() ? "yes" : "NO")
                      << " org/app=" << store.organizationName() << "/" << store.applicationName()
                      << " entries(" << entries.size() << ")=["
                      << entries.join(QStringLiteral(", ")) << ']';

    reportStoreFailure(store, what);
    // A missing file on a first run is not a failure — the caller's status already
    // distinguishes that — so only a write that did not land is called out.
    if (!landed && qstrcmp(what, "write") == 0) {
        qWarning().nospace() << "[settings] this " << what << " did not reach " << path
                             << "; it will not survive a restart (status="
                             << int(store.status()) << ")";
    }
}

}   // namespace

// Workspace groups the reader folded away. Same store, same policy, same
// silence — and the same fix, because a drawer that remembers one of its two
// folds is worse than one that remembers neither.
QStringList collapsed()
{
    QSettings store;
    openStore(store);
    const QStringList ids = store.value(QStringLiteral("rail/collapsed")).toStringList();
    // The same line the expansion store gets. Which of the two folds persists and
    // which does not is not something the window can tell you from the inside.
    reportStoreState(store, QStringLiteral("rail/collapsed"), ids, "read");
    QSettings after;
    reportStoreFailure(after, "read");
    return ids;
}

void setCollapsed(const QStringList &ids)
{
    QSettings store;
    openStore(store);
    store.setValue(QStringLiteral("rail/collapsed"), ids);
    store.sync();
    // Sync before the report, so the verdict describes this write rather than
    // whatever the destructor does afterwards.
    reportStoreState(store, QStringLiteral("rail/collapsed"), ids, "write");
    QSettings after;
    reportStoreFailure(after, "write");
}


// Session expansion is tri-state — open, closed, or nobody has said — and it has
// to survive a restart, which the in-memory map did not. Two ids per entry, so a
// parent the reader deliberately closed stays closed instead of being reopened by
// the "a parent with children is open" default.
QStringList expanded()
{
    QSettings store;
    openStore(store);
    const QStringList entries =
        store.value(QStringLiteral("rail/expandedSessions")).toStringList();
    // One line at startup: which file was read, what came back, and whether the
    // read succeeded. Without it "the store was missing", "the store was never
    // read" and "it was read and returned nothing" look the same from inside the
    // window.
    reportStoreState(store, QStringLiteral("rail/expandedSessions"), entries, "read");
    QSettings after;
    reportStoreFailure(after, "read");
    return entries;
}

void setExpanded(const QStringList &entries)
{
    QSettings store;
    openStore(store);
    store.setValue(QStringLiteral("rail/expandedSessions"), entries);
    // Sync here rather than leaving it to the destructor, so the verdict below
    // describes this write and not whatever happens at teardown.
    store.sync();

    // The file's own state, not the shared cache's — plus the entries that were
    // written. This is the line that says whether the write landed, and the one
    // to read first when it does not.
    reportStoreState(store, QStringLiteral("rail/expandedSessions"), entries, "write");

    QSettings after;
    reportStoreFailure(after, "write");
}

}   // namespace RailState

QStringList AcpCompat::railCollapsed() const
{
    return RailState::collapsed();
}

void AcpCompat::setRailCollapsed(const QStringList &ids)
{
    RailState::setCollapsed(ids);
}

QStringList AcpCompat::railExpandedSessions() const
{
    return RailState::expanded();
}

void AcpCompat::setRailExpandedSessions(const QStringList &entries)
{
    RailState::setExpanded(entries);
}

// TransportSwitch::relaunch() used to live here — the rail's "Fall back to ACP"
// entry restarted the whole client on `--transport acp`. That is gone with the
// automatic fallback it shared a premise with: the mux is the transport, ACP is
// a PANE opened from the header (a second AcpClient with its own child), and
// nothing switches transport at runtime, on the reader's behalf or otherwise.
// `--transport acp` remains a LAUNCH flag for the headless --prompt path.
