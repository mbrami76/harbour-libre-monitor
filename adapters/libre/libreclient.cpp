// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libreclient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegExp>
#include <QStringList>

namespace lm {

namespace {

const int kBlobVersion = 1;
// A blob this close to its expiry is not worth trying (seconds).
const qint64 kExpirySafetySeconds = 60;
// Upper bound for a Retry-After hint we pass on (one day).
const int kMaxRetryAfterSeconds = 24 * 3600;

QStringList knownRegions()
{
    return QStringList() << "eu" << "us" << "de" << "fr" << "jp" << "ap" << "au" << "ae" << "ca";
}

// Error mapping of CP-08 for the HTTP status. Returns true for 200, false with *error set
// for everything else. 403 and the other statuses are PROVISIONAL (never seen).
bool mapHttpStatus(const HttpResponse &r, LibreError *error)
{
    const int s = r.status;
    if (s == 200)
        return true;
    const QString code = QStringLiteral("HTTP ") + QString::number(s);
    if (s == 400 || s == 401) {
        *error = LibreError(LibreError::AuthFailed, code);
    } else if (s == 429) {
        int retry = -1;
        bool ok = false;
        const int v = QString::fromLatin1(r.retryAfter).trimmed().toInt(&ok);
        if (ok && v >= 0)
            retry = qMin(v, kMaxRetryAfterSeconds);
        *error = LibreError(LibreError::RateLimited, code, retry);
    } else if (s >= 500 && s <= 599) {
        *error = LibreError(LibreError::Transient, code);
    } else {
        // 403 (suspected: header version too old), other 4xx, 3xx and anything odd.
        *error = LibreError(LibreError::ProtocolChanged, code);
    }
    return false;
}

} // namespace

QString libreHostForRegion(const QString &region)
{
    if (!knownRegions().contains(region))
        return QString();
    return QStringLiteral("api-") + region + QStringLiteral(".libreview.io");
}

bool isAllowedLibreHost(const QString &host)
{
    if (host == libreDefaultHost())
        return true;
    const QStringList regions = knownRegions();
    for (int i = 0; i < regions.size(); ++i) {
        if (host == libreHostForRegion(regions.at(i)))
            return true;
    }
    return false;
}

bool LibreSession::isComplete() const
{
    QRegExp hex(QStringLiteral("[0-9a-f]{64}"));
    return !token.isEmpty() && expiresUnix > 0 && isAllowedLibreHost(host)
           && hex.exactMatch(accountId);
}

QString LibreSession::toBlob() const
{
    QJsonObject o;
    o.insert(QStringLiteral("v"), kBlobVersion);
    o.insert(QStringLiteral("token"), token);
    o.insert(QStringLiteral("expires"), double(expiresUnix));
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("account_id"), accountId);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool LibreSession::fromBlob(const QString &blob, LibreSession *out)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("v")).isDouble() || o.value(QStringLiteral("v")).toDouble() != kBlobVersion)
        return false;
    LibreSession s;
    s.token = o.value(QStringLiteral("token")).toString();
    s.expiresUnix = qint64(o.value(QStringLiteral("expires")).toDouble());
    s.host = o.value(QStringLiteral("host")).toString();
    s.accountId = o.value(QStringLiteral("account_id")).toString();
    if (!s.isComplete())
        return false;
    *out = s;
    return true;
}

LibreClient::LibreClient(HttpTransport *transport, const LibreConfig &config)
    : m_transport(transport), m_config(config)
{
    if (!isAllowedLibreHost(m_config.host))
        m_config.host = libreDefaultHost();
}

bool LibreClient::setHost(const QString &host)
{
    if (!isAllowedLibreHost(host))
        return false;
    m_config.host = host;
    return true;
}

void LibreClient::log(const QString &level, const QString &text) const
{
    if (m_logger)
        m_logger(level, text);
}

UtcTime LibreClient::now() const
{
    return m_clock ? m_clock() : UtcTime::now();
}

