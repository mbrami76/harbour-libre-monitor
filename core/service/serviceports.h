// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICEPORTS_H
#define LM_SERVICEPORTS_H

// Interfaces through which the service lifecycle reaches
// everything that only a process or a phone has: the three clocks, the D-Bus name, the
// journal, the store, the notification, the keepalive wake-ups and the secret store's
// approval dialog. Pattern: ports and adapters, the same as modules 4 and 5 use. The
// lifecycle is pure logic that is polled by whoever owns the event loop, so every row of its
// transition table can be tested on the PC with a manual clock and fakes, and the same class
// runs on the phone with the real ports (REQ-ARC-07 to 10).
//
// Each port has a fake in tests/support/servicetest.h. The real ones are in daemon/ (they need
// QtDBus, libkeepalive, the notification library or Sailfish Secrets) or in this folder when
// they only wrap code of the core (journal, scheduler).

#include "guard/ports.h"
#include "scheduler/scheduler.h"
#include "store/journal.h"
#include "time/utctime.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace lm {

// ---- time ---------------------------------------------------------------------------------

// The three time sources of REQ-SCH-09. The wall time (Clock::now) is what records and the
// scheduler use. CLOCK_BOOTTIME keeps counting while the phone is suspended, CLOCK_MONOTONIC
// does not: the difference between two readings of both is the time spent suspended while
// The service kept running, which is not a gap (REQ-SCH-11). CLOCK_BOOTTIME also restarts at
// zero after a phone restart, which is how the start cause tells "restart" from "crash"
//.
class ServiceClock : public Clock
{
public:
    virtual qint64 bootTimeMs() const = 0;
    virtual qint64 monotonicMs() const = 0;
};

// ---- process identity -------------------------------------------------------------------

// The D-Bus name <OrganizationName>.<ApplicationName>. At most one service instance runs
// (REQ-ARC-09): the first thing a service does is try to take the name, and an instance that
// cannot exits without having written anything anywhere.
class NameOwner
{
public:
    virtual ~NameOwner() {}
    virtual bool take() = 0;        // false: another instance owns it
    virtual void release() = 0;
};

// ---- storage ------------------------------------------------------------------------------

// Opens and closes what the service needs on disk, and runs the short weekly housekeeping
// (REQ-STO-01, REQ-STO-02, REQ-SCH-14). The real one wraps the Store and the Journal of
// The store. Kept as one port because  treats "the journal or the store cannot be opened"
// as one failure with a cause key, and because a failed open must leave nothing half-open.
class ServiceStorage
{
public:
    virtual ~ServiceStorage() {}
    // On failure `cause` is a stable key ("journal-open-failed", "store-open-failed") for the
    // notice; it never carries a path or an error text that could hold a name.
    virtual bool open(QString *cause) = 0;
    virtual void close() = 0;
    virtual bool storageCheckDue(const UtcTime &now) = 0;
    virtual void runStorageCheck(const UtcTime &now) = 0;
};

// The part of the journal the lifecycle writes and reads (REQ-SCH-09, 10). JournalLifecycleLog
// wraps the real Journal; tests use an in-memory one, which can also be told to fail.
class ServiceJournal
{
public:
    virtual ~ServiceJournal() {}
    // The newest entry of the kinds heartbeat, start or stop (never prune, storage-check or
    // connection-state), by wall time. False when there is none.
    virtual bool lastLifecycleEntry(JournalEntry *entry) = 0;
    virtual bool hasStartRecord() = 0;
    virtual bool append(const JournalEntry &entry) = 0;
};

// ---- the notice, the wake-ups, the approval ---------------------------------------------

// The state of the one resident monitoring-state notice (REQ-ALR-06). Only
// keys and times travel here; the words belong to the user interface.
enum class NoticeState { Active, AwaitingApproval, Attention, Stopped };
QString noticeStateName(NoticeState state);

struct Notice
{
    Notice() : state(NoticeState::Active) {}

    NoticeState state;
    UtcTime lastHeartbeat;      // active and awaiting-approval
    QString connection;         // awaiting-approval: an opaque connection id, never a name
    QString cause;              // attention: the cause key
    UtcTime stoppedAt;          // stopped
    QString stopReason;         // stopped
};

// Posts or updates THE notice: always the same one (the real notifier keeps its id and replaces it in place, S4), so a service that fails to start every minute does
// not pile up notices. The notice outlives the service: that is why `stopped` is shown.
class Notifier
{
public:
    virtual ~Notifier() {}
    virtual void show(const Notice &notice) = 0;
    // Takes THE notice away without a banner ( the package was removed, so no application is
    // left that a "stopped" notice could lead to). Does nothing when no notice is known.
    virtual void withdraw() {}
};

// The keepalive wake-up (REQ-ARC-08, S3): the only clock that keeps running while the phone
// sleeps. One wake-up is armed at a time; arming again replaces the previous one. A plain
// timer must never stand in for it.
class WakeSource
{
public:
    virtual ~WakeSource() {}
    // The wake-up frequencies the platform offers, in seconds, ascending. The real source
    // reports what libkeepalive supports; records that this list is not verified.
    virtual QList<int> supportedFrequenciesS() const = 0;
    // What to call at each wake-up; set once, before the first arm().
    virtual void setHandler(const std::function<void()> &handler) = 0;
    virtual void arm(int frequencyS) = 0;
    virtual void cancel() = 0;
};

// The one-time user approval of the secret collection after a phone restart (REQ-SEC-05). request() performs the read that is allowed to raise the system's approval
// notification; `done(true)` means the user approved and the read worked. It may complete at
// once or much later, and it may never complete (the user ignores it).
class ApprovalSource
{
public:
    typedef std::function<void(bool granted)> Done;

    virtual ~ApprovalSource() {}
    virtual void request(const QString &connectionId, const Done &done) = 0;
};

// Non-secret settings of the service itself, for example the id of the resident notice
// A small file in the application's data directory in the
// real service; never credentials, never names.
class ServiceSettingsStore
{
public:
    virtual ~ServiceSettingsStore() {}
    virtual QString value(const QString &key) const = 0;
    virtual void setValue(const QString &key, const QString &value) = 0;
};

// ---- the scheduler, seen from the lifecycle ---------------------------------------------

// What the lifecycle asks of the scheduler. A narrow interface so that the
// lifecycle tests need no links and no store; SchedulerHostAdapter (schedulerhost.h) puts
// the real Scheduler behind it.
class SchedulerHost
{
public:
    virtual ~SchedulerHost() {}
    virtual void start(const UtcTime &serviceDownSince) = 0;
    virtual void tick() = 0;
    virtual UtcTime nextWakeAt() const = 0;     // invalid: nothing is due
    virtual void shutdown() = 0;
    // True once every adapter process has ended (shutdown gives them 2 s).
    virtual bool linksEnded() const = 0;
    // Readings held in memory because the store was busy (REQ-STO-15).
    virtual int queuedReadings() const = 0;
    // Tries to write the held readings; returns how many are still held (step 2).
    virtual int flushQueuedReadings() = 0;
    virtual void secretsAvailable(const QString &connectionId) = 0;
};

} // namespace lm

#endif
