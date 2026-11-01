// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The ledger of silent mode and
// its journal kind `silent`. What the mode does to the alerts is tested in tst_alertengine (the
// "silent:" rows) and in tst_alertcoordinator; this program checks the book itself: durations, the
// boundary of "until tomorrow", the end by wake-up, the fold at start and the journal lines.
// Every value is synthetic.
#include "alerttest.h"

#include "alerts/alertjournal.h"
#include "alerts/silentbook.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

// Writes a journal line straight into the database file, past the checks of Journal::append:
// the way a line the reader cannot use can still reach the journal (a damaged file, or a line
// written by another version of the application). The fold must skip it and keep what it can.
bool writeUncheckedLine(const QString &journalPath, const JournalEntry &e)
{
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("unchecked"));
        db.setDatabaseName(journalPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("INSERT INTO journal(id, kind, wall_time, extra) VALUES(?,?,?,?)"));
            q.addBindValue(e.id.toString().mid(1, 36));
            q.addBindValue(e.kind);
            q.addBindValue(qlonglong(e.wallTime.msecsSinceEpoch()));
            q.addBindValue(QString::fromUtf8(QJsonDocument(e.extra).toJson(QJsonDocument::Compact)));
            ok = q.exec();
            if (!ok)
                qWarning("unchecked line: %s", qPrintable(q.lastError().text()));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("unchecked"));
    return ok;
}

QUuid sid(int n = 1) { return alertTestSubject(n); }

IdFactory counterIds(int *n)
{
    return [n]() { return QUuid::createUuidV5(QUuid("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"), QString("silent-%1").arg(++*n)); };
}

UtcTime at(double s) { return alertTime(s); }

// A local time in a zone with a fixed offset, as UTC: "2026-10-18 20:00 at UTC+1".
UtcTime localAt(const QTimeZone &zone, int y, int mo, int d, int h, int mi)
{
    return UtcTime::fromMSecsSinceEpoch(QDateTime(QDate(y, mo, d), QTime(h, mi), zone).toMSecsSinceEpoch());
}

} // namespace

class TstAlertSilent : public QObject
{
    Q_OBJECT

private slots:
    // ---- the book ------------------------------------------------------------------------------
    void aTimedModeIsSilentUntilItsEndAndNotAfter()
    {
        SilentBook b;
        QVERIFY(!b.isSilent(sid(), at(0)));
        b.start(sid(), at(3600), at(0));
        QVERIFY(b.isOn(sid()));
        QVERIFY(b.isSilent(sid(), at(0)));
        QVERIFY(b.isSilent(sid(), at(3599.999)));
        QVERIFY(!b.isSilent(sid(), at(3600)));              // the end instant is no longer silent
        QCOMPARE(b.until(sid()), at(3600));
    }

    void aModeWithoutLimitHasNoEndAndNeverEnds()
    {
        SilentBook b;
        b.start(sid(), UtcTime(), at(0));
        QVERIFY(b.isSilent(sid(), at(1e9)));
        QVERIFY(!b.until(sid()).isValid());
        QVERIFY(!b.nextEnd().isValid());
        QVERIFY(b.expire(at(1e9)).isEmpty());
        QVERIFY(b.isOn(sid()));
    }

    void subjectsAreIndependent()
    {
        SilentBook b;
        b.start(sid(1), at(100), at(0));
        QVERIFY(b.isSilent(sid(1), at(50)));
        QVERIFY(!b.isSilent(sid(2), at(50)));
    }

    void settingTheModeAgainReplacesIt()
    {
        SilentBook b;
        b.start(sid(), at(100), at(0));
        b.start(sid(), at(5000), at(10));
        QCOMPARE(b.until(sid()), at(5000));
        b.start(sid(), UtcTime(), at(20));
        QVERIFY(!b.until(sid()).isValid());
        QVERIFY(b.isSilent(sid(), at(1e8)));
    }

    void theUserCanEndItAndEndingNothingGivesNothing()
    {
        SilentBook b;
        QVERIFY(b.end(sid(), at(0)).event.isEmpty());
        b.start(sid(), UtcTime(), at(0));
        const SilentChange c = b.end(sid(), at(10));
        QCOMPARE(c.event, SilentEvent::end());
        QVERIFY(!c.until.isValid());
        QVERIFY(!b.isSilent(sid(), at(11)));
        QVERIFY(!b.isOn(sid()));
    }

    void aTimedModeEndsByWakeUpWithTheLineStampedWithItsEndTime()
    {
        int n = 0;
        SilentBook b(counterIds(&n));
        b.start(sid(1), at(1000), at(0));
        b.start(sid(2), at(500), at(0));
        b.start(sid(3), UtcTime(), at(0));
        QCOMPARE(b.nextEnd(), at(500));
        QVERIFY(b.expire(at(499)).isEmpty());
        // one coarse tick finds both ends: in time order, each stamped with its own end time
        const QVector<SilentChange> c = b.expire(at(2000));
        QCOMPARE(c.size(), 2);
        QCOMPARE(c.at(0).subject, sid(2));
        QCOMPARE(c.at(0).time, at(500));
        QCOMPARE(c.at(0).event, SilentEvent::end());
        QCOMPARE(c.at(1).subject, sid(1));
        QCOMPARE(c.at(1).time, at(1000));
        QVERIFY(b.expire(at(3000)).isEmpty());              // each end happens once
        QVERIFY(!b.nextEnd().isValid());
        QVERIFY(b.isSilent(sid(3), at(3000)));
    }

