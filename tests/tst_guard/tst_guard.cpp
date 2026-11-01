// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Guard (REQ-GRD-01 to 07, 10, 11): score and decay, budgets, reaction ladder,
// circuit breaker, spawn rate, trip, per-version trip, persistence, manual re-enable. No real
// time passes: every test moves a manual clock. Synthetic data only.
#include "guard/guard.h"
#include "guard/journalstatelog.h"
#include "modtest.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

const qint64 kMin = 60 * 1000;

LinkId linkOf(const QString &connection, const QString &version = QStringLiteral("0.1.0"),
              const QString &session = QStringLiteral("s1"))
{
    LinkId l;
    l.connectionId = connection;
    l.adapterId = QStringLiteral("libre");
    l.adapterVersion = version;
    l.sessionId = session;
    return l;
}

Violation violation(const LinkId &link, ViolationKind kind, Severity severity)
{
    Violation v;
    v.link = link;
    v.kind = kind;
    v.severity = severity;
    v.detail = QStringLiteral("limit named, no content");
    return v;
}

// Everything a guard needs, wired to in-memory fakes.
struct Rig
{
    Rig(const GuardSettings &s = GuardSettings())
        : keeper(&stateLog, &clock), ingest(&sink, s.ingestQueueMax), guard(&clock, &keeper, &ingest, s)
    {
        keeper.load();
    }

    ManualClock clock;
    MemoryStateLog stateLog;
    RecordingIngestSink sink;
    StateKeeper keeper;
    IngestLog ingest;
    Guard guard;
};

class Listener : public GuardListener
{
public:
    void guardTripped(const QString &c) { tripped.append(c); }
    void guardVersionTripped(const QString &a, const QString &v) { versionTripped.append(a + "/" + v); }
    void guardReenabled(const QString &c) { reenabled.append(c); }
    void guardVersionReenabled(const QString &a, const QString &v) { versionReenabled.append(a + "/" + v); }

    QStringList tripped, versionTripped, reenabled, versionReenabled;
};

StartRequest request(const QString &connection)
{
    StartRequest r;
    r.connectionId = connection;
    r.adapterId = QStringLiteral("libre");
    return r;
}

} // namespace

