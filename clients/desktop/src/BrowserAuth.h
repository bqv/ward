#pragma once

// BrowserAuth — mint DSH's own signed session cookie.
//
// DSH's browser auth cannot be disabled: every /api request carries a signed
// cookie, and there is no bearer-token path. The cookie is minted locally from a
// secret this user owns, which is the right shape for a first-class local
// client: it is the same secret the harness and nginx use, so nothing has to be
// borrowed from a browser or reconfigured on the host.
//
// Scheme (verified against the installed 0.2.0 `dsh-client-connection`):
//
//   name  = "dsh-auth-" + b64url(sha256(authority))
//   value = "v1." + body + "." + b64url(hmac_sha256(secret, body))
//   body  = b64url(JSON {version, authority, issuedAt, expiresAt})
//
// Two details that are easy to get wrong, both of which cost a silent 401:
//
//  * The authority is the one the *server* believes it is served at, not the
//    client's Host. Behind the auth proxy that is the carrier, 127.0.0.1:8081 —
//    a cookie minted for the authority on :8080 is rejected.
//  * The HMAC is over the **base64url body string**, not the raw JSON.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

class BrowserAuth
{
public:
    /// How long a minted cookie claims to be valid. Must not exceed the host's
    /// `connection.cookieMaxAgeDays` (3650 in this deployment) or the cookie is
    /// rejected for exceeding the maximum age.
    static constexpr int DefaultMaxAgeDays = 3650;

    /// Backdate `issuedAt` slightly so a server clock running ahead of ours does
    /// not see a not-yet-valid cookie. Cheap insurance against a hard-to-read 401.
    static constexpr qint64 ClockSkewToleranceMs = 120000;

    BrowserAuth();

    /// The harness home: $DSH_HOME when set, else ~/.dsh. The credential file
    /// lives there as `.credentials.yaml`.
    static QString resolveHome();

    /// Read the browser-session secret. Returns a null QByteArray on failure and
    /// fills `error` with something a human can act on — never fails silently,
    /// because the only symptom upstream is a bare 401.
    QByteArray secret(QString *error = nullptr) const;

    /// Mint the Cookie header value for `authority`, e.g. "127.0.0.1:8081".
    /// Results are cached per authority; minting is cheap, but there is no reason
    /// to redo it per request.
    QString cookieHeader(const QString &authority, QString *error = nullptr) const;

    /// The Cookie header value for cookies already minted, or an empty string.
    QString cachedCookieHeader(const QString &authority) const { return m_cache.value(authority); }

    /// The candidate authorities this host answers on, most likely first. The
    /// proxy-fronted carrier comes first because that is the authority the
    /// running system actually validates.
    static QStringList candidateAuthorities(int carrierPort = 8081);

private:
    QString m_credentialsPath;

    // Lazily populated by the const accessors below, so the caches are mutable:
    // reading the secret and minting a cookie are logically const operations on
    // an object whose only job is to answer "what cookie?" for a given authority.
    mutable QByteArray m_secret;
    mutable bool m_secretLoaded = false;
    mutable QHash<QString, QString> m_cache;
};
