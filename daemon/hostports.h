// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_HOSTPORTS_H
#define LM_HOSTPORTS_H

// Stand-ins for the ports that only the phone has, so the service runs and can be tested on the
// PC: a timer where the phone has
// BackgroundActivity, the console where it has the notification, and a secret store that holds
// nothing on disk. Built into the host service executable and the tests only; the packaged
// application uses phoneports.h.
//
// A plain timer is acceptable HERE because the PC does not suspend the way the phone does;
// REQ-ARC-08 forbids it as the clock of the real service, and the phone ports do not use it.

#include "alerts/alertcoordinator.h"
#include "daemon/provisioning.h"
#include "scheduler/schedulerports.h"
#include "service/serviceports.h"

#include <QHash>
#include <QObject>
#include <QTimer>

namespace lm {

class TimerWakeSource : public QObject, public WakeSource
{
    Q_OBJECT
public:
    // `frequenciesS` ascending; the default is the set the policy is written for.
    explicit TimerWakeSource(const QList<int> &frequenciesS = QList<int>(), QObject *parent = 0);

    QList<int> supportedFrequenciesS() const { return m_frequencies; }
    void setHandler(const std::function<void()> &handler) { m_handler = handler; }
    void arm(int frequencyS);
    void cancel();

private slots:
    void onTimeout();

private:
    QList<int> m_frequencies;
    QTimer m_timer;
    std::function<void()> m_handler;
};

// Writes the notice as one line on standard error, so a test or a developer can read the
// states. It carries only keys and opaque ids.
class ConsoleNotifier : public Notifier
{
public:
    void show(const Notice &notice);
    void withdraw();
};

// Writes each alert notification as one line on standard error, for the integration run
// and for a developer: "alert: kind=... subject=... slot=... alert=... state=..." and, for missing
// data, the cause. It carries what the effect carries, the Subject's UUID, the slot, the level
// word and the state, never a value or a name.
class ConsoleAlertNotifier : public AlertNotifier
{
public:
    void deliver(const AlertEffect &effect);
};

// The PC has no device lock: the approval is granted at once.
class ImmediateApproval : public ApprovalSource
{
public:
    void request(const QString &, const Done &done) { done(true); }
};

// Credentials and sessions in memory only. Holds nothing on disk, ever: the real store is
// Sailfish Secrets and a plain file with credentials is not allowed (REQ-SEC-01). Tests
// fill it with invented credentials; a development executable may fill it from a
// permission-checked file for the simulator (see hostmain, devsource only).
//
// It is also the fake of the CredentialWriter port, so the tests of the provisioning code
// write through the real Provisioner and read back through the same load() that the service
// uses: what is stored is what the service would read.
class MemorySecretStore : public SecretStore, public CredentialWriter
{
public:
    MemorySecretStore() : m_locked(false), m_failStore(false), m_failRemove(false) {}

    // CredentialWriter. Like the phone's store, a stored secret replaces an earlier one and
    // starts with no session.
    bool store(const QString &connectionId, const QString &username, const QString &password,
               QString *errorKey);
    bool remove(const QString &connectionId, QString *errorKey);
    // Test hooks: make the next calls fail, as a locked or broken platform store would.
    void failStores(bool fail) { m_failStore = fail; }
    void failRemoves(bool fail) { m_failRemove = fail; }
    bool hasSecret(const QString &connectionId) const { return m_credentials.contains(connectionId); }
    int storeCount() const { return m_stores; }

    void setCredentials(const QString &connectionId, const QString &user, const QString &password);
    // Test hook: the next loads answer `Locked`, as after a phone restart, until unlock().
    void lock() { m_locked = true; }
    void unlock() { m_locked = false; }

    void load(const QString &connectionId, const LoadCallback &done);
    void saveSession(const QString &connectionId, const QString &session, const UtcTime &expiresAt);
    void clearSession(const QString &connectionId);

    QString sessionOf(const QString &connectionId) const { return m_session.value(connectionId); }

private:
    bool m_locked;
    bool m_failStore;
    bool m_failRemove;
    int m_stores = 0;
    QHash<QString, Credentials> m_credentials;
    QHash<QString, QString> m_session;
    QHash<QString, UtcTime> m_expires;
};

} // namespace lm

#endif
