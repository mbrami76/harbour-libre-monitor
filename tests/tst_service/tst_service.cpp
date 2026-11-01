// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of the service lifecycle as logic:
// the start cause table, the wake-up policy, the transition table  row by row,
// heartbeats, the gap start, the notice states, the approval flow, the stop sequence with its
// time limits, and a kill and restart through a REAL journal file in a temporary directory.
// No test lets real time pass: the three clocks are moved by hand.

#include "service/journallog.h"
#include "service/reportproposal.h"
#include "servicetest.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

JournalEntry entry(const QString &kind, qint64 wallOffsetS, qint64 bootMs, const QString &cause = QString())
{
    JournalEntry e;
    e.kind = kind;
    e.wallTime = UtcTime::fromMSecsSinceEpoch(1768000000000LL + wallOffsetS * 1000);
    e.bootTimeMs = bootMs;
    e.monotonicMs = bootMs;
    e.cause = cause;
    return e;
}

// An instant `s` seconds after the rig's wall clock.
UtcTime in(const ServiceRig &r, qint64 s)
{
    return UtcTime::fromMSecsSinceEpoch(r.clock.now().msecsSinceEpoch() + s * 1000);
}

// Puts one start record and some heartbeats into the fake journal, as an earlier run would.
void seedRun(ServiceRig &r, qint64 startOffsetS, qint64 startBootMs, int heartbeats, bool withStop)
{
    r.journal.entries.append(entry(JournalKind::start(), startOffsetS, startBootMs, StartCause::firstStart()));
    for (int i = 1; i <= heartbeats; ++i)
        r.journal.entries.append(entry(JournalKind::heartbeat(), startOffsetS + 60 * i, startBootMs + 60000 * i));
    if (withStop)
        r.journal.entries.append(entry(JournalKind::stop(), startOffsetS + 60 * (heartbeats + 1),
                                       startBootMs + 60000 * (heartbeats + 1)));
}

} // namespace

class TstService : public QObject
{
    Q_OBJECT

private slots:
    // ---- the start cause ------------------------------------------------------
    void causeFirstStartWhenNoStartRecord();
    void causeFirstStartWhenJournalEmpty();
    void causeAfterRestartWhenBootCounterIsSmaller();
    void causeAfterRestartKeepsPreviousStop();
    void causeAfterStop();
    void causeAfterMissingStopFromHeartbeat();
    void causeAfterMissingStopFromStart();
    void clockJumpWithinOneBoot();
    void suspendIsNotAClockJump();
    void restartOrderOfRowsFirstMatchWins();

    // ---- the wake-up policy -----------------------------------------------
    void wakeLargestFrequencyNotAfterDue();
    void wakeCappedAtTheNominalInterval();
    void wakeIdleUsesLongerFrequency();
    void wakeNeverShorterThanThePlatformOffers();
    void wakeWithoutFrequenciesGivesZero();

    // ---- the transition table ---------------------------------------------
    void L01_L04_startWritesStartRecordAndArms();
    void L02_nameTakenWritesNothing();
    void L03_openFailureShowsAttentionAndWritesNoJournal();
    void L03_theSameNoticeEveryTime();
    void L04_startRecordCarriesPreviousAndClockJump();
    void L04_gapStartGoesToTheScheduler();
    void L04_gapEventOnlyAboveTheLimit();
    void L05_L06_approvalFlow();
    void L05_ignoredApprovalIsRaisedAgainEveryFifteenMinutes();
    void L05_otherConnectionsKeepRunning();
    void L05_aTapRaisesTheApprovalAgain();
    void L06_noticeReturnsOnlyWhenNoConnectionWaits();
    void L07_wakeTicksSchedulerAndArmsNext();
    void L07_idleSchedulerStillWakesForTheHeartbeat();
    void L07_heartbeatCadenceUsesBoottime();
    void L07_aWakeUpASecondEarlyStillWritesTheHeartbeat();
    void L07_suspendIsNotAGapAndCountersDiffer();
    void L07_noticeRefreshedOncePerHeartbeat();
    void L07_failedHeartbeatIsRetriedAndMonitoringContinues();
    void L07_weeklyStorageCheckWhenDue();
    void L07_highWaterOncePerCrossing();
    void L07_wakeAfterStopIsHarmless();
    void L08_stopSequenceOrderAndStopRecord();
    void L08_stopWaitsForLinksThenGivesUp();
    void L08_stopFlushLimitCountsLostReadings();
    void L08_stopWithinOverallLimit();
    void L08_stopTwiceIsOneStop();
    void L09_nameLostStopsWithReason();
    void L10_stopRecordHasBothCounters();
    void L10_stopRecordCarriesExtraKeysButKeepsItsReason();
    void L07_heartbeatsWrittenCountsOnlyWrittenHeartbeats();
    void D114_anUpgradeStopShowsNoStoppedNotice();
    void D114_aRemovalStopWithdrawsTheNoticeQuietly();
    void D114_everyOtherStopStillShowsStopped_data();
    void D114_everyOtherStopStillShowsStopped();
    void L11_killedServiceIsRecognisedAtNextStart();

    // ---- a real journal -----------------------------------------------------------------------
    void realJournalKillAndRestart();
    void realJournalAfterOrderlyStopAndPhoneRestart();

    // ---- events -------------------------------------------------------------------------------
    void fanOutPassesEventsToEverySink();
    void notifyingStoreSignalsOnlyOnNewReadings();
    void noSecretInNoticeOrJournal();

    // ---- the report proposal for Status() (REQ-GRD-15) --------------------------
    void aSecondConsecutiveGuardKillProposesTheReportOnce();
    void aSuccessBetweenTwoKillsEndsTheSeries();
    void killsOfAnotherConnectionDoNotAdd();
    void aReenableOrAnAnswerClearsTheProposal();
    void otherBackoffCausesAreNotGuardKills();
};

// ---- start cause --------------------------------------------------------------------------

void TstService::causeFirstStartWhenNoStartRecord()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::heartbeat(), 0, 1000));
    const StartCauseInfo i = determineStartCause(false, m, UtcTime::fromMSecsSinceEpoch(1768000100000LL), 50000, 60);
    QCOMPARE(i.cause, StartCause::firstStart());
    QVERIFY(!i.downSince.isValid());
}

