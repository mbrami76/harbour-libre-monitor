// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICELIFECYCLE_H
#define LM_SERVICELIFECYCLE_H

// The lifecycle of the monitoring service process (REQ-ARC-07 to 10, REQ-SCH-09 to
// 11, REQ-SEC-05, REQ-ALR-06). It decides when the
// service starts, what it writes while it runs, how it stops and how it waits for the
// secret store's approval. It does NOT decide anything about polls, errors or trust: the
// scheduler and the guard do, and this class only hosts them.
//
// Pattern: a polled state machine behind ports, like the scheduler. It owns no timer, no
// thread and no event loop. The process calls start() once, onWake() at every keepalive
// wake-up, and after a stop request polls stepStop() until it reports completion. That is
// what lets the transition table be tested with a
// manual clock and fakes, and what keeps the wake-up the only clock (REQ-ARC-08).
//
// Assumptions: the journal is the only record of what happened while the service was down
// (nothing is written on a crash); the second instance must be able to find out that
// it is second without touching the journal or the store (REQ-ARC-09); a stop request can
// arrive at any time, also during start-up. Everything runs on one thread.

#include "service/serviceevents.h"
#include "service/serviceports.h"
#include "service/servicesettings.h"
#include "service/startcause.h"
#include "service/wakepolicy.h"

#include <QHash>
#include <QSet>

namespace lm {

enum class ServiceState { NotRunning, Starting, Running, RunningAwaitingApproval, Stopping };
QString serviceStateName(ServiceState state);

enum class StartOutcome {
    Started,
    NameTaken,      // another instance owns the name; nothing was written
    OpenFailed,     // the journal or the store could not be opened; the notice says why
    NotIdle         // start() called while already running: ignored
};

// Why the service stopped (the `reason` key of the stop record).
namespace StopReason {
inline QString term() { return QStringLiteral("term"); }
inline QString interrupt() { return QStringLiteral("int"); }
inline QString hangup() { return QStringLiteral("hup"); }
inline QString requested() { return QStringLiteral("requested"); }
inline QString nameLost() { return QStringLiteral("name-lost"); }
// core/service/versionwatch.h: the package's version stamp names another version (the
// service then starts the new binary by one activation of its name), or the stamp is gone because
// the package was removed (nothing starts again).
inline QString upgraded() { return QStringLiteral("upgraded"); }
inline QString packageRemoved() { return QStringLiteral("package-removed"); }
}

// What Status() reports about the process itself (the per-Subject part comes from the
// scheduler). Never a name.
struct ServiceStatus
{
    ServiceStatus() : state(ServiceState::NotRunning), notice(NoticeState::Stopped) {}

    ServiceState state;
    NoticeState notice;
    UtcTime lastHeartbeat;
    QString startCause;
    QStringList waitingConnections;     // connections that wait for the approval
};

struct LifecyclePorts
{
    LifecyclePorts()
        : clock(0), names(0), storage(0), journal(0), notifier(0), wake(0), approval(0),
          scheduler(0), events(0), ingestLog(0) {}

    ServiceClock *clock;
    NameOwner *names;
    ServiceStorage *storage;
    ServiceJournal *journal;
    Notifier *notifier;
    WakeSource *wake;
    ApprovalSource *approval;
    SchedulerHost *scheduler;
    ServiceEventSink *events;       // usually the fan-out; may be null in tests
    IngestLogSink *ingestLog;       // for `readings-lost`; may be null
};

// The lifecycle is itself a sink of the events: the scheduler's `approval-waiting` warning
// is the signal for, and the high-water mark is read from the scheduler at each wake-up.
class ServiceLifecycle : public ServiceEventSink
{
public:
    // The ports are not owned and must outlive the lifecycle.
    ServiceLifecycle(const LifecyclePorts &ports,
                     const LifecycleSettings &settings = LifecycleSettings());

    // Tries the name first and writes nothing at all if it is taken.
    StartOutcome start();

    // Call at every wake-up and nowhere else.
    void onWake();

    //  / D-Bus Stop: begins the stop sequence. Several requests are one stop.
    void requestStop(const QString &reason);
    // The same, with extra keys for the stop record (the versions of an `upgraded` stop). Keys that
    // collide with the record's own (`reason` and the journal's columns) are not allowed.
    void requestStop(const QString &reason, const QJsonObject &stopExtra);
    // Drives the stop sequence; call it every ~100 ms after requestStop (the keepalive
    // clock is not needed while the phone is awake for a shutdown). True once the process
    // may exit: the stop record is written and the notice says `stopped`.
    bool stepStop();

    // The user asks for the approval again (a tap on the notice, the D-Bus Start call) while
    // connections wait: the system dialog times out after a few minutes on the phone (test
    // 2026-10-15), and the hourly re-raise would leave the user without a prompt. Does
    // nothing when no connection waits.
    void raiseApprovalNow();

    // Shows the resident notice again now, with the facts of the moment: an alert changed, a silent
    // mode or Do not disturb started or ended. Nothing happens
    // unless the service runs.
    void refreshNotice();

    // The D-Bus name was taken away from us.
    void nameLost() { requestStop(StopReason::nameLost()); }

    ServiceState state() const { return m_state; }
    ServiceStatus status() const;
    // The cause of the start record written by start(); empty before.
    QString startCause() const { return m_startCause; }
    // Heartbeats and records that could not be written to the journal, for diagnostics.
    int journalWriteFailures() const { return m_journalFailures; }
    // Heartbeats written by this process (the start record not counted). The runtime compares it
    // before and after onWake() to run the checks that belong to a heartbeat (the version stamp),
    // without a clock of its own.
    int heartbeatsWritten() const { return m_heartbeats; }
    // Why the current or last stop was requested; empty before any stop.
    QString stopReason() const { return m_stopReason; }

    // ServiceEventSink:  (a connection waits for the approval).
    void connectionWarning(const QString &connection, const QString &kind, const QString &detail);

private:
    enum StopStage { StageLinks, StageFlush, StageDone };

    JournalEntry lifecycleEntry(const QString &kind) const;
    bool writeEntry(const JournalEntry &entry);
    void writeHeartbeat();
    void showNotice(NoticeState state);
    void requestApproval(const QString &connection);
    void approvalAnswered(const QString &connection, bool granted);
    void armNextWake();
    void checkQueueHighWater();
    void finishStop();
    bool active() const;

    LifecyclePorts m_p;
    LifecycleSettings m_s;
    WakePolicy m_wakePolicy;
    ServiceState m_state;
    QString m_startCause;

    qint64 m_lastHeartbeatBootMs;       // CLOCK_BOOTTIME of the newest heartbeat or start record
    UtcTime m_lastHeartbeat;            // its wall time, shown in the notice
    bool m_noticeDirty;                 // a heartbeat has not been shown yet
    qint64 m_noticeShownMonoMs;         // CLOCK_MONOTONIC of the last time the notice was shown

    QSet<QString> m_waiting;            // connections that wait for the approval
    QHash<QString, qint64> m_approvalRaisedMonoMs;
    int m_approvalEpoch;                // answers of a previous run are ignored

    bool m_aboveHighWater;

    QString m_stopReason;
    QJsonObject m_stopExtra;
    int m_heartbeats;
    StopStage m_stopStage;
    qint64 m_stopStartMonoMs;
    qint64 m_stageStartMonoMs;
    int m_lostReadings;
    int m_journalFailures;
};

} // namespace lm

#endif
