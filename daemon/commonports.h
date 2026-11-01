// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_COMMONPORTS_H
#define LM_COMMONPORTS_H

// The real implementations of the service ports that need nothing from the phone, so the same
// code runs on the PC and on the device: the system clocks, the D-Bus name, the non-secret
// settings file and the random source. The ports that only the phone has (keepalive wake-ups,
// the notification, Sailfish Secrets) are in phoneports.h; their stand-ins for the PC are in
// hostports.h.

#include "scheduler/schedulerports.h"
#include "service/serviceports.h"
#include "service/versionwatch.h"

#include <QJsonObject>
#include <QObject>

#include <functional>

namespace lm {

// Wall time from the system, CLOCK_BOOTTIME and CLOCK_MONOTONIC from clock_gettime
// (REQ-SCH-09). Linux-only by design: the application runs on Sailfish OS, and the PC build
// runs in the SDK engine, which is Linux too.
class SystemServiceClock : public ServiceClock
{
public:
    UtcTime now() const;
    qint64 bootTimeMs() const;
    qint64 monotonicMs() const;
};

// Jitter for the backoff (REQ-SCH-04). Uses qrand, which is enough: nothing here is secret.
class QrandRandom : public RandomSource
{
public:
    QrandRandom();
    double next();
};

// Owns the D-Bus name on the session bus (REQ-ARC-09). take() asks the bus not to queue, so
// an instance that is second finds out at once and exits. The name can also be taken away
// (a replace request is NOT allowed, so in practice only when the bus goes away); `lost`
// is then called, which the runtime turns into the stop reason `name-lost`.
class DBusNameOwner : public QObject, public NameOwner
{
    Q_OBJECT
public:
    explicit DBusNameOwner(const QString &busName, QObject *parent = 0);

    bool take();
    void release();
    void setLostHandler(const std::function<void()> &lost) { m_lost = lost; }

private slots:
    void onServiceUnregistered(const QString &name);

private:
    QString m_name;
    bool m_owned;
    std::function<void()> m_lost;
};

// A small JSON file of non-secret key/value pairs, written atomically (to a temporary file,
// then renamed). Two roles: the service's own settings (for example the id of the resident
// notice) and the settings an adapter asks the core to keep
// (`settings_update`, for example the regional host). Never credentials, never names.
class JsonFileSettings : public ServiceSettingsStore, public ConnectionSettingsStore
{
public:
    explicit JsonFileSettings(const QString &path);

    // ServiceSettingsStore
    QString value(const QString &key) const;
    void setValue(const QString &key, const QString &value);

    // ConnectionSettingsStore
    QJsonObject get(const QString &connectionId);
    void merge(const QString &connectionId, const QJsonObject &settings);

private:
    void load() const;
    void save();

    QString m_path;
    mutable bool m_loaded;
    mutable QJsonObject m_root;     // { "service": {k: v}, "connections": {id: {..}} }
};

// The package's version stamp (the packaging installs it): /usr/share/<package>/version, one
// line with the package's Version-Release. Inside Sailjail that folder is a live read-only view of
// the installed files, so an upgrade or a removal shows at once.
// Absent file: Missing. A file that exists but cannot be opened or read: Unreadable. At most 256
// bytes are read; whether the text is a version is VersionWatch's to judge.
class FileVersionStamp : public VersionStampReader
{
public:
    explicit FileVersionStamp(const QString &path) : m_path(path) {}
    StampRead read();

private:
    QString m_path;
};

// Starts the service again after it stopped for an upgrade and released its name.
// First a `org.freedesktop.DBus.Peer.Ping` to the service's own name, which is what the add-on
// timer sends and what the bus answers by activating the service (REQ-PKG-03, S5); if that fails,
// the bus's own `StartServiceByName`. Both wait for the new process (at most `timeoutMs` each), so
// the old process exits only after the new one owns the name or the attempt failed. Whether this
// works from inside the Sailjail sandbox was measured on the emulator.
class DBusSelfActivator : public ServiceActivator
{
public:
    DBusSelfActivator(const QString &busName, const QString &objectPath, int timeoutMs = 30000)
        : m_name(busName), m_path(objectPath), m_timeoutMs(timeoutMs) {}
    bool activate(QString *how);

private:
    QString m_name;
    QString m_path;
    int m_timeoutMs;
};

} // namespace lm

#endif