void TstService::causeFirstStartWhenJournalEmpty()
{
    const StartCauseInfo i = determineStartCause(false, LifecycleMarker(), UtcTime::fromMSecsSinceEpoch(1768000000000LL), 1000, 60);
    QCOMPARE(i.cause, StartCause::firstStart());
    QCOMPARE(i.previous, QStringLiteral("none"));
    QVERIFY(!i.downSince.isValid());
}

void TstService::causeAfterRestartWhenBootCounterIsSmaller()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::heartbeat(), 0, 900000));
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000600000LL), 40000, 60);
    QCOMPARE(i.cause, StartCause::afterRestart());
    QCOMPARE(i.previous, QStringLiteral("heartbeat"));
    QVERIFY(i.downSince.isValid());
    QVERIFY(!i.hasClockJump);       // no guess across a restart
}

void TstService::causeAfterRestartKeepsPreviousStop()
{
    // An orderly shutdown of the phone: stop record, then restart.
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::stop(), 0, 900000));
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000600000LL), 40000, 60);
    QCOMPARE(i.cause, StartCause::afterRestart());
    QCOMPARE(i.previous, QStringLiteral("stop"));
    QCOMPARE(i.downSince.msecsSinceEpoch(), 1768000000000LL);   // the gap starts at the stop record
}

void TstService::causeAfterStop()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::stop(), 0, 100000));
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000300000LL), 400000, 60);
    QCOMPARE(i.cause, StartCause::afterStop());
    QCOMPARE(i.previous, QStringLiteral("stop"));
}

void TstService::causeAfterMissingStopFromHeartbeat()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::heartbeat(), 0, 100000));
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000300000LL), 400000, 60);
    QCOMPARE(i.cause, StartCause::afterMissingStop());
    QCOMPARE(i.previous, QStringLiteral("heartbeat"));
}

void TstService::causeAfterMissingStopFromStart()
{
    // Killed right after starting: the newest entry is the start record of the same boot.
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::start(), 0, 100000, StartCause::firstStart()));
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000020000LL), 120000, 60);
    QCOMPARE(i.cause, StartCause::afterMissingStop());
    QCOMPARE(i.previous, QStringLiteral("start"));
}

void TstService::clockJumpWithinOneBoot()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::heartbeat(), 0, 100000));
    // 120 s on BOOTTIME, but the wall clock moved 3600 s.
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768003600000LL), 220000, 60);
    QVERIFY(i.hasClockJump);
    QCOMPARE(i.clockJumpS, qint64(3600 - 120));
}

void TstService::suspendIsNotAClockJump()
{
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::heartbeat(), 0, 100000));
    // The phone slept for an hour: both clocks advanced by the same amount.
    const StartCauseInfo i = determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768003600000LL), 100000 + 3600000, 60);
    QVERIFY(!i.hasClockJump);
}

void TstService::restartOrderOfRowsFirstMatchWins()
{
    // Counter smaller AND last entry is a stop: the restart row comes first.
    const LifecycleMarker m = LifecycleMarker::fromEntry(entry(JournalKind::stop(), 0, 500000));
    QCOMPARE(determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000100000LL), 1000, 60).cause,
             StartCause::afterRestart());
    // Equal counters are the same boot.
    QCOMPARE(determineStartCause(true, m, UtcTime::fromMSecsSinceEpoch(1768000100000LL), 500000, 60).cause,
             StartCause::afterStop());
}

// ---- wake policy ----------------------------------------------------------------------------

void TstService::wakeLargestFrequencyNotAfterDue()
{
    WakePolicy p;
    QList<int> f; f << 30 << 150 << 300;
    QCOMPARE(p.choose(f, 45000), 30);       // 150 would pass the due instant
    QCOMPARE(p.choose(f, 30000), 30);
    QCOMPARE(p.choose(f, 1000), 30);        // earlier than anything offered: the smallest
}

void TstService::wakeCappedAtTheNominalInterval()
{
    WakePolicy p;
    QList<int> f; f << 30 << 150 << 300;
    // Due in 200 s, but the cap while something is due is 60 s (the nominal interval).
    QCOMPARE(p.choose(f, 200000), 30);
}

void TstService::wakeIdleUsesLongerFrequency()
{
    WakePolicy p;
    QList<int> f; f << 30 << 150 << 300 << 600;
    QCOMPARE(p.choose(f, 3600000), 300);    // far away: idle cap 300 s
    QCOMPARE(p.choose(f, -1), 300);         // nothing due at all
}

void TstService::wakeNeverShorterThanThePlatformOffers()
{
    WakePolicy p;
    QList<int> f; f << 150 << 300;
    QCOMPARE(p.choose(f, 20000), 150);
}

void TstService::wakeWithoutFrequenciesGivesZero()
{
    WakePolicy p;
    QCOMPARE(p.choose(QList<int>(), 20000), 0);
}

// ---- transition table ----------------------------------------------------------------------

void TstService::L01_L04_startWritesStartRecordAndArms()
{
    ServiceRig r;
    r.scheduler.nextWake = in(r, 45);
    QCOMPARE(r.lifecycle->state(), ServiceState::NotRunning);
    QCOMPARE(r.lifecycle->start(), StartOutcome::Started);
    QCOMPARE(r.lifecycle->state(), ServiceState::Running);
    QVERIFY(r.names.owned);
    QVERIFY(r.storage.isOpen);
    QCOMPARE(r.journal.count(JournalKind::start()), 1);
    const JournalEntry s = r.journal.last(JournalKind::start());
    QCOMPARE(s.cause, StartCause::firstStart());
    QVERIFY(s.bootTimeMs >= 0 && s.monotonicMs >= 0);       // both counters (REQ-SCH-09)
    QCOMPARE(s.extra.value("previous").toString(), QStringLiteral("none"));
    QCOMPARE(r.notifier.last().state, NoticeState::Active);
    QCOMPARE(r.scheduler.started, 1);
    QVERIFY(!r.scheduler.downSince.isValid());              // first start: no gap
    QCOMPARE(r.wake.armed, 30);                             // due in 45 s: the 30 s frequency
}

