// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "commonports.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDBusMessage>

#include <errno.h>
#include <sys/stat.h>
#include <time.h>

namespace lm {

namespace {

qint64 clockMs(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0)
        return 0;
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

UtcTime SystemServiceClock::now() const
{
    return UtcTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch());
}

qint64 SystemServiceClock::bootTimeMs() const { return clockMs(CLOCK_BOOTTIME); }
qint64 SystemServiceClock::monotonicMs() const { return clockMs(CLOCK_MONOTONIC); }

QrandRandom::QrandRandom()
{
    qsrand(uint(QDateTime::currentMSecsSinceEpoch() & 0xffffffff));
}

double QrandRandom::next()
{
    return double(qrand()) / (double(RAND_MAX) + 1.0);
}

// ---- the bus name ---------------------------------------------------------------------------

DBusNameOwner::DBusNameOwner(const QString &busName, QObject *parent)
    : QObject(parent), m_name(busName), m_owned(false)
{
}

bool DBusNameOwner::take()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface())
        return false;
    // DontQueueService: a second instance must learn at once that it is second, not wait in
    // the bus's queue for the first one to end (REQ-ARC-09).
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        bus.interface()->registerService(m_name, QDBusConnectionInterface::DontQueueService,
                                         QDBusConnectionInterface::DontAllowReplacement);
    m_owned = reply.isValid() && reply.value() == QDBusConnectionInterface::ServiceRegistered;
    if (m_owned)
        connect(bus.interface(), SIGNAL(serviceUnregistered(QString)),
                this, SLOT(onServiceUnregistered(QString)), Qt::UniqueConnection);
    return m_owned;
}

void DBusNameOwner::release()
{
    if (!m_owned)
        return;
    m_owned = false;
    QDBusConnection::sessionBus().unregisterService(m_name);
}

void DBusNameOwner::onServiceUnregistered(const QString &name)
{
    if (name != m_name || !m_owned)
        return;
    m_owned = false;
    if (m_lost)
        m_lost();
}

// ---- the settings file --------------------------------------------------------------------

JsonFileSettings::JsonFileSettings(const QString &path) : m_path(path), m_loaded(false) {}

void JsonFileSettings::load() const
{
    if (m_loaded)
        return;
    m_loaded = true;
    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.read(1 << 20));
        if (doc.isObject())
            m_root = doc.object();
    }
}

void JsonFileSettings::save()
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QFile tmp(m_path + QStringLiteral(".tmp"));
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;     // settings are conveniences; a failed write is retried at the next change
    tmp.write(QJsonDocument(m_root).toJson(QJsonDocument::Compact));
    tmp.close();
    QFile::remove(m_path);
    tmp.rename(m_path);
}

QString JsonFileSettings::value(const QString &key) const
{
    load();
    return m_root.value(QStringLiteral("service")).toObject().value(key).toString();
}

void JsonFileSettings::setValue(const QString &key, const QString &value)
{
    load();
    QJsonObject s = m_root.value(QStringLiteral("service")).toObject();
    if (s.value(key).toString() == value)
        return;
    s.insert(key, value);
    m_root.insert(QStringLiteral("service"), s);
    save();
}

QJsonObject JsonFileSettings::get(const QString &connectionId)
{
    load();
    return m_root.value(QStringLiteral("connections")).toObject().value(connectionId).toObject();
}

void JsonFileSettings::merge(const QString &connectionId, const QJsonObject &settings)
{
    load();
    QJsonObject all = m_root.value(QStringLiteral("connections")).toObject();
    QJsonObject one = all.value(connectionId).toObject();
    for (QJsonObject::const_iterator it = settings.constBegin(); it != settings.constEnd(); ++it)
        one.insert(it.key(), it.value());
    all.insert(connectionId, one);
    m_root.insert(QStringLiteral("connections"), all);
    save();
}

// ---- the version stamp and the self-activation ---------------------------------

StampRead FileVersionStamp::read()
{
    // stat() first, so that only "no such file" (or a folder that is gone) counts as a removed
    // package; any other failure (a permission, an I/O error) is a doubtful read and never stops
    // the service.
    struct stat st;
    if (::stat(QFile::encodeName(m_path).constData(), &st) != 0)
        return (errno == ENOENT || errno == ENOTDIR) ? StampRead::missing() : StampRead::unreadable();
    if (!S_ISREG(st.st_mode))
        return StampRead::unreadable();
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return StampRead::unreadable();
    const QByteArray bytes = f.read(256);
    if (f.error() != QFileDevice::NoError)
        return StampRead::unreadable();
    return StampRead::present(QString::fromUtf8(bytes));
}

bool DBusSelfActivator::activate(QString *how)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (how) *how = QStringLiteral("no-bus");
        return false;
    }
    // 1. A ping to our own name: the bus has no holder for it any more (the stop released it), so it
    // activates the service from its activation file, which Sailjail wrote for the NEW package. The
    // new process answers the ping once it owns the name (the bus library answers Peer.Ping itself).
    QDBusMessage ping = QDBusMessage::createMethodCall(m_name, m_path, QStringLiteral("org.freedesktop.DBus.Peer"),
                                                       QStringLiteral("Ping"));
    ping.setAutoStartService(true);
    const QDBusMessage pong = bus.call(ping, QDBus::Block, m_timeoutMs);
    if (pong.type() == QDBusMessage::ReplyMessage) {
        if (how) *how = QStringLiteral("ping");
        return true;
    }
    const QString pingError = pong.errorName();

    // 2. The bus's own activation request, in case a filter in the sandbox lets the bus's methods
    // through but not an auto-starting call to the name.
    QDBusMessage start = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                        QStringLiteral("/org/freedesktop/DBus"),
                                                        QStringLiteral("org.freedesktop.DBus"),
                                                        QStringLiteral("StartServiceByName"));
    start << m_name << uint(0);
    const QDBusMessage started = bus.call(start, QDBus::Block, m_timeoutMs);
    if (started.type() == QDBusMessage::ReplyMessage) {
        if (how) *how = QStringLiteral("start-service-by-name");
        return true;
    }
    // Only the D-Bus error names: they hold no name of a person, no path and no value.
    if (how)
        *how = QStringLiteral("failed ping=%1 start=%2").arg(pingError, started.errorName());
    return false;
}

} // namespace lm
