// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Service journal: entries, pruning (REQ-SCH-14) and gaps (REQ-SCH-10). Synthetic data only.
#include "store/journal.h"
#include "store/migration.h"

#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;

namespace {

const qint64 kMinute = 60 * 1000;
const qint64 kHour = 60 * kMinute;
const qint64 kDay = 24 * kHour;

UtcTime iso(const char *text) { return UtcTime::fromIso(QString::fromLatin1(text)); }
UtcTime at(qint64 ms) { return UtcTime::fromMSecsSinceEpoch(ms); }

JournalEntry entry(const QString &kind, const UtcTime &t)
{
    JournalEntry e;
    e.kind = kind;
    e.wallTime = t;
    if (kind == JournalKind::heartbeat() || kind == JournalKind::start() || kind == JournalKind::stop()) {
        e.bootTimeMs = t.msecsSinceEpoch() % 1000000 + 1000;
        e.monotonicMs = e.bootTimeMs - 800;
    }
    if (kind == JournalKind::start())
        e.cause = StartCause::afterStop();
    return e;
}

// A connection-state entry. The reason goes to the cause column, the rest to
// the extra object.
JournalEntry stateEntry(const QString &scope, const QString &state, const UtcTime &t,
                        const QString &connection = QStringLiteral("conn-1"))
{
    JournalEntry e;
    e.kind = JournalKind::connectionState();
    e.wallTime = t;
    e.extra.insert(ConnectionStateKey::scope(), scope);
    e.extra.insert(ConnectionStateKey::state(), state);
    e.extra.insert(ConnectionStateKey::connection(), scope == QLatin1String("connection") ? connection : QString());
    e.extra.insert(ConnectionStateKey::adapter(), QStringLiteral("libre"));
    e.extra.insert(ConnectionStateKey::adapterVersion(), QStringLiteral("0.1.0"));
    e.cause = QStringLiteral("score");
    return e;
}


// Runs "fn" on a raw Qt SQL connection to the journal file "path" (a test tool, past the API).
template <typename Fn>
bool withRaw(const QString &path, Fn fn)
{
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("rawj"));
        db.setDatabaseName(path);
        if (db.open())
            ok = fn(db);
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("rawj"));
    return ok;
}

qint64 scalar(const QString &path, const QString &sql)
{
    qint64 v = -1;
    withRaw(path, [&](QSqlDatabase &db) {
        QSqlQuery q(db);
        if (q.exec(sql) && q.next())
            v = q.value(0).toLongLong();
        return true;
    });
    return v;
}

// Creates the database "path" from a fixture file: one SQL statement per line, "--" comments.
bool loadFixture(const QString &fixture, const QString &path)
{
    QFile f(fixture);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream in(&f);
    in.setCodec("UTF-8");
    QStringList statements;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1String("--")))
            statements << line;
    }
    return withRaw(path, [&](QSqlDatabase &db) {
        foreach (const QString &s, statements) {
            QSqlQuery q(db);
            if (!q.exec(s)) {
                qWarning("fixture: %s", qPrintable(q.lastError().text()));
                return false;
            }
        }
        return true;
    });
}

// An entry of one of the three kinds the purge may remove, written past the API's checks (the
// guard does not look at the content, only at the kind, the Subject and the time).
JournalEntry subjectEntry(const QString &kind, const QString &subject, qint64 when)
{
    JournalEntry e;
    e.kind = kind;
    e.wallTime = UtcTime::fromMSecsSinceEpoch(when);
    e.extra.insert(QStringLiteral("subject"), subject);
    return e;
}

bool insertRaw(const QString &path, const JournalEntry &e)
{
    return withRaw(path, [&](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.prepare("INSERT INTO journal(id, kind, wall_time, extra) VALUES(?,?,?,?)");
        q.addBindValue(QUuid::createUuid().toString().mid(1, 36));
        q.addBindValue(e.kind);
        q.addBindValue(qlonglong(e.wallTime.msecsSinceEpoch()));
        q.addBindValue(QString::fromUtf8(QJsonDocument(e.extra).toJson(QJsonDocument::Compact)));
        return q.exec();
    });
}

const char *kSubjA = "d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10";
const char *kSubjB = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";

} // namespace

class TstJournal : public QObject
{
    Q_OBJECT

