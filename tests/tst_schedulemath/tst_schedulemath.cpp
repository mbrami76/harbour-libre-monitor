// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The pure arithmetic of the scheduler: backoff and
// jitter, effective intervals, the next run of the `current` feed, and the staleness cause.
// Each rule is a table row; no clock and no randomness of its own.
#include "scheduler/schedulemath.h"

#include <QtTest>

using namespace lm;

namespace {
const qint64 kS = 1000;
}

class TstScheduleMath : public QObject
{
    Q_OBJECT

private slots:
    // ---- backoff (REQ-SCH-04) ---------------------------------------------------------------
    void backoffGrowsAndIsCapped_data()
    {
        QTest::addColumn<int>("failures");
        QTest::addColumn<qint64>("expectedMs");
        QTest::newRow("first") << 0 << qint64(30 * kS);
        QTest::newRow("second") << 1 << qint64(60 * kS);
        QTest::newRow("third") << 2 << qint64(120 * kS);
        QTest::newRow("fourth") << 3 << qint64(240 * kS);
        QTest::newRow("fifth") << 4 << qint64(480 * kS);
        QTest::newRow("capped") << 5 << qint64(900 * kS);
        QTest::newRow("still capped") << 12 << qint64(900 * kS);
    }
    void backoffGrowsAndIsCapped()
    {
        QFETCH(int, failures);
        QFETCH(qint64, expectedMs);
        QCOMPARE(backoffDelayMs(failures, -1, SchedulerSettings(), 0.5), expectedMs);   // no jitter
    }

    void jitterStaysInItsBounds()
    {
        const SchedulerSettings s;
        for (int failures = 0; failures < 8; ++failures) {
            const qint64 nominal = backoffDelayMs(failures, -1, s, 0.5);
            for (int i = 0; i <= 100; ++i) {
                const qint64 d = backoffDelayMs(failures, -1, s, i / 101.0);
                QVERIFY2(d >= qint64(double(nominal) * 0.75) - 1 && d <= qint64(double(nominal) * 1.25) + 1,
                         qPrintable(QString("failures %1 draw %2 gave %3").arg(failures).arg(i).arg(d)));
                QVERIFY(d >= s.minFetchGapMs);
            }
            QCOMPARE(backoffDelayMs(failures, -1, s, 0.0), qint64(double(nominal) * 0.75));
        }
    }

    void aRetryHintRaisesTheDelayButIsCapped()
    {
        const SchedulerSettings s;
        QCOMPARE(backoffDelayMs(0, 120, s, 0.5), qint64(120 * kS));              // the hint wins
        QCOMPARE(backoffDelayMs(5, 120, s, 0.5), qint64(900 * kS));              // the backoff wins
        QCOMPARE(backoffDelayMs(0, 999999, s, 0.5), s.rateHoldCapMs);            // a huge hint is capped
    }