void TstService::L02_nameTakenWritesNothing()
{
    ServiceRig r;
    r.names.available = false;
    QCOMPARE(r.lifecycle->start(), StartOutcome::NameTaken);
    QCOMPARE(r.lifecycle->state(), ServiceState::NotRunning);
    // REQ-ARC-09: nothing opened, nothing written, nothing posted, nothing started.
    QCOMPARE(r.storage.opens, 0);
    QCOMPARE(r.journal.appendAttempts, 0);
    QVERIFY(r.notifier.shown.isEmpty());
    QCOMPARE(r.scheduler.started, 0);
    QCOMPARE(r.wake.armCount, 0);
}

void TstService::L03_openFailureShowsAttentionAndWritesNoJournal()
{
    ServiceRig r;
    r.storage.openOk = false;
    r.storage.failCause = QStringLiteral("journal-open-failed");
    QCOMPARE(r.lifecycle->start(), StartOutcome::OpenFailed);
    QCOMPARE(r.lifecycle->state(), ServiceState::NotRunning);
    QCOMPARE(r.notifier.shown.size(), 1);
    QCOMPARE(r.notifier.last().state, NoticeState::Attention);
    QCOMPARE(r.notifier.last().cause, QStringLiteral("journal-open-failed"));
    QCOMPARE(r.journal.appendAttempts, 0);          // not even a stop record
    QCOMPARE(r.scheduler.started, 0);
    QVERIFY(!r.names.owned);                        // the name is given back, so a retry can take it
}

void TstService::L03_theSameNoticeEveryTime()
{
    // The add-on timer retries every minute: each retry shows the SAME notice
    // state with the same content; the notifier replaces it in place.
    ServiceRig r;
    r.storage.openOk = false;
    r.storage.failCause = QStringLiteral("store-open-failed");
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(r.lifecycle->start(), StartOutcome::OpenFailed);
        r.clock.advanceS(60);
    }
    QCOMPARE(r.notifier.shown.size(), 3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(r.notifier.shown.at(i).state, NoticeState::Attention);
        QCOMPARE(r.notifier.shown.at(i).cause, QStringLiteral("store-open-failed"));
    }
    QCOMPARE(r.journal.appendAttempts, 0);
}

void TstService::L04_startRecordCarriesPreviousAndClockJump()
{
    ServiceRig r;
    seedRun(r, -7200, 100000, 2, false);        // the last heartbeat is 2 h + before "now"
    r.clock.setBootMs(100000 + 7200000 - 3600000);   // BOOTTIME moved 1 h less than the wall clock
    QCOMPARE(r.lifecycle->start(), StartOutcome::Started);
    const JournalEntry s = r.journal.last(JournalKind::start());
    QCOMPARE(s.cause, StartCause::afterMissingStop());
    QCOMPARE(s.extra.value("previous").toString(), QStringLiteral("heartbeat"));
    QVERIFY(s.extra.contains("clock_jump_s"));
}

void TstService::L04_gapStartGoesToTheScheduler()
{
    ServiceRig r;
    seedRun(r, -1800, 100000, 1, true);         // stop record 1740 s before now
    r.clock.setBootMs(100000 + 1800000);
    QCOMPARE(r.lifecycle->start(), StartOutcome::Started);
    QCOMPARE(r.lifecycle->startCause(), StartCause::afterStop());
    QVERIFY(r.scheduler.downSince.isValid());
    QCOMPARE(r.scheduler.downSince.msecsSinceEpoch(), 1768000000000LL - 1800000 + 120000);
}

void TstService::L04_gapEventOnlyAboveTheLimit()
{
    {   // 5 minutes: below the default limit of 10, no event, but the scheduler still knows.
        ServiceRig r;
        seedRun(r, -300, 100000, 0, true);
        r.clock.setBootMs(100000 + 300000);
        r.lifecycle->start();
        QVERIFY(r.sink.gaps.isEmpty());
        QVERIFY(r.scheduler.downSince.isValid());
    }
    {   // 30 minutes: gapDetected(from, to).
        ServiceRig r;
        seedRun(r, -2000, 100000, 0, true);
        r.clock.setBootMs(100000 + 2000000);
        r.lifecycle->start();
        QCOMPARE(r.sink.gaps.size(), 1);
        QCOMPARE(r.sink.gaps.first().first.msecsSinceEpoch(), r.scheduler.downSince.msecsSinceEpoch());
        QCOMPARE(r.sink.gaps.first().second.msecsSinceEpoch(), r.clock.now().msecsSinceEpoch());
    }
}

void TstService::L05_L06_approvalFlow()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->connectionWarning(QStringLiteral("c1"), QStringLiteral("approval-waiting"), QString());
    QCOMPARE(r.lifecycle->state(), ServiceState::RunningAwaitingApproval);
    QCOMPARE(r.notifier.last().state, NoticeState::AwaitingApproval);
    QCOMPARE(r.notifier.last().connection, QStringLiteral("c1"));
    QCOMPARE(r.approval.requests.size(), 1);                    // the service raises the read itself
    QCOMPARE(r.approval.requests.first().connection, QStringLiteral("c1"));
    QVERIFY(r.scheduler.available.isEmpty());

    r.approval.answer(0, true);
    QCOMPARE(r.scheduler.available, QStringList() << "c1");
    QCOMPARE(r.lifecycle->state(), ServiceState::Running);
    QCOMPARE(r.notifier.last().state, NoticeState::Active);
}

// while any connection waits, the dialog is raised again every 15 minutes (it dies by
// itself after 2 to 3 minutes), and the notice shows the waiting state the whole time.
void TstService::L05_ignoredApprovalIsRaisedAgainEveryFifteenMinutes()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->connectionWarning("c1", "approval-waiting", QString());
    QCOMPARE(r.notifier.last().state, NoticeState::AwaitingApproval);   // shown as soon as entered
    r.approval.answer(0, false);                                // ignored or refused
    QCOMPARE(r.lifecycle->state(), ServiceState::RunningAwaitingApproval);
    for (int i = 0; i < 28; ++i) {                              // 14 minutes of wake-ups
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.approval.requests.size(), 1);
    for (int i = 0; i < 4; ++i) {                               // past 15 minutes
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.approval.requests.size(), 2);
    for (int i = 0; i < 26; ++i) {                              // 14 more minutes: not yet again
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.approval.requests.size(), 2);
    for (int i = 0; i < 4; ++i) {
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.approval.requests.size(), 3);                    // and every 15 minutes after that
    QCOMPARE(r.lifecycle->state(), ServiceState::RunningAwaitingApproval);  // still shown all along
    QCOMPARE(r.notifier.last().state, NoticeState::AwaitingApproval);
}