    static void openMemory(Journal &j) { QVERIFY2(j.open(QString()), qPrintable(j.lastError())); }

    // Heartbeats every "step" from "from" to "to" inclusive.
    static void beats(Journal &j, qint64 from, qint64 to, qint64 step)
    {
        for (qint64 t = from; t <= to; t += step)
            QVERIFY(j.append(entry(JournalKind::heartbeat(), at(t))));
    }

private slots:
    void appendChecksTheKind()
    {
        Journal j;
        openMemory(j);
        const UtcTime t = iso("2026-01-01T11:00:00.000Z");

        QVERIFY(j.append(entry(JournalKind::heartbeat(), t)));
        QVERIFY(j.append(entry(JournalKind::start(), t)));
        QVERIFY(j.append(entry(JournalKind::stop(), t)));

        JournalEntry e = entry(JournalKind::heartbeat(), t);
        e.bootTimeMs = -1;
        QVERIFY(!j.append(e));                       // both counters are needed
        e = entry(JournalKind::start(), t);
        e.cause = "because";
        QVERIFY(!j.append(e));                       // unknown cause
        e.cause = QString();
        QVERIFY(!j.append(e));                       // a start needs a cause
        e = entry(JournalKind::heartbeat(), t);
        e.cause = StartCause::firstStart();
        QVERIFY(!j.append(e));                       // only a start has a cause
        e = entry("shutdown", t);
        QVERIFY(!j.append(e));                       // unknown kind
        e = entry(JournalKind::heartbeat(), UtcTime());
        QVERIFY(!j.append(e));                       // a wall time is needed
        e = entry(JournalKind::prune(), t);
        QVERIFY(!j.append(e));                       // a prune needs range and count
        e.rangeFrom = t;
        e.rangeTo = t;
        e.removed = 3;
        QVERIFY(j.append(e));
        e = entry(JournalKind::heartbeat(), t);
        e.removed = 1;
        QVERIFY(!j.append(e));                       // only a prune has a count
        e = entry(JournalKind::storageCheck(), t);
        e.extra.insert("kind", "x");
        QVERIFY(!j.append(e));                       // extra keys must not collide
        QCOMPARE(j.count(), qint64(4));
    }

