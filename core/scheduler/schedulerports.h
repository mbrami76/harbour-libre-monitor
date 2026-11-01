// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDULERPORTS_H
#define LM_SCHEDULERPORTS_H

// Interfaces through which the scheduler and the session manager reach everything outside
// their own logic. Pattern: ports
// and adapters. The logic is pure; the link, the secret store, the configuration
// file and the store are injected, so tests drive it with scripted fakes and no real time or
// process. Real implementations: the link wrapper and the store wrapper
// in this folder; the secret store and the configuration come.

#include "session/adapterlink.h"        // Credentials, CommandReport, SessionGrant & co
#include "session/linkpolicy.h"
#include "time/utctime.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>
#include <QVector>

#include <functional>

namespace lm {

// ---- the link ----------------------------------------------------------------------------

// What the scheduler needs from one adapter link. The real
// AdapterLink is wrapped by AdapterLinkPort; tests use a scripted fake. One command at a time;
// every operation completes through its callback exactly once.
class LinkPort
{
public:
    typedef std::function<void(const StartResult &, const QString &)> StartCallback;
    typedef std::function<void(const CommandReport &, const SessionGrant &)> AuthCallback;
    typedef std::function<void(const CommandReport &, const QList<SubjectInfo> &)> DiscoverCallback;
    typedef std::function<void(const CommandReport &, const FetchCompletion &)> FetchCallback;
    typedef std::function<void()> StopCallback;

    virtual ~LinkPort() {}
    virtual void start(const QString &adapterId, const QString &connectionId,
                       const QJsonObject &settings, const StartCallback &done) = 0;
    virtual void authenticate(const Credentials &credentials, const QString &session,
                              const AuthCallback &done) = 0;
    virtual void discoverSubjects(const DiscoverCallback &done) = 0;
    virtual void fetchCurrent(const QString &subjectRef, const FetchCallback &done) = 0;
    virtual void fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                            const QStringList &feeds, const FetchCallback &done) = 0;
    virtual void stop(const StopCallback &done) = 0;
    virtual bool isAlive() const = 0;
    virtual LinkId linkId() const = 0;
};

// Makes the link of a connection. The scheduler owns what it gets and deletes it after the
// link has ended (never inside one of its own callbacks).
class LinkFactory
{
public:
    virtual ~LinkFactory() {}
    virtual LinkPort *create(const QString &connectionId) = 0;
};

// ---- secrets and settings -----------------------------------------------------

struct SecretLoad
{
    enum Kind { Ok, Locked, Missing, Error };

    SecretLoad() : kind(Error) {}

    Kind kind;
    Credentials credentials;    // Ok only
    QString session;            // the stored opaque blob, may be empty
    UtcTime expiresAt;          // invalid = unknown
};

// Credentials and the session blob live only here, in Sailfish Secrets (REQ-SEC-01,
// REQ-SEC-05). `Locked` is a normal state after a phone restart: the first access needs the
// user's one-time approval, and it is the access attempt that raises the notification.
class SecretStore
{
public:
    typedef std::function<void(const SecretLoad &)> LoadCallback;

    virtual ~SecretStore() {}
    virtual void load(const QString &connectionId, const LoadCallback &done) = 0;
    virtual void saveSession(const QString &connectionId, const QString &session,
                             const UtcTime &expiresAt) = 0;
    virtual void clearSession(const QString &connectionId) = 0;
};

// The non-secret settings an adapter asks the core to keep (`settings_update`, for example
// the regional host). The configuration file in the real service.
class ConnectionSettingsStore
{
public:
    virtual ~ConnectionSettingsStore() {}
    virtual QJsonObject get(const QString &connectionId) = 0;
    virtual void merge(const QString &connectionId, const QJsonObject &settings) = 0;
};

// ---- the measurement store ---------------------------------------------------------------

// The part of the store the scheduler uses (the store offers it as StoreReadingStore). Kept
// narrow on purpose: the scheduler never sees a name, only the opaque Subject UUID.
class ReadingStore
{
public:
    virtual ~ReadingStore() {}
    // Measurement time of the newest stored record of the Subject for a source tag such as
    // "libre.current"; invalid when there is none.
    virtual UtcTime newestMeasurement(const QUuid &subject, const QString &source) = 0;
    // Stores valid records. Returns the number added, or -1 when the store cannot take them
    // now (maintenance, REQ-STO-15): the scheduler then keeps them in its bounded queue.
    virtual int ingest(const QUuid &subject, const LinkId &link, const QVector<Record> &records) = 0;
    virtual void quarantine(const LinkId &link, const QString &reason, const QString &detail,
                            const QJsonObject &rawRecord) = 0;
    // The newest known sensor of the Subject (REQ-SEN-01), for the warm-up cause.
    virtual bool latestSensor(const QUuid &subject, UtcTime *activatedAt, int *warmupMinutes) = 0;
    virtual void noteSensor(const QUuid &subject, const QJsonObject &sensor) = 0;
};

} // namespace lm

#endif