void TstService::L05_otherConnectionsKeepRunning()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->connectionWarning("c1", "approval-waiting", QString());
    const int ticks = r.scheduler.ticks;
    r.clock.advanceS(30);
    r.lifecycle->onWake();
    QCOMPARE(r.scheduler.ticks, ticks + 1);                     // the scheduler keeps ticking
    // Warnings of other kinds do not change the state.
    r.lifecycle->connectionWarning("c2", "renewal-refused", QString());
    r.lifecycle->connectionWarning("c2", "rate-limited", QString());
    QCOMPARE(r.lifecycle->state(), ServiceState::RunningAwaitingApproval);
    QCOMPARE(r.approval.requests.size(), 1);
}

void TstService::L05_aTapRaisesTheApprovalAgain()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->raiseApprovalNow();                            // nobody waits: nothing happens
    QCOMPARE(r.approval.requests.size(), 0);
    r.lifecycle->connectionWarning("c1", "approval-waiting", QString());
    r.approval.answer(0, false);                                // the dialog timed out
    r.lifecycle->raiseApprovalNow();
    QCOMPARE(r.approval.requests.size(), 2);
    r.approval.answer(1, true);
    QCOMPARE(r.lifecycle->state(), ServiceState::Running);
    r.lifecycle->raiseApprovalNow();
    QCOMPARE(r.approval.requests.size(), 2);                    // no longer waiting
}

void TstService::L06_noticeReturnsOnlyWhenNoConnectionWaits()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->connectionWarning("c1", "approval-waiting", QString());
    r.lifecycle->connectionWarning("c2", "approval-waiting", QString());
    QCOMPARE(r.approval.requests.size(), 2);
    r.approval.answer(0, true);
    QCOMPARE(r.lifecycle->state(), ServiceState::RunningAwaitingApproval);
    QCOMPARE(r.notifier.last().state, NoticeState::AwaitingApproval);
    QCOMPARE(r.notifier.last().connection, QStringLiteral("c2"));
    r.approval.answer(1, true);
    QCOMPARE(r.lifecycle->state(), ServiceState::Running);
    QCOMPARE(r.scheduler.available, QStringList() << "c1" << "c2");
    // A second answer for a connection that is no longer waiting does nothing.
    r.approval.answer(1, true);
    QCOMPARE(r.scheduler.available.size(), 2);
}

void TstService::L07_wakeTicksSchedulerAndArmsNext()
{
    ServiceRig r;
    r.scheduler.nextWake = in(r, 20);
    r.lifecycle->start();
    r.clock.advanceS(20);
    r.scheduler.nextWake = in(r, 100);          // the scheduler wants to run in 100 s
    r.lifecycle->onWake();
    QCOMPARE(r.scheduler.ticks, 1);
    QCOMPARE(r.wake.armed, 30);                 // capped at the nominal interval while busy
    r.scheduler.nextWake = UtcTime();           // idle
    r.clock.advanceS(30);
    r.lifecycle->onWake();
    // Nothing is due, but the next heartbeat is: the wake-up never passes it (REQ-SCH-09).
    QCOMPARE(r.wake.armed, 30);
}

void TstService::L07_idleSchedulerStillWakesForTheHeartbeat()
{
    ServiceRig r;
    r.lifecycle->start();                       // nothing is ever due
    for (int i = 0; i < 20; ++i) {
        QVERIFY(r.wake.armed <= 60);            // armed after the previous wake-up
        r.clock.advanceS(r.wake.armed);
        r.lifecycle->onWake();
    }
    // 20 wake-ups of 30 s = 600 s: a heartbeat for every minute.
    QVERIFY(r.journal.count(JournalKind::heartbeat()) >= 9);
}

void TstService::L07_heartbeatCadenceUsesBoottime()
{
    ServiceRig r;
    r.lifecycle->start();
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 0);
    for (int i = 0; i < 10; ++i) {              // 10 wake-ups 30 s apart = 300 s
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    // The start record counts as the first beat; one heartbeat per 60 s afterwards.
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 5);
    const JournalEntry h = r.journal.last(JournalKind::heartbeat());
    QVERIFY(h.bootTimeMs >= 0 && h.monotonicMs >= 0);
    // A wall-clock change alone does not make a heartbeat due.
    r.clock.setWallOffsetS(3600);
    const int before = r.journal.count(JournalKind::heartbeat());
    r.lifecycle->onWake();
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), before);
}

void TstService::L07_aWakeUpASecondEarlyStillWritesTheHeartbeat()
{
    // Night run on the phone: wake-ups every 30 s with a little jitter gave spacings of 90 s.
    ServiceRig r;
    r.lifecycle->start();
    r.clock.advanceMs(59000);
    r.lifecycle->onWake();
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 1);
    r.clock.advanceMs(30000);
    r.lifecycle->onWake();                      // 30 s after the last: not due
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 1);
}

void TstService::L07_suspendIsNotAGapAndCountersDiffer()
{
    ServiceRig r;
    r.lifecycle->start();
    r.clock.suspendS(55);                       // asleep, then a wake-up arrives
    r.clock.advanceS(5);
    r.lifecycle->onWake();
    const JournalEntry h = r.journal.last(JournalKind::heartbeat());
    const JournalEntry s = r.journal.last(JournalKind::start());
    // The difference between the two counters grew by the suspended time (REQ-SCH-11).
    QCOMPARE((h.bootTimeMs - s.bootTimeMs) - (h.monotonicMs - s.monotonicMs), qint64(55000));
    QVERIFY(r.sink.gaps.isEmpty());
}

