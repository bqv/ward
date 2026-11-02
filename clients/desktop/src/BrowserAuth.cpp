#include "BrowserAuth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QTextStream>

namespace {

/// base64url without padding — the encoding DSH uses for every generated value.
QByteArray toBase64Url(const QByteArray &raw)
{
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray fromBase64Url(const QByteArray &text)
{
    return QByteArray::fromBase64(text, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

} // namespace

BrowserAuth::BrowserAuth()
    : m_credentialsPath(QDir(resolveHome()).filePath(QStringLiteral(".credentials.yaml")))
{
}

QString BrowserAuth::resolveHome()
{
    const QString fromEnv = qEnvironmentVariable("DSH_HOME");
    if (!fromEnv.trimmed().isEmpty())
        return fromEnv;
    return QDir::homePath() + QStringLiteral("/.dsh");
}

QStringList BrowserAuth::candidateAuthorities(int carrierPort)
{
    // Order matters. The running system validates the authority it was told it is
    // served at; with the auth proxy in front that is the carrier port, not the
    // proxy port a browser would use.
    return {
        QStringLiteral("127.0.0.1:%1").arg(carrierPort),
        QStringLiteral("localhost:%1").arg(carrierPort),
    };
}

QByteArray BrowserAuth::secret(QString *error) const
{
    if (m_secretLoaded)
        return m_secret;

    m_secretLoaded = true;

    QFile file(m_credentialsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot read %1 (%2)").arg(m_credentialsPath, file.errorString());
        return {};
    }

    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    // Narrow read, deliberately: the trailing `refs:` block holds unrelated
    // provider keys, so this looks only for a `secret:` line and takes the first
    // one (the browser-session grant). A schema-aware YAML parse would pull in a
    // dependency for one field.
    static const QRegularExpression re(
        QStringLiteral("^\\s*secret:\\s*(\\S+)\\s*$"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) {
        if (error)
            *error = QStringLiteral("no browser-session secret in %1").arg(m_credentialsPath);
        return {};
    }

    const QByteArray encoded = match.captured(1).toLatin1();
    m_secret = fromBase64Url(encoded);
    if (m_secret.isEmpty()) {
        if (error)
            *error = QStringLiteral("browser-session secret in %1 is not valid base64url")
                         .arg(m_credentialsPath);
    }
    return m_secret;
}

QString BrowserAuth::cookieHeader(const QString &authority, QString *error) const
{
    const auto cached = m_cache.constFind(authority);
    if (cached != m_cache.constEnd())
        return cached.value();

    const QByteArray key = secret(error);
    if (key.isEmpty())
        return {};

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 issuedAt = now - ClockSkewToleranceMs;

    QJsonObject payload{
        {QStringLiteral("version"), 1},
        {QStringLiteral("authority"), authority},
        {QStringLiteral("issuedAt"), issuedAt},
        {QStringLiteral("expiresAt"), issuedAt + qint64(DefaultMaxAgeDays) * 86400 * 1000},
    };

    const QByteArray body = toBase64Url(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QByteArray signature = toBase64Url(hmacSha256(key, body));

    const QString name = QStringLiteral("dsh-auth-")
        + QString::fromLatin1(toBase64Url(sha256(authority.toUtf8())));
    const QString value = QStringLiteral("v1.%1.%2")
        .arg(QString::fromLatin1(body), QString::fromLatin1(signature));

    const QString header = name + QLatin1Char('=') + value;
    m_cache.insert(authority, header);

    // The cookie name is derived from the authority, so it is safe to log; the
    // value is a bearer credential and is never logged.
    qInfo() << "minted browser-session cookie for authority" << authority
            << "name" << name;
    return header;
}