QString LibreClient::baseUrl(const QString &host) const
{
#ifdef LIBREMON_DEVSOURCE
    if (!m_config.baseUrlOverride.isEmpty()) {
        QString base = m_config.baseUrlOverride;
        while (base.endsWith(QLatin1Char('/')))
            base.chop(1);
        return base;
    }
#endif
    return QStringLiteral("https://") + host;
}

bool LibreClient::perform(const QByteArray &method, const QString &host, const QString &path,
                          const QByteArray &body, const LibreSession *auth, Deadline &deadline,
                          QJsonObject *reply, LibreError *error)
{
    HttpRequest req;
    req.method = method;
    req.url = QUrl::fromEncoded((baseUrl(host) + path).toLatin1());
    req.timeoutMs = deadline.remainingMs();
    req.body = body;
    if (req.timeoutMs <= 0) {
        *error = LibreError(LibreError::Transient, QStringLiteral("deadline exceeded"));
        return false;
    }

    typedef QPair<QByteArray, QByteArray> Header;
    req.headers << Header("product", libreProductHeader())
                << Header("version", libreVersionHeader())
                << Header("Accept", "application/json")
                << Header("Cache-Control", "no-cache");
    if (method == "POST")
        req.headers << Header("Content-Type", "application/json");
    if (auth) {
        req.headers << Header("Authorization", "Bearer " + auth->token.toUtf8())
                    << Header("account-id", auth->accountId.toLatin1());
    }

    const HttpResponse r = m_transport->send(req);

    switch (r.fault) {
    case HttpResponse::NoFault: break;
    case HttpResponse::Timeout:
        *error = LibreError(LibreError::Transient, QStringLiteral("request timed out"));
        return false;
    case HttpResponse::NetworkError:
        *error = LibreError(LibreError::Transient, QStringLiteral("network fault"));
        return false;
    case HttpResponse::TooLarge:
        *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("reply too large"));
        return false;
    }

    // The lvapi header is a backend version: worth a log line when it changes.
    static const QRegExp lvapiShape(QStringLiteral("v[0-9][0-9.]{0,20}"));
    if (!r.lvapi.isEmpty() && r.lvapi != m_lastLvapi) {
        m_lastLvapi = r.lvapi;
        const QString text = QString::fromLatin1(r.lvapi);
        log(QStringLiteral("info"), lvapiShape.exactMatch(text)
                ? QStringLiteral("source backend version ") + text
                : QStringLiteral("source backend version changed (unrecognised form)"));
    }

    if (!mapHttpStatus(r, error))
        return false;

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(r.body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("reply is not a JSON object"));
        return false;
    }
    const QJsonObject o = doc.object();
    const QJsonValue st = o.value(QStringLiteral("status"));
    // Both the HTTP status and the body status count.
    if (!st.isDouble() || st.toDouble() != int(st.toDouble())) {
        *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("reply has no integer status"));
        return false;
    }
    if (int(st.toDouble()) != 0) {
        *error = LibreError(LibreError::Transient,
                            QStringLiteral("source status ") + QString::number(int(st.toDouble())));
        return false;
    }
    *reply = o;
    return true;
}

