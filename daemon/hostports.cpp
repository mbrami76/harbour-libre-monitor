// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "hostports.h"

#include <cstdio>

namespace lm {

TimerWakeSource::TimerWakeSource(const QList<int> &frequenciesS, QObject *parent)
    : QObject(parent), m_frequencies(frequenciesS)
{
    if (m_frequencies.isEmpty())
        m_frequencies << 30 << 150 << 300 << 600 << 900 << 1800 << 3600;
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

void TimerWakeSource::arm(int frequencyS)
{
    m_timer.start(frequencyS * 1000);
}

void TimerWakeSource::cancel()
{
    m_timer.stop();
}

void TimerWakeSource::onTimeout()
{
    if (m_handler)
        m_handler();
}

void ConsoleNotifier::show(const Notice &n)
{
    std::fprintf(stderr, "notice: state=%s connection=%s cause=%s reason=%s\n",
                 qPrintable(noticeStateName(n.state)), qPrintable(n.connection),
                 qPrintable(n.cause), qPrintable(n.stopReason));
}

void ConsoleNotifier::withdraw()
{
    std::fprintf(stderr, "notice: withdrawn\n");
}

void ConsoleAlertNotifier::deliver(const AlertEffect &e)
{
    static const char *kinds[] = { "journal", "notify", "remind", "update", "withdraw", "interruption" };
    if (e.kind == AlertEffect::Interruption) {
        std::fprintf(stderr, "alert: kind=interruption from=%s to=%s\n", qPrintable(e.gapFrom.toIso()),
                     qPrintable(e.gapTo.toIso()));
        return;
    }
    std::fprintf(stderr, "alert: kind=%s subject=%s slot=%s alert=%s state=%s cause=%s silent=%d\n", kinds[e.kind],
                 qPrintable(e.subject.toString()), qPrintable(alertSlotName(e.slot)),
                 qPrintable(alertTypeName(e.type)), qPrintable(alertStateName(e.state)), qPrintable(e.cause),
                 e.silent ? 1 : 0);
}

void MemorySecretStore::setCredentials(const QString &connectionId, const QString &user,
                                       const QString &password)
{
    Credentials c;
    c.username = user;
    c.password = password;
    m_credentials.insert(connectionId, c);
}

bool MemorySecretStore::store(const QString &connectionId, const QString &username,
                              const QString &password, QString *errorKey)
{
    if (m_failStore) {
        *errorKey = QStringLiteral("secret-store-failed");
        return false;
    }
    ++m_stores;
    setCredentials(connectionId, username, password);
    m_session.remove(connectionId);
    m_expires.remove(connectionId);
    return true;
}

bool MemorySecretStore::remove(const QString &connectionId, QString *errorKey)
{
    if (m_failRemove) {
        *errorKey = QStringLiteral("secret-store-failed");
        return false;
    }
    m_credentials.remove(connectionId);
    m_session.remove(connectionId);
    m_expires.remove(connectionId);
    return true;
}

void MemorySecretStore::load(const QString &connectionId, const LoadCallback &done)
{
    SecretLoad r;
    if (m_locked) {
        r.kind = SecretLoad::Locked;
    } else if (!m_credentials.contains(connectionId)) {
        r.kind = SecretLoad::Missing;
    } else {
        r.kind = SecretLoad::Ok;
        r.credentials = m_credentials.value(connectionId);
        r.session = m_session.value(connectionId);
        r.expiresAt = m_expires.value(connectionId);
    }
    done(r);
}

void MemorySecretStore::saveSession(const QString &connectionId, const QString &session,
                                    const UtcTime &expiresAt)
{
    m_session.insert(connectionId, session);
    m_expires.insert(connectionId, expiresAt);
}

void MemorySecretStore::clearSession(const QString &connectionId)
{
    m_session.remove(connectionId);
    m_expires.remove(connectionId);
}

} // namespace lm
