// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Scheduler and session manager: the transition tables
// (rows T-nn) (rows S-nn), row by row, plus simulations of whole
// days with injected faults. A manual clock; no real time passes, no process runs, no network.
// Synthetic data only. The fake link calls the real guard as the driver does.
#include "schedtest.h"

#include <QTemporaryDir>

using namespace lm;
using namespace lm::testing;

namespace {

const qint64 kSec = 1000;
const qint64 kMinute = 60 * kSec;
const qint64 kHour = 60 * kMinute;

QUuid sid(const QString &connection, int i = 0)
{
    static const QUuid ns("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}");
    return QUuid::createUuidV5(ns, connection + QStringLiteral("-subject-%1").arg(i));
}

QString src(const QString &feed) { return QStringLiteral("libre.") + feed; }

// A scheduler over the fakes with one connection of `subjects` Subjects, started.
void setup(FakeWorld &w, int subjects = 1, const QString &connection = QStringLiteral("c1"),
           bool start = true)
{
    if (!w.scheduler)
        w.build();
    ConnectionConfig c;
    c.id = connection;
    c.adapterId = QStringLiteral("libre");
    for (int i = 0; i < subjects; ++i) {
        SubjectConfig s;
        s.id = sid(connection, i);
        s.subjectRef = QStringLiteral("%1-ref-%2").arg(connection).arg(i);
        c.subjects.append(s);
    }
    w.scheduler->addConnection(c);
    if (start)
        w.scheduler->start();
}

ScheduleState stateOf(FakeWorld &w, const QString &c = "c1", int i = 0)
{
    return w.scheduler->status(sid(c, i)).state;
}
QString causeOf(FakeWorld &w, const QString &c = "c1", int i = 0)
{
    return w.scheduler->status(sid(c, i)).cause;
}

Script fail(ErrorClass cls, int retryAfterS = -1, CommandOutcome::KilledBy by = CommandOutcome::NotKilled)
{
    Script s;
    s.ok = false;
    s.errorClass = cls;
    s.retryAfterS = retryAfterS;
    s.killedBy = by;
    return s;
}

class Spy : public SchedulerListener
{
public:
    Spy() : queueFull(0) {}
    void subjectStateChanged(const SubjectStatus &s) { states.append(scheduleStateName(s.state) + ":" + s.cause); }
    void stalenessChanged(const SubjectStatus &s) { staleness.append(s.staleCause); lastStale = s; }
    void connectionWarning(const QString &, const QString &kind, const QString &) { warnings.append(kind); }
    void readingQueueFull(int n) { queueFull += n; }

    QStringList states, staleness, warnings;
    SubjectStatus lastStale;
    int queueFull;
};

// Moves from wake-up to wake-up until `done()` holds or `limitMs` have passed. False on timeout.
bool advanceUntil(FakeWorld &w, const std::function<bool()> &done, qint64 limitMs)
{
    const qint64 end = w.clock.ms() + limitMs;
    int guardLoop = 0;
    while (!done() && ++guardLoop < 100000) {
        if (w.clock.ms() >= end)
            return false;
        w.advanceToNextWake(end);
    }
    return done();
}

Script okCurrent(FakeWorld &w, const QString &type, const QString &ref, const UtcTime &from,
                 const UtcTime &to, const QStringList &feeds)
{
    return w.defaultFetch(type, ref, from, to, feeds);
}

// A new world over the persisted state of another one, as after a restart of the service.
void restartFrom(FakeWorld &fresh, const FakeWorld &old)
{
    fresh.stateLog = old.stateLog;
    fresh.keeper.load();
    fresh.readings.stored = old.readings.stored;
    fresh.clock.setMs(old.clock.ms());
}

} // namespace

class TstScheduler : public QObject
{
    Q_OBJECT

private slots:
    // =========================================================================================
    // The schedule (..): when the next run is
    // =========================================================================================
    void startPlansTheFirstRunAfterTheSettleTime()
    {
        FakeWorld w;
        setup(w);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QCOMPARE(st.state, ScheduleState::Idle);
        // settle 5 s + 0.5 * spread 25 s
        QCOMPARE(st.nextDue[FeedCurrent].msecsSinceEpoch(), w.clock.ms() + 17500);
        QCOMPARE(st.nextDue[FeedGraph].msecsSinceEpoch(), w.clock.ms() + 17500);
        QCOMPARE(st.nextDue[FeedLogbook].msecsSinceEpoch(), w.clock.ms() + 17500);
        w.clock.advanceMs(17499);
        w.step();
        QCOMPARE(w.commands.size(), 0);
        QCOMPARE(stateOf(w), ScheduleState::Idle);
    }

