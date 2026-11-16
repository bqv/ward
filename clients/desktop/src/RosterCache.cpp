#include "RosterCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr int CacheVersion = 1;
constexpr qint64 MaxCacheBytes = 32 * 1024 * 1024;

void setDetail(QString *detail, const QString &text)
{
    if (detail != nullptr)
        *detail = text;
}

QJsonObject readRosters(QJsonObject *root)
{
    QFile file(RosterCache::path());
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > MaxCacheBytes)
        return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {};
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != CacheVersion)
        return {};
    if (root != nullptr)
        *root = object;
    return object.value(QStringLiteral("rosters")).toObject();
}

}

QString RosterCache::path()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share");
    return dir + QStringLiteral("/ward-gui/session-roster.json");
}

RosterCache::Result RosterCache::load(const QString &endpoint, Snapshot *snapshot,
                                      QString *detail)
{
    if (snapshot == nullptr) {
        setDetail(detail, QStringLiteral("no destination for the snapshot"));
        return Result::Corrupt;
    }

    QFile file(path());
    if (!file.exists()) {
        setDetail(detail, path());
        return Result::Absent;
    }

    const QJsonObject rosters = readRosters(nullptr);
    if (rosters.isEmpty()) {
        setDetail(detail, QStringLiteral("no readable roster in %1").arg(path()));
        return Result::Unreadable;
    }

    const QJsonValue entry = rosters.value(endpoint);
    if (!entry.isObject()) {
        setDetail(detail, QStringLiteral("no cached roster for this endpoint"));
        return Result::OtherEndpoint;
    }

    const QJsonObject roster = entry.toObject();
    const QJsonValue sessions = roster.value(QStringLiteral("sessions"));
    if (!sessions.isArray()) {
        setDetail(detail, QStringLiteral("the sessions field is not an array"));
        return Result::Corrupt;
    }

    snapshot->endpoint = endpoint;
    snapshot->savedAt = QDateTime::fromString(
        roster.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
    snapshot->sessions = sessions.toArray().toVariantList();
    if (snapshot->sessions.isEmpty()) {
        setDetail(detail, QStringLiteral("no rows"));
        return Result::Absent;
    }
    setDetail(detail, path());
    return Result::Loaded;
}

bool RosterCache::save(const QString &endpoint, const QVariantList &sessions, QString *detail)
{
    if (endpoint.isEmpty() || sessions.isEmpty()) {
        setDetail(detail, QStringLiteral("nothing to save"));
        return false;
    }

    const QString cachePath = path();
    const QDir dir = QFileInfo(cachePath).dir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        setDetail(detail, QStringLiteral("cannot create %1").arg(dir.absolutePath()));
        return false;
    }

    QJsonObject root;
    QJsonObject rosters;
    if (QFile::exists(cachePath)) {
        readRosters(&root);
        rosters = root.value(QStringLiteral("rosters")).toObject();
    }

    QJsonObject roster;
    roster.insert(QStringLiteral("savedAt"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    roster.insert(QStringLiteral("sessions"), QJsonArray::fromVariantList(sessions));
    rosters.insert(endpoint, roster);

    root.insert(QStringLiteral("version"), CacheVersion);
    root.insert(QStringLiteral("rosters"), rosters);

    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setDetail(detail, file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        setDetail(detail, file.errorString());
        return false;
    }
    setDetail(detail, cachePath);
    return true;
}
