#ifndef WARD_APPRESTART_H
#define WARD_APPRESTART_H

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <cstdlib>

inline bool restartApplication()
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QStringList args = QCoreApplication::arguments().mid(1);
    const QString wd = QDir::currentPath();
    const bool started = QProcess::startDetached(exe, args, wd.isEmpty() ? QDir::homePath() : wd);
    qInfo() << "restarting:" << exe << args << "started:" << started;
    if (!started) {
        qWarning() << "restart failed:" << exe;
        return false;
    }
    QTimer::singleShot(1500, [] { std::exit(0); });
    QCoreApplication::quit();
    return true;
}

#endif