bool LibreClient::login(const QString &email, const QString &password, Deadline &deadline,
                        LibreSession *session, bool *hostChanged, LibreError *error)
{
    QString host = m_config.host;
    bool redirected = false;
    *hostChanged = false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        QJsonObject body;
        body.insert(QStringLiteral("email"), email);
        body.insert(QStringLiteral("password"), password);

        QJsonObject reply;
        if (!perform("POST", host, QStringLiteral("/llu/auth/login"),
                     QJsonDocument(body).toJson(QJsonDocument::Compact), 0, deadline, &reply, error))
            return false;

        const QJsonValue dataValue = reply.value(QStringLiteral("data"));
        if (!dataValue.isObject()) {
            *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("login reply has no data object"));
            return false;
        }
        const QJsonObject data = dataValue.toObject();

        // Regional redirect: described by the author's code, never observed.
        if (data.value(QStringLiteral("redirect")).toBool()) {
            const QString region = data.value(QStringLiteral("region")).toString();
            const QString regionHost = libreHostForRegion(region);
            bool overridden = false;
#ifdef LIBREMON_DEVSOURCE
            overridden = !m_config.baseUrlOverride.isEmpty();
#endif
            if (redirected || overridden || regionHost.isEmpty()) {
                *error = LibreError(LibreError::ProtocolChanged,
                                    QStringLiteral("login redirect not usable"));
                return false;
            }
            host = regionHost;
            redirected = true;
            log(QStringLiteral("info"), QStringLiteral("login redirected to a regional host"));
            continue;
        }

        // Only the ticket and the user identifier are read; the rest of `user` is
        // personal data and is never looked at.
        const QJsonObject ticket = data.value(QStringLiteral("authTicket")).toObject();
        const QString token = ticket.value(QStringLiteral("token")).toString();
        const QJsonValue expires = ticket.value(QStringLiteral("expires"));
        const QString userId = data.value(QStringLiteral("user")).toObject()
                                   .value(QStringLiteral("id")).toString();
        if (token.isEmpty() || !expires.isDouble() || expires.toDouble() <= 0 || userId.isEmpty()) {
            // Includes the terms reply, which was never observed: never accepted.
            *error = LibreError(LibreError::ProtocolChanged,
                                QStringLiteral("login reply lacks the expected ticket or user id"));
            return false;
        }

        LibreSession s;
        s.token = token;
        s.expiresUnix = qint64(expires.toDouble());
        s.host = host;
        s.accountId = QString::fromLatin1(
            QCryptographicHash::hash(userId.toUtf8(), QCryptographicHash::Sha256).toHex());
        if (!s.isComplete()) {
            *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("login reply not usable"));
            return false;
        }
        *session = s;
        *hostChanged = host != m_config.host;
        return true;
    }
    *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("login redirect not usable"));
    return false;
}

bool LibreClient::authenticate(const QString &email, const QString &password, const QString &blob,
                               Deadline &deadline, AuthResult *result, LibreError *error)
{
    m_session = LibreSession();

    LibreSession stored;
    if (!blob.isEmpty() && LibreSession::fromBlob(blob, &stored)) {
        const UtcTime t = now();
        const bool fresh = !t.isValid()
                           || stored.expiresUnix * 1000 > t.msecsSinceEpoch() + kExpirySafetySeconds * 1000;
        if (fresh) {
            m_session = stored;
            QJsonValue ignored;
            LibreError probeError;
            if (getData(QStringLiteral("/llu/connections"), deadline, &ignored, &probeError)) {
                result->blob = blob;
                result->expiresUnix = stored.expiresUnix;
                result->host = stored.host;
                result->hostChanged = false;
                return true;
            }
            m_session = LibreSession();
            if (probeError.cls != LibreError::AuthFailed) {
                *error = probeError;     // network trouble and the like: do not log in blindly
                return false;
            }
            log(QStringLiteral("info"), QStringLiteral("stored session rejected, logging in"));
        }
    }

    LibreSession fresh;
    bool hostChanged = false;
    if (!login(email, password, deadline, &fresh, &hostChanged, error))
        return false;
    m_session = fresh;
    result->blob = fresh.toBlob();
    result->expiresUnix = fresh.expiresUnix;
    result->host = fresh.host;
    result->hostChanged = hostChanged;
    return true;
}

bool LibreClient::getData(const QString &path, Deadline &deadline, QJsonValue *data, LibreError *error)
{
    if (!m_session.isComplete()) {
        *error = LibreError(LibreError::AuthFailed, QStringLiteral("not authenticated"));
        return false;
    }
    QJsonObject reply;
    if (!perform("GET", m_session.host, path, QByteArray(), &m_session, deadline, &reply, error))
        return false;
    if (!reply.contains(QStringLiteral("data"))) {
        *error = LibreError(LibreError::ProtocolChanged, QStringLiteral("reply has no data member"));
        return false;
    }
    *data = reply.value(QStringLiteral("data"));
    return true;
}

} // namespace lm