    // ---- effective interval (REQ-SCH-13, quiet stretch) ---------------------------------------
    void theFloorRaisesTheIntervalAndSilenceStretchesIt()
    {
        const SchedulerSettings s;
        const qint64 now = 100000000;
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, now - 30 * kS, s), qint64(60 * kS));
        QCOMPARE(effectiveIntervalMs(60 * kS, 120 * kS, true, now, now - 30 * kS, s), qint64(120 * kS));
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, -1, s), qint64(60 * kS));             // never a reading
        // Quiet stretch: doubled per further 30 minutes without a reading, up to 300 s.
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, now - 31 * 60 * kS, s), qint64(120 * kS));
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, now - 61 * 60 * kS, s), qint64(240 * kS));
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, now - 91 * 60 * kS, s), qint64(300 * kS));
        QCOMPARE(effectiveIntervalMs(60 * kS, 0, true, now, now - 600 * 60 * kS, s), qint64(300 * kS));
        // Only the `current` feed is stretched.
        QCOMPARE(effectiveIntervalMs(300 * kS, 0, false, now, now - 600 * 60 * kS, s), qint64(300 * kS));
    }

    // ---- next run of the current feed (REQ-SCH-02) --------------------------------------------
    void aNewReadingAimsJustAfterTheNextOne()
    {
        const SchedulerSettings s;
        const qint64 t = 1000000000;
        // Reading stamped at t, seen 7 s later: next at t + 60 s + 10 s.
        const NextRun r = nextCurrentRun(t + 7 * kS, 60 * kS, true, t, 3, s);
        QCOMPARE(r.atMs, t + 70 * kS);
        QCOMPARE(r.recheckN, 0);
    }

    void aMissingReadingIsRecheckedFourTimesThenTheRhythmIsTheInterval()
    {
        const SchedulerSettings s;
        qint64 now = 1000000000;
        int n = 0;
        for (int i = 1; i <= 4; ++i) {
            const NextRun r = nextCurrentRun(now, 60 * kS, false, 0, n, s);
            QCOMPARE(r.atMs, now + 15 * kS);
            QCOMPARE(r.recheckN, i);
            now = r.atMs;
            n = r.recheckN;
        }
        const NextRun late = nextCurrentRun(now, 60 * kS, false, 0, n, s);                         // late mode
        QCOMPARE(late.atMs, now + 60 * kS);
        QCOMPARE(late.recheckN, 4);
    }

    void theNextRunIsBoundedBelowAndAbove()
    {
        const SchedulerSettings s;
        const qint64 now = 1000000000;
        // A reading that is already old would give a time in the past: never sooner than the gap.
        QCOMPARE(nextCurrentRun(now, 60 * kS, true, now - 200 * kS, 0, s).atMs, now + 10 * kS);
        // A reading stamped four minutes in the future must not push the poll minutes away.
        QCOMPARE(nextCurrentRun(now, 60 * kS, true, now + 240 * kS, 0, s).atMs, now + 70 * kS);
    }

    // ---- staleness (REQ-SCH-06, REQ-SCH-07) ---------------------------------------------------
    static StalenessInput stale()
    {
        StalenessInput in;
        in.nowMs = 1000 * kS;
        in.startedAtMs = 0;
        in.lastSuccessMs = 500 * kS;            // 500 s ago: stale (threshold 180 s)
        in.lastContactOkMs = 990 * kS;          // contact is fine
        in.effCurrentMs = 60 * kS;
        return in;
    }

    void notStaleWithinThreeIntervals()
    {
        StalenessInput in = stale();
        in.lastSuccessMs = in.nowMs - 179 * kS;
        QVERIFY(!computeStaleness(in).stale);
        in.lastSuccessMs = in.nowMs - 181 * kS;
        QVERIFY(computeStaleness(in).stale);
    }

    void eachCauseAndTheirOrder()
    {
        StalenessInput in = stale();
        QCOMPARE(computeStaleness(in).cause, StaleCause::noNewReadings());              // (2)

        in.sensorKnown = true;
        in.sensorReadyMs = in.nowMs + 60 * kS;
        QCOMPARE(computeStaleness(in).cause, StaleCause::warmUp());                     // (3)

        in.lastContactOkMs = 100 * kS;                                                  // contact lost
        QCOMPARE(computeStaleness(in).cause, StaleCause::unreachable());                // (1) before (3)
        in.lastContactOkMs = 990 * kS;
        in.inBackoff = true;
        in.backoffDetail = "rate-limited";
        StalenessResult r = computeStaleness(in);
        QCOMPARE(r.cause, StaleCause::unreachable());                                   // a rate limit is (1)
        QCOMPARE(r.detail, QStringLiteral("rate-limited"));

        in.serviceDownMs = 3600 * kS;
        QCOMPARE(computeStaleness(in).cause, StaleCause::serviceNotRunning());          // (5) before (1)
        in.contactOkSinceStart = true;                                                  // the first poll has run
        QCOMPARE(computeStaleness(in).cause, StaleCause::unreachable());

        in.needsAttention = true;
        in.attentionDetail = "auth-failed";
        r = computeStaleness(in);
        QCOMPARE(r.cause, StaleCause::needsAttention());                                // (7)
        QCOMPARE(r.detail, QStringLiteral("auth-failed"));

        in.credentialsLocked = true;
        QCOMPARE(computeStaleness(in).cause, StaleCause::credentialsLocked());          // (6) before (7)

        in.disabledByGuard = true;
        QCOMPARE(computeStaleness(in).cause, StaleCause::disabledByGuard());            // (4) first
    }

    void withoutAnyReadingTheStartCountsAsTheReference()
    {
        StalenessInput in = stale();
        in.lastSuccessMs = -1;
        in.startedAtMs = in.nowMs - 100 * kS;
        QVERIFY(!computeStaleness(in).stale);
        in.startedAtMs = in.nowMs - 200 * kS;
        QVERIFY(computeStaleness(in).stale);
    }
};

QTEST_APPLESS_MAIN(TstScheduleMath)
#include "tst_schedulemath.moc"