    // ---- "until tomorrow" ---------------------------------------------------------------------
    void untilTomorrowIsTheNextLocalSixInTheMorningAtLeastAnHourAway_data()
    {
        QTest::addColumn<int>("offsetSeconds");
        QTest::addColumn<int>("h"); QTest::addColumn<int>("mi");
        QTest::addColumn<int>("endDay"); QTest::addColumn<int>("endHour");
        // local date 2026-10-18; the expected end is a local date and hour
        QTest::newRow("evening: tomorrow 06:00") << 3600 << 20 << 0 << 19 << 6;
        QTest::newRow("late evening") << 3600 << 23 << 30 << 19 << 6;
        QTest::newRow("middle of the night: 06:00 the same day") << 3600 << 2 << 0 << 18 << 6;
        QTest::newRow("just under an hour before 06:00: the next day") << 3600 << 5 << 30 << 19 << 6;
        QTest::newRow("exactly an hour before 06:00: the next day (not more than an hour away)") << 3600 << 5 << 0 << 19 << 6;
        QTest::newRow("just over an hour before") << 3600 << 4 << 59 << 18 << 6;
        QTest::newRow("at 06:00 itself: the next day") << 3600 << 6 << 0 << 19 << 6;
        QTest::newRow("another zone, behind UTC") << -5 * 3600 << 22 << 15 << 19 << 6;
    }
    void untilTomorrowIsTheNextLocalSixInTheMorningAtLeastAnHourAway()
    {
        QFETCH(int, offsetSeconds); QFETCH(int, h); QFETCH(int, mi); QFETCH(int, endDay); QFETCH(int, endHour);
        const QTimeZone zone(offsetSeconds);
        const UtcTime now = localAt(zone, 2026, 10, 18, h, mi);
        const UtcTime end = SilentBook::untilTomorrow(now, zone, 6, 3600 * 1000LL);
        QCOMPARE(end, localAt(zone, 2026, 10, endDay, endHour, 0));
    }

    void untilTomorrowFollowsTheClockAcrossTheChangeOfSummerTime()
    {
        const QTimeZone rome("Europe/Rome");
        if (!rome.isValid())
            QSKIP("no time zone database for Europe/Rome in this build engine");
        // 2026-10-24 22:00 CEST (UTC+2): tomorrow 06:00 is after the clocks go back, UTC+1: 9 hours.
        const UtcTime now = localAt(rome, 2026, 10, 24, 22, 0);
        const UtcTime end = SilentBook::untilTomorrow(now, rome, 6, 3600 * 1000LL);
        QCOMPARE((end.msecsSinceEpoch() - now.msecsSinceEpoch()) / 3600000, qint64(9));
        QCOMPARE(end, localAt(rome, 2026, 10, 25, 6, 0));
    }

    // ---- the fold at start --------------------------------------------------------------------
    void aModeWithoutLimitAndATimedModeInTheFutureComeBack()
    {
        SilentBook was;
        QVector<SilentChange> lines;
        lines << was.start(sid(1), UtcTime(), at(0)) << was.start(sid(2), at(5000), at(10));
        SilentBook b;
        QVERIFY(b.restore(lines, at(1000)).isEmpty());
        QVERIFY(b.isSilent(sid(1), at(1e8)));
        QVERIFY(b.isSilent(sid(2), at(4999)));
        QVERIFY(!b.isSilent(sid(2), at(5000)));
        QCOMPARE(b.until(sid(2)), at(5000));
    }

    void aTimedModeThatRanOutWhileTheServiceWasOffEndsAtStartAndSaysSo()
    {
        SilentBook was;
        const SilentChange start = was.start(sid(), at(1000), at(0));
        SilentBook b;
        const QVector<SilentChange> exp = b.restore(QVector<SilentChange>() << start, at(9000));
        QCOMPARE(exp.size(), 1);
        QCOMPARE(exp.first().event, SilentEvent::expired());
        QCOMPARE(exp.first().time, at(9000));               // the moment it was found
        QCOMPARE(exp.first().until, at(1000));              // the end that had passed
        QVERIFY(!b.isOn(sid()));
        QVERIFY(!b.isSilent(sid(), at(9000)));
    }

    void anEndedOrExpiredModeStaysOff()
    {
        SilentBook was;
        QVector<SilentChange> lines;
        lines << was.start(sid(1), UtcTime(), at(0)) << was.end(sid(1), at(10));
        lines << was.start(sid(2), at(100), at(0));
        SilentChange expired = was.start(sid(3), at(50), at(0));
        expired.event = SilentEvent::expired();
        expired.time = at(60);
        expired.seq = 99;
        lines << expired;
        SilentBook b;
        QVERIFY(b.restore(lines, at(20)).isEmpty());        // sid(2) is still in the future at 20
        QVERIFY(!b.isOn(sid(1)));
        QVERIFY(b.isOn(sid(2)));
        QVERIFY(!b.isOn(sid(3)));
    }

