// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The rhythm of the clock must not change what the engine decides
// (asked the same of the guard). One six-hour story with
// every kind of event (a high value that comes and goes, a low that falls to urgent low, an
// acknowledgement and its re-alert, a source that goes quiet, a warm-up, an urgent high and a
// refused snooze, a snooze that ends, a threshold change) is run against the engine with a tick
// every 1 s, 2 s, 5 s, 10 s, 15 s, 30 s, 60 s, 90 s, 120 s, 180 s and 300 s.
//
// Two ways of ticking are compared with the 1-second run:
//  - "exact": the caller also ticks at the engine's own nextWakeup(), as the service will. Every
//    effect, with its time, must be identical, reminders included.
//  - "coarse": the caller ticks only at its rhythm. Every journal entry, notification, silent
//    update, withdrawal and refusal must still be identical and at the same logical time; only
//    reminders may differ, and then only by being fewer (a burst of reminders due in one coarse
//    step is raised once) and always on the schedule of the 1-second run.
// Every value is synthetic.
#include "alerttest.h"

#include <QtTest>

#include <algorithm>

using namespace lm;
using namespace lm::testing;

namespace {

struct Feed
{
    enum Kind { Reading, Ack, Snooze, Stale, Thresholds, Silent };
    Feed() : t(0), kind(Reading), v(0), slot(AlertSlot::Low), minutes(0), v2(0) {}
    Feed(double t_, Kind k, double v_ = 0) : t(t_), kind(k), v(v_), slot(AlertSlot::Low), minutes(0), v2(0) {}
    double t;
    Kind kind;
    double v;               // reading value, or L for Thresholds
    AlertSlot slot;
    int minutes;
    QString cause;
    double v2;              // H for Thresholds
};

void readings(QVector<Feed> *f, int fromMinute, int toMinute, double value)
{
    for (int m = fromMinute; m <= toMinute; ++m)
        f->append(Feed(m * 60.0, Feed::Reading, value));
}

Feed stale(double t, const char *cause)
{
    Feed e(t, Feed::Stale);
    e.cause = QString::fromLatin1(cause);
    return e;
}

QVector<Feed> story()
{
    QVector<Feed> f;
    readings(&f, 0, 29, 100);
    readings(&f, 30, 59, 190);          // H: pending at 30, active at 45, cleared at 61
    readings(&f, 60, 69, 120);
    readings(&f, 70, 80, 65);           // L active at 70
    readings(&f, 81, 99, 50);           // LL at 81, reminders every 2 min, re-alert after the ack
    { Feed e(82.5 * 60, Feed::Ack); e.slot = AlertSlot::Low; f.append(e); }
    // silent mode for 10 minutes while the urgent low sounds (reminders silent, one silent Update at
    // the start, nothing at the end); a second, unlimited one later on the urgent high
    { Feed e(84 * 60, Feed::Silent); e.minutes = 10; f.append(e); }
    readings(&f, 100, 109, 100);        // cleared at 101
    f.append(stale(112 * 60, "source-unreachable"));        // the source goes quiet: no readings 110 to 150
    f.append(stale(130 * 60, "credentials-locked"));
    f.append(stale(140 * 60, "sensor-warm-up"));
    f.append(stale(146 * 60, "no-new-readings"));
    readings(&f, 151, 169, 100);
    readings(&f, 170, 199, 255);        // HH at 170, a refused snooze, an ack, a re-alert
    { Feed e(170.5 * 60, Feed::Snooze); e.slot = AlertSlot::High; e.minutes = 15; f.append(e); }
    { Feed e(172 * 60, Feed::Ack); e.slot = AlertSlot::High; f.append(e); }
    readings(&f, 200, 229, 100);
    readings(&f, 230, 270, 66);         // L at 230, snoozed 30 min at 231.5, back at 261.5
    { Feed e(231.5 * 60, Feed::Snooze); e.slot = AlertSlot::Low; e.minutes = 30; f.append(e); }
    readings(&f, 271, 300, 80);         // cleared at 272
    { Feed e(275 * 60, Feed::Thresholds, 85); e.v2 = 180; f.append(e); }   // L raised to 85: 80 is low again
    readings(&f, 301, 359, 100);
    std::stable_sort(f.begin(), f.end(), [](const Feed &a, const Feed &b) { return a.t < b.t; });
    return f;
}

const double kHorizonS = 360 * 60.0;

void apply(AlertRig *rig, const Feed &e)
{
    switch (e.kind) {
    case Feed::Reading: rig->reading(e.t, e.v); break;
    case Feed::Ack: rig->acknowledge(e.t, e.slot); break;
    case Feed::Snooze: rig->snooze(e.t, e.slot, e.minutes); break;
    case Feed::Stale: rig->staleness(e.t, e.cause); break;
    case Feed::Silent: rig->setSilent(e.t, e.minutes); break;
    case Feed::Thresholds: {
        SubjectAlertConfig c = rig->config();
        c.thresholds.l = int(e.v);
        c.thresholds.h = int(e.v2);
        rig->configure(e.t, c);
        break;
    }
    }
}

// Runs the story with a tick every `rhythm` seconds; with `exact` also at the engine's wake-ups.
QStringList runStory(double rhythm, bool exact)
{
    AlertRig rig;
    const QVector<Feed> feed = story();
    int next = 0;
    double nextTick = rhythm;
    double last = 0;
    for (;;) {
        double t = nextTick;
        if (next < feed.size())
            t = qMin(t, feed.at(next).t);
        if (exact) {
            const UtcTime w = rig.engine().nextWakeup();
            if (w.isValid()) {
                double ws = double(w.msecsSinceEpoch() - kAlertTestStartMs) / 1000.0;
                if (ws <= last) ws = last + 0.001;
                t = qMin(t, ws);
            }
        }
        if (t > kHorizonS)
            break;
        last = t;
        while (next < feed.size() && feed.at(next).t <= t)
            apply(&rig, feed.at(next++));
        rig.tick(t);
        if (t >= nextTick)
            nextTick += rhythm;
    }
    rig.tick(kHorizonS);
    return rig.take();
}

QStringList without(const QStringList &lines, QChar kind)
{
    QStringList out;
    for (int i = 0; i < lines.size(); ++i)
        if (lines.at(i).at(0) != kind)
            out.append(lines.at(i));
    return out;
}

// "R 6720.001 missing MISSING #3" without the count: where on the schedule the reminder was.
QString reminderKey(const QString &line)
{
    return line.left(line.lastIndexOf(QLatin1String(" #")));
}

} // namespace