class TstGuard : public QObject
{
    Q_OBJECT

private slots:
    // ---- score and decay (REQ-GRD-04) ------------------------------------------------------
    void aSingleHardViolationKillsButNeverTrips()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        const Action a = r.guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
        QCOMPARE(a.kind, Action::Kill);
        QVERIFY(!r.guard.isDisabled("c1"));
        QCOMPARE(r.guard.scoreOf("c1"), 10.0);
    }

    void twoHardViolationsTenMinutesApartTrip()
    {
        Rig r;
        Listener lis;
        r.guard.setListener(&lis);
        const LinkId l = linkOf("c1");
        r.guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
        r.clock.advanceMs(10 * kMin);
        r.guard.onViolation(violation(l, ViolationKind::BadHeader, Severity::Hard));
        QVERIFY(r.guard.isDisabled("c1"));
        QCOMPARE(lis.tripped, QStringList() << "c1");
        QCOMPARE(r.keeper.connectionDisabledCause("c1"), StateCause::score());
        QCOMPARE(r.sink.count("disabled"), 1);
        QCOMPARE(r.sink.count("violation"), 2);
    }

    void twoHardViolationsFortyMinutesApartDoNotTrip()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        r.guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
        r.clock.advanceMs(40 * kMin);
        r.guard.onViolation(violation(l, ViolationKind::BadHeader, Severity::Hard));
        QVERIFY(!r.guard.isDisabled("c1"));
        QVERIFY(r.guard.scoreOf("c1") < 15.0);
    }

    void softViolationsThatDecayNeverTrip()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        for (int i = 0; i < 30; ++i) {
            const Action a = r.guard.onViolation(violation(l, ViolationKind::StderrFlood, Severity::Soft));
            QCOMPARE(a.kind, Action::Proceed);
            r.clock.advanceMs(20 * kMin);
        }
        QVERIFY(!r.guard.isDisabled("c1"));
    }

    void fastSoftViolationsTripAtTheLimit()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        for (int i = 0; i < 14; ++i)
            r.guard.onViolation(violation(l, ViolationKind::StderrFlood, Severity::Soft));
        QVERIFY(!r.guard.isDisabled("c1"));
        const Action a = r.guard.onViolation(violation(l, ViolationKind::StderrFlood, Severity::Soft));
        QCOMPARE(a.kind, Action::Kill);
        QVERIFY(r.guard.isDisabled("c1"));
    }

    void theReactionLadderAnswers_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("hard");
        QTest::addColumn<int>("action");
        QTest::newRow("line too long") << int(ViolationKind::LineTooLong) << true << int(Action::Kill);
        QTest::newRow("malformed") << int(ViolationKind::MalformedFraming) << true << int(Action::Kill);
        QTest::newRow("undeclared unit") << int(ViolationKind::UndeclaredUnit) << true << int(Action::Kill);
        QTest::newRow("non protocol") << int(ViolationKind::NonProtocolOutput) << true << int(Action::Kill);
        QTest::newRow("batch too large") << int(ViolationKind::BatchTooLarge) << false << int(Action::Drop);
        QTest::newRow("rate above registry") << int(ViolationKind::RateAboveRegistry) << false << int(Action::Drop);
        QTest::newRow("stderr flood") << int(ViolationKind::StderrFlood) << false << int(Action::Proceed);
        QTest::newRow("records sent mismatch") << int(ViolationKind::RecordsSentMismatch) << false << int(Action::Proceed);
    }
    void theReactionLadderAnswers()
    {
        QFETCH(int, kind);
        QFETCH(bool, hard);
        QFETCH(int, action);
        Rig r;
        const Action a = r.guard.onViolation(
            violation(linkOf("c1"), ViolationKind(kind), hard ? Severity::Hard : Severity::Soft));
        QCOMPARE(int(a.kind), action);
    }

    // ---- budgets (REQ-GRD-01) --------------------------------------------------------------
    void lineBucketPausesAndCountsOnePerEpisode()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        r.guard.onSpawned(l);
        int pauses = 0;
        for (int i = 0; i < 260; ++i)
            if (r.guard.onLine(l, 100, r.clock.now()).kind == Action::Pause) ++pauses;
        QVERIFY(pauses > 0);
        QCOMPARE(r.sink.count("violation"), 1);          // one episode, not one per line
        QCOMPARE(r.sink.count("pause"), 1);

        r.clock.advanceS(10);                            // the bucket refills past the re-arm level
        QCOMPARE(r.guard.onLine(l, 100, r.clock.now()).kind, Action::Proceed);
        for (int i = 0; i < 260; ++i)
            r.guard.onLine(l, 100, r.clock.now());
        QCOMPARE(r.sink.count("violation"), 2);          // a second episode is counted again
    }

    void pauseIsCappedAndByteBucketWorks()
    {
        GuardSettings s;
        s.byteBucketCap = 1000;
        s.byteRefillPerS = 10;
        Rig r(s);
        const LinkId l = linkOf("c1");
        r.guard.onSpawned(l);
        QCOMPARE(r.guard.onLine(l, 1000, r.clock.now()).kind, Action::Proceed);
        const Action a = r.guard.onLine(l, 5000, r.clock.now());      // needs 500 s of refill
        QCOMPARE(a.kind, Action::Pause);
        QCOMPARE(a.pauseMs, s.pauseMaxMs);
    }

    void recordRateIsCheckedAgainstTheRegistryPerCommand()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        r.guard.onSpawned(l);
        r.guard.beginCommand(l, "fetch_current", 1, 1.0);
        QCOMPARE(r.guard.onBatch(l, "current", 7, 500).kind, Action::Proceed);   // allowance 2 + 5
        QCOMPARE(r.guard.onBatch(l, "current", 1, 100).kind, Action::Drop);
        QCOMPARE(r.sink.count("violation"), 1);
        QCOMPARE(r.guard.onBatch(l, "current", 1, 100).kind, Action::Drop);      // still dropped
        QCOMPARE(r.sink.count("violation"), 1);                                  // counted once

        // A 12 h graph with its 141 points is far inside the allowance of a range.
        r.guard.beginCommand(l, "fetch_range", 1, 720.0);
        QCOMPARE(r.guard.onBatch(l, "graph", 141, 20000).kind, Action::Proceed);
        // The logbook is events: only the absolute caps apply.
        QCOMPARE(r.guard.onBatch(l, "logbook", 400, 20000).kind, Action::Proceed);
    }

    void absoluteCapsDropAndCountOnce()
    {
        GuardSettings s;
        s.recordsPerCommandMax = 100;
        s.recordsWindowMax = 150;
        Rig r(s);
        const LinkId l = linkOf("c1");
        r.guard.beginCommand(l, "fetch_range", 1, 100000.0);
        QCOMPARE(r.guard.onBatch(l, "logbook", 90, 1000).kind, Action::Proceed);
        QCOMPARE(r.guard.onBatch(l, "logbook", 20, 1000).kind, Action::Drop);     // 110 in the command
        QCOMPARE(r.sink.count("violation"), 1);
        r.guard.beginCommand(l, "fetch_range", 1, 100000.0);
        QCOMPARE(r.guard.onBatch(l, "logbook", 90, 1000).kind, Action::Drop);     // window: 90+90+20 > 150
        r.clock.advanceMs(11 * kMin);
        r.guard.beginCommand(l, "fetch_range", 1, 100000.0);
        QCOMPARE(r.guard.onBatch(l, "logbook", 90, 1000).kind, Action::Proceed);  // the window moved on
    }

    // ---- admission and circuit breaker -----------------------------------------------------
    void spawnRateIsLimitedAndRefills()
    {
        Rig r;
        for (int i = 0; i < 5; ++i)
            QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
        const MayStartResult denied = r.guard.mayStart(request("c1"));
        QCOMPARE(denied.kind, MayStartResult::Denied);
        QCOMPARE(denied.reason, MayStartResult::SpawnRate);
        QVERIFY(r.guard.spawnReadyAt("c1").isValid());
        r.clock.advanceS(121);
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
        QVERIFY(!r.guard.spawnReadyAt("c2").isValid());
    }

    void circuitOpensAfterThreeFailuresTriesOnceAndCloses()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        for (int i = 0; i < 3; ++i) {
            CommandOutcome o;
            o.link = linkOf("c1", "0.1.0", QString("s%1").arg(i));
            o.result = CommandOutcome::Error;
            o.killedBy = CommandOutcome::Deadline;
            r.guard.onCommandFinished(o);
            r.clock.advanceS(10);
        }
        MayStartResult m = r.guard.mayStart(request("c1"));
        QCOMPARE(m.kind, MayStartResult::Denied);
        QCOMPARE(m.reason, MayStartResult::CircuitOpen);
        QVERIFY(r.guard.circuitReopenAt("c1").isValid());
        QCOMPARE(r.sink.count("circuit-open"), 1);

        r.clock.advanceS(15 * 60);                          // open time over: one trial
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
        QCOMPARE(r.guard.mayStart(request("c1")).reason, MayStartResult::CircuitOpen);   // only one

        CommandOutcome bad;                                   // the trial fails: twice as long
        bad.link = linkOf("c1", "0.1.0", "s9");
        bad.result = CommandOutcome::Error;
        bad.killedBy = CommandOutcome::Exit;
        r.guard.onCommandFinished(bad);
        const qint64 reopen = r.guard.circuitReopenAt("c1").msecsSinceEpoch() - r.clock.ms();
        QCOMPARE(reopen, qint64(1800 * 1000));
        r.clock.advanceS(1801);
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);

        CommandOutcome login;                                 // a login that works does not close it
        login.link = linkOf("c1", "0.1.0", "s10");
        login.commandType = "authenticate";
        r.guard.onCommandFinished(login);
        QVERIFY(r.guard.circuitReopenAt("c1").isValid());

        CommandOutcome ok;                                    // a fetch that works closes it and resets
        ok.commandType = "fetch_current";
        ok.link = linkOf("c1", "0.1.0", "s10");
        r.guard.onCommandFinished(ok);
        QVERIFY(!r.guard.circuitReopenAt("c1").isValid());
        QCOMPARE(r.sink.count("circuit-close"), 1);
    }

    void aWorkingLoginBetweenFailedFetchesDoesNotClearTheCount()
    {
        Rig r;
        for (int i = 0; i < 3; ++i) {
            CommandOutcome login;
            login.link = linkOf("c1", "0.1.0", QString("s%1").arg(i));
            login.commandType = "authenticate";
            r.guard.onCommandFinished(login);
            CommandOutcome bad;
            bad.link = login.link;
            bad.commandType = "fetch_current";
            bad.result = CommandOutcome::Error;
            bad.killedBy = CommandOutcome::Exit;
            r.guard.onCommandFinished(bad);
            r.clock.advanceS(30);
        }
        QCOMPARE(r.guard.mayStart(request("c1")).reason, MayStartResult::CircuitOpen);
    }

    void failuresOutsideTheWindowAndOfTheSourceDoNotOpenTheCircuit()
    {
        Rig r;
        for (int i = 0; i < 6; ++i) {
            CommandOutcome o;
            o.link = linkOf("c1", "0.1.0", QString("s%1").arg(i));
            o.result = CommandOutcome::Error;
            o.errorClass = ErrorClass::RateLimited;           // about the source: not counted
            r.guard.onCommandFinished(o);
        }
        for (int i = 0; i < 4; ++i) {
            CommandOutcome o;
            o.link = linkOf("c1", "0.1.0", QString("t%1").arg(i));
            o.result = CommandOutcome::Error;
            o.killedBy = CommandOutcome::Guard;
            r.guard.onCommandFinished(o);
            r.clock.advanceS(6 * 60);                          // never three within 10 minutes
        }
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
    }

    void aSessionCountsOnceWhenTheCommandAndTheExitBothReportIt()
    {
        Rig r;
        for (int i = 0; i < 2; ++i) {
            const LinkId l = linkOf("c1", "0.1.0", QString("s%1").arg(i));
            CommandOutcome o;
            o.link = l;
            o.result = CommandOutcome::Error;
            o.killedBy = CommandOutcome::Exit;
            r.guard.onCommandFinished(o);
            ExitStatus e;
            e.crashed = true;
            r.guard.onExited(l, e);                            // the same failure again
        }
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);   // 2 failures, not 4
        r.guard.noteStartFailure("c1");                                            // the third
        QCOMPARE(r.guard.mayStart(request("c1")).reason, MayStartResult::CircuitOpen);
    }

    // ---- trip, persistence, per-version trip ------------------------------------------------
    void aTripIsDeniedAtEveryDoorAndSurvivesARestart()
    {
        ManualClock clock;
        MemoryStateLog stateLog;
        RecordingIngestSink sink;
        {
            StateKeeper keeper(&stateLog, &clock);
            keeper.load();
            IngestLog ingest(&sink, 100);
            Guard guard(&clock, &keeper, &ingest);
            const LinkId l = linkOf("c1");
            guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
            clock.advanceMs(kMin);
            guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
            QVERIFY(guard.isDisabled("c1"));
            QCOMPARE(guard.mayStart(request("c1")).reason, MayStartResult::DisabledByGuard);
            QCOMPARE(guard.onAdmitted(l).reason, MayStartResult::DisabledByGuard);
        }
        // A new object over the same log: the state is back, nothing was said again.
        StateKeeper keeper2(&stateLog, &clock);
        keeper2.load();
        IngestLog ingest2(&sink, 100);
        Guard guard2(&clock, &keeper2, &ingest2);
        QVERIFY(guard2.isDisabled("c1"));
        QCOMPARE(guard2.mayStart(request("c1")).reason, MayStartResult::DisabledByGuard);
        QCOMPARE(guard2.mayStart(request("c2")).kind, MayStartResult::Allowed);
        clock.advanceS(10LL * 24 * 3600);                      // never left automatically
        QVERIFY(guard2.isDisabled("c1"));
        QCOMPARE(guard2.mayStart(request("c1")).reason, MayStartResult::DisabledByGuard);
    }

    void theSameVersionTrippingOnTwoConnectionsDisablesTheVersion()
    {
        Rig r;
        Listener lis;
        r.guard.setListener(&lis);
        const LinkId a = linkOf("c1", "0.1.0", "sa");
        const LinkId b = linkOf("c2", "0.1.0", "sb");
        r.guard.mayStart(request("c1"));
        r.guard.mayStart(request("c2"));
        r.guard.onAdmitted(a);
        r.guard.onAdmitted(b);
        for (int i = 0; i < 2; ++i)
            r.guard.onViolation(violation(a, ViolationKind::UnknownType, Severity::Hard));
        QVERIFY(r.guard.isDisabled("c1"));
        QVERIFY(!r.keeper.versionDisabled("libre", "0.1.0"));      // one connection is not enough
        r.clock.advanceS(3 * 24 * 3600);
        for (int i = 0; i < 2; ++i)
            r.guard.onViolation(violation(b, ViolationKind::UnknownType, Severity::Hard));
        QVERIFY(r.guard.isDisabled("c2"));
        QVERIFY(r.keeper.versionDisabled("libre", "0.1.0"));
        QCOMPARE(lis.versionTripped, QStringList() << "libre/0.1.0");

        // A third connection of that version is refused at both doors; another version is not.
        r.guard.onAdmitted(linkOf("c3", "0.1.0", "sc"));              // remembers the version of c3
        QCOMPARE(r.guard.mayStart(request("c3")).reason, MayStartResult::AdapterVersionDisabled);
        QCOMPARE(r.guard.onAdmitted(linkOf("c3", "0.1.0", "sc")).reason,
                 MayStartResult::AdapterVersionDisabled);
        QCOMPARE(r.guard.onAdmitted(linkOf("c4", "0.2.0", "sd")).kind, MayStartResult::Allowed);
    }

    void tripsOutsideTheWindowDoNotAddUp()
    {
        Rig r;
        const LinkId a = linkOf("c1", "0.1.0", "sa");
        const LinkId b = linkOf("c2", "0.1.0", "sb");
        for (int i = 0; i < 2; ++i)
            r.guard.onViolation(violation(a, ViolationKind::UnknownType, Severity::Hard));
        r.clock.advanceS(8LL * 24 * 3600);                          // window is 7 days
        for (int i = 0; i < 2; ++i)
            r.guard.onViolation(violation(b, ViolationKind::UnknownType, Severity::Hard));
        QVERIFY(r.guard.isDisabled("c1") && r.guard.isDisabled("c2"));
        QVERIFY(!r.keeper.versionDisabled("libre", "0.1.0"));
    }

    void aNewVersionIsAnotherIdentity()
    {
        Rig r;
        r.keeper.disableVersion("libre", "0.1.0", "test");
        QCOMPARE(r.guard.onAdmitted(linkOf("c1", "0.1.0")).reason, MayStartResult::AdapterVersionDisabled);
        QCOMPARE(r.guard.onAdmitted(linkOf("c1", "0.1.1")).kind, MayStartResult::Allowed);
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
    }

    // ---- manual re-enable (REQ-GRD-08) ------------------------------------------------
    void reenableClearsTheStateAndWritesTheAcknowledgement()
    {
        Rig r;
        Listener lis;
        r.guard.setListener(&lis);
        const LinkId l = linkOf("c1");
        r.guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
        r.guard.onViolation(violation(l, ViolationKind::UnknownType, Severity::Hard));
        QVERIFY(r.guard.isDisabled("c1"));

        QVERIFY(!r.guard.reenable("c-unknown"));
        QVERIFY(r.guard.reenable("c1"));
        QVERIFY(!r.guard.isDisabled("c1"));
        QCOMPARE(r.guard.scoreOf("c1"), 0.0);
        QCOMPARE(lis.reenabled, QStringList() << "c1");
        QCOMPARE(r.sink.count("acknowledgement"), 1);
        QCOMPARE(r.guard.mayStart(request("c1")).kind, MayStartResult::Allowed);
        QVERIFY(!r.guard.reenable("c1"));                           // not disabled any more
    }

    void reenablingTheVersionDoesNotClearAConnectionThatTrippedByItself()
    {
        Rig r;
        r.keeper.disableConnection("c1", "libre", "0.1.0", StateCause::score(), "test");
        r.keeper.disableVersion("libre", "0.1.0", "test");
        QVERIFY(r.guard.reenableVersion("libre", "0.1.0"));
        QVERIFY(!r.keeper.versionDisabled("libre", "0.1.0"));
        QVERIFY(r.guard.isDisabled("c1"));
        QVERIFY(!r.guard.reenableVersion("libre", "0.1.0"));
    }

    // ---- ingest log (REQ-GRD-10, REQ-GRD-11) -------------------------------------------------
    void sessionsAreTaggedAndEntriesCarryAdapterAndVersion()
    {
        Rig r;
        const LinkId l = linkOf("c1", "0.1.0", "sess-7");
        QCOMPARE(r.guard.onAdmitted(l).kind, MayStartResult::Allowed);
        r.guard.onViolation(violation(l, ViolationKind::StderrFlood, Severity::Soft));
        ExitStatus e;
        r.guard.onExited(l, e);
        QCOMPARE(r.sink.entries.size(), 3);
        QStringList events;
        for (int i = 0; i < r.sink.entries.size(); ++i) {
            const IngestLogEntry &x = r.sink.entries.at(i);
            events << x.event;
            QCOMPARE(x.adapter, QStringLiteral("libre"));
            QCOMPARE(x.adapterVersion, QStringLiteral("0.1.0"));
            QCOMPARE(x.sessionId, QStringLiteral("sess-7"));
            QCOMPARE(x.connection, QStringLiteral("c1"));
        }
        QCOMPARE(events, QStringList() << "session-start" << "violation" << "session-end");
    }

    // the end of every session carries the adapter's highest resident size, a stop and a
    // kill for memory alike, and says so when the driver ended it for memory.
    void theEndOfASessionCarriesThePeakResidentSize()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        r.guard.onAdmitted(l);
        ExitStatus stop;
        stop.peakResidentBytes = 27262976;
        r.guard.onExited(l, stop);
        QCOMPARE(r.sink.entries.last().event, QStringLiteral("session-end"));
        QCOMPARE(r.sink.entries.last().detail, QStringLiteral("exit 0, peak_resident_bytes=27262976"));

        ExitStatus memory;
        memory.crashed = true;
        memory.killedForMemory = true;
        memory.peakResidentBytes = 140000000;
        r.guard.onExited(l, memory);
        QCOMPARE(r.sink.entries.last().detail,
                 QStringLiteral("crashed, killed for memory, peak_resident_bytes=140000000"));

        ExitStatus unknown;                                     // the size could not be read
        r.guard.onExited(l, unknown);
        QCOMPARE(r.sink.entries.last().detail, QStringLiteral("exit 0"));
    }

    void theQueueHoldsEntriesWhileTheStoreIsBusyAndNeverLosesThemSilently()
    {
        GuardSettings s;
        s.ingestQueueMax = 3;
        Rig r(s);
        r.sink.setAccepting(false);                                   // maintenance
        const LinkId l = linkOf("c1");
        for (int i = 0; i < 5; ++i)
            r.guard.onViolation(violation(l, ViolationKind::StderrFlood, Severity::Soft));
        QCOMPARE(r.sink.entries.size(), 0);
        QCOMPARE(r.ingest.queued(), 3);
        QCOMPARE(r.ingest.lost(), 2);
        r.sink.setAccepting(true);
        r.ingest.flush();
        QCOMPARE(r.ingest.queued(), 0);
        QCOMPARE(r.sink.count("entries-lost"), 1);                    // the hole is written first
        QCOMPARE(r.sink.entries.first().event, QStringLiteral("entries-lost"));
        QVERIFY(r.sink.entries.first().detail.contains("2"));
        QCOMPARE(r.sink.count("violation"), 3);
    }

    void noEntryCarriesContent()
    {
        Rig r;
        const LinkId l = linkOf("c1");
        r.guard.onAdmitted(l);
        r.guard.onViolation(violation(l, ViolationKind::UndeclaredUnit, Severity::Hard));
        r.guard.onViolation(violation(l, ViolationKind::UndeclaredUnit, Severity::Hard));
        for (int i = 0; i < r.sink.entries.size(); ++i) {
            const QString all = r.sink.entries.at(i).detail;
            QVERIFY2(!all.contains("password") && !all.contains("token"), qPrintable(all));
        }
    }

    // ---- limits ----------------------------------------------------------------------------
    void limitsKeepTheAddressSpaceLimitOffAndTheResidentLimitOnByDefault()
    {
        Rig r;
        const LinkLimits l = r.guard.limits();
        QCOMPARE(l.memoryLimitBytes, qint64(0));                      // address space: never
        QCOMPARE(l.residentLimitBytes, qint64(128) * 1024 * 1024);    // resident size: on, 128 MB
        QCOMPARE(l.cpuTimeLimitS, 1800);
        QCOMPARE(l.fileDescriptorLimit, 64);
        QCOMPARE(l.maxLineBytes, 1048576);
        QCOMPARE(l.maxRecordsPerBatch, 500);
    }

    // ---- state keeper and the journal-backed log ---------------------------------------------
    void theKeeperFoldsAndRetriesFailedWrites()
    {
        ManualClock clock;
        MemoryStateLog log;
        StateKeeper k(&log, &clock);
        k.load();
        log.setFailWrites(true);
        k.setAttention("c1", StateCause::authFailed(), "refused");
        k.setHold("c2", UtcTime::fromMSecsSinceEpoch(clock.ms() + 3600000), "429");
        QCOMPARE(k.pendingWrites(), 2);
        QCOMPARE(log.size(), 0);
        QVERIFY(k.attention("c1").active());                          // applies in memory at once
        QVERIFY(k.holdUntil("c2").isValid());
        k.retryPending();
        QCOMPARE(k.pendingWrites(), 2);
        log.setFailWrites(false);
        k.retryPending();
        QCOMPARE(k.pendingWrites(), 0);
        QCOMPARE(log.size(), 2);

        k.clearConnection("c1", "user");
        QVERIFY(!k.attention("c1").active());
        QCOMPARE(log.size(), 3);

        StateKeeper again(&log, &clock);                               // a restart
        again.load();
        QVERIFY(!again.attention("c1").active());
        QVERIFY(again.holdUntil("c2").isValid());
    }

    void entriesOfTheSameMillisecondStayInOrder()
    {
        ManualClock clock;
        MemoryStateLog log;
        StateKeeper k(&log, &clock);
        k.load();
        k.setAttention("c1", StateCause::terms(), "x");
        k.clearConnection("c1", "y");
        k.setAttention("c1", StateCause::internal(), "z");
        QVERIFY(log.load().at(0).time < log.load().at(1).time);
        QVERIFY(log.load().at(1).time < log.load().at(2).time);
        QCOMPARE(k.attention("c1").cause, StateCause::internal());
    }

    void theJournalBackedLogSurvivesARestartInATemporaryDirectory()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        ManualClock clock;
        {
            Journal j;
            QVERIFY2(j.open(path), qPrintable(j.lastError()));
            JournalStateLog log(&j);
            StateKeeper k(&log, &clock);
            k.load();
            k.disableConnection("c1", "libre", "0.1.0", StateCause::score(), "score 17 >= 15");
            k.disableVersion("libre", "0.1.0", "2 connections tripped");
            k.setAttention("c2", StateCause::authFailed(), "refused");
            k.setHold("c3", UtcTime::fromMSecsSinceEpoch(clock.ms() + 7200000), "retry after 7200 s");
            k.clearConnection("c2", "new credentials");
            QCOMPARE(k.pendingWrites(), 0);
        }
        Journal j;                                                    // a new object, same file
        QVERIFY(j.open(path));
        QCOMPARE(j.entries(JournalKind::connectionState()).size(), 5);
        JournalStateLog log(&j);
        StateKeeper k(&log, &clock);
        k.load();
        QVERIFY(k.connectionDisabled("c1"));
        QCOMPARE(k.connectionDisabledCause("c1"), StateCause::score());
        QVERIFY(k.versionDisabled("libre", "0.1.0"));
        QVERIFY(!k.attention("c2").active());                          // cleared
        QCOMPARE(k.holdUntil("c3").msecsSinceEpoch(), clock.ms() + 7200000);
        QCOMPARE(k.connectionsTrippedWithin("libre", "0.1.0", clock.now(), 7LL * 24 * 3600 * 1000), 1);

        // A guard on that state refuses the connection with no new violation.
        RecordingIngestSink sink;
        IngestLog ingest(&sink, 10);
        Guard guard(&clock, &k, &ingest);
        QCOMPARE(guard.mayStart(request("c1")).reason, MayStartResult::DisabledByGuard);
    }
};

QTEST_APPLESS_MAIN(TstGuard)
#include "tst_guard.moc"
