// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/servicelifecycle.h"

namespace lm {

QString noticeStateName(NoticeState state)
{
    switch (state) {
    case NoticeState::Active: return QStringLiteral("active");
    case NoticeState::AwaitingApproval: return QStringLiteral("awaiting-approval");
    case NoticeState::Attention: return QStringLiteral("attention");
    case NoticeState::Stopped: return QStringLiteral("stopped");
    }
    return QString();
}

QString serviceStateName(ServiceState state)
{
    switch (state) {
    case ServiceState::NotRunning: return QStringLiteral("not-running");
    case ServiceState::Starting: return QStringLiteral("starting");
    case ServiceState::Running: return QStringLiteral("running");
    case ServiceState::RunningAwaitingApproval: return QStringLiteral("running-awaiting-approval");
    case ServiceState::Stopping: return QStringLiteral("stopping");
    }
    return QString();
}

ServiceLifecycle::ServiceLifecycle(const LifecyclePorts &ports, const LifecycleSettings &settings)
    : m_p(ports), m_s(settings), m_wakePolicy(settings.wake), m_state(ServiceState::NotRunning),
      m_lastHeartbeatBootMs(-1), m_noticeDirty(false), m_noticeShownMonoMs(-1), m_approvalEpoch(0),
      m_aboveHighWater(false), m_heartbeats(0), m_stopStage(StageDone), m_stopStartMonoMs(0),
      m_stageStartMonoMs(0), m_lostReadings(0), m_journalFailures(0)
{
}

bool ServiceLifecycle::active() const
{
    return m_state == ServiceState::Running || m_state == ServiceState::RunningAwaitingApproval;
}

ServiceStatus ServiceLifecycle::status() const
{
    ServiceStatus s;
    s.state = m_state;
    s.lastHeartbeat = m_lastHeartbeat;
    s.startCause = m_startCause;
    s.waitingConnections = m_waiting.toList();
    s.waitingConnections.sort();
    switch (m_state) {
    case ServiceState::Running: s.notice = NoticeState::Active; break;
    case ServiceState::RunningAwaitingApproval: s.notice = NoticeState::AwaitingApproval; break;
    case ServiceState::Starting: s.notice = NoticeState::Active; break;
    default: s.notice = NoticeState::Stopped; break;
    }
    return s;
}

// ---- journal entries ---------------------------------------------------------------------

// The two counters of REQ-SCH-09 are on every lifecycle entry, so a later start can tell a
// restart from a crash and the time suspended from the time not running (REQ-SCH-11).
JournalEntry ServiceLifecycle::lifecycleEntry(const QString &kind) const
{
    JournalEntry e;
    e.kind = kind;
    e.wallTime = m_p.clock->now();
    e.bootTimeMs = m_p.clock->bootTimeMs();
    e.monotonicMs = m_p.clock->monotonicMs();
    return e;
}

// A failed journal write must never stop the monitoring itself: the readings matter more than
// the evidence. The failure is counted; a heartbeat that failed stays due and is tried again
// at the next wake-up.
bool ServiceLifecycle::writeEntry(const JournalEntry &entry)
{
    if (m_p.journal->append(entry))
        return true;
    ++m_journalFailures;
    return false;
}

void ServiceLifecycle::writeHeartbeat()
{
    const JournalEntry e = lifecycleEntry(JournalKind::heartbeat());
    if (!writeEntry(e))
        return;
    m_lastHeartbeatBootMs = e.bootTimeMs;
    m_lastHeartbeat = e.wallTime;
    m_noticeDirty = true;
    ++m_heartbeats;
}

void ServiceLifecycle::showNotice(NoticeState state)
{
    Notice n;
    n.state = state;
    n.lastHeartbeat = m_lastHeartbeat;
    if (state == NoticeState::AwaitingApproval && !m_waiting.isEmpty()) {
        QStringList ids = m_waiting.toList();
        ids.sort();
        n.connection = ids.first();     // the first in a stable order; the UI lists all by Status()
    }
    m_p.notifier->show(n);
    m_noticeDirty = false;
    m_noticeShownMonoMs = m_p.clock->monotonicMs();
}

// ---- start ------------------------------------------------------------------

StartOutcome ServiceLifecycle::start()
{
    if (m_state != ServiceState::NotRunning)
        return StartOutcome::NotIdle;

    //  /. The name is taken before anything else, so the second instance has not
    // opened, written or posted anything when it learns that it is second (REQ-ARC-09).
    if (!m_p.names->take())
        return StartOutcome::NameTaken;
    m_state = ServiceState::Starting;

    // Nothing is written to a journal that did not open. The notice is the only thing
    // posted, and it is the same notice every time, so the add-on timer that retries every
    // minute does not pile up notices.
    QString cause;
    if (!m_p.storage->open(&cause)) {
        Notice n;
        n.state = NoticeState::Attention;
        n.cause = cause;
        m_p.notifier->show(n);
        m_p.names->release();
        m_state = ServiceState::NotRunning;
        return StartOutcome::OpenFailed;
    }

    // Work out why we are starting from what the journal says and the clocks now.
    LifecycleMarker last;
    JournalEntry lastEntry;
    if (m_p.journal->lastLifecycleEntry(&lastEntry))
        last = LifecycleMarker::fromEntry(lastEntry);
    const UtcTime wallNow = m_p.clock->now();
    const StartCauseInfo info = determineStartCause(m_p.journal->hasStartRecord(), last, wallNow,
                                                    m_p.clock->bootTimeMs(),
                                                    m_s.clockJumpToleranceS);
    m_startCause = info.cause;

    JournalEntry e = lifecycleEntry(JournalKind::start());
    e.cause = info.cause;
    e.extra.insert(QStringLiteral("previous"), info.previous);
    if (info.hasClockJump)
        e.extra.insert(QStringLiteral("clock_jump_s"), double(info.clockJumpS));
    if (writeEntry(e)) {
        m_lastHeartbeatBootMs = e.bootTimeMs;   // the start record counts as the first heartbeat
        m_lastHeartbeat = e.wallTime;
    } else {
        m_lastHeartbeat = wallNow;
    }

    m_state = ServiceState::Running;
    showNotice(NoticeState::Active);

    // A gap is reported only above the limit: a quick restart (an update, a user tap) is
    // not worth a notification. The scheduler still gets the start of the gap for its
    // staleness cause and the backfill, whatever the length.
    if (info.downSince.isValid()) {
        const qint64 gapMs = wallNow.msecsSinceEpoch() - info.downSince.msecsSinceEpoch();
        if (gapMs > qint64(m_s.gapLimitS) * 1000 && m_p.events)
            m_p.events->gapDetected(info.downSince, wallNow);
    }

    m_p.scheduler->start(info.downSince);
    armNextWake();
    return StartOutcome::Started;
}

// ---- a wake-up ----------------------------------------------------------------------

void ServiceLifecycle::onWake()
{
    if (!active())
        return;     // a late wake-up during the stop sequence or after it is harmless

    m_p.scheduler->tick();

    // Heartbeat when a full interval has passed on CLOCK_BOOTTIME, not on the wall clock: the
    // wall clock may jump, and BOOTTIME keeps counting while the phone sleeps.
    const qint64 bootNow = m_p.clock->bootTimeMs();
    if (m_lastHeartbeatBootMs < 0 || bootNow - m_lastHeartbeatBootMs >= qint64(m_s.heartbeatIntervalS - m_s.heartbeatSlackS) * 1000)
        writeHeartbeat();

    // The approval notification is raised again at most once per approvalReraiseS.
    if (!m_waiting.isEmpty()) {
        const qint64 mono = m_p.clock->monotonicMs();
        const QStringList ids = m_waiting.toList();
        for (int i = 0; i < ids.size(); ++i) {
            if (mono - m_approvalRaisedMonoMs.value(ids.at(i), mono) >= qint64(m_s.approvalReraiseS) * 1000)
                requestApproval(ids.at(i));
        }
    }

    checkQueueHighWater();

    // The weekly storage check and the pruning of the journal are short and run here, on the
    // main thread. The check itself keeps to once a week.
    const UtcTime now = m_p.clock->now();
    if (m_p.storage->storageCheckDue(now))
        m_p.storage->runStorageCheck(now);

    // A heartbeat refreshes the notice only every noticeRefreshS: each republication raises a
    // banner on the phone. A change of state is shown at once by showNotice() itself.
    if (m_noticeDirty && m_p.clock->monotonicMs() - m_noticeShownMonoMs >= qint64(m_s.noticeRefreshS) * 1000)
        showNotice(m_state == ServiceState::RunningAwaitingApproval ? NoticeState::AwaitingApproval
                                                                   : NoticeState::Active);

    armNextWake();
}

void ServiceLifecycle::armNextWake()
{
    // The wake-up must not pass the scheduler's next due instant NOR the next heartbeat
    // (REQ-SCH-09: one per interval). Without the second bound an idle scheduler (no
    // credentials, a hold) would let the policy pick the long idle frequency and the
    // heartbeats would stretch to minutes; the phone test of 2026-10-15 showed exactly that
    // (spacing of 120 s and more).
    const qint64 nowMs = m_p.clock->now().msecsSinceEpoch();
    const UtcTime next = m_p.scheduler->nextWakeAt();
    qint64 delta = next.isValid() ? next.msecsSinceEpoch() - nowMs : -1;
    if (m_lastHeartbeatBootMs >= 0) {
        const qint64 heartbeatDelta = m_lastHeartbeatBootMs + qint64(m_s.heartbeatIntervalS) * 1000
                                      - m_p.clock->bootTimeMs();
        if (delta < 0 || heartbeatDelta < delta)
            delta = qMax<qint64>(heartbeatDelta, 0);
    }
    int f = m_wakePolicy.choose(m_p.wake->supportedFrequenciesS(), delta);
    if (f <= 0)
        f = m_s.wake.busyMaxS;      // the platform reported nothing: ask for the nominal interval
    m_p.wake->arm(f);
}

// Provisional: the scheduler is to raise readingQueueHighWater itself. Until it does,
// the crossing is detected here: once per crossing, and again after the queue fell below.
void ServiceLifecycle::checkQueueHighWater()
{
    const int queued = m_p.scheduler->queuedReadings();
    if (queued >= m_s.readingQueueHighWater) {
        if (!m_aboveHighWater) {
            m_aboveHighWater = true;
            if (m_p.events)
                m_p.events->readingQueueHighWater(queued);
        }
    } else {
        m_aboveHighWater = false;
    }
}

// ---- the approval after a phone restart -----------------------------------------

void ServiceLifecycle::connectionWarning(const QString &connection, const QString &kind,
                                         const QString & /*detail*/)
{
    if (kind != QLatin1String("approval-waiting") || !active())
        return;
    if (m_waiting.contains(connection))
        return;

    // The scheduler already marked the credentials locked (staleness cause 6) and keeps
    // retrying the read on its own schedule. The service raises the interactive read ITSELF,
    // because that read is what makes the system show the approval notification (S5), and
    // shows the notice `awaiting-approval` all along. Credentials are never copied out of
    // the secret store to avoid the prompt.
    m_waiting.insert(connection);
    m_state = ServiceState::RunningAwaitingApproval;
    showNotice(NoticeState::AwaitingApproval);
    requestApproval(connection);
}

void ServiceLifecycle::refreshNotice()
{
    if (!active())
        return;
    showNotice(m_state == ServiceState::RunningAwaitingApproval ? NoticeState::AwaitingApproval : NoticeState::Active);
}

void ServiceLifecycle::raiseApprovalNow()
{
    if (m_state != ServiceState::RunningAwaitingApproval)
        return;
    const QStringList ids = m_waiting.toList();
    for (int i = 0; i < ids.size(); ++i)
        requestApproval(ids.at(i));
}

void ServiceLifecycle::requestApproval(const QString &connection)
{
    m_approvalRaisedMonoMs.insert(connection, m_p.clock->monotonicMs());
    if (m_p.events)
        m_p.events->approvalRaised(connection);
    const int epoch = m_approvalEpoch;
    m_p.approval->request(connection, [this, connection, epoch](bool granted) {
        if (epoch == m_approvalEpoch)
            approvalAnswered(connection, granted);
    });
}

void ServiceLifecycle::approvalAnswered(const QString &connection, bool granted)
{
    if (!granted || !m_waiting.contains(connection) || !active())
        return;     // ignored or refused: stay in awaiting-approval, the re-raise rule decides

    // Tell the scheduler; it retries the session at once. When no connection waits any
    // more the notice goes back to `active`; the other connections were never affected.
    m_waiting.remove(connection);
    m_approvalRaisedMonoMs.remove(connection);
    if (m_p.events)
        m_p.events->approvalSettled(connection);
    m_p.scheduler->secretsAvailable(connection);
    if (m_waiting.isEmpty()) {
        m_state = ServiceState::Running;
        showNotice(NoticeState::Active);
    } else {
        showNotice(NoticeState::AwaitingApproval);      // names the connection that still waits
    }
}

// ---- the stop sequence -----------------------------------------------------

void ServiceLifecycle::requestStop(const QString &reason)
{
    requestStop(reason, QJsonObject());
}

void ServiceLifecycle::requestStop(const QString &reason, const QJsonObject &stopExtra)
{
    if (m_state == ServiceState::Stopping || m_state == ServiceState::NotRunning)
        return;
    m_stopExtra = stopExtra;
    m_stopExtra.remove(QStringLiteral("reason"));   // the reason has its own key; never overwritten

    if (m_state == ServiceState::Starting) {
        // A signal during start-up: there is nothing to end, so the sequence ends at once.
        m_stopReason = reason;
        m_state = ServiceState::Stopping;
        m_stopStage = StageDone;
        return;
    }

    // Step 1: stop starting new commands and end the links through the scheduler. The wake-up
    // is cancelled so no tick can start something new while the links end.
    m_stopReason = reason;
    m_state = ServiceState::Stopping;
    m_stopStage = StageLinks;
    m_stopStartMonoMs = m_stageStartMonoMs = m_p.clock->monotonicMs();
    m_lostReadings = 0;
    ++m_approvalEpoch;      // an approval that arrives from now on is ignored
    m_p.wake->cancel();
    m_p.scheduler->shutdown();
}

bool ServiceLifecycle::stepStop()
{
    if (m_state == ServiceState::NotRunning)
        return true;
    if (m_state != ServiceState::Stopping)
        return false;

    const qint64 mono = m_p.clock->monotonicMs();
    const bool overall = mono - m_stopStartMonoMs >= qint64(m_s.stopTotalMaxS) * 1000;

    if (m_stopStage == StageLinks) {
        if (m_p.scheduler->linksEnded() || overall
            || mono - m_stageStartMonoMs >= qint64(m_s.stopLinksMaxS) * 1000) {
            m_stopStage = StageFlush;
            m_stageStartMonoMs = mono;
        }
    }

    if (m_stopStage == StageFlush) {
        // Step 2: the readings the scheduler held while the store was busy. The next start
        // refetches the overlapping windows, so only what no feed returns again is lost.
        const int remaining = m_p.scheduler->flushQueuedReadings();
        if (remaining == 0 || overall || mono - m_stageStartMonoMs >= qint64(m_s.stopFlushMaxS) * 1000) {
            m_lostReadings = remaining;
            m_stopStage = StageDone;
        }
    }

    if (m_stopStage != StageDone)
        return false;
    finishStop();
    return true;
}

void ServiceLifecycle::finishStop()
{
    if (m_lostReadings > 0 && m_p.ingestLog) {
        IngestLogEntry log;
        log.time = m_p.clock->now();
        log.event = QStringLiteral("readings-lost");
        log.detail = QStringLiteral("count=%1 cause=stop").arg(m_lostReadings);
        m_p.ingestLog->append(log);
    }

    // Steps 3 and 4. The stop record is written only if the journal is open, which it is
    // whenever start() got past; a stop during start-up before that writes nothing.
    const bool opened = !m_startCause.isEmpty();
    UtcTime stoppedAt = m_p.clock->now();
    if (opened) {
        JournalEntry e = lifecycleEntry(JournalKind::stop());
        e.extra = m_stopExtra;
        e.extra.insert(QStringLiteral("reason"), m_stopReason);
        writeEntry(e);
        stoppedAt = e.wallTime;
        // a stop the service makes by itself because of its package is quiet. After an
        // upgrade the new service replaces the notice within a second, so a "monitoring stopped"
        // in between would only alarm the user; after a removal there is no application left to
        // restart, and the resident notice is taken away instead of being left saying "active".
        // The journal has the stop record either way. Every other stop shows `stopped`.
        if (m_stopReason == StopReason::packageRemoved()) {
            m_p.notifier->withdraw();
        } else if (m_stopReason != StopReason::upgraded()) {
            Notice n;
            n.state = NoticeState::Stopped;
            n.stoppedAt = stoppedAt;
            n.stopReason = m_stopReason;
            m_p.notifier->show(n);
        }
        m_p.storage->close();
    }
    m_p.names->release();
    m_state = ServiceState::NotRunning;
}

} // namespace lm