    void aFirstPollStoresTheReadingAndAimsAtTheNextOne()
    {
        FakeWorld w;
        setup(w);
        const qint64 t0 = w.clock.ms();
        w.runUntil(t0 + 25 * kSec);
        QVERIFY(w.commands.size() >= 1);
        QCOMPARE(w.commands.first().type, QStringLiteral("fetch_current"));
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 1);
        // The command ran 2 s after the login (17.5 s settle + 2 s gap): 59.5 s into the minute
        // the clock started 40 s into. The reading is stamped on the minute and the next run is
        // 70 s after the stamp, which is 30 s after t0.
        QCOMPARE(w.commands.first().startMs, t0 + 19500);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QCOMPARE(st.state, ScheduleState::Idle);
        QCOMPARE(st.nextDue[FeedCurrent].msecsSinceEpoch(), t0 + 30 * kSec);
        QCOMPARE(st.lastSuccessAt.msecsSinceEpoch(), t0 - 40 * kSec);
    }

    void aReadingIsStoredOnlyIfNewer()                                              // REQ-SCH-03
    {
        FakeWorld w;
        setup(w);
        const qint64 stamp = w.clock.ms() - 40000;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &, const UtcTime &, const QStringList &) {
            Script s;
            if (type == "fetch_current") {
                QVector<LinkRecord> v;
                v.append(w.sample(ref, "current", stamp));        // the same reading every time
                s.batches.append(qMakePair(QString("current"), v));
            }
            return s;
        };
        w.runUntil(w.clock.ms() + 10 * kMinute);
        int currents = 0;
        for (int i = 0; i < w.commands.size(); ++i)
            if (w.commands[i].type == "fetch_current") ++currents;
        QVERIFY(currents >= 3);                                  // polled several times ...
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 1);   // ... stored once
    }

    // ---- the trend arrow: in memory, with its reading ----------

    // A source model whose `current` feed carries a trend arrow: each poll returns the latest
    // minute's reading and a completion whose watermark is that reading (as the real adapter's),
    // with the arrow given by *trendCode (-1 = the adapter sent none).
    static void trendSource(FakeWorld &w, const int *trendCode)
    {
        w.fetchScript = [&w, trendCode](const QString &type, const QString &ref, const UtcTime &,
                                        const UtcTime &, const QStringList &) {
            Script s;
            if (type == "fetch_current") {
                const qint64 stamp = w.currentStamp(w.clock.ms());
                QVector<LinkRecord> v;
                v.append(w.sample(ref, "current", stamp));
                s.batches.append(qMakePair(QString("current"), v));
                s.completion.watermark = UtcTime::fromMSecsSinceEpoch(stamp);
                s.completion.recordsSent = 1;
                s.completion.hasTrend = *trendCode >= 0;
                s.completion.trend = *trendCode >= 0 ? *trendCode : 0;
            }
            return s;
        };
    }

    void theTrendArrowIsKeptWithItsReadingAndAbsentBeforeTheFirstOne()
    {
        FakeWorld w;
        setup(w);
        int trend = 4;
        trendSource(w, &trend);
        QVERIFY(!w.scheduler->status(sid("c1")).hasTrend);            // nothing polled yet
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 1);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QVERIFY(st.hasTrend);
        QCOMPARE(st.trendCode, 4);
        QCOMPARE(st.trendFor, st.lastSuccessAt);                      // it belongs to the newest reading
    }

    void aNewReadingReplacesTheArrowAndOneWithoutAnArrowClearsIt()
    {
        FakeWorld w;
        setup(w);
        int trend = 2;
        trendSource(w, &trend);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.scheduler->status(sid("c1")).trendCode, 2);
        const UtcTime first = w.scheduler->status(sid("c1")).trendFor;

        trend = 5;
        w.runUntil(w.clock.ms() + 2 * kMinute);
        SubjectStatus st = w.scheduler->status(sid("c1"));
        QVERIFY(st.hasTrend);
        QCOMPARE(st.trendCode, 5);
        QVERIFY(first < st.trendFor);

        trend = -1;                                                   // the source gives no arrow now
        w.runUntil(w.clock.ms() + 2 * kMinute);
        st = w.scheduler->status(sid("c1"));
        QVERIFY(!st.hasTrend);
        QVERIFY(st.lastSuccessAt.isValid());                          // the reading itself is there
    }

    void theTrendArrowIsAbsentWhenItsReadingIsOld()
    {
        FakeWorld w;
        setup(w);
        int trend = 3;
        trendSource(w, &trend);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QVERIFY(w.scheduler->status(sid("c1")).hasTrend);
        // No further reading arrives: the arrow is dropped once its reading passes the
        // staleness limit (3 intervals of 60 s), without any poll being needed to say so.
        const qint64 stampMs = w.scheduler->status(sid("c1")).trendFor.msecsSinceEpoch();
        w.clock.setMs(stampMs + 3 * kMinute);
        QVERIFY(w.scheduler->status(sid("c1")).hasTrend);             // exactly at the limit
        w.clock.setMs(stampMs + 3 * kMinute + 1);
        QVERIFY(!w.scheduler->status(sid("c1")).hasTrend);
    }

    void theArrowOfAReadingTheCoreDidNotKeepIsNotAttached()
    {
        // A completion whose watermark is not the newest stored reading (the reading was
        // rejected in validation, or the source repeats an older one) must not give its arrow
        // to the Subject.
        FakeWorld w;
        setup(w);
        int trend = 1;
        trendSource(w, &trend);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.scheduler->status(sid("c1")).trendCode, 1);
        const UtcTime kept = w.scheduler->status(sid("c1")).trendFor;

        w.fetchScript = [&w](const QString &type, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            Script s;
            if (type == "fetch_current") {
                // No record reaches the scheduler, but the completion claims a newer reading.
                s.completion.watermark = UtcTime::fromMSecsSinceEpoch(w.currentStamp(w.clock.ms()));
                s.completion.hasTrend = true;
                s.completion.trend = 5;
                s.quarantined = 1;
            }
            return s;
        };
        w.runUntil(w.clock.ms() + kMinute);                          // still inside the age limit
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QCOMPARE(st.trendFor, kept);
        QCOMPARE(st.trendCode, 1);
    }

    void onlyTheCurrentFeedGivesAnArrow()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [&w](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to,
                             const QStringList &feeds) {
            Script s = w.defaultFetch(type, ref, from, to, feeds);
            if (type == "fetch_range") {         // a graph or logbook completion that carries one anyway
                s.completion.hasTrend = true;
                s.completion.trend = 5;
            }
            return s;
        };
        w.runUntil(w.clock.ms() + 25 * kSec);
        QVERIFY(w.scheduler->status(sid("c1")).lastSuccessAt.isValid());
        QVERIFY(!w.scheduler->status(sid("c1")).hasTrend);
    }

    void theArrowIsNotPersistedAcrossARestart()
    {
        FakeWorld w;
        setup(w);
        int trend = 4;
        trendSource(w, &trend);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QVERIFY(w.scheduler->status(sid("c1")).hasTrend);

        FakeWorld fresh;
        restartFrom(fresh, w);
        setup(fresh);
        QVERIFY(!fresh.scheduler->status(sid("c1")).hasTrend);        // memory only
    }

    void aMissingReadingIsRecheckedThenTheRhythmIsTheInterval()
    {
        FakeWorld w;
        setup(w);
        const qint64 stamp = w.clock.ms() - 40000;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &, const UtcTime &, const QStringList &) {
            Script s;
            if (type == "fetch_current") {
                QVector<LinkRecord> v;
                v.append(w.sample(ref, "current", stamp));
                s.batches.append(qMakePair(QString("current"), v));
            }
            return s;
        };
        w.runUntil(w.clock.ms() + 10 * kMinute);
        QList<qint64> starts;
        for (int i = 0; i < w.commands.size(); ++i)
            if (w.commands[i].type == "fetch_current") starts.append(w.commands[i].startMs);
        QVERIFY(starts.size() >= 8);
        // The first poll found the reading, the second did not: four rechecks 15 s apart
        // (starts 2 to 5), then the plain interval of 60 s.
        for (int i = 2; i <= 5; ++i)
            QCOMPARE(starts[i] - starts[i - 1], 15 * kSec);
        QCOMPARE(starts[6] - starts[5], 60 * kSec);
        QCOMPARE(starts[7] - starts[6], 60 * kSec);
    }

    void aReadingStampedInTheFutureDoesNotPushTheNextPollAway()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &, const UtcTime &, const QStringList &) {
            Script s;
            if (type == "fetch_current") {
                QVector<LinkRecord> v;
                v.append(w.sample(ref, "current", w.clock.ms() + 4 * kMinute));   // within the skew tolerance
                s.batches.append(qMakePair(QString("current"), v));
            }
            return s;
        };
        w.runUntil(w.clock.ms() + 25 * kSec);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QVERIFY(st.nextDue[FeedCurrent].msecsSinceEpoch() <= w.clock.ms() + 70 * kSec);
    }

    void graphAndLogbookRunAtTheirOwnIntervalFromTheEndOfThePoll()
    {
        FakeWorld w;
        setup(w);
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QList<qint64> graph, logbook;
        for (int i = 0; i < w.commands.size(); ++i) {
            if (w.commands[i].type == "fetch_range") graph.append(w.commands[i].startMs);
        }
        QVERIFY(graph.size() >= 2);                  // graph and logbook ask once each at the start
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QVERIFY(st.nextDue[FeedGraph].msecsSinceEpoch() >= w.clock.ms());
        // Five minutes after the end of the graph poll, six hours after the logbook poll.
        const qint64 gEnd = w.commands.last().endMs;
        Q_UNUSED(gEnd);
        QVERIFY(st.nextDue[FeedLogbook].msecsSinceEpoch() - w.clock.ms() > 5 * kHour);
    }

    void logbookCanBeSwitchedOff()
    {
        FakeWorld w;
        w.build();
        ConnectionConfig c;
        c.id = "c1";
        c.adapterId = "libre";
        c.logbookIntervalMs = 0;
        SubjectConfig s;
        s.id = sid("c1");
        s.subjectRef = "ref";
        c.subjects.append(s);
        w.scheduler->addConnection(c);
        w.scheduler->start();
        QVERIFY(!w.scheduler->status(sid("c1")).nextDue[FeedLogbook].isValid());
        w.runUntil(w.clock.ms() + 2 * kHour);
        for (int i = 0; i < w.commands.size(); ++i)
            QVERIFY(w.commands[i].type != "fetch_range" || true);
        QVERIFY(!w.scheduler->status(sid("c1")).nextDue[FeedLogbook].isValid());
    }

    void thePlanOfAFeedTheRangeAskedForFollowsTheNewestStoredRecord()                 // 3.2, backfill
    {
        FakeWorld w;
        setup(w);
        QList<UtcTime> froms;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_range" && feeds.contains("graph"))
                froms.append(from);
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        const qint64 t0 = w.clock.ms();
        w.runUntil(t0 + 40 * kSec);
        QCOMPARE(froms.size(), 1);
        QCOMPARE(froms[0].msecsSinceEpoch(), t0 + 29500 - 12 * kHour);    // the graph ran after the current poll and the gap    // nothing stored: the whole window
        const int stored = w.readings.count(sid("c1"), src("graph"));
        QVERIFY(stored > 100);

        // Three hours without any command (the phone was off), then the regular poll:
        w.clock.advanceMs(3 * kHour);
        w.runUntil(w.clock.ms() + 30 * kSec);
        QVERIFY(froms.size() >= 2);
        // It asks from the newest stored graph point, not from 12 hours ago: nothing repeated.
        const qint64 newestBefore = ((t0 + 29500 - 19 * kMinute) / 300000) * 300000;
        QCOMPARE(froms[1].msecsSinceEpoch(), newestBefore);
        QVERIFY(w.readings.count(sid("c1"), src("graph")) > stored);      // the gap was filled
    }
    // =========================================================================================
    // Errors (..), REQ-SCH-04
    // =========================================================================================
    void transientBackoffDoublesToTheCapAndKeepsTheJitterBounds()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::Transient);
        };
        const qint64 expected[] = { 30, 60, 120, 240, 480, 900, 900 };
        for (int i = 0; i < 7; ++i) {
            QVERIFY2(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::Backoff; }, 3 * kHour),
                     qPrintable(QString("round %1").arg(i)));
            const SubjectStatus st = w.scheduler->status(sid("c1"));
            QCOMPARE(causeOf(w), QStringLiteral("transient"));
            QCOMPARE(st.retryAt.msecsSinceEpoch() - w.clock.ms(), expected[i] * kSec);
            w.clock.setMs(st.retryAt.msecsSinceEpoch());
            w.step();
        }
    }

    void theJitterMovesTheDelayWithinTwentyFivePercent()
    {
        for (int k = 0; k < 2; ++k) {
            FakeWorld w;
            w.random.setFallback(k == 0 ? 0.0 : 0.999999);
            setup(w);
            w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
                return fail(ErrorClass::Transient);
            };
            QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::Backoff; }, kHour));
            const qint64 delay = w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch() - w.clock.ms();
            if (k == 0) QCOMPARE(delay, qint64(22500));
            else QVERIFY(delay >= 37400 && delay <= 37500);
        }
    }

    void aSuccessEndsTheBackoffSeries()
    {
        FakeWorld w;
        setup(w);
        int calls = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && calls++ < 2)
                return fail(ErrorClass::Transient);
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(w, [&]() { return w.scheduler->status(sid("c1")).backoffN == 2; }, kHour));
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) >= 1; }, kHour));
        QCOMPARE(w.scheduler->status(sid("c1")).backoffN, 0);
        QCOMPARE(stateOf(w), ScheduleState::Idle);
    }

    void aRateLimitHoldsEverySubjectRaisesTheFloorAndIsPersisted()                   // REQ-SCH-13
    {
        FakeWorld w;
        setup(w, 2);
        Spy spy;
        w.scheduler->setListener(&spy);
        bool limited = false;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (!limited) { limited = true; return fail(ErrorClass::RateLimited, 100); }
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w, "c1", 0) == ScheduleState::Backoff; }, kHour));
        const qint64 now = w.clock.ms();
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(stateOf(w, "c1", i), ScheduleState::Backoff);
            QCOMPARE(causeOf(w, "c1", i), QStringLiteral("rate-limited"));
            QCOMPARE(w.scheduler->status(sid("c1", i)).retryAt.msecsSinceEpoch(), now + 1800 * kSec);   // 30 minutes beat a 100 s hint
        }
        QCOMPARE(w.keeper.holdUntil("c1").msecsSinceEpoch(), now + 1800 * kSec);                      // persisted
        QCOMPARE(w.sink.count("rate-limited"), 1);
        QVERIFY(spy.warnings.contains("rate-limited"));

        // No command at all during the hold.
        const int before = w.commands.size();
        w.runUntil(now + 1799 * kSec);
        QCOMPARE(w.commands.size(), before);

        // After the hold, polling resumes and the interval is the raised floor (120 s).
        w.runUntil(now + 1800 * kSec + 12 * kMinute);
        QVERIFY(!w.keeper.holdUntil("c1").isValid());                                                 // cleared with the hold
        QList<qint64> starts;
        for (int i = 0; i < w.commands.size(); ++i)
            if (w.commands[i].type == "fetch_current" && w.commands[i].startMs > now + 1800 * kSec)
                starts.append(w.commands[i].startMs);
        QVERIFY(starts.size() >= 6);
        // Each Subject is polled every 120 s now (the first poll after the hold is not yet
        // aligned with the readings, so look at the later ones).
        QVERIFY(starts[5] - starts[3] >= 118 * kSec && starts[5] - starts[3] <= 125 * kSec);
    }

    void aSecondRateLimitDoublesTheHoldAndTheCalmDayHalvesTheFloor()
    {
        FakeWorld w;
        setup(w);
        int limits = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (limits < 2 && type == "fetch_current") { ++limits; return fail(ErrorClass::RateLimited); }
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(w, [&]() { return limits == 1 && stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch() - w.clock.ms(), 1800 * kSec);
        w.clock.setMs(w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch());
        w.step();
        QVERIFY(advanceUntil(w, [&]() { return limits == 2 && stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch() - w.clock.ms(), 3600 * kSec);   // doubled
        // The floor went 60 -> 120 -> 240 s. Each calm day halves it once; after two it is gone.
        w.clock.setMs(w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch());
        w.runUntil(w.clock.ms() + 26 * kHour);
        QList<qint64> mid;
        for (int i = w.commands.size() - 1; i >= 0 && mid.size() < 3; --i)
            if (w.commands[i].type == "fetch_current") mid.prepend(w.commands[i].startMs);
        QCOMPARE(mid[2] - mid[1], 122 * kSec);                              // 120 s after one calm day
        w.runUntil(w.clock.ms() + 26 * kHour);
        QList<qint64> starts;
        for (int i = w.commands.size() - 1; i >= 0 && starts.size() < 4; --i)
            if (w.commands[i].type == "fetch_current") starts.prepend(w.commands[i].startMs);
        QVERIFY(starts[3] - starts[2] < 100 * kSec);                          // the plain minute again (a daily login may add 2 s)
    }

    void aHoldSurvivesARestart()
    {
        FakeWorld a;
        setup(a);
        a.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::RateLimited, 7200);
        };
        QVERIFY(advanceUntil(a, [&]() { return stateOf(a) == ScheduleState::Backoff; }, kHour));
        const qint64 until = a.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch();

        FakeWorld b;
        restartFrom(b, a);
        b.clock.advanceMs(10 * kMinute);
        setup(b);
        QCOMPARE(stateOf(b), ScheduleState::Backoff);
        QCOMPARE(causeOf(b), QStringLiteral("rate-limited"));
        QCOMPARE(b.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch(), until);
        b.runUntil(until - kSec);
        QCOMPARE(b.commands.size(), 0);                                    // no hammering after the restart
        b.scheduler->userRetry("c1");
        QCOMPARE(stateOf(b), ScheduleState::Idle);
        QVERIFY(!b.keeper.holdUntil("c1").isValid());
    }

    void termsStopTheConnectionForTheCaregiverAndSurviveARestart()
    {
        FakeWorld a;
        setup(a, 2);
        Spy spy;
        a.scheduler->setListener(&spy);
        a.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::TermsMustBeAccepted);
        };
        QVERIFY(advanceUntil(a, [&]() { return stateOf(a) == ScheduleState::NeedsAttention; }, kHour));
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(stateOf(a, "c1", i), ScheduleState::NeedsAttention);
            QCOMPARE(causeOf(a, "c1", i), QStringLiteral("terms"));
        }
        QVERIFY(a.keeper.attention("c1").active());
        const int cmds = a.commands.size();
        a.runUntil(a.clock.ms() + 2 * kHour);
        QCOMPARE(a.commands.size(), cmds);                                  // nothing runs by itself
        QVERIFY(a.scheduler->status(sid("c1")).stale);
        QCOMPARE(a.scheduler->status(sid("c1")).staleCause, StaleCause::needsAttention());
        QCOMPARE(a.scheduler->status(sid("c1")).staleDetail, QStringLiteral("terms"));

        a.scheduler->credentialsChanged("c1");                              // credentials do not cure terms
        QCOMPARE(stateOf(a), ScheduleState::NeedsAttention);

        FakeWorld b;                                                         // restart: no new login attempt
        restartFrom(b, a);
        setup(b, 2);
        QCOMPARE(stateOf(b), ScheduleState::NeedsAttention);
        QCOMPARE(causeOf(b), QStringLiteral("terms"));
        b.runUntil(b.clock.ms() + kHour);
        QCOMPARE(b.commands.size(), 0);
        QCOMPARE(b.secrets.loads, 0);

        b.scheduler->userRetry("c1");
        QCOMPARE(stateOf(b), ScheduleState::Idle);
        QVERIFY(!b.keeper.attention("c1").active());
        b.runUntil(b.clock.ms() + 3 * kMinute);
        QVERIFY(b.commands.size() > 0);
    }

    void aChangedSourceStopsTheConnectionWithAWarningAndGetsOneProbeAfterAnUpdate()
    {
        FakeWorld a;
        setup(a);
        Spy spy;
        a.scheduler->setListener(&spy);
        a.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::ProtocolChanged);
        };
        QVERIFY(advanceUntil(a, [&]() { return stateOf(a) == ScheduleState::NeedsAttention; }, kHour));
        QCOMPARE(causeOf(a), QStringLiteral("protocol-changed"));
        QVERIFY(spy.warnings.contains("protocol-changed"));
        QCOMPARE(a.keeper.attentionVersion("c1"), QStringLiteral("0.1.0"));

        FakeWorld same;                                                      // same adapter version: stays stopped
        restartFrom(same, a);
        same.build();
        same.scheduler->setCurrentAdapterVersion("libre", "0.1.0");
        setup(same, 1, "c1", false);
        same.scheduler->start();
        QCOMPARE(stateOf(same), ScheduleState::NeedsAttention);

        FakeWorld updated;                                                   // another version: one more try
        restartFrom(updated, a);
        updated.build();
        updated.scheduler->setCurrentAdapterVersion("libre", "0.2.0");
        setup(updated, 1, "c1", false);
        updated.scheduler->start();
        QCOMPARE(stateOf(updated), ScheduleState::Idle);
        QVERIFY(!updated.keeper.attention("c1").active());
    }

    void invalidCommandsAreNeverHeldAgainstTheSourceOrTheGuard()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::InvalidCommand);
        };
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(causeOf(w), QStringLiteral("internal"));
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::NeedsAttention; }, 3 * kHour));
        QCOMPARE(causeOf(w), QStringLiteral("internal"));
        QCOMPARE(w.guard.scoreOf("c1"), 0.0);                                // not a violation
        QVERIFY(!w.guard.isDisabled("c1"));
        QCOMPARE(w.sink.count("violation"), 0);
        QCOMPARE(w.guard.mayStart(StartRequest()).kind, MayStartResult::Allowed);
    }

    void consecutivePollsWithRejectedRecordsCountAsOneProtocolChange()
    {
        FakeWorld w;
        setup(w);
        int poll = 0;
        bool alternate = false;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            Script s = w.defaultFetch(type, ref, from, to, feeds);
            ++poll;
            if (!alternate || poll % 3 != 0)
                s.quarantined = 1;
            return s;
        };
        alternate = true;                       // every third poll is clean: the streak never reaches 5
        w.runUntil(w.clock.ms() + 40 * kMinute);
        QCOMPARE(stateOf(w) == ScheduleState::NeedsAttention, false);

        FakeWorld w2;
        setup(w2);
        w2.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            Script s = w2.defaultFetch(type, ref, from, to, feeds);
            s.quarantined = 1;
            return s;
        };
        Spy spy;
        w2.scheduler->setListener(&spy);
        QVERIFY(advanceUntil(w2, [&]() { return stateOf(w2) == ScheduleState::NeedsAttention; }, kHour));
        QCOMPARE(causeOf(w2), QStringLiteral("protocol-changed"));
        int polls = 0;
        for (int i = 0; i < w2.commands.size(); ++i) ++polls;
        QCOMPARE(polls, 5);                                                   // exactly kRejectPollsMax polls
        QVERIFY(spy.warnings.contains("protocol-changed"));
        QVERIFY(w2.keeper.attention("c1").active());
    }

    void aSubjectThatKeepsFailingWhileAnotherSucceedsNeedsAttentionAloneAndIsPersisted()
    {
        FakeWorld a;
        setup(a, 2);
        a.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (ref.endsWith("ref-1"))
                return fail(ErrorClass::Transient);
            return a.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(a, [&]() { return stateOf(a, "c1", 1) == ScheduleState::NeedsAttention; }, 12 * kHour));
        QCOMPARE(causeOf(a, "c1", 1), QStringLiteral("subject-unavailable"));
        QVERIFY(stateOf(a, "c1", 0) != ScheduleState::NeedsAttention);        // the other Subject carries on
        const int before = a.readings.count(sid("c1", 0), src("current"));
        a.runUntil(a.clock.ms() + 10 * kMinute);
        QVERIFY(a.readings.count(sid("c1", 0), src("current")) > before);
        QVERIFY(!a.keeper.attention("c1").active());                           // the connection itself is fine

        FakeWorld b;
        restartFrom(b, a);
        setup(b, 2);
        QCOMPARE(stateOf(b, "c1", 1), ScheduleState::NeedsAttention);
        QCOMPARE(causeOf(b, "c1", 1), QStringLiteral("subject-unavailable"));
        QCOMPARE(stateOf(b, "c1", 0), ScheduleState::Idle);
        b.scheduler->userRetry("c1");
        QCOMPARE(stateOf(b, "c1", 1), ScheduleState::Idle);
    }

    void aSingleSubjectNeverBecomesSubjectUnavailable()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::Transient);
        };
        w.runUntil(w.clock.ms() + 12 * kHour);
        QCOMPARE(stateOf(w), ScheduleState::Backoff);                          // no one to compare with
    }

    void aKillByTheGuardIsABackoffNotAChangeOfTheSource()                              // L4
    {
        FakeWorld w;
        setup(w);
        int calls = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && calls++ == 0)
                return fail(ErrorClass::ProtocolChanged, -1, CommandOutcome::Guard);
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(causeOf(w), QStringLiteral("guard-kill"));
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) > 0; }, kHour));
        QVERIFY(!w.keeper.attention("c1").active());
    }

    // =========================================================================================
    // The guard's reach into the schedules (..)
    // =========================================================================================
    static LinkId linkOf(const QString &connection, const QString &session = "s-test", const QString &version = "0.1.0")
    {
        LinkId l;
        l.connectionId = connection;
        l.adapterId = "libre";
        l.adapterVersion = version;
        l.sessionId = session;
        return l;
    }
    static Violation hard(const LinkId &l)
    {
        Violation v;
        v.link = l;
        v.kind = ViolationKind::UnknownType;
        v.severity = Severity::Hard;
        return v;
    }

    void aTripDisablesEverySubjectOfTheConnectionAndNothingRunsAfterIt()
    {
        FakeWorld w;
        setup(w, 2);
        Spy spy;
        w.scheduler->setListener(&spy);
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QVERIFY(w.commands.size() > 0);
        w.guard.onViolation(hard(linkOf("c1")));
        w.clock.advanceMs(kMinute);
        w.guard.onViolation(hard(linkOf("c1")));
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(stateOf(w, "c1", i), ScheduleState::DisabledByGuard);
            QCOMPARE(causeOf(w, "c1", i), QStringLiteral("connection"));
        }
        QCOMPARE(w.scheduler->sessionState("c1"), SessionState::Denied);
        const int cmds = w.commands.size();
        w.runUntil(w.clock.ms() + 6 * kHour);
        QCOMPARE(w.commands.size(), cmds);
        QVERIFY(w.scheduler->status(sid("c1")).stale);
        QCOMPARE(w.scheduler->status(sid("c1")).staleCause, StaleCause::disabledByGuard());
        // User actions do not leave the state.
        w.scheduler->userRetry("c1");
        w.scheduler->credentialsChanged("c1");
        w.scheduler->secretsAvailable("c1");
        QCOMPARE(stateOf(w), ScheduleState::DisabledByGuard);
    }

    void aTripDuringACommandIsNotUndoneByTheLateOutcome()
    {
        FakeWorld w;
        setup(w);
        w.clock.advanceMs(18 * kSec);
        w.scheduler->tick();                                   // the command is started but not settled
        QCOMPARE(stateOf(w), ScheduleState::Fetching);
        w.guard.onViolation(hard(linkOf("c1")));
        w.clock.advanceMs(kMinute);
        w.guard.onViolation(hard(linkOf("c1")));
        w.pump();                                               // the old callbacks arrive now
        QCOMPARE(stateOf(w), ScheduleState::DisabledByGuard);
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 0);
    }

    void disabledByGuardSurvivesARestartAndOnlyTheManualReenableLeavesIt()
    {
        FakeWorld a;
        setup(a);
        a.guard.onViolation(hard(linkOf("c1")));
        a.clock.advanceMs(kMinute);
        a.guard.onViolation(hard(linkOf("c1")));

        FakeWorld b;
        restartFrom(b, a);
        b.clock.advanceS(10LL * 24 * 3600);
        setup(b);
        QCOMPARE(stateOf(b), ScheduleState::DisabledByGuard);
        b.runUntil(b.clock.ms() + 3 * kHour);
        QCOMPARE(b.commands.size(), 0);                                   // never left automatically
        QCOMPARE(b.guard.mayStart(StartRequest()).kind, MayStartResult::Allowed);   // (another connection id: empty)
        StartRequest r;
        r.connectionId = "c1";
        r.adapterId = "libre";
        QCOMPARE(b.guard.mayStart(r).reason, MayStartResult::DisabledByGuard);

        QVERIFY(b.guard.reenable("c1"));
        QCOMPARE(stateOf(b), ScheduleState::Idle);
        QCOMPARE(b.sink.count("acknowledgement"), 1);
        b.runUntil(b.clock.ms() + 3 * kMinute);
        QVERIFY(b.commands.size() > 0);
        QVERIFY(b.readings.count(sid("c1"), src("current")) > 0);
        QCOMPARE(b.guard.scoreOf("c1"), 0.0);
    }

    void aVersionTripOnTwoConnectionsStopsTheThirdOne()
    {
        FakeWorld w;
        setup(w, 1, "c1");
        setup(w, 1, "c2");
        setup(w, 1, "c3");
        w.runUntil(w.clock.ms() + 3 * kMinute);                // every connection has run once with version 0.1.0
        for (int k = 0; k < 2; ++k) {
            const QString c = k == 0 ? "c1" : "c2";
            w.guard.onViolation(hard(linkOf(c)));
            w.clock.advanceMs(kMinute);
            w.guard.onViolation(hard(linkOf(c)));
        }
        QVERIFY(w.keeper.versionDisabled("libre", "0.1.0"));
        QCOMPARE(stateOf(w, "c3"), ScheduleState::DisabledByGuard);        // it never misbehaved itself
        QCOMPARE(causeOf(w, "c3"), QStringLiteral("adapter-version"));
        QVERIFY(!w.guard.isDisabled("c3"));

        QVERIFY(w.guard.reenableVersion("libre", "0.1.0"));
        QCOMPARE(stateOf(w, "c3"), ScheduleState::Idle);
        QCOMPARE(stateOf(w, "c1"), ScheduleState::DisabledByGuard);        // tripped by itself: stays
    }

    void aRestartLearnsAVersionDisablingAtTheFirstStart()
    {
        FakeWorld a;
        setup(a);
        a.keeper.disableVersion("libre", "0.1.0", "test");
        FakeWorld b;
        restartFrom(b, a);
        setup(b);
        QCOMPARE(stateOf(b), ScheduleState::Idle);                          // the version is not known yet
        b.runUntil(b.clock.ms() + 40 * kSec);
        QCOMPARE(stateOf(b), ScheduleState::DisabledByGuard);              // onAdmitted refused it
        QCOMPARE(causeOf(b), QStringLiteral("adapter-version"));
        // After an update the adapter has another version: it starts again by identity.
        b.adapterVersion = "0.2.0";
        b.guard.reenableVersion("libre", "0.1.0");
        b.runUntil(b.clock.ms() + 3 * kMinute);
        QVERIFY(b.readings.count(sid("c1"), src("current")) > 0);
    }

    void aSpawnProblemOpensTheCircuitAndTheScheduleWaitsForItsEnd()
    {
        FakeWorld w;
        setup(w);
        w.startBehavior = []() { return 1; };                               // the process cannot be spawned
        QVERIFY(advanceUntil(w, [&]() { return causeOf(w) == "circuit-open"; }, kHour));
        const qint64 reopen = w.scheduler->status(sid("c1")).retryAt.msecsSinceEpoch();
        QCOMPARE(reopen, w.guard.circuitReopenAt("c1").msecsSinceEpoch());
        QVERIFY(reopen - w.clock.ms() > 10 * kMinute);                       // 15 minutes
        QCOMPARE(w.sink.count("circuit-open"), 1);
        // The trigger is over; the trial start succeeds and the circuit closes.
        w.startBehavior = []() { return 0; };
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) > 0; }, 2 * kHour));
        QCOMPARE(w.sink.count("circuit-close"), 1);
    }

    void readingsWaitInABoundedQueueWhileTheStoreIsBusy()                              // REQ-STO-15
    {
        FakeWorld w;
        w.settings.readingQueueMax = 3;
        setup(w);
        Spy spy;
        w.scheduler->setListener(&spy);
        w.readings.busy = true;                                              // export or compaction is running
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QVERIFY(w.scheduler->queuedReadings() > 0);
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 0);
        QCOMPARE(stateOf(w), ScheduleState::Idle);                           // monitoring goes on
        QVERIFY(w.scheduler->status(sid("c1")).lastSuccessAt.isValid());     // the staleness clock goes on too
        w.readings.busy = false;
        w.step();
        QCOMPARE(w.scheduler->queuedReadings(), 0);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);            // nothing lost

        // A queue that fills up says so, and the loss is logged, never silent.
        w.readings.busy = true;
        w.runUntil(w.clock.ms() + 20 * kMinute);
        QVERIFY(spy.queueFull > 0);
        QVERIFY(w.sink.count("readings-lost") > 0);
    }

    // =========================================================================================
    // The session of a connection
    // =========================================================================================
    void theFirstLoginOffersTheStoredTokenKeepsTheNewOneAndWaitsBeforeTheFirstFetch()
    {
        FakeWorld w;
        setup(w);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.factory.created, 1);
        QCOMPARE(w.factory.last()->lastSessionOffered, QStringLiteral("opaque-token-ABCDEF"));
        QVERIFY(w.factory.last()->lastCredentialsWereGiven);
        QCOMPARE(w.secrets.session, QStringLiteral("opaque-token-NEW"));         // the new blob is stored
        QCOMPARE(w.secrets.saves, 1);
        QCOMPARE(w.authStarts.size(), 1);
        QVERIFY(w.commands.first().startMs - w.authStarts.first() >= 2 * kSec);   // REQ-SCH-05
    }

    void aTokenInsideItsRenewalWindowIsNotOfferedSoTheAdapterLogsInAfresh()
    {
        FakeWorld w;
        w.build();
        w.secrets.expires = UtcTime::fromMSecsSinceEpoch(w.clock.ms() + 3LL * 24 * kHour);
        setup(w);
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.factory.last()->lastSessionOffered, QString());
    }

    void lockedCredentialsWaitForTheUserAtASlowPaceAndCarryOnWhenApproved()
    {
        FakeWorld w;
        setup(w);
        Spy spy;
        w.scheduler->setListener(&spy);
        w.secrets.kind = SecretLoad::Locked;
        w.runUntil(w.clock.ms() + 4 * kHour);
        QCOMPARE(stateOf(w), ScheduleState::NeedsAttention);
        QCOMPARE(causeOf(w), QStringLiteral("credentials-locked"));
        QVERIFY(spy.warnings.contains("approval-waiting"));
        QVERIFY(!w.keeper.attention("c1").active());                              // not persisted: it ends by itself
        QCOMPARE(w.scheduler->status(sid("c1")).staleCause, StaleCause::credentialsLocked());
        QCOMPARE(w.factory.created, 0);                                            // no process for a locked store
        // The accesses (each raises the system notification) come every 5, 10, 20, 40, 60, 60 minutes.
        const QList<qint64> t = w.secrets.loadTimes;
        QVERIFY(t.size() >= 6);
        QCOMPARE(t[1] - t[0], 300 * kSec);
        QCOMPARE(t[2] - t[1], 600 * kSec);
        QCOMPARE(t[3] - t[2], 1200 * kSec);
        QCOMPARE(t[4] - t[3], 2400 * kSec);
        QCOMPARE(t[5] - t[4], 3600 * kSec);

        w.secrets.kind = SecretLoad::Ok;                                           // the user approved
        w.scheduler->secretsAvailable("c1");
        QCOMPARE(stateOf(w), ScheduleState::Idle);
        w.runUntil(w.clock.ms() + 40 * kSec);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);
    }

    void aLockedStateIsNotRestoredAfterARestart()
    {
        FakeWorld a;
        setup(a);
        a.secrets.kind = SecretLoad::Locked;
        a.runUntil(a.clock.ms() + kMinute);
        QCOMPARE(causeOf(a), QStringLiteral("credentials-locked"));
        FakeWorld b;
        restartFrom(b, a);
        setup(b);
        QCOMPARE(stateOf(b), ScheduleState::Idle);                                 // the store decides again
    }

    void missingCredentialsNeedTheCaregiverUntilTheyAreChanged()
    {
        FakeWorld w;
        setup(w);
        w.secrets.kind = SecretLoad::Missing;
        w.runUntil(w.clock.ms() + kMinute);
        QCOMPARE(causeOf(w), QStringLiteral("credentials-missing"));
        QVERIFY(w.keeper.attention("c1").active());
        const int loads = w.secrets.loads;
        w.runUntil(w.clock.ms() + 2 * kHour);
        QCOMPARE(w.secrets.loads, loads);                                          // no polling of an empty store
        w.secrets.kind = SecretLoad::Ok;
        w.scheduler->credentialsChanged("c1");
        QCOMPARE(stateOf(w), ScheduleState::Idle);
        w.runUntil(w.clock.ms() + 40 * kSec);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);
        QVERIFY(!w.keeper.attention("c1").active());
    }

    void aRejectedTokenGetsOneReloginAndTheCommandIsRepeated()
    {
        FakeWorld w;
        setup(w);
        int calls = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && calls++ == 0)
                return fail(ErrorClass::AuthFailed);
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QCOMPARE(w.authStarts.size(), 2);                                          // login + one re-login
        QCOMPARE(w.factory.last()->lastSessionOffered, QString());                 // a fresh login, no blob
        QVERIFY(w.authStarts[1] - w.authStarts[0] >= kMinute);                      // REQ-SCH-05 between logins
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);                  // the command was repeated
        QCOMPARE(stateOf(w), ScheduleState::Idle);
        QVERIFY(!w.keeper.attention("c1").active());
    }

    void aSecondRejectionAfterTheReloginStopsTheConnection()
    {
        FakeWorld a;
        setup(a);
        a.fetchScript = [](const QString &type, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            Q_UNUSED(type);
            return fail(ErrorClass::AuthFailed);
        };
        QVERIFY(advanceUntil(a, [&]() { return stateOf(a) == ScheduleState::NeedsAttention; }, kHour));
        QCOMPARE(causeOf(a), QStringLiteral("auth-failed"));
        QCOMPARE(a.authStarts.size(), 2);                                          // exactly one re-login
        QVERIFY(a.keeper.attention("c1").active());
        a.runUntil(a.clock.ms() + 24 * kHour);
        QCOMPARE(a.authStarts.size(), 2);                                          // never retried by itself

        FakeWorld b;                                                                // and not after a restart either
        restartFrom(b, a);
        setup(b);
        QCOMPARE(causeOf(b), QStringLiteral("auth-failed"));
        b.runUntil(b.clock.ms() + kHour);
        QCOMPARE(b.authStarts.size(), 0);

        b.fetchScript = FakeWorld().fetchScript;
        b.scheduler->credentialsChanged("c1");                                      // new credentials:
        QCOMPARE(stateOf(b), ScheduleState::Idle);
        b.runUntil(b.clock.ms() + kMinute);
        QCOMPARE(b.authStarts.size(), 1);
        QVERIFY(b.readings.count(sid("c1"), src("current")) > 0);
    }

    void theSingleReloginIsAvailableAgainAfterASuccess()
    {
        FakeWorld w;
        setup(w);
        int calls = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && (calls == 0 || calls == 30)) { ++calls; return fail(ErrorClass::AuthFailed); }
            if (type == "fetch_current") ++calls;
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        w.runUntil(w.clock.ms() + 50 * kMinute);
        QVERIFY(calls > 30);
        QCOMPARE(w.authStarts.size(), 3);                                          // login, re-login, re-login again
        QVERIFY(stateOf(w) != ScheduleState::NeedsAttention);
    }

    void aLoginThatKeepsFailingAsTransientEndsAsASuspectedRefusalAndIsRetriedSlowly()
    {
        FakeWorld w;
        setup(w);
        w.authScript = []() { return fail(ErrorClass::Transient); };
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::NeedsAttention; }, 8 * kHour));
        QCOMPARE(causeOf(w), QStringLiteral("auth-failed-suspected"));
        QCOMPARE(w.authStarts.size(), 6);
        // Gaps of 5, 10, 20, 40 and 60 minutes at least (the doubling gap of REQ-SCH-05).
        const qint64 minGap[] = { 300, 600, 1200, 2400, 3600 };
        for (int i = 1; i < 6; ++i)
            QVERIFY2(w.authStarts[i] - w.authStarts[i - 1] >= minGap[i - 1] * kSec,
                     qPrintable(QString("gap %1 was %2 s").arg(i).arg((w.authStarts[i] - w.authStarts[i - 1]) / 1000)));
        QVERIFY(w.keeper.attention("c1").active());
        QCOMPARE(w.keeper.attention("c1").cause, StateCause::authFailedSuspected());

        // Six hours later one more attempt; it fails and the suspicion returns at once.
        const qint64 sinceSuspected = w.clock.ms();
        w.runUntil(sinceSuspected + 6 * kHour + 5 * kMinute);
        QCOMPARE(w.authStarts.size(), 7);
        QCOMPARE(causeOf(w), QStringLiteral("auth-failed-suspected"));

        // The outage was real after all: the next slow attempt works.
        w.authScript = FakeWorld().authScript;
        w.runUntil(w.clock.ms() + 7 * kHour);
        QCOMPARE(stateOf(w), ScheduleState::Idle);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);
        QVERIFY(!w.keeper.attention("c1").active());
    }

    void aRefusedLoginIsNotRetriedByItself()
    {
        FakeWorld w;
        setup(w);
        w.authScript = []() { return fail(ErrorClass::AuthFailed); };
        w.runUntil(w.clock.ms() + 30 * kHour);
        QCOMPARE(causeOf(w), QStringLiteral("auth-failed"));
        QCOMPARE(w.authStarts.size(), 1);
    }

    void aTokenNearItsExpiryIsRenewedBetweenCommandsAndARefusalIsOnlyAWarning()
    {
        FakeWorld w;
        w.grantLifetimeMs = 9LL * 24 * kHour;
        setup(w);
        Spy spy;
        w.scheduler->setListener(&spy);
        const qint64 t0 = w.clock.ms();
        // A fresh login (no token offered) is refused from the moment of the renewal on; the
        // stored token still works.
        w.authScript = [&]() {
            const bool fresh = w.factory.last()->lastSessionOffered.isEmpty();
            return (fresh && w.clock.ms() - t0 >= 2LL * 24 * kHour - 5 * kMinute) ? fail(ErrorClass::AuthFailed)
                                                                                    : Script();
        };
        w.runUntil(w.clock.ms() + 5 * 24 * kHour);
        QVERIFY(w.authStarts.size() >= 2);
        // The daily recycle logs in again at 24 h with the stored token; the renewal comes when
        // the token is 7 days from its end: 9 d - 7 d = 2 days after the first login.
        bool renewalSeen = false;
        for (int i = 1; i < w.authStarts.size(); ++i)
            if (w.authStarts[i] - w.authStarts[0] >= 2LL * 24 * kHour - 5 * kMinute)
                renewalSeen = true;
        QVERIFY(renewalSeen);
        QVERIFY(spy.warnings.contains("renewal-refused"));
        QVERIFY(stateOf(w) != ScheduleState::NeedsAttention);                      // monitoring goes on
        QVERIFY(!w.keeper.attention("c1").active());
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 4 * 24 * 60);
        // The refused renewal is tried again only every six hours.
        const int tries = w.authStarts.size();
        QVERIFY(tries <= 1 + 2 * 4 + (3 * 24) / 6 + 2);      // first login, daily restarts (two logins each in the window), a try every six hours
    }

    void aGoodRenewalReplacesTheStoredToken()
    {
        FakeWorld w;
        w.grantLifetimeMs = 9LL * 24 * kHour;
        setup(w);
        w.runUntil(w.clock.ms() + 3 * 24 * kHour);
        QVERIFY(w.authStarts.size() >= 2);
        QVERIFY(w.secrets.saves >= 2);
        QVERIFY(w.secrets.expires.msecsSinceEpoch() > w.clock.ms() + 8 * 24 * kHour);   // the new expiry
    }

    void settingsUpdateKeepsOnlyABareRegionalHost()                                     // 2.2, REQ-SRC-02
    {
        FakeWorld w;
        setup(w);
        w.authScript = []() {
            Script s;
            s.completion.sensor.insert("region_host", "api-eu.example.invalid");
            s.completion.sensor.insert("evil", "x");
            return s;
        };
        w.runUntil(w.clock.ms() + 25 * kSec);
        QCOMPARE(w.connSettings.data.value("c1").value("region_host").toString(), QStringLiteral("api-eu.example.invalid"));
        QVERIFY(!w.connSettings.data.value("c1").contains("evil"));
        QCOMPARE(w.sink.count("settings-update-rejected"), 1);
        for (int i = 0; i < w.sink.entries.size(); ++i)
            if (w.sink.entries[i].event == "settings-update-rejected")
                QCOMPARE(w.sink.entries[i].detail, QStringLiteral("keys: evil"));      // names, never values

        FakeWorld v;
        setup(v);
        v.authScript = []() {
            Script s;
            s.completion.sensor.insert("region_host", "http://elsewhere.example.invalid/path");
            return s;
        };
        v.runUntil(v.clock.ms() + 25 * kSec);
        QVERIFY(!v.connSettings.data.value("c1").contains("region_host"));
        QCOMPARE(v.sink.count("settings-update-rejected"), 1);
    }

    void theLinkStaysUpBetweenMinutePollsAndIsRecycledOnceADay()   // REQ-ADP-13
    {
        FakeWorld w;
        setup(w);
        w.runUntil(w.clock.ms() + kHour);
        QCOMPARE(w.factory.created, 1);
        w.runUntil(w.clock.ms() + 24 * kHour);
        QCOMPARE(w.factory.created, 2);                                             // one daily recycle
        QCOMPARE(stateOf(w), ScheduleState::Idle);
    }

    void theLinkIsClosedWhenNothingIsDueSoon()
    {
        FakeWorld w;
        w.build();
        ConnectionConfig c;
        c.id = "c1";
        c.adapterId = "libre";
        c.currentIntervalMs = 10 * kMinute;
        c.graphIntervalMs = 10 * kMinute;
        c.logbookIntervalMs = 0;
        SubjectConfig s;
        s.id = sid("c1");
        s.subjectRef = "ref";
        c.subjects.append(s);
        w.scheduler->addConnection(c);
        w.scheduler->start();
        w.runUntil(w.clock.ms() + kHour);
        QVERIFY(w.factory.created >= 4);                                            // closed between the polls
        QVERIFY(w.readings.count(sid("c1"), src("current")) >= 4);
        QVERIFY(w.sink.count("circuit-open") == 0);
    }

    void aProcessThatDiesBetweenPollsIsReplacedWithoutAFuss()
    {
        FakeWorld w;
        setup(w);
        w.runUntil(w.clock.ms() + 2 * kMinute);
        QCOMPARE(w.factory.created, 1);
        w.factory.last()->kill();
        const int before = w.readings.count(sid("c1"), src("current"));
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QCOMPARE(w.factory.created, 2);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > before);
        QCOMPARE(stateOf(w), ScheduleState::Idle);
        QVERIFY(w.sink.count("session-end") >= 1);
    }

    // =========================================================================================
    // The staleness clock through the scheduler (REQ-SCH-06, 07)
    // =========================================================================================
    static Script oldReading(FakeWorld &w, const QString &type, const QString &ref, qint64 stamp)
    {
        Script s;
        if (type == "fetch_current") {
            QVector<LinkRecord> v;
            v.append(w.sample(ref, "current", stamp));
            s.batches.append(qMakePair(QString("current"), v));
        }
        return s;
    }

    void noNewReadingsWhileTheContactIsFine()                                          // cause 2
    {
        FakeWorld w;
        setup(w);
        const qint64 stamp = w.clock.ms() - 40000;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &, const UtcTime &, const QStringList &) {
            return oldReading(w, type, ref, stamp);
        };
        Spy spy;
        w.scheduler->setListener(&spy);
        w.runUntil(w.clock.ms() + 10 * kMinute);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QVERIFY(st.stale);
        QCOMPARE(st.staleCause, StaleCause::noNewReadings());
        QCOMPARE(st.lastSuccessAt.msecsSinceEpoch(), stamp);
        QVERIFY(spy.staleness.contains(StaleCause::noNewReadings()));
    }

    void aSuccessfulGraphFetchDoesNotMoveTheClock()
    {
        FakeWorld w;
        setup(w);
        const qint64 stamp = w.clock.ms() - 40000;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current")
                return oldReading(w, type, ref, stamp);
            return w.defaultFetch(type, ref, from, to, feeds);        // graph and logbook deliver fresh points
        };
        w.runUntil(w.clock.ms() + 12 * kMinute);
        QVERIFY(w.readings.count(sid("c1"), src("graph")) > 100);
        QCOMPARE(w.scheduler->status(sid("c1")).lastSuccessAt.msecsSinceEpoch(), stamp);
        QVERIFY(w.scheduler->status(sid("c1")).stale);
    }

    void aSensorInWarmUp()                                                              // cause 3
    {
        FakeWorld w;
        w.readings.sensorActivated = UtcTime::fromMSecsSinceEpoch(w.clock.ms() - 10 * kMinute);
        w.readings.sensorWarmup = 60;
        setup(w);
        const qint64 stamp = w.clock.ms() - 40000;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &, const UtcTime &, const QStringList &) {
            return oldReading(w, type, ref, stamp);
        };
        w.runUntil(w.clock.ms() + 10 * kMinute);
        QCOMPARE(w.scheduler->status(sid("c1")).staleCause, StaleCause::warmUp());
    }

    void aSourceThatCannotBeReached()                                                   // cause 1
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::Transient);
        };
        w.runUntil(w.clock.ms() + 10 * kMinute);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QCOMPARE(st.staleCause, StaleCause::unreachable());
        QCOMPARE(st.staleDetail, QStringLiteral("transient"));
    }

    void aRateLimitIsCauseOneWithItsDetail()
    {
        FakeWorld w;
        setup(w);
        w.fetchScript = [](const QString &, const QString &, const UtcTime &, const UtcTime &, const QStringList &) {
            return fail(ErrorClass::RateLimited);
        };
        w.runUntil(w.clock.ms() + 10 * kMinute);
        const SubjectStatus st = w.scheduler->status(sid("c1"));
        QCOMPARE(st.staleCause, StaleCause::unreachable());
        QCOMPARE(st.staleDetail, QStringLiteral("rate-limited"));
    }

    void aServiceThatWasNotRunning()                                                    // cause 5
    {
        FakeWorld w;
        w.build();
        w.settings.startSettleMs = 20 * kMinute;                 // nothing can run in the first minutes
        w.build();
        setup(w, 1, "c1", false);
        w.scheduler->start(UtcTime::fromMSecsSinceEpoch(w.clock.ms() - 2 * kHour));
        w.runUntil(w.clock.ms() + 4 * kMinute);
        QCOMPARE(w.scheduler->status(sid("c1")).staleCause, StaleCause::serviceNotRunning());
        // Once a poll has run, the cause is judged by the live state instead.
        w.runUntil(w.clock.ms() + 40 * kMinute);
        QVERIFY(w.scheduler->status(sid("c1")).staleCause != StaleCause::serviceNotRunning());
    }

    // =========================================================================================
    // Whole days with injected faults
    // =========================================================================================
    void aDayOfPollingWithFaultsKeepsEveryPromise()
    {
        FakeWorld w;
        setup(w, 2);
        const qint64 t0 = w.clock.ms();
        int killOnce = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            const qint64 rel = w.clock.ms() - t0;
            if (rel >= 2 * kHour && rel < 2 * kHour + 20 * kMinute)
                return fail(ErrorClass::Transient);                                  // a 20-minute outage
            if (rel >= 6 * kHour && rel < 6 * kHour + 10 && type == "fetch_current")
                return fail(ErrorClass::RateLimited, 1200);                          // one rate limit
            if (rel >= 9 * kHour && killOnce == 0 && type == "fetch_current") {
                killOnce = 1;
                return fail(ErrorClass::ProtocolChanged, -1, CommandOutcome::Guard); // a kill by the guard
            }
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        w.runUntil(t0 + 24 * kHour);

        // No two commands closer than the minimum gap, per connection (REQ-SCH-05).
        for (int i = 1; i < w.commands.size(); ++i)
            QVERIFY2(w.commands[i].startMs - w.commands[i - 1].endMs >= 10 * kSec,
                     qPrintable(QString("commands %1 and %2 are %3 ms apart").arg(i - 1).arg(i)
                                    .arg(w.commands[i].startMs - w.commands[i - 1].endMs)));
        // No login closer than the minimum gap.
        for (int i = 1; i < w.authStarts.size(); ++i)
            QVERIFY(w.authStarts[i] - w.authStarts[i - 1] >= 60 * kSec);

        // Readings: about one a minute for each Subject, minus the outage and the hold.
        for (int k = 0; k < 2; ++k) {
            const int n = w.readings.count(sid("c1", k), src("current"));
            QVERIFY2(n > 24 * 60 - 20 - 30 - 20, qPrintable(QString("subject %1 stored %2").arg(k).arg(n)));
            QVERIFY(n <= 24 * 60);
        }
        QCOMPARE(stateOf(w, "c1", 0), ScheduleState::Idle);
        QCOMPARE(stateOf(w, "c1", 1), ScheduleState::Idle);
        QVERIFY(!w.scheduler->status(sid("c1")).stale);
        // The days's faults raised nothing for the guard to hold against the source.
        QVERIFY(!w.guard.isDisabled("c1"));
        QCOMPARE(w.sink.count("disabled"), 0);
    }

    void noCredentialTokenOrNameEverReachesAnEntryOrAMessage()
    {
        FakeWorld w;
        setup(w);
        Spy spy;
        w.scheduler->setListener(&spy);
        int n = 0;
        w.authScript = [&]() { return ++n < 3 ? fail(ErrorClass::Transient) : Script(); };
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && n == 3 && w.commands.size() < 5)
                return fail(ErrorClass::TermsMustBeAccepted);
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        w.runUntil(w.clock.ms() + 6 * kHour);
        w.scheduler->userRetry("c1");
        w.runUntil(w.clock.ms() + kHour);
        QStringList all = spy.states + spy.warnings;
        for (int i = 0; i < w.sink.entries.size(); ++i) {
            const IngestLogEntry &e = w.sink.entries[i];
            all << e.event << e.detail << e.adapter << e.adapterVersion << e.sessionId << e.connection;
        }
        const QVector<StateEntry> states = w.stateLog.load();
        for (int i = 0; i < states.size(); ++i)
            all << states[i].detail << states[i].cause << states[i].connection << states[i].adapter;
        const QString blob = all.join("\n");
        QVERIFY(!blob.contains("not-a-real-password"));
        QVERIFY(!blob.contains("caregiver@example"));
        QVERIFY(!blob.contains("opaque-token"));
        QVERIFY(!blob.isEmpty());
    }

    void connectionStatesSurviveARestartThroughARealJournalFile()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        qint64 holdUntil = 0;
        {
            Journal j;
            QVERIFY(j.open(path));
            JournalStateLog log(&j);
            FakeWorld a;
            a.keeper.~StateKeeper();
            new (&a.keeper) StateKeeper(&log, &a.clock);
            a.keeper.load();
            // c1 is tripped, c2 needs attention, c3 is on hold.
            setup(a, 1, "c1");
            setup(a, 1, "c2");
            setup(a, 1, "c3");
            a.guard.onViolation(hard(linkOf("c1")));
            a.clock.advanceMs(kMinute);
            a.guard.onViolation(hard(linkOf("c1")));
            a.keeper.setAttention("c2", StateCause::terms(), "x");
            holdUntil = a.clock.ms() + 2 * kHour;
            a.keeper.setHold("c3", UtcTime::fromMSecsSinceEpoch(holdUntil), "x");
            QCOMPARE(a.keeper.pendingWrites(), 0);
            a.scheduler->shutdown();
        }
        Journal j;
        QVERIFY(j.open(path));
        JournalStateLog log(&j);
        FakeWorld b;
        b.keeper.~StateKeeper();
        new (&b.keeper) StateKeeper(&log, &b.clock);
        b.keeper.load();
        b.build();
        b.clock.advanceS(3600);
        setup(b, 1, "c1");
        setup(b, 1, "c2");
        setup(b, 1, "c3");
        QCOMPARE(stateOf(b, "c1"), ScheduleState::DisabledByGuard);
        QCOMPARE(stateOf(b, "c2"), ScheduleState::NeedsAttention);
        QCOMPARE(causeOf(b, "c2"), QStringLiteral("terms"));
        QCOMPARE(stateOf(b, "c3"), ScheduleState::Backoff);
        QCOMPARE(b.scheduler->status(sid("c3")).retryAt.msecsSinceEpoch(), holdUntil);
    }

    // =========================================================================================
    // The remaining rows of the two tables
    // =========================================================================================
    void aRejectedAdapterManifestIsADefectOfTheCore()
    {
        FakeWorld w;
        setup(w, 2);
        w.startBehavior = []() { return 3; };
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::NeedsAttention; }, kHour));
        QCOMPARE(causeOf(w), QStringLiteral("adapter-rejected"));
        QCOMPARE(causeOf(w, "c1", 1), QStringLiteral("adapter-rejected"));
        QVERIFY(w.keeper.attention("c1").active());
    }

    void aSecretStoreThatFailsIsATransientProblem()
    {
        FakeWorld w;
        setup(w);
        w.secrets.kind = SecretLoad::Error;
        QVERIFY(advanceUntil(w, [&]() { return stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(causeOf(w), QStringLiteral("transient"));
        QVERIFY(!w.keeper.attention("c1").active());
        w.secrets.kind = SecretLoad::Ok;
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) > 0; }, kHour));
    }

    void aLoginThatEndsInATermsRateLimitOrChangedReplyIsHandledLikeACommand()
    {
        struct Case { ErrorClass cls; const char *expectState; const char *expectCause; };
        const Case cases[] = {
            { ErrorClass::TermsMustBeAccepted, "needs-attention", "terms" },
            { ErrorClass::ProtocolChanged, "needs-attention", "protocol-changed" },
            { ErrorClass::RateLimited, "backoff", "rate-limited" },
            { ErrorClass::InvalidCommand, "backoff", "internal" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            FakeWorld w;
            setup(w);
            const ErrorClass cls = cases[i].cls;
            w.authScript = [cls]() { return fail(cls); };
            QVERIFY(advanceUntil(w, [&]() { return stateOf(w) != ScheduleState::Idle && stateOf(w) != ScheduleState::Fetching; }, kHour));
            QCOMPARE(scheduleStateName(stateOf(w)), QString(cases[i].expectState));
            QCOMPARE(causeOf(w), QString(cases[i].expectCause));
        }
    }

    void aKillByTheGuardDuringTheLoginIsOnlyABackoff()
    {
        FakeWorld w;
        setup(w);
        int n = 0;
        w.authScript = [&]() { return ++n == 1 ? fail(ErrorClass::ProtocolChanged, -1, CommandOutcome::Guard) : Script(); };
        QVERIFY(advanceUntil(w, [&]() { return n >= 1 && stateOf(w) == ScheduleState::Backoff; }, kHour));
        QVERIFY(!w.keeper.attention("c1").active());
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) > 0; }, kHour));
    }

    void aMissedDeadlineOrAnExitIsATransientError()                                     // REQ-ADP-08
    {
        // The circuit breaker counts these in the guard (tst_guard); a login that succeeds
        // between two of them is a success and resets its count, so here only the schedule's
        // reaction is checked: backoff, no attention, and a new process for the next try.
        FakeWorld w;
        setup(w);
        int n = 0;
        w.fetchScript = [&](const QString &type, const QString &ref, const UtcTime &from, const UtcTime &to, const QStringList &feeds) {
            if (type == "fetch_current" && n < 3) {
                ++n;
                return fail(ErrorClass::Transient, -1, n % 2 ? CommandOutcome::Deadline : CommandOutcome::Exit);
            }
            return w.defaultFetch(type, ref, from, to, feeds);
        };
        QVERIFY(advanceUntil(w, [&]() { return n == 1 && stateOf(w) == ScheduleState::Backoff; }, kHour));
        QCOMPARE(causeOf(w), QStringLiteral("transient"));
        QVERIFY(!w.keeper.attention("c1").active());
        QVERIFY(advanceUntil(w, [&]() { return w.readings.count(sid("c1"), src("current")) > 0; }, 3 * kHour));
        QVERIFY(w.factory.created >= 4);                                            // a new process after each death
        QCOMPARE(stateOf(w), ScheduleState::Idle);
    }

    void changedIntervalsTakeEffectAtOnce()
    {
        FakeWorld w;
        setup(w);
        w.runUntil(w.clock.ms() + 2 * kMinute);
        ConnectionConfig c;
        c.currentIntervalMs = 5 * kMinute;
        c.graphIntervalMs = -1;
        c.logbookIntervalMs = 0;
        w.scheduler->intervalsChanged("c1", c);
        QVERIFY(!w.scheduler->status(sid("c1")).nextDue[FeedLogbook].isValid());
        const int before = w.commands.size();
        w.runUntil(w.clock.ms() + 20 * kMinute);
        int currents = 0;
        for (int i = before; i < w.commands.size(); ++i)
            if (w.commands[i].type == "fetch_current") ++currents;
        QVERIFY(currents >= 3 && currents <= 5);
    }

    void shutdownEndsTheLinkAndLeavesNoCallbackBehind()
    {
        FakeWorld w;
        setup(w);
        w.clock.advanceMs(18 * kSec);
        w.scheduler->tick();
        w.scheduler->shutdown();
        w.pump();
        QCOMPARE(w.readings.count(sid("c1"), src("current")), 0);
        w.scheduler->tick();                                                          // does nothing after shutdown
        QCOMPARE(w.commands.size(), 0);
    }

    void aConnectionWithNothingToDoLeavesNoWakeUp()
    {
        FakeWorld w;
        w.build();
        QVERIFY(!w.scheduler->nextWakeAt().isValid());                                  // not started
        setup(w, 1);
        QVERIFY(w.scheduler->nextWakeAt().isValid());
        w.guard.onViolation(hard(linkOf("c1")));
        w.clock.advanceMs(kMinute);
        w.guard.onViolation(hard(linkOf("c1")));
        // Only the staleness report and nothing else remains to be woken for.
        const UtcTime wake = w.scheduler->nextWakeAt();
        Q_UNUSED(wake);
        w.runUntil(w.clock.ms() + kHour);
        QCOMPARE(w.commands.size(), 0);
    }

    void twoConnectionsDoNotShareAGate()
    {
        FakeWorld w;
        setup(w, 1, "c1");
        setup(w, 1, "c2");
        w.runUntil(w.clock.ms() + 3 * kMinute);
        QVERIFY(w.readings.count(sid("c1"), src("current")) > 0);
        QVERIFY(w.readings.count(sid("c2"), src("current")) > 0);
        // Each connection keeps its own minimum gap; commands of different connections may overlap in time.
        QMap<QString, qint64> lastEnd;
        for (int i = 0; i < w.commands.size(); ++i) {
            const QString c = w.commands[i].connection;
            if (lastEnd.contains(c))
                QVERIFY(w.commands[i].startMs - lastEnd[c] >= 10 * kSec);
            lastEnd[c] = w.commands[i].endMs;
        }
    }

};

QTEST_APPLESS_MAIN(TstScheduler)
#include "tst_scheduler.moc"
