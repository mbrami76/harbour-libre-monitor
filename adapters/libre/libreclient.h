// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBRECLIENT_H
#define LM_LIBRECLIENT_H

#include "httptransport.h"
#include "libreerror.h"
#include "time/utctime.h"

#include <QElapsedTimer>
#include <QJsonValue>
#include <QString>

#include <functional>

namespace lm {

// Values of the two headers the source expects on every call (c). They may need
// raising when the source starts refusing an old version (a 403 is the suspected sign);
// this is the only place they are defined.
inline QByteArray libreProductHeader() { return "llu.android"; }
inline QByteArray libreVersionHeader() { return "4.16.0"; }

// Host used until a region is known (b, the global host accepted an EU
// account without redirect).
inline QString libreDefaultHost() { return QStringLiteral("api.libreview.io"); }

// Host of a region code (eu, us, de, fr, jp, ap, au, ae, ca); empty for any other text.
QString libreHostForRegion(const QString &region);

// True for the default host and the regional hosts above, nothing else. Credentials and
// tokens are sent only to these hosts, whatever a reply or a stored blob says.
bool isAllowedLibreHost(const QString &host);

struct LibreConfig
{
    LibreConfig() : host(libreDefaultHost()) {}

    QString host;           // from `configure` (region_host) or the default
#ifdef LIBREMON_DEVSOURCE
    // Development builds only (REQ-SIM-06): base URL such as "http://127.0.0.1:8080",
    // plain HTTP allowed. Replaces the host in every URL; no redirect is followed.
    QString baseUrlOverride;
#endif
};

// Time left for one command (`deadline_ms`). Every request takes the
// remaining time as its timeout.
class Deadline
{
public:
    explicit Deadline(int totalMs) : m_total(totalMs) { m_timer.start(); }
    int remainingMs() const
    {
        const qint64 left = qint64(m_total) - m_timer.elapsed();
        return left > 0 ? int(left) : 0;
    }

private:
    int m_total;
    QElapsedTimer m_timer;
};

// What the core must keep between runs (REQ-ADP-03): token, expiry, host and the
// `account-id` hash. Never the user identifier or any personal datum of the login reply.
struct LibreSession
{
    LibreSession() : expiresUnix(0) {}

    QString token;
    qint64 expiresUnix;     // Unix seconds, as the source reports
    QString host;
    QString accountId;      // SHA-256 hex of the user identifier

    bool isComplete() const;
    QString toBlob() const;                         // opaque to the core
    static bool fromBlob(const QString &blob, LibreSession *out);
};

// HTTP side of the adapter: headers, login and session, status mapping. Holds no
// credentials after `authenticate` returns. Nothing about requests or replies is logged
// except the value of the `lvapi` header when it changes.
class LibreClient
{
public:
    typedef std::function<void(const QString &level, const QString &text)> Logger;
    typedef std::function<UtcTime()> Clock;

    LibreClient(HttpTransport *transport, const LibreConfig &config);

    void setLogger(const Logger &logger) { m_logger = logger; }
    void setClock(const Clock &clock) { m_clock = clock; }

    // Host from `configure`; false (and unchanged) if it is not an allowed host.
    bool setHost(const QString &host);

    struct AuthResult
    {
        AuthResult() : expiresUnix(0), hostChanged(false) {}
        QString blob;
        qint64 expiresUnix;
        QString host;
        bool hostChanged;   // a regional redirect moved the session to another host
    };

    // Reuses `blob` when it is complete, not expired and the source still accepts it
    // (one GET of the connections; HTTP 400 or 401 means rejected), otherwise logs in.
    bool authenticate(const QString &email, const QString &password, const QString &blob,
                      Deadline &deadline, AuthResult *result, LibreError *error);

    bool hasSession() const { return m_session.isComplete(); }

    // GET of an authenticated path; returns the `data` member of the reply (may be null).
    bool getData(const QString &path, Deadline &deadline, QJsonValue *data, LibreError *error);

private:
    bool perform(const QByteArray &method, const QString &host, const QString &path,
                 const QByteArray &body, const LibreSession *auth, Deadline &deadline,
                 QJsonObject *reply, LibreError *error);
    bool login(const QString &email, const QString &password, Deadline &deadline,
               LibreSession *session, bool *hostChanged, LibreError *error);
    QString baseUrl(const QString &host) const;
    void log(const QString &level, const QString &text) const;
    UtcTime now() const;

    HttpTransport *m_transport;
    LibreConfig m_config;
    LibreSession m_session;
    Logger m_logger;
    Clock m_clock;
    QByteArray m_lastLvapi;
};

} // namespace lm

#endif