Q_DECLARE_METATYPE(QStringList)

class TstAlertRhythm : public QObject
{
    Q_OBJECT

private slots:
    void theStoryExercisesEveryKindOfDecision()
    {
        const QStringList base = runStory(1, true);
        QVERIFY2(base.size() > 40, qPrintable(QString::number(base.size())));
        const QString all = base.join('\n');
        const char *needles[] = {
            "high H pending pending",                   // persistence
            "high H active active",
            "high H cleared cleared reason=recovered",  // hysteresis
            "low L active active",
            "low LL active escalated",                  // escalation
            "R ",                                       // reminders
            "low LL acknowledged acknowledged",
            "low LL active re-alerted",                 // the re-alert of an acknowledged LL
            " silent",                                  // silent mode: the Update, the reminders
            "missing MISSING pending pending",          // missing data
            "missing MISSING active active reason=source-unreachable",
            "U ",                                       // a cause that follows
            "missing MISSING cleared cleared reason=warm-up",
            "high HH active active",
            "A 10230 snz high not-snoozable",           // a refused snooze
            "high HH active re-alerted",
            "low L snoozed snoozed",
            "low L active snooze-ended",
            "low L active active"                       // after the threshold change
        };
        for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); ++i)
            QVERIFY2(all.contains(QLatin1String(needles[i])), needles[i]);
    }

    void exactWakeUpsGiveTheSameEffectsAtEveryRhythm_data()
    {
        QTest::addColumn<double>("rhythm");
        const double r[] = { 1, 2, 5, 10, 15, 30, 60, 90, 120, 180, 300 };
        for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); ++i)
            QTest::newRow(qPrintable(QString("%1 s").arg(r[i]))) << r[i];
    }
    void exactWakeUpsGiveTheSameEffectsAtEveryRhythm()
    {
        QFETCH(double, rhythm);
        static const QStringList base = runStory(1, true);
        QCOMPARE(runStory(rhythm, true), base);
    }

    void aCoarseTickDecidesTheSameThingsOnlyLater_data()
    {
        QTest::addColumn<double>("rhythm");
        const double r[] = { 2, 5, 10, 15, 30, 60, 90, 120, 180, 300 };
        for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); ++i)
            QTest::newRow(qPrintable(QString("%1 s").arg(r[i]))) << r[i];
    }
    void aCoarseTickDecidesTheSameThingsOnlyLater()
    {
        QFETCH(double, rhythm);
        static const QStringList base = runStory(1, true);
        const QStringList coarse = runStory(rhythm, false);

        // Everything but reminders: identical, in order, with the same logical times.
        QCOMPARE(without(coarse, 'R'), without(base, 'R'));

        // Reminders: never more than the exact run raised, and each on its schedule.
        QSet<QString> schedule;
        for (int i = 0; i < base.size(); ++i)
            if (base.at(i).at(0) == 'R')
                schedule.insert(reminderKey(base.at(i)));
        int coarseReminders = 0, baseReminders = 0;
        for (int i = 0; i < coarse.size(); ++i)
            if (coarse.at(i).at(0) == 'R') {
                ++coarseReminders;
                QVERIFY2(schedule.contains(reminderKey(coarse.at(i))), qPrintable(coarse.at(i)));
            }
        for (int i = 0; i < base.size(); ++i)
            baseReminders += base.at(i).at(0) == 'R';
        QVERIFY(coarseReminders <= baseReminders);
        if (rhythm <= 30)                       // finer than the shortest reminder interval (120 s): none lost
            QCOMPARE(coarseReminders, baseReminders);
    }

    void aReminderDueManyTimesInOneCoarseStepIsRaisedOnce()
    {
        // LL is active from second 120 with a reminder every 2 minutes. One tick at 700 s finds
        // the reminders of 240, 360, 480 and 600 all due: one is raised, the latest.
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 50\n");
        rig.take();
        rig.tick(700);
        const QStringList got = rig.take("R");
        QCOMPARE(got.size(), 1);
        QVERIFY2(got.first().startsWith("R 600 low LL"), qPrintable(got.first()));
        // and the schedule goes on from there
        rig.tick(721);
        QCOMPARE(rig.take("R"), QStringList() << QString("R 720 low LL #2"));
    }
};

QTEST_APPLESS_MAIN(TstAlertRhythm)
#include "tst_alertrhythm.moc"