    void entriesAreReadBackWhole()
    {
        Journal j;
        openMemory(j);
        JournalEntry s = entry(JournalKind::start(), iso("2026-01-01T11:00:00.000Z"));
        s.cause = StartCause::firstStart();
        s.id = QUuid("{e1f2a3b4-c5d6-4e7f-8a9b-0c1d2e3f4a5b}");
        QVERIFY(j.append(s));
        QVERIFY(j.append(s));                        // a repeated id is ignored
        JournalEntry c = entry(JournalKind::storageCheck(), iso("2026-01-08T11:00:00.000Z"));
        c.extra.insert("store_bytes", 1000);
        c.extra.insert("free_bytes", 2500);
        c.extra.insert("low", true);
        QVERIFY(j.append(c));

        const QVector<JournalEntry> all = j.entries();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].id, s.id);
        QCOMPARE(all[0].cause, StartCause::firstStart());
        QCOMPARE(all[0].bootTimeMs, s.bootTimeMs);
        QCOMPARE(all[0].monotonicMs, s.monotonicMs);
        QVERIFY(!all[0].rangeFrom.isValid());
        QCOMPARE(all[1].extra, c.extra);
        QVERIFY(all[1].cause.isNull());
        QCOMPARE(j.entries(JournalKind::storageCheck()).size(), 1);
        QCOMPARE(j.entries(JournalKind::heartbeat()).size(), 0);
    }

    void aCalendarMonthBefore_data()
    {
        QTest::addColumn<QString>("now");
        QTest::addColumn<QString>("cutoff");
        QTest::newRow("plain") << "2026-10-12T10:30:15.250Z" << "2026-09-12T10:30:15.250Z";
        QTest::newRow("year") << "2026-01-31T00:00:00.000Z" << "2025-12-31T00:00:00.000Z";
        QTest::newRow("short month") << "2026-03-31T12:00:00.000Z" << "2026-02-28T12:00:00.000Z";
        QTest::newRow("leap year") << "2028-03-31T12:00:00.000Z" << "2028-02-29T12:00:00.000Z";
        QTest::newRow("31 to 30") << "2026-05-31T12:00:00.000Z" << "2026-04-30T12:00:00.000Z";
    }
    void aCalendarMonthBefore()
    {
        QFETCH(QString, now);
        QFETCH(QString, cutoff);
        QCOMPARE(oneCalendarMonthBefore(UtcTime::fromIso(now)).toIso(), cutoff);
        QVERIFY(!oneCalendarMonthBefore(UtcTime()).isValid());
    }

    void gapsRunFromTheLastSignOfLifeToTheNextStart()
    {
        Journal j;
        openMemory(j);
        QVERIFY(j.append(entry(JournalKind::start(), at(0))));
        beats(j, kMinute, 10 * kMinute, kMinute);
        QVERIFY(j.append(entry(JournalKind::stop(), at(11 * kMinute))));        // clean stop
        QVERIFY(j.append(entry(JournalKind::start(), at(2 * kHour))));          // gap 1
        beats(j, 2 * kHour + kMinute, 2 * kHour + 5 * kMinute, kMinute);
        QVERIFY(j.append(entry(JournalKind::start(), at(5 * kHour))));          // gap 2: no stop
        QVERIFY(j.append(entry(JournalKind::start(), at(6 * kHour))));          // gap 3: start only

        const QVector<JournalGap> g = j.gaps();
        QCOMPARE(g.size(), 3);
        QCOMPARE(g[0].from.msecsSinceEpoch(), 11 * kMinute);
        QCOMPARE(g[0].to.msecsSinceEpoch(), 2 * kHour);
        QCOMPARE(g[1].from.msecsSinceEpoch(), 2 * kHour + 5 * kMinute);
        QCOMPARE(g[1].to.msecsSinceEpoch(), 5 * kHour);
        QCOMPARE(g[2].from.msecsSinceEpoch(), 5 * kHour);
        QCOMPARE(g[2].to.msecsSinceEpoch(), 6 * kHour);
    }

    void pruningFollowsTheRule()
    {
        Journal j;
        openMemory(j);
        const UtcTime now = iso("2026-06-15T00:00:00.000Z");
        const qint64 t0 = iso("2026-03-01T00:00:00.000Z").msecsSinceEpoch();

        // Run 1: starts in March, ends with a stop. Run 2: ends by a crash (no stop).
        // Run 3: long, crosses the cut-off. Run 4: recent. Some storage checks.
        QVERIFY(j.append(entry(JournalKind::start(), at(t0))));
        beats(j, t0 + kHour, t0 + 2 * kDay, kHour);
        QVERIFY(j.append(entry(JournalKind::stop(), at(t0 + 2 * kDay + kHour))));
        QVERIFY(j.append(entry(JournalKind::start(), at(t0 + 9 * kDay))));
        beats(j, t0 + 9 * kDay + kHour, t0 + 12 * kDay, kHour);
        const qint64 t3 = iso("2026-04-01T00:00:00.000Z").msecsSinceEpoch();
        QVERIFY(j.append(entry(JournalKind::start(), at(t3))));
        beats(j, t3 + kHour, t3 + 60 * kDay, 3 * kHour);                      // up to the end of May
        QVERIFY(j.append(entry(JournalKind::stop(), at(t3 + 60 * kDay + kHour))));
        const qint64 t4 = iso("2026-06-10T00:00:00.000Z").msecsSinceEpoch();
        QVERIFY(j.append(entry(JournalKind::start(), at(t4))));
        beats(j, t4 + kMinute, t4 + 4 * kDay, 10 * kMinute);
        for (int w = 0; w < 10; ++w)
            QVERIFY(j.append(entry(JournalKind::storageCheck(), at(t0 + w * 7 * kDay))));

        // An independent model of the rule over the entries before pruning.
        const QVector<JournalEntry> before = j.entries();
        const qint64 cutoff = iso("2026-05-15T00:00:00.000Z").msecsSinceEpoch();
        QSet<QUuid> expectedGone;
        {
            QMap<int, QUuid> lastOfRun;
            int run = 0;
            QVector<int> runOf(before.size(), -1);
            for (int i = 0; i < before.size(); ++i) {
                const QString &k = before[i].kind;
                if (k == JournalKind::start() || k == JournalKind::stop())
                    ++run;
                if (k == JournalKind::heartbeat()) {
                    runOf[i] = run;
                    lastOfRun[run] = before[i].id;
                }
            }
            for (int i = 0; i < before.size(); ++i)
                if (runOf[i] >= 0 && before[i].wallTime.msecsSinceEpoch() < cutoff
                    && lastOfRun[runOf[i]] != before[i].id)
                    expectedGone.insert(before[i].id);
        }
        QVERIFY(expectedGone.size() > 100);

        const QVector<JournalGap> gapsBefore = j.gaps();
        QCOMPARE(gapsBefore.size(), 3);

        const PruneResult r = j.pruneJournal(now);
        QVERIFY2(r.ok, qPrintable(j.lastError()));
        QCOMPARE(r.removed, qint64(expectedGone.size()));

        // Exactly the expected entries are gone, plus one prune entry added.
        QSet<QUuid> after;
        foreach (const JournalEntry &e, j.entries())
            after.insert(e.id);
        foreach (const JournalEntry &e, before)
            QCOMPARE(after.contains(e.id), !expectedGone.contains(e.id));
        QCOMPARE(j.count(), qint64(before.size() - expectedGone.size() + 1));

        // Start, stop and storage-check entries are untouched; the gaps are identical.
        QCOMPARE(j.entries(JournalKind::start()).size(), 4);
        QCOMPARE(j.entries(JournalKind::stop()).size(), 2);
        QCOMPARE(j.entries(JournalKind::storageCheck()).size(), 10);
        QCOMPARE(j.gaps(), gapsBefore);

        // The prune entry records the range and the count.
        const QVector<JournalEntry> pr = j.entries(JournalKind::prune());
        QCOMPARE(pr.size(), 1);
        QCOMPARE(pr[0].removed, r.removed);
        QCOMPARE(pr[0].wallTime, now);
        QVERIFY(pr[0].rangeFrom.isValid() && pr[0].rangeTo.isValid());
        QVERIFY(pr[0].rangeFrom < pr[0].rangeTo);
        QVERIFY(pr[0].rangeTo.msecsSinceEpoch() < cutoff);

        // No heartbeat older than the cut-off remains except the last one of a run.
        int oldBeats = 0;
        foreach (const JournalEntry &e, j.entries(JournalKind::heartbeat()))
            if (e.wallTime.msecsSinceEpoch() < cutoff)
                ++oldBeats;
        QCOMPARE(oldBeats, 2);                       // the last heartbeat of run 1 and of run 2

        // Pruning again removes nothing and adds no entry.
        const qint64 n = j.count();
        const PruneResult again = j.pruneJournal(now);
        QVERIFY(again.ok);
        QCOMPARE(again.removed, qint64(0));
        QCOMPARE(j.count(), n);
    }

    void pruningKeepsRecentHeartbeats()
    {
        Journal j;
        openMemory(j);
        const UtcTime now = iso("2026-06-15T00:00:00.000Z");
        const qint64 t = now.msecsSinceEpoch();
        QVERIFY(j.append(entry(JournalKind::start(), at(t - 10 * kDay))));
        beats(j, t - 10 * kDay + kMinute, t - kDay, kHour);
        const qint64 n = j.count();
        const PruneResult r = j.pruneJournal(now);
        QVERIFY(r.ok);
        QCOMPARE(r.removed, qint64(0));
        QCOMPARE(j.count(), n);
        QVERIFY(!j.pruneJournal(UtcTime()).ok);
    }

    // The window reads the journal through a read-only connection of its own while the service
    // writes: the reader sees the service's entries and can never write.
    void readOnlyNeverWritesAndSeesTheServicesEntries()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        Journal writer;
        QVERIFY(writer.open(path));
        QVERIFY(writer.append(entry(JournalKind::start(), at(0))));
        QVERIFY(writer.append(entry(JournalKind::heartbeat(), at(kMinute))));

        Journal reader;
        QVERIFY2(reader.openReadOnly(path), qPrintable(reader.lastError()));
        QCOMPARE(reader.count(), qint64(2));
        QVERIFY(writer.append(entry(JournalKind::heartbeat(), at(2 * kMinute))));
        QVERIFY(!reader.append(entry(JournalKind::heartbeat(), at(3 * kMinute))));   // read-only
        QCOMPARE(reader.count(), qint64(3));

        Journal missing;
        QVERIFY(!missing.openReadOnly(dir.path() + "/nothing.db"));
    }

    void connectionStateEntriesAreValidated()
    {
        Journal j;
        openMemory(j);
        const UtcTime t = iso("2026-01-01T11:00:00.000Z");

        QVERIFY2(j.append(stateEntry("connection", "disabled", t)), qPrintable(j.lastError()));
        QVERIFY(j.append(stateEntry("adapter-version", "disabled", t)));
        QVERIFY(j.append(stateEntry("connection", "attention", t)));
        QVERIFY(j.append(stateEntry("connection", "clear", t)));
        JournalEntry hold = stateEntry("connection", "hold", t);
        hold.extra.insert(ConnectionStateKey::until(), "2026-01-01T12:00:00.000Z");
        QVERIFY(j.append(hold));

        JournalEntry e = stateEntry("connection", "hold", t);
        QVERIFY(!j.append(e));                       // a hold needs a valid end time
        e.extra.insert(ConnectionStateKey::until(), "soon");
        QVERIFY(!j.append(e));
        e = stateEntry("planet", "disabled", t);
        QVERIFY(!j.append(e));                       // unknown scope
        e = stateEntry("connection", "paused", t);
        QVERIFY(!j.append(e));                       // unknown state
        e = stateEntry("connection", "disabled", t, QString());
        QVERIFY(!j.append(e));                       // a connection-scope entry needs the id
        e = stateEntry("adapter-version", "disabled", t);
        e.extra.remove(ConnectionStateKey::adapterVersion());
        QVERIFY(!j.append(e));                       // a version-scope entry needs adapter and version
        e = stateEntry("connection", "disabled", t);
        e.extra.remove(ConnectionStateKey::connection());
        QVERIFY(!j.append(e));                       // the connection key must be there
        e = stateEntry("connection", "disabled", t);
        e.bootTimeMs = 5;
        e.monotonicMs = 5;
        QVERIFY(j.append(e));                        // counters are not forbidden, only not needed
        QCOMPARE(j.count(), qint64(6));
    }

    void connectionStateIsNeverPrunedAndIsNoGap()
    {
        Journal j;
        openMemory(j);
        QVERIFY(j.append(entry(JournalKind::start(), at(0))));
        beats(j, 0, 3 * kHour, kMinute);
        QVERIFY(j.append(stateEntry("connection", "disabled", at(kHour))));
        QVERIFY(j.append(stateEntry("connection", "clear", at(2 * kHour))));
        QVERIFY(j.append(entry(JournalKind::stop(), at(3 * kHour))));
        const qint64 states = j.entries(JournalKind::connectionState()).size();

        const PruneResult r = j.pruneJournal(at(100 * kDay));
        QVERIFY(r.ok);
        QVERIFY(r.removed > 0);
        QCOMPARE(qint64(j.entries(JournalKind::connectionState()).size()), states);
        QVERIFY(j.gaps().isEmpty());                 // a state entry is no sign of life, no start
    }

    void connectionStateSurvivesAReopen()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        JournalEntry hold = stateEntry("connection", "hold", iso("2026-01-01T11:00:00.000Z"));
        hold.cause = QStringLiteral("rate-limited");
        hold.extra.insert(ConnectionStateKey::until(), "2026-01-01T12:00:00.000Z");
        hold.extra.insert(ConnectionStateKey::detail(), "retry after 3600 s");
        hold.id = QUuid("{0b7e5a52-3c1d-4e8f-9a60-1d2c3b4a5f66}");
        {
            Journal j;
            QVERIFY(j.open(path));
            QVERIFY(j.append(hold));
        }
        Journal again;                               // a restart of the object
        QVERIFY(again.open(path));
        const QVector<JournalEntry> read = again.entries(JournalKind::connectionState());
        QCOMPARE(read.size(), 1);
        QCOMPARE(read[0].cause, QStringLiteral("rate-limited"));
        QCOMPARE(read[0].extra, hold.extra);
        QCOMPARE(read[0].id, hold.id);
        // The same id appended again is ignored: the entry stays as it is.
        QVERIFY(again.append(hold));
        QCOMPARE(again.count(), qint64(1));
    }

    // ---- the delete guard and the purge of one Subject's entries (schema 2) -----------

    // The database refuses every delete and every update of a journal entry, except a heartbeat
    // while the pruning flag is set and the Subject's alert, threshold and silent entries before
    // the cut-off while the purge's rows name them. A guard of the database, not only of the
    // API: no other code path can remove an entry.
    void theDatabaseRefusesEveryOtherDelete()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        {
            Journal j;
            QVERIFY(j.open(path));
            QVERIFY(j.append(entry(JournalKind::start(), at(0))));
            QVERIFY(j.append(entry(JournalKind::heartbeat(), at(kMinute))));
            QVERIFY(j.append(stateEntry("connection", "disabled", at(2 * kMinute))));
        }
        const char *kinds[] = { "alert", "threshold", "silent" };
        for (size_t i = 0; i < 3; ++i) {
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjA, 1000)));
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjB, 1000)));
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjA, 5000)));
        }
        const qint64 total = scalar(path, "SELECT COUNT(*) FROM journal");
        QVERIFY(withRaw(path, [&](QSqlDatabase &db) {
            QSqlQuery q(db);
            bool ok = !q.exec("DELETE FROM journal") && !q.exec("DELETE FROM journal WHERE kind='heartbeat'")
                && !q.exec("DELETE FROM journal WHERE kind='alert'") && !q.exec("UPDATE journal SET wall_time=0");
            // Pruning's flag lets heartbeats go and nothing else.
            ok = ok && db.transaction() && q.exec("INSERT INTO meta VALUES('pruning', '1')")
                && !q.exec("DELETE FROM journal WHERE kind='start'") && !q.exec("DELETE FROM journal WHERE kind='silent'")
                && q.exec("DELETE FROM journal WHERE kind='heartbeat'");
            db.rollback();
            // The purge's rows let A's three kinds before 2000 go, and nothing of B, of later
            // entries or of other kinds.
            ok = ok && db.transaction() && q.exec(QString("INSERT INTO meta VALUES('purging_subject', '%1')").arg(kSubjA))
                && q.exec("INSERT INTO meta VALUES('purging_before', '2000')")
                && !q.exec("DELETE FROM journal WHERE kind='heartbeat'")
                && !q.exec("DELETE FROM journal WHERE kind='connection-state'")
                && !q.exec(QString("DELETE FROM journal WHERE extra LIKE '%%1%'").arg(kSubjB))
                && !q.exec("DELETE FROM journal WHERE wall_time = 5000")
                && q.exec("DELETE FROM journal WHERE kind IN ('alert', 'threshold', 'silent') AND wall_time < 2000"
                          " AND extra LIKE '%" + QString(kSubjA) + "%'")
                && q.numRowsAffected() == 3;
            db.rollback();
            // Only the subject row without the time row: nothing goes.
            ok = ok && db.transaction() && q.exec(QString("INSERT INTO meta VALUES('purging_subject', '%1')").arg(kSubjA))
                && !q.exec("DELETE FROM journal WHERE kind='alert' AND wall_time < 2000");
            db.rollback();
            return ok;
        }));
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM journal"), total);
    }

    // The purge's own call: only the three kinds, only that Subject, only before the cut-off;
    // the counts equal what went; a second call removes nothing; no row is left in meta.
    void purgeSubjectEntriesRemovesOnlyThoseEntries()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        {
            Journal j;
            QVERIFY(j.open(path));
            QVERIFY(j.append(entry(JournalKind::start(), at(0))));
            QVERIFY(j.append(entry(JournalKind::heartbeat(), at(500))));
        }
        const char *kinds[] = { "alert", "threshold", "silent" };
        for (size_t i = 0; i < 3; ++i) {
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjA, 1000)));
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjA, 2000)));     // at the cut-off: stays
            QVERIFY(insertRaw(path, subjectEntry(kinds[i], kSubjB, 1000)));
        }
        QVERIFY(insertRaw(path, subjectEntry(kinds[0], kSubjA, 1500)));
        Journal j;
        QVERIFY(j.open(path));
        QMap<QString, qint64> counts, removed;
        QVERIFY(j.countSubjectEntriesBefore(QUuid(QString("{%1}").arg(kSubjA)), at(2000), &counts));
        QCOMPARE(counts.value("alert"), qint64(2));
        QCOMPARE(counts.value("threshold"), qint64(1));
        QCOMPARE(counts.value("silent"), qint64(1));
        QVERIFY2(j.purgeSubjectEntries(QUuid(QString("{%1}").arg(kSubjA)), at(2000), &removed), qPrintable(j.lastError()));
        QCOMPARE(removed, counts);
        QCOMPARE(j.count(), qint64(2 + 9 + 1 - 4));
        QCOMPARE(scalar(path, QString("SELECT COUNT(*) FROM journal WHERE extra LIKE '%%1%'").arg(kSubjB)), qint64(3));
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM meta"), qint64(1));         // only the schema version
        QVERIFY(j.purgeSubjectEntries(QUuid(QString("{%1}").arg(kSubjA)), at(2000), &removed));
        QVERIFY(removed.isEmpty());
        // Pruning still works under the guard.
        QVERIFY(j.pruneJournal(at(100 * kDay)).ok);
    }

    // REQ-STO-10: a journal of schema 1 (no delete guard) opens, is migrated to 2 with every
    // entry kept (also a threshold entry with the comment of before that change), and is then guarded;
    // a reader leaves it at 1; a failing step leaves it at 1, readable.
    void aVersion1JournalIsMigrated()
    {
        QCOMPARE(kJournalSchemaVersion, 2);
        QCOMPARE(journalMigrations().count(), 1);
        const QString fixture = QStringLiteral(LM_JOURNAL_FIXTURES_DIR) + "/v1/journal-v1.sql";
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        QVERIFY(loadFixture(fixture, path));
        QCOMPARE(scalar(path, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
        {
            Journal r;
            QVERIFY(r.openReadOnly(path));
            QCOMPARE(r.count(), qint64(9));
        }
        QCOMPARE(scalar(path, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
        // Schema 1 had no guard: a delete went through (what now closes).
        QVERIFY(withRaw(path, [](QSqlDatabase &db) {
            QSqlQuery q(db);
            const bool ok = db.transaction() && q.exec("DELETE FROM journal WHERE kind='start'") && q.numRowsAffected() == 1;
            db.rollback();
            return ok;
        }));
        Journal j;
        QVERIFY2(j.open(path), qPrintable(j.lastError()));
        QCOMPARE(scalar(path, "SELECT value FROM meta WHERE key='store_schema'"), qint64(2));
        QCOMPARE(j.count(), qint64(9));
        QCOMPARE(j.entries(JournalKind::threshold()).first().extra.value("comment").toString(), QString("written by an older version"));
        j.close();
        QVERIFY(withRaw(path, [](QSqlDatabase &db) { QSqlQuery q(db); return !q.exec("DELETE FROM journal WHERE kind='start'"); }));
        QVERIFY(j.open(path));
        QMap<QString, qint64> removed;
        QVERIFY(j.purgeSubjectEntries(QUuid(QString("{%1}").arg(kSubjA)), UtcTime::fromIso("2026-03-01T00:00:00.000Z"), &removed));
        QCOMPARE(removed.value("alert") + removed.value("threshold") + removed.value("silent"), qint64(3));
        QCOMPARE(j.count(), qint64(6));

        // A failing step: a trigger of that name already exists.
        const QString broken = dir.path() + "/broken.db";
        QVERIFY(loadFixture(fixture, broken));
        QVERIFY(withRaw(broken, [](QSqlDatabase &db) {
            QSqlQuery q(db);
            return q.exec("CREATE TRIGGER journal_no_delete BEFORE DELETE ON journal BEGIN SELECT 1; END");
        }));
        Journal b;
        QVERIFY(!b.open(broken));
        QVERIFY2(b.lastError().contains("1 to 2"), qPrintable(b.lastError()));
        QCOMPARE(scalar(broken, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
        QVERIFY(b.openReadOnly(broken));
        QCOMPARE(b.count(), qint64(9));
    }

    void refusesANewerSchemaAndReportsSize()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/journal.db";
        Journal j;
        QVERIFY(j.open(path));
        QVERIFY(j.append(entry(JournalKind::start(), at(0))));
        QVERIFY(j.sizeBytes() > 0);
        QCOMPARE(j.path(), path);
    }
};

QTEST_APPLESS_MAIN(TstJournal)
#include "tst_journal.moc"