void TstService::L07_noticeRefreshedOncePerHeartbeat()
{
    // A heartbeat alone refreshes the notice only every noticeRefreshS (1800 s): each
    // republication raises a banner on the phone (phone test 2026-10-15).
    ServiceRig r;
    r.lifecycle->start();
    const int afterStart = r.notifier.shown.size();
    for (int i = 0; i < 40; ++i) {              // 20 minutes of wake-ups
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.notifier.shown.size(), afterStart);
    for (int i = 0; i < 20; ++i) {              // past 30 minutes
        r.clock.advanceS(30);
        r.lifecycle->onWake();
    }
    QCOMPARE(r.notifier.shown.size(), afterStart + 1);
    QCOMPARE(r.notifier.last().state, NoticeState::Active);
    QVERIFY(r.notifier.last().lastHeartbeat.isValid());
    // A change of state is shown at once.
    r.lifecycle->connectionWarning("c1", "approval-waiting", QString());
    QCOMPARE(r.notifier.last().state, NoticeState::AwaitingApproval);
}

void TstService::L07_failedHeartbeatIsRetriedAndMonitoringContinues()
{
    ServiceRig r;
    r.lifecycle->start();
    r.journal.failAppend = true;
    r.clock.advanceS(60);
    r.lifecycle->onWake();
    QCOMPARE(r.scheduler.ticks, 1);             // the tick ran regardless
    QVERIFY(r.lifecycle->journalWriteFailures() >= 1);
    QCOMPARE(r.lifecycle->state(), ServiceState::Running);
    r.journal.failAppend = false;
    r.clock.advanceS(30);
    r.lifecycle->onWake();                      // still due: written now
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 1);
}

void TstService::L07_weeklyStorageCheckWhenDue()
{
    ServiceRig r;
    r.lifecycle->start();
    r.clock.advanceS(30);
    r.lifecycle->onWake();
    QCOMPARE(r.storage.checksRun, 0);
    r.storage.checkDue = true;
    r.clock.advanceS(30);
    r.lifecycle->onWake();
    QCOMPARE(r.storage.checksRun, 1);
    r.clock.advanceS(30);
    r.lifecycle->onWake();
    QCOMPARE(r.storage.checksRun, 1);           // not again until due
}

void TstService::L07_highWaterOncePerCrossing()
{
    LifecycleSettings s;
    s.readingQueueHighWater = 100;
    ServiceRig r(s);
    r.lifecycle->start();
    r.scheduler.queued = 99;
    r.lifecycle->onWake();
    QVERIFY(r.sink.highWater.isEmpty());
    r.scheduler.queued = 120;
    r.lifecycle->onWake();
    r.lifecycle->onWake();
    QCOMPARE(r.sink.highWater, QList<int>() << 120);    // once per crossing
    r.scheduler.queued = 10;
    r.lifecycle->onWake();
    r.scheduler.queued = 101;
    r.lifecycle->onWake();
    QCOMPARE(r.sink.highWater.size(), 2);               // again after the queue fell below
}

void TstService::L07_wakeAfterStopIsHarmless()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->requestStop(StopReason::term());
    const int ticks = r.scheduler.ticks;
    r.lifecycle->onWake();
    QCOMPARE(r.scheduler.ticks, ticks);
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    r.lifecycle->onWake();
    QCOMPARE(r.scheduler.ticks, ticks);
    QCOMPARE(r.journal.count(JournalKind::heartbeat()), 0);
}

void TstService::L08_stopSequenceOrderAndStopRecord()
{
    ServiceRig r;
    r.lifecycle->start();
    r.clock.advanceS(100);
    r.lifecycle->onWake();
    r.scheduler.ended = true;
    r.lifecycle->requestStop(StopReason::term());
    QCOMPARE(r.lifecycle->state(), ServiceState::Stopping);
    QVERIFY(r.wake.cancelled);                              // no new tick can start a command
    QCOMPARE(r.scheduler.shutdowns, 1);
    QVERIFY(r.lifecycle->stepStop());                       // links already ended, nothing queued
    QCOMPARE(r.lifecycle->state(), ServiceState::NotRunning);
    // Order: shutdown, then flush, and only then the stop record.
    QVERIFY(r.scheduler.log.indexOf("shutdown") < r.scheduler.log.indexOf("flush"));
    const JournalEntry stop = r.journal.last(JournalKind::stop());
    QCOMPARE(stop.extra.value("reason").toString(), QStringLiteral("term"));
    QCOMPARE(r.notifier.last().state, NoticeState::Stopped);
    QCOMPARE(r.notifier.last().stopReason, QStringLiteral("term"));
    QVERIFY(!r.storage.isOpen);
    QVERIFY(!r.names.owned);
}

void TstService::L08_stopWaitsForLinksThenGivesUp()
{
    ServiceRig r;
    r.lifecycle->start();
    r.scheduler.ended = false;
    r.lifecycle->requestStop(StopReason::interrupt());
    QVERIFY(!r.lifecycle->stepStop());
    QCOMPARE(r.journal.count(JournalKind::stop()), 0);
    r.clock.advanceS(2);
    QVERIFY(!r.lifecycle->stepStop());                      // still within the 4 s for the links
    r.scheduler.ended = true;
    QVERIFY(r.lifecycle->stepStop());                       // ended: the sequence completes
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), QStringLiteral("int"));

    ServiceRig r2;
    r2.lifecycle->start();
    r2.scheduler.ended = false;                             // an adapter that never ends
    r2.lifecycle->requestStop(StopReason::hangup());
    int steps = 0;
    while (!r2.lifecycle->stepStop() && steps++ < 1000) r2.clock.advanceMs(100);
    QVERIFY(steps < 1000);
    QCOMPARE(r2.journal.count(JournalKind::stop()), 1);     // the limit is over: the record is written
}

void TstService::L08_stopFlushLimitCountsLostReadings()
{
    ServiceRig r;
    r.lifecycle->start();
    r.scheduler.queued = 42;
    r.scheduler.flushStays = true;                          // the store stays busy
    r.lifecycle->requestStop(StopReason::requested());
    QVERIFY(!r.lifecycle->stepStop());
    r.clock.advanceS(3);
    QVERIFY(!r.lifecycle->stepStop());                      // within the 5 s
    r.clock.advanceS(3);
    QVERIFY(r.lifecycle->stepStop());                       // past the limit
    QCOMPARE(r.ingestLog.entries.size(), 1);
    QCOMPARE(r.ingestLog.entries.first().event, QStringLiteral("readings-lost"));
    QVERIFY(r.ingestLog.entries.first().detail.contains(QStringLiteral("count=42")));
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), QStringLiteral("requested"));
}