    void theLastLineWinsInAnyOrderAndTheSequenceBreaksTiesOfTheWallTime()
    {
        SilentBook was;
        QVector<SilentChange> lines;
        lines << was.start(sid(), UtcTime(), at(5)) << was.end(sid(), at(5));      // the same millisecond
        QCOMPARE(lines.at(0).time, lines.at(1).time);
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::swap(lines[0].id, lines[1].id);            // random ids must not decide
            QVector<SilentChange> shuffled = lines;
            std::reverse(shuffled.begin(), shuffled.end());
            SilentBook b;
            b.restore(shuffled, at(10));
            QVERIFY(!b.isOn(sid()));                        // start, then end: off
        }
    }

    void theSequenceContinuesAfterARestart()
    {
        SilentBook was;
        const SilentChange a = was.start(sid(), UtcTime(), at(0));
        SilentBook b;
        b.restore(QVector<SilentChange>() << a, at(1));
        QVERIFY(b.end(sid(), at(2)).seq > a.seq);
    }

    // ---- the journal lines ---------------------------------------------------------------------
    void aLineIsWrittenReadBackAndFoldedByAFreshBook()
    {
        QScopedPointer<Journal> j(new Journal);
        QVERIFY(j->open(QStringLiteral(":memory:")));
        SilentBook was;
        QVERIFY(writeSilentChange(j.data(), was.start(sid(1), at(7200), at(0))));
        QVERIFY(writeSilentChange(j.data(), was.start(sid(2), UtcTime(), at(1))));
        QVERIFY(writeSilentChange(j.data(), was.end(sid(2), at(2))));
        const QVector<SilentChange> loaded = loadSilentChanges(j.data());
        QCOMPARE(loaded.size(), 3);
        SilentBook b;
        QVERIFY(b.restore(loaded, at(100)).isEmpty());
        QVERIFY(b.isSilent(sid(1), at(100)));
        QCOMPARE(b.until(sid(1)), at(7200));
        QVERIFY(!b.isOn(sid(2)));
    }

    void aLineHoldsTheSubjectTheEventTheEndAndTheSequenceAndNothingElse()
    {
        SilentBook was;
        const JournalEntry e = toJournalEntry(was.start(sid(), at(60), at(0)));
        QCOMPARE(e.kind, JournalKind::silent());
        QSet<QString> keys = QSet<QString>::fromList(e.extra.keys());
        QCOMPARE(keys, QSet<QString>() << "subject" << "event" << "until" << "seq");
        const JournalEntry open = toJournalEntry(was.start(sid(), UtcTime(), at(1)));
        QVERIFY(!open.extra.contains(SilentKey::until()));
        QVERIFY(e.cause.isNull());
    }

    void theJournalRefusesALineItCouldNotReadBack_data()
    {
        QTest::addColumn<QString>("key"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("unknown event") << "event" << QJsonValue("pause");
        QTest::newRow("subject is no UUID") << "subject" << QJsonValue("nobody");
        QTest::newRow("end is no time") << "until" << QJsonValue("tomorrow");
        QTest::newRow("sequence negative") << "seq" << QJsonValue(-1);
        QTest::newRow("sequence not whole") << "seq" << QJsonValue(1.5);
    }
    void theJournalRefusesALineItCouldNotReadBack()
    {
        QFETCH(QString, key); QFETCH(QJsonValue, value);
        QScopedPointer<Journal> j(new Journal);
        QVERIFY(j->open(QStringLiteral(":memory:")));
        SilentBook was;
        JournalEntry e = toJournalEntry(was.start(sid(), at(60), at(0)));
        QVERIFY(j->append(e));
        e.id = QUuid::createUuid();
        e.extra.insert(key, value);
        QVERIFY2(!j->append(e), qPrintable(key));
    }

    void aDamagedLineIsSkippedAndTheEarlierOneStands()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<Journal> j(new Journal);
        QVERIFY(j->open(dir.path() + "/journal.db"));
        SilentBook was;
        QVERIFY(writeSilentChange(j.data(), was.start(sid(), UtcTime(), at(0))));
        JournalEntry bad = toJournalEntry(was.end(sid(), at(1)));
        bad.extra.insert(SilentKey::event(), QStringLiteral("exploded"));
        QVERIFY(!j->append(bad));                           // append refuses it
        QVERIFY(writeUncheckedLine(j->path(), bad));
        const QVector<SilentChange> loaded = loadSilentChanges(j.data());
        QCOMPARE(loaded.size(), 1);
        SilentBook b;
        b.restore(loaded, at(5));
        QVERIFY(b.isOn(sid()));                             // the end was lost, the mode is kept: the safe side for a visible mode
    }
};

QTEST_APPLESS_MAIN(TstAlertSilent)
#include "tst_alertsilent.moc"
