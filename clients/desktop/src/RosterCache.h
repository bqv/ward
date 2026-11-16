#pragma once

#include <QDateTime>
#include <QString>
#include <QVariantList>

class RosterCache
{
public:
    enum class Result {
        Loaded,
        Absent,
        OtherEndpoint,
        Unreadable,
        VersionMismatch,
        Corrupt,
    };

    struct Snapshot
    {
        QString endpoint;
        QDateTime savedAt;
        QVariantList sessions;
    };

    static QString path();
    static Result load(const QString &endpoint, Snapshot *snapshot, QString *detail);
    static bool save(const QString &endpoint, const QVariantList &sessions, QString *detail);
};