void TstService::L08_stopWithinOverallLimit()
{
    ServiceRig r;
    r.lifecycle->start();
    r.scheduler.ended = false;
    r.scheduler.queued = 5;
    r.scheduler.flushStays = true;
    r.lifecycle->requestStop(StopReason::term());
    qint64 total = 0;
    while (!r.lifecycle->stepStop()) { r.clock.advanceMs(100); total += 100; QVERIFY(total < 20000); }
    QVERIFY(total <= 10000 + 200);                          // about 10 s in all
}

void TstService::L08_stopTwiceIsOneStop()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->requestStop(StopReason::term());
    r.lifecycle->requestStop(StopReason::interrupt());
    QCOMPARE(r.scheduler.shutdowns, 1);
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(r.journal.count(JournalKind::stop()), 1);
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), QStringLiteral("term"));
}

void TstService::L09_nameLostStopsWithReason()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->nameLost();
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), StopReason::nameLost());
}

void TstService::L10_stopRecordHasBothCounters()
{
    ServiceRig r;
    r.lifecycle->start();
    r.clock.suspendS(10);
    r.lifecycle->requestStop(StopReason::term());
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    const JournalEntry s = r.journal.last(JournalKind::stop());
    QVERIFY(s.bootTimeMs >= 0);
    QVERIFY(s.monotonicMs >= 0);
    QVERIFY(s.bootTimeMs > s.monotonicMs);                  // the suspended time shows
}

// a stop for an upgrade names both versions in its stop record; an extra key can never
// replace the reason.
void TstService::L10_stopRecordCarriesExtraKeysButKeepsItsReason()
{
    ServiceRig r;
    r.lifecycle->start();
    QJsonObject extra;
    extra.insert(QStringLiteral("app_version"), QStringLiteral("0.1-3"));
    extra.insert(QStringLiteral("stamp"), QStringLiteral("0.1-4"));
    extra.insert(QStringLiteral("reason"), QStringLiteral("forged"));
    r.lifecycle->requestStop(StopReason::upgraded(), extra);
    r.lifecycle->requestStop(StopReason::term());          // a second request is the same stop
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(r.journal.count(JournalKind::stop()), 1);
    const JournalEntry s = r.journal.last(JournalKind::stop());
    QCOMPARE(s.extra.value("reason").toString(), QStringLiteral("upgraded"));
    QCOMPARE(s.extra.value("app_version").toString(), QStringLiteral("0.1-3"));
    QCOMPARE(s.extra.value("stamp").toString(), QStringLiteral("0.1-4"));
    QCOMPARE(r.lifecycle->stopReason(), StopReason::upgraded());
}

namespace {
int stoppedNotices(const FakeNotifier &n)
{
    int c = 0;
    for (int i = 0; i < n.shown.size(); ++i)
        if (n.shown.at(i).state == NoticeState::Stopped)
            ++c;
    return c;
}
}

// the stop for an upgrade raises no "monitoring stopped" notice (the new service shows its
// own within a second); the journal still has the stop record. The name is released as usual.
void TstService::D114_anUpgradeStopShowsNoStoppedNotice()
{
    ServiceRig r;
    r.lifecycle->start();
    const int before = r.notifier.shown.size();
    r.lifecycle->requestStop(StopReason::upgraded());
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(stoppedNotices(r.notifier), 0);
    QCOMPARE(r.notifier.shown.size(), before);
    QCOMPARE(r.notifier.withdrawals, 0);
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), StopReason::upgraded());
    QVERIFY(!r.names.owned);
}

// a removal ends quietly too, and the resident notice is taken away (no banner) instead of
// staying "active" for an application that is gone.
void TstService::D114_aRemovalStopWithdrawsTheNoticeQuietly()
{
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->requestStop(StopReason::packageRemoved());
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(stoppedNotices(r.notifier), 0);
    QCOMPARE(r.notifier.withdrawals, 1);
    QCOMPARE(r.journal.last(JournalKind::stop()).extra.value("reason").toString(), StopReason::packageRemoved());
}

// every other stop keeps its notice, with its reason.
void TstService::D114_everyOtherStopStillShowsStopped_data()
{
    QTest::addColumn<QString>("reason");
    QTest::newRow("term") << StopReason::term();
    QTest::newRow("int") << StopReason::interrupt();
    QTest::newRow("hup") << StopReason::hangup();
    QTest::newRow("requested") << StopReason::requested();
    QTest::newRow("name-lost") << StopReason::nameLost();
}

void TstService::D114_everyOtherStopStillShowsStopped()
{
    QFETCH(QString, reason);
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->requestStop(reason);
    while (!r.lifecycle->stepStop()) r.clock.advanceMs(100);
    QCOMPARE(stoppedNotices(r.notifier), 1);
    QCOMPARE(r.notifier.last().state, NoticeState::Stopped);
    QCOMPARE(r.notifier.last().stopReason, reason);
    QCOMPARE(r.notifier.withdrawals, 0);
}

// The runtime runs the checks of a heartbeat (the version stamp) when this counter moved; a
// failed heartbeat write is no heartbeat, and the start record does not count.
void TstService::L07_heartbeatsWrittenCountsOnlyWrittenHeartbeats()
{
    ServiceRig r;
    r.lifecycle->start();
    QCOMPARE(r.lifecycle->heartbeatsWritten(), 0);
    r.clock.advanceS(30); r.lifecycle->onWake();
    QCOMPARE(r.lifecycle->heartbeatsWritten(), 0);         // 30 s: not due
    r.clock.advanceS(30); r.lifecycle->onWake();
    QCOMPARE(r.lifecycle->heartbeatsWritten(), 1);
    r.journal.failAppend = true;
    r.clock.advanceS(60); r.lifecycle->onWake();
    QCOMPARE(r.lifecycle->heartbeatsWritten(), 1);         // the write failed
    r.journal.failAppend = false;
    r.clock.advanceS(30); r.lifecycle->onWake();
    QCOMPARE(r.lifecycle->heartbeatsWritten(), 2);         // the retry at the next wake-up
}

void TstService::L11_killedServiceIsRecognisedAtNextStart()
{
    // A kill writes nothing: the first rig is simply dropped and a new service starts
    // on a copy of its journal, in the same boot, 700 s later.
    ServiceRig a;
    a.lifecycle->start();
    for (int i = 0; i < 6; ++i) { a.clock.advanceS(30); a.lifecycle->onWake(); }
    QCOMPARE(a.journal.count(JournalKind::stop()), 0);

    ServiceRig b;
    b.journal.entries = a.journal.entries;
    b.clock.setBootMs(a.clock.bootTimeMs() + 700000);
    b.clock.setWallOffsetS((a.clock.now().msecsSinceEpoch() - b.clock.now().msecsSinceEpoch()) / 1000 + 700);
    QCOMPARE(b.lifecycle->start(), StartOutcome::Started);
    QCOMPARE(b.lifecycle->startCause(), StartCause::afterMissingStop());
    QCOMPARE(b.journal.last(JournalKind::start()).extra.value("previous").toString(), QStringLiteral("heartbeat"));
    QCOMPARE(b.sink.gaps.size(), 1);                        // 700 s is above the limit of 600 s
}

// ---- a real journal ------------------------------------------------------------------------------

void TstService::realJournalKillAndRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/journal.db";

    qint64 bootAtKill = 0;
    UtcTime wallAtKill;
    {
        Journal journal;
        QVERIFY(journal.open(path));
        JournalLifecycleLog log(&journal);
        ServiceRig r;
        // Replace the fake journal's role: drive the lifecycle with the real one.
        LifecyclePorts p;
        p.clock = &r.clock; p.names = &r.names; p.storage = &r.storage; p.journal = &log;
        p.notifier = &r.notifier; p.wake = &r.wake; p.approval = &r.approval;
        p.scheduler = &r.scheduler; p.events = &r.sink; p.ingestLog = &r.ingestLog;
        ServiceLifecycle life(p);
        QCOMPARE(life.start(), StartOutcome::Started);
        QCOMPARE(life.startCause(), StartCause::firstStart());
        for (int i = 0; i < 10; ++i) { r.clock.advanceS(30); life.onWake(); }
        bootAtKill = r.clock.bootTimeMs();
        wallAtKill = r.clock.now();
        // kill -9: the lifecycle and the journal handle are dropped without a stop record.
    }

    Journal journal;
    QVERIFY(journal.open(path));
    QCOMPARE(journal.entries(JournalKind::stop()).size(), 0);
    QVERIFY(journal.entries(JournalKind::heartbeat()).size() >= 4);

    JournalLifecycleLog log(&journal);
    ServiceRig r2;
    r2.clock.setBootMs(bootAtKill + 120000);
    r2.clock.setWallOffsetS((wallAtKill.msecsSinceEpoch() - r2.clock.now().msecsSinceEpoch()) / 1000 + 120);
    LifecyclePorts p;
    p.clock = &r2.clock; p.names = &r2.names; p.storage = &r2.storage; p.journal = &log;
    p.notifier = &r2.notifier; p.wake = &r2.wake; p.approval = &r2.approval;
    p.scheduler = &r2.scheduler; p.events = &r2.sink; p.ingestLog = &r2.ingestLog;
    ServiceLifecycle life(p);
    QCOMPARE(life.start(), StartOutcome::Started);
    QCOMPARE(life.startCause(), StartCause::afterMissingStop());
    const QVector<JournalEntry> starts = journal.entries(JournalKind::start());
    QCOMPARE(starts.size(), 2);
    QCOMPARE(starts.last().cause, StartCause::afterMissingStop());
    // The gap is visible in the journal (REQ-SCH-10) and the scheduler was told where it began.
    QCOMPARE(journal.gaps().size(), 1);
    QCOMPARE(r2.scheduler.downSince.msecsSinceEpoch(), journal.gaps().first().from.msecsSinceEpoch());
}

void TstService::realJournalAfterOrderlyStopAndPhoneRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/journal.db";
    {
        Journal journal;
        QVERIFY(journal.open(path));
        JournalLifecycleLog log(&journal);
        ServiceRig r;
        LifecyclePorts p;
        p.clock = &r.clock; p.names = &r.names; p.storage = &r.storage; p.journal = &log;
        p.notifier = &r.notifier; p.wake = &r.wake; p.approval = &r.approval;
        p.scheduler = &r.scheduler; p.events = &r.sink; p.ingestLog = &r.ingestLog;
        ServiceLifecycle life(p);
        life.start();
        r.clock.advanceS(120);
        life.onWake();
        life.requestStop(StopReason::term());      // system shutdown: SIGTERM
        while (!life.stepStop()) r.clock.advanceMs(100);
    }
    Journal journal;
    QVERIFY(journal.open(path));
    JournalLifecycleLog log(&journal);
    ServiceRig r2;
    r2.clock.restartPhone(600, 30000);              // ten minutes off, a fresh boot counter
    LifecyclePorts p;
    p.clock = &r2.clock; p.names = &r2.names; p.storage = &r2.storage; p.journal = &log;
    p.notifier = &r2.notifier; p.wake = &r2.wake; p.approval = &r2.approval;
    p.scheduler = &r2.scheduler; p.events = &r2.sink; p.ingestLog = &r2.ingestLog;
    ServiceLifecycle life(p);
    life.start();
    QCOMPARE(life.startCause(), StartCause::afterRestart());
    const QVector<JournalEntry> starts = journal.entries(JournalKind::start());
    QCOMPARE(starts.last().extra.value("previous").toString(), QStringLiteral("stop"));
}

// ---- events ---------------------------------------------------------------------------------------

void TstService::fanOutPassesEventsToEverySink()
{
    ServiceEventFanOut fan;
    RecordingSink a, b;
    fan.add(&a);
    fan.add(&b);
    fan.add(&a);                                // twice: ignored
    fan.connectionWarning("c1", "protocol-changed", QString());
    fan.dataChanged(QUuid::createUuid());
    QCOMPARE(a.warnings, QStringList() << "c1:protocol-changed");
    QCOMPARE(b.warnings, QStringList() << "c1:protocol-changed");
    QCOMPARE(a.changed.size(), 1);
    fan.remove(&b);
    fan.connectionWarning("c2", "rate-limited", QString());
    QCOMPARE(a.warnings.size(), 2);
    QCOMPARE(b.warnings.size(), 1);
}

namespace {
class CountingReadingStore : public ReadingStore
{
public:
    CountingReadingStore() : result(0) {}
    UtcTime newestMeasurement(const QUuid &, const QString &) { return UtcTime(); }
    int ingest(const QUuid &, const LinkId &, const QVector<Record> &) { return result; }
    void quarantine(const LinkId &, const QString &, const QString &, const QJsonObject &) {}
    bool latestSensor(const QUuid &, UtcTime *, int *) { return false; }
    void noteSensor(const QUuid &, const QJsonObject &) {}
    int result;
};
}

void TstService::notifyingStoreSignalsOnlyOnNewReadings()
{
    CountingReadingStore inner;
    RecordingSink sink;
    NotifyingReadingStore store(&inner, &sink);
    const QUuid subject = QUuid::createUuid();
    inner.result = -1;                          // the store is busy
    QCOMPARE(store.ingest(subject, LinkId(), QVector<Record>()), -1);
    inner.result = 0;                           // only duplicates
    store.ingest(subject, LinkId(), QVector<Record>());
    QVERIFY(sink.changed.isEmpty());
    inner.result = 3;
    store.ingest(subject, LinkId(), QVector<Record>());
    QCOMPARE(sink.changed, QList<QUuid>() << subject);
}

void TstService::noSecretInNoticeOrJournal()
{
    // The lifecycle never sees a credential, but the connection id travels in the notice:
    // it is an opaque id. Check that nothing else is carried by the notice or the entries.
    ServiceRig r;
    r.lifecycle->start();
    r.lifecycle->connectionWarning("conn-1", "approval-waiting", "detail that must not travel");
    r.clock.advanceS(60);
    r.lifecycle->onWake();
    for (int i = 0; i < r.notifier.shown.size(); ++i) {
        const Notice &n = r.notifier.shown.at(i);
        QVERIFY(!n.cause.contains("detail"));
        QVERIFY(!n.connection.contains("detail"));
    }
    for (int i = 0; i < r.journal.entries.size(); ++i) {
        const QString text = QString::fromUtf8(QJsonDocument(r.journal.entries.at(i).extra).toJson());
        QVERIFY(!text.contains("detail"));
    }
}

// ---- the report proposal -----------------------------------------------------------------

namespace {

class ProposalSink : public ServiceEventSink
{
public:
    void diagnosticReportProposed(const QString &connection) { proposed << connection; }
    QStringList proposed;
};

SubjectStatus status(const QString &connection, ScheduleState state, const QString &cause,
                     const UtcTime &lastContactOk = UtcTime())
{
    SubjectStatus s;
    s.connection = connection;
    s.state = state;
    s.cause = cause;
    s.lastContactOkAt = lastContactOk;
    return s;
}

} // namespace

void TstService::aSecondConsecutiveGuardKillProposesTheReportOnce()
{
    FakeServiceClock clock;
    ProposalSink out;
    ReportProposalTracker t(&clock, &out);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    QCOMPARE(t.consecutiveKills("c1"), 1);
    QVERIFY(!t.proposed("c1"));
    QVERIFY(out.proposed.isEmpty());

    clock.advanceS(60);
    t.subjectStateChanged(status("c1", ScheduleState::Idle, QString()));        // the backoff ended
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));  // the second kill
    QVERIFY(t.proposed("c1"));
    QCOMPARE(out.proposed, QStringList() << "c1");

    clock.advanceS(60);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));  // a third: no second proposal
    QCOMPARE(t.consecutiveKills("c1"), 3);
    QCOMPARE(out.proposed.size(), 1);
    QVERIFY(t.proposed("c1"));
}

void TstService::aSuccessBetweenTwoKillsEndsTheSeries()
{
    FakeServiceClock clock;
    ProposalSink out;
    ReportProposalTracker t(&clock, &out);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    clock.advanceS(60);
    // A good contact after the kill, seen on the next state change.
    t.subjectStateChanged(status("c1", ScheduleState::Idle, QString(), clock.now()));
    QCOMPARE(t.consecutiveKills("c1"), 0);
    clock.advanceS(60);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    QCOMPARE(t.consecutiveKills("c1"), 1);                  // the series started again: not the second
    QVERIFY(!t.proposed("c1"));
    QVERIFY(out.proposed.isEmpty());

    // A success also ends an OPEN proposal: the problem did not repeat.
    clock.advanceS(60);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    QVERIFY(t.proposed("c1"));
    clock.advanceS(60);
    t.subjectStateChanged(status("c1", ScheduleState::Idle, QString(), clock.now()));
    QVERIFY(!t.proposed("c1"));
}

void TstService::killsOfAnotherConnectionDoNotAdd()
{
    FakeServiceClock clock;
    ProposalSink out;
    ReportProposalTracker t(&clock, &out);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    t.subjectStateChanged(status("c2", ScheduleState::Backoff, "guard-kill"));
    QCOMPARE(t.consecutiveKills("c1"), 1);
    QCOMPARE(t.consecutiveKills("c2"), 1);
    QVERIFY(out.proposed.isEmpty());
}

void TstService::aReenableOrAnAnswerClearsTheProposal()
{
    FakeServiceClock clock;
    ProposalSink out;
    ReportProposalTracker t(&clock, &out);
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    QVERIFY(t.proposed("c1"));
    t.clear("c1");
    QVERIFY(!t.proposed("c1"));
    QCOMPARE(t.consecutiveKills("c1"), 0);
    // Two new kills propose again.
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    t.subjectStateChanged(status("c1", ScheduleState::Backoff, "guard-kill"));
    QCOMPARE(out.proposed.size(), 2);
    t.clearAll();
    QVERIFY(!t.proposed("c1"));
}

void TstService::otherBackoffCausesAreNotGuardKills()
{
    FakeServiceClock clock;
    ProposalSink out;
    ReportProposalTracker t(&clock, &out);
    for (int i = 0; i < 4; ++i)
        t.subjectStateChanged(status("c1", ScheduleState::Backoff, "transient"));
    t.subjectStateChanged(status("c1", ScheduleState::DisabledByGuard, "guard-kill"));
    t.subjectStateChanged(status(QString(), ScheduleState::Backoff, "guard-kill"));    // no connection: ignored
    QCOMPARE(t.consecutiveKills("c1"), 0);
    QVERIFY(out.proposed.isEmpty());
}

QTEST_APPLESS_MAIN(TstService)
#include "tst_service.moc"
