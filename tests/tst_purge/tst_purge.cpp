// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The purge of one Subject's old data (REQ-STO-17; REQ-STO-15, REQ-STO-16). It is
// irreversible and leaves no record, so these tests protect what decides a deletion: only the
// chosen Subject, only rows older than the cut-off (the boundary itself stays), every kind and
// feed of that Subject, nothing of another Subject (byte for byte), nothing written while the
// purge runs, no purge record anywhere, all or nothing on a failure, and the space check that
// refuses before anything is deleted, with an estimate equal to the result. Synthetic data.
#include "alerts/alertjournal.h"
#include "record/registry.h"
#include "store/journal.h"
#include "store/purge.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;

namespace {

const QUuid kA("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");      // the Subject purged
const QUuid kB("{0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d}");      // another Subject, never touched
const qint64 kMinute = 60000;
const qint64 kDay = 86400000;
const qint64 kNow = 1781525400000LL;                    // 2026-06-15T12:10:00Z
const qint64 kCutoff3 = 1773576600000LL;                // 3 months before: 2026-03-15T12:10:00Z
const qint64 kT0 = 1769904000000LL;                     // 2026-02-01T00:00:00Z, older than the cut-off
const qint64 kPlenty = qint64(1) << 40;

UtcTime at(qint64 ms) { return UtcTime::fromMSecsSinceEpoch(ms); }
QString text(const QUuid &id) { return id.toString().mid(1, 36); }

Record sample(const QUuid &subject, qint64 measuredAt, double value, const QString &source = QStringLiteral("libre.current"),
              const QString &unit = QStringLiteral("mg/dL"))
{
    Record r;
    r.shape = Record::Sample;
    r.subject = subject;
    r.metric = metricGlucose();
    r.value = value;
    r.unit = unit;
    r.measuredAt = at(measuredAt);
    r.ingestedAt = at(qMin(measuredAt + kMinute, kNow));
    r.originalTimestamp = QStringLiteral("synthetic");
    r.source = source;
    r.origin = Origin::Adapter;
    return r;
}

Record logbookEvent(const QUuid &subject, qint64 measuredAt)
{
    Record e = sample(subject, measuredAt, 0, QStringLiteral("libre.logbook"));
    e.shape = Record::Event;
    e.metric = metricLogbookEntry();
    e.unit.clear();
    e.payload.insert(QStringLiteral("entry_type"), 3);
    e.payload.insert(QStringLiteral("raw"), QJsonObject());
    return e;
}

IngestContext context(qint64 now = kNow)
{
    IngestContext c;
    c.now = at(now);
    c.adapter = QStringLiteral("libre");
    c.adapterVersion = QStringLiteral("0.0.1");
    c.sessionId = QStringLiteral("s-1");
    return c;
}

} // namespace

class TstPurge : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_dir;
    Store *m_store;
    Journal *m_journal;

    QString path(const QString &name) const { return m_dir->path() + "/" + name; }

    bool ingest(const QVector<Record> &records, qint64 now = kNow)
    {
        QVector<IngestItem> v;
        foreach (const Record &r, records) {
            IngestItem i;
            i.record = r;
            i.raw.insert(QStringLiteral("marker"), 1);
            v << i;
        }
        const IngestResult res = m_store->ingest(v, context(now));
        return res.ok && res.quarantined == 0;
    }

    // A quarantined raw record of a source reference, received at "when".
    bool quarantine(const QString &subjectRef, qint64 when)
    {
        IngestItem i;
        i.rejected.reason = Validation::UnknownMetric;
        i.rejected.detail = QStringLiteral("metric");
        i.raw.insert(QStringLiteral("subject_ref"), subjectRef);
        i.raw.insert(QStringLiteral("metric"), QStringLiteral("nonsense"));
        QVector<IngestItem> v;
        v << i;
        return m_store->ingest(v, context(when)).ok;
    }

    bool ingestLog(const QString &connection, qint64 when, const QString &detail)
    {
        IngestEntry e;
        e.time = at(when);
        e.event = QStringLiteral("session-end");
        e.adapter = QStringLiteral("libre");
        e.adapterVersion = QStringLiteral("0.0.1");
        e.sessionId = QStringLiteral("s");
        e.detail = detail;
        e.extra.insert(QStringLiteral("connection"), connection);
        return m_store->appendIngestEntry(e);
    }

    bool mark(const Record &target, qint64 when, const QString &comment = QStringLiteral("synthetic"))
    {
        Mark m;
        m.time = at(when);
        m.target = target.key();
        m.comment = comment;
        return m_store->appendMark(m);
    }

    bool sensor(const QUuid &subject, const QString &id, qint64 first, qint64 last)
    {
        Sensor s;
        s.subject = subject;
        s.sensorId = id;
        s.firstSeen = at(first);
        s.lastSeen = at(last);
        return m_store->noteSensor(s);
    }

    // Rows of the store as text, through a raw read-only connection: what "byte-identical" is
    // compared with.
    QStringList rowsOf(const QString &sql)
    {
        QStringList out;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "raw");
            db.setDatabaseName(m_store->path());
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (db.open()) {
                QSqlQuery q(db);
                if (!q.exec(sql))
                    qWarning("%s", qPrintable(q.lastError().text()));
                while (q.next()) {
                    QStringList cells;
                    for (int i = 0; i < q.record().count(); ++i)
                        cells << (q.value(i).isNull() ? QStringLiteral("NULL") : q.value(i).toString());
                    out << cells.join('|');
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("raw");
        return out;
    }

    QStringList rowsOfSubject(const QUuid &s)
    {
        const QString id = text(s);
        return rowsOf(QString("SELECT * FROM record WHERE subject='%1' ORDER BY subject, metric, measured_at, source, entry_id").arg(id))
            + rowsOf(QString("SELECT * FROM mark WHERE t_subject='%1' ORDER BY id").arg(id))
            + rowsOf(QString("SELECT * FROM sensor WHERE subject='%1' ORDER BY sensor_id").arg(id))
            + rowsOf(QString("SELECT * FROM subject WHERE id='%1'").arg(id));
    }

    QStringList everything()
    {
        const char *tables[][2] = { { "meta", "key" }, { "subject", "id" },
                                    { "record", "subject, metric, measured_at, source, entry_id" },
                                    { "mark", "id" }, { "sensor", "subject, sensor_id" }, { "quarantine", "id" },
                                    { "ingest_log", "id" }, { "compaction", "id" }, { "reimport", "id" } };
        QStringList out;
        for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
            out << QString("== %1").arg(tables[i][0])
                << rowsOf(QString("SELECT * FROM %1 ORDER BY %2").arg(tables[i][0], tables[i][1]));
        return out;
    }

    QByteArray fileDigest()
    {
        QCryptographicHash h(QCryptographicHash::Sha256);
        const char *suffixes[] = { "", "-wal" };
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
            QFile f(m_store->path() + suffixes[i]);
            if (f.open(QIODevice::ReadOnly))
                h.addData(f.readAll());
            h.addData("|");
        }
        return h.result();
    }

    static PurgeOptions options(const QUuid &subject = kA, int months = 3, qint64 freeBytes = kPlenty)
    {
        PurgeOptions o;
        o.subject = subject;
        o.months = months;
        o.now = at(kNow);
        o.freeSpace = [freeBytes](const QString &) { return freeBytes; };
        return o;
    }

    // Old and new data of both Subjects in every table: both feeds, events, manual and derived
    // records, marks, sensors, quarantine and ingest-log entries. Returns the keys of A's old records.
    QSet<QString> seed()
    {
        QSet<QString> oldA;
        QVector<Record> all;
        for (int s = 0; s < 2; ++s) {
            const QUuid sub = s ? kB : kA;
            for (int m = 0; m < 30; ++m) {
                all << sample(sub, kT0 + m * kMinute, 100 + m);                         // current, old
                all << sample(sub, kNow - 10 * kDay + m * kMinute, 110 + m);            // current, new
            }
            for (int m = 0; m < 30; m += 5) {
                all << sample(sub, kT0 + m * kMinute + 20000, 101, "libre.graph");     // graph, old
                all << sample(sub, kNow - 10 * kDay + m * kMinute + 20000, 111, "libre.graph");
            }
            all << logbookEvent(sub, kT0 + 40 * kMinute) << logbookEvent(sub, kNow - 9 * kDay);
            Record manual = sample(sub, kT0 + 50 * kMinute, 140, "manual.entry");
            manual.origin = Origin::Manual; manual.entryId = "m1";
            Record derived = sample(sub, kT0 + 51 * kMinute, 141, "derived.rule");
            derived.origin = Origin::Derived; derived.ruleVersion = "r1";
            all << manual << derived;
        }
        foreach (const Record &r, all)
            if (r.subject == kA && r.measuredAt.msecsSinceEpoch() < kCutoff3)
                oldA << r.key().toString();
        if (!ingest(all))
            qWarning("seed: ingest failed");
        mark(sample(kA, kT0 + 3 * kMinute, 0), kT0 + kDay);
        mark(sample(kB, kT0 + 3 * kMinute, 0), kT0 + kDay);
        sensor(kA, "a-old", kT0 - kDay, kT0 + kDay);
        sensor(kA, "a-now", kNow - 12 * kDay, kNow);
        sensor(kB, "b-old", kT0 - kDay, kT0 + kDay);
        quarantine("ref-a", kT0 + kDay);
        quarantine("ref-a", kNow - kDay);
        quarantine("ref-b", kT0 + kDay);
        ingestLog("conn-a", kT0 + kDay, "old a");
        ingestLog("conn-a", kNow - kDay, "new a");
        ingestLog("conn-b", kT0 + kDay, "old b");
        // A compaction record and a reimport record of schema 1: records of the store, not of a
        // Subject; the purge never removes them.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "seed");
            db.setDatabaseName(m_store->path());
            if (db.open()) {
                QSqlQuery q(db);
                if (!q.exec("INSERT INTO compaction(id, time, range_from, range_to, removed, rule_version)"
                            " VALUES('c1', 1769900000000, 1769800000000, 1769850000000, '{\"record\":3}', '2')")
                    || !q.exec("INSERT INTO reimport VALUES('r1', 1769900000000, 'x', 'y', '1.0', '{}')"))
                    qWarning("seed: %s", qPrintable(q.lastError().text()));
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("seed");
        return oldA;
    }

    // ---- the journal ----

    bool alert(const QUuid &subject, qint64 when, int seq)
    {
        AlertRecord r;
        r.id = QUuid::createUuid();
        r.subject = subject;
        r.episode = QUuid::createUuid();
        r.slot = AlertSlot::Low;
        r.type = AlertType::L;
        r.state = AlertState::Pending;
        r.from = AlertState::Normal;
        r.event = QStringLiteral("pending");
        r.time = at(when);
        r.since = at(when);
        r.seq = seq;
        return m_journal->append(toJournalEntry(r));
    }

    bool threshold(const QUuid &subject, qint64 when)
    {
        ThresholdChange c;
        c.id = QUuid::createUuid();
        c.subject = subject;
        c.boundary = Boundary::L;
        c.value = 75;
        c.previous = 70;
        c.time = at(when);
        return writeThresholdChanges(m_journal, QVector<ThresholdChange>() << c);
    }

    bool silent(const QUuid &subject, qint64 when)
    {
        SilentChange c;
        c.id = QUuid::createUuid();
        c.subject = subject;
        c.event = QStringLiteral("start");
        c.time = at(when);
        return writeSilentChange(m_journal, c);
    }

    bool service(const QString &kind, qint64 when)
    {
        JournalEntry e;
        e.kind = kind;
        e.wallTime = at(when);
        e.bootTimeMs = 1000;
        e.monotonicMs = 200;
        if (kind == JournalKind::start())
            e.cause = StartCause::afterStop();
        return m_journal->append(e);
    }

    // Old and new alert, threshold and silent entries of both Subjects, and service events.
    void seedJournal()
    {
        int seq = 0;
        foreach (const QUuid &s, QList<QUuid>() << kA << kB) {
            if (!alert(s, kT0 + kDay, ++seq) || !alert(s, kNow - kDay, ++seq) || !threshold(s, kT0 + 2 * kDay)
                || !threshold(s, kNow - 2 * kDay) || !silent(s, kT0 + 3 * kDay) || !silent(s, kNow - 3 * kDay))
                qWarning("seedJournal: %s", qPrintable(m_journal->lastError()));
        }
        if (!service(JournalKind::start(), kT0) || !service(JournalKind::heartbeat(), kT0 + kMinute)
            || !service(JournalKind::stop(), kT0 + 2 * kMinute))
            qWarning("seedJournal: %s", qPrintable(m_journal->lastError()));
    }

    // The journal's rows as text, read through a raw connection.
    QStringList journalRows(const QString &where = QString())
    {
        QStringList out;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "rawj");
            db.setDatabaseName(m_journal->path());
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (db.open()) {
                QSqlQuery q(db);
                q.exec("SELECT * FROM journal " + where + " ORDER BY id");
                while (q.next()) {
                    QStringList cells;
                    for (int i = 0; i < q.record().count(); ++i)
                        cells << (q.value(i).isNull() ? QStringLiteral("NULL") : q.value(i).toString());
                    out << cells.join('|');
                }
                q.exec("SELECT * FROM meta ORDER BY key");
                while (q.next())
                    out << "meta|" + q.value(0).toString() + "|" + q.value(1).toString();
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("rawj");
        return out;
    }

    bool flagLeft()
    {
        return !rowsOf("SELECT key FROM meta WHERE key IN ('purging', 'compacting')").isEmpty();
    }

private slots:
    void init()
    {
        m_dir = new QTemporaryDir;
        m_store = new Store;
        QVERIFY(m_store->open(path("store.db")));
        m_journal = new Journal;
        QVERIFY(m_journal->open(path("journal.db")));
    }
    void cleanup()
    {
        delete m_store;
        delete m_journal;
        delete m_dir;
    }

    // ---- the age ------------------------------------------------------------------------------

    // REQ-STO-17: 3, 6, 12 or 24 calendar months, nothing else; never inside the 14-day floor.
    void theAgeIsOneOfFourAndCountsCalendarMonths()
    {
        QCOMPARE(allowedPurgeAges(), QList<int>() << 3 << 6 << 12 << 24);
        QCOMPARE(purgeCutoff(3, at(kNow)).msecsSinceEpoch(), kCutoff3);
        QCOMPARE(purgeCutoff(6, at(kNow)).toIso(), QString("2025-12-15T12:10:00.000Z"));
        QCOMPARE(purgeCutoff(12, at(kNow)).toIso(), QString("2025-06-15T12:10:00.000Z"));
        QCOMPARE(purgeCutoff(24, at(kNow)).toIso(), QString("2024-06-15T12:10:00.000Z"));
        QCOMPARE(purgeCutoff(3, UtcTime::fromIso("2026-05-31T08:00:00.000Z")).toIso(), QString("2026-02-28T08:00:00.000Z"));
        const int bad[] = { 0, 1, 2, 4, 5, 9, 18, 36, -3 };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
            QVERIFY2(!purgeCutoff(bad[i], at(kNow)).isValid(), qPrintable(QString::number(bad[i])));
        QVERIFY(!purgeCutoff(3, UtcTime()).isValid());
        foreach (int m, allowedPurgeAges())
            QVERIFY(purgeCutoff(m, at(kNow)).msecsSinceEpoch() <= kNow - 14 * kDay);
    }

    void aBadAgeOrNoSubjectIsRefusedAndNothingChanges()
    {
        seed();
        const QStringList before = everything();
        PurgeReport r = purgeSubject(*m_store, *m_journal, options(kA, 1));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, PurgeError::ageNotAllowed());
        r = purgeSubject(*m_store, *m_journal, options(QUuid()));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, PurgeError::noSubject());
        PurgeEstimate e;
        QString error;
        QVERIFY(!estimatePurge(*m_store, *m_journal, options(kA, 7), &e, &error));
        QCOMPARE(everything(), before);
    }

    // ---- what goes and what stays ----------------------------------------------------------------

    // Everything of A older than the cut-off goes, in every table the purge covers; A's newer rows,
    // A's own row, every row of B (byte for byte), the compaction records stay; nothing is added.
    void everythingOldOfTheSubjectGoesAndNothingElse()
    {
        const QSet<QString> oldA = seed();
        QVERIFY(oldA.size() > 30);
        const QStringList rowsB = rowsOfSubject(kB);
        const QString newSql = QString("SELECT * FROM record WHERE subject='%1' AND measured_at >= %2 ORDER BY 1, 2, 3, 4, 5")
                                   .arg(text(kA)).arg(kCutoff3);
        const QStringList newA = rowsOf(newSql);
        const QStringList compactionBefore = rowsOf("SELECT * FROM compaction") + rowsOf("SELECT * FROM reimport");
        const QStringList meta = rowsOf("SELECT * FROM meta ORDER BY key");

        PurgeOptions o = options();
        o.subjectRefs << "ref-a";
        o.connections << "conn-a";
        const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
        QVERIFY2(r.ok, qPrintable(r.failure.errorClass + ": " + r.failure.message));

        QCOMPARE(r.removed.value("record"), qint64(oldA.size()));           // current, graph, event, manual, derived
        QCOMPARE(r.removed.value("mark"), qint64(1));
        QCOMPARE(r.removed.value("sensor"), qint64(1));
        QCOMPARE(r.removed.value("quarantine"), qint64(1));
        QCOMPARE(r.removed.value("ingest"), qint64(1));
        QCOMPARE(rowsOf(QString("SELECT COUNT(*) FROM record WHERE subject='%1' AND measured_at < %2").arg(text(kA)).arg(kCutoff3)),
                 QStringList() << "0");
        QCOMPARE(rowsOf(newSql), newA);
        QCOMPARE(m_store->subjects().size(), 2);                            // A's own row stays
        QCOMPARE(rowsOf(QString("SELECT sensor_id FROM sensor WHERE subject='%1'").arg(text(kA))), QStringList() << "a-now");
        QCOMPARE(rowsOf("SELECT COUNT(*) FROM quarantine").first(), QString("2"));
        QCOMPARE(rowsOf("SELECT detail FROM ingest_log ORDER BY detail"), QStringList() << "new a" << "old b");
        QCOMPARE(rowsOfSubject(kB), rowsB);
        QCOMPARE(rowsOf("SELECT * FROM compaction") + rowsOf("SELECT * FROM reimport"), compactionBefore);
        QCOMPARE(rowsOf("SELECT * FROM meta ORDER BY key"), meta);          // no purge record, no flag
        QVERIFY(!flagLeft());
    }

    // Without source references and connections from the caller, no quarantine or ingest-log
    // entry is touched: the store cannot tell whose they are.
    void quarantineAndTheIngestLogNeedTheCallersReferences()
    {
        seed();
        const QStringList q = rowsOf("SELECT * FROM quarantine ORDER BY id");
        const QStringList l = rowsOf("SELECT * FROM ingest_log ORDER BY id");
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(r.removed.value("record") > 0);
        QCOMPARE(r.removed.value("quarantine"), qint64(0));
        QCOMPARE(r.removed.value("ingest"), qint64(0));
        QCOMPARE(rowsOf("SELECT * FROM quarantine ORDER BY id"), q);
        QCOMPARE(rowsOf("SELECT * FROM ingest_log ORDER BY id"), l);
    }

    // The boundary: a row at exactly the cut-off stays, one a millisecond before it goes; a mark
    // goes only when it and its record are both older; a sensor only when last seen before it.
    void theCutoffItselfIsKept()
    {
        QVector<Record> v;
        const Record before = sample(kA, kCutoff3 - 1, 100);
        const Record exactly = sample(kA, kCutoff3, 101);
        const Record graphBefore = sample(kA, kCutoff3 - 1, 102, "libre.graph");
        const Record oldTarget = sample(kA, kT0, 103);
        v << before << exactly << graphBefore << oldTarget;
        QVERIFY(ingest(v));
        QVERIFY(mark(oldTarget, kT0 + kMinute, "old mark on an old record"));
        QVERIFY(mark(oldTarget, kCutoff3, "a mark made at the cut-off"));
        QVERIFY(mark(exactly, kT0 + kMinute, "old mark on a record at the cut-off"));
        QVERIFY(sensor(kA, "before", kT0, kCutoff3 - 1));
        QVERIFY(sensor(kA, "exactly", kT0, kCutoff3));

        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QSet<QString> left;
        m_store->forEachRecord([&](const Record &x) { left << x.key().toString(); });
        QCOMPARE(left, QSet<QString>() << exactly.key().toString());
        QCOMPARE(r.removed.value("mark"), qint64(1));
        QStringList comments;
        foreach (const Mark &m, m_store->marks()) comments << m.comment;
        comments.sort();
        QCOMPARE(comments, QStringList() << "a mark made at the cut-off" << "old mark on a record at the cut-off");
        QCOMPARE(rowsOf("SELECT sensor_id FROM sensor"), QStringList() << "exactly");
    }

    // Each age moves the cut-off: a longer age removes less.
    void aLongerAgeRemovesLess()
    {
        QVector<Record> v;
        v << sample(kA, kNow - 100 * kDay, 100) << sample(kA, kNow - 200 * kDay, 101) << sample(kA, kNow - 400 * kDay, 102)
          << sample(kA, kNow - 800 * kDay, 103);
        QVERIFY(ingest(v));
        QList<qint64> counts;
        foreach (int m, allowedPurgeAges()) {
            PurgeEstimate e;
            QString error;
            QVERIFY2(estimatePurge(*m_store, *m_journal, options(kA, m), &e, &error), qPrintable(error));
            counts << e.records;
        }
        QCOMPARE(counts, QList<qint64>() << 4 << 3 << 2 << 1);
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options(kA, 12));
        QVERIFY(r.ok);
        QCOMPARE(m_store->recordCount(), qint64(2));
    }

    // in the journal the Subject's alert, threshold and silent entries before the cut-off
    // go; its newer entries, every entry of B and every service event stay byte for byte; no
    // record of the purge is written there either.
    void theSubjectsJournalEntriesGoAndNothingElse()
    {
        seed();
        seedJournal();
        const QString a = QString("'%\"subject\":\"%1\"%'").arg(text(kA));
        const QStringList others = journalRows(QString("WHERE NOT (kind IN ('alert', 'threshold', 'silent') AND extra LIKE %1"
                                                       " AND wall_time < %2)").arg(a).arg(kCutoff3));
        const qint64 total = m_journal->count();
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.journalRemoved.value("alert"), qint64(1));
        QCOMPARE(r.journalRemoved.value("threshold"), qint64(1));
        QCOMPARE(r.journalRemoved.value("silent"), qint64(1));
        QCOMPARE(r.estimate.journal, r.journalRemoved);
        QCOMPARE(m_journal->count(), total - 3);
        QCOMPARE(journalRows(), others);                // includes the journal's meta: no purge record
        QCOMPARE(m_journal->entries(JournalKind::heartbeat()).size(), 1);
        QVERIFY(!r.storePartDone || r.ok);
        // The alert and threshold folds still read what is left.
        QCOMPARE(loadAlertRecords(m_journal).size(), 3);
        QCOMPARE(loadThresholdChanges(m_journal).size(), 3);
        QCOMPARE(loadSilentChanges(m_journal).size(), 3);
    }

    // the store part first, then the journal part. A journal part that fails (here the
    // journal is open read-only) leaves the journal unchanged and says that the store part is
    // done; run again with a writable journal the purge removes only the journal entries.
    void aFailedJournalPartCanBeRepeated()
    {
        const QSet<QString> oldA = seed();
        seedJournal();
        const QStringList journalBefore = journalRows();
        delete m_journal;
        m_journal = new Journal;
        QVERIFY(m_journal->openReadOnly(path("journal.db")));
        PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY(!r.ok);
        QVERIFY(r.storePartDone);
        QCOMPARE(r.failure.errorClass, CompactionError::io());
        QCOMPARE(r.removed.value("record"), qint64(oldA.size()));
        QVERIFY(r.journalRemoved.isEmpty());
        QCOMPARE(journalRows(), journalBefore);
        QCOMPARE(m_store->countRecordsOfSubjectBefore(kA, at(kCutoff3)), qint64(0));

        delete m_journal;
        m_journal = new Journal;
        QVERIFY(m_journal->open(path("journal.db")));
        r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(r.storePartDone);
        QVERIFY(r.removed.isEmpty());                    // nothing more in the store
        QCOMPARE(r.journalRemoved.value("alert") + r.journalRemoved.value("threshold") + r.journalRemoved.value("silent"), qint64(3));
        // A third run finds nothing anywhere and changes nothing.
        const QStringList after = journalRows();
        r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY(r.ok);
        QCOMPARE(r.totalRemoved(), qint64(0));
        QCOMPARE(journalRows(), after);
    }

    // A failure in the store part leaves the journal untouched as well.
    void aFailedStorePartLeavesTheJournalUntouched()
    {
        seed();
        seedJournal();
        const QStringList journalBefore = journalRows();
        PurgeOptions o = options();
        o.hook = [](qint64 done) { return done < 5; };
        const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
        QVERIFY(!r.ok);
        QVERIFY(!r.storePartDone);
        QCOMPARE(journalRows(), journalBefore);
    }

    // ---- what arrives while the purge runs ----------------------------------------------------

    // Rows the service writes during the dry run (from the progress callback, before the deleting
    // pass takes the write lock) are not in the snapshot and are never removed, also when old; a
    // sensor of the snapshot that is seen again meanwhile stays.
    void rowsWrittenDuringThePurgeAreNeverRemoved()
    {
        seed();
        QVector<Record> late;
        for (int m = 0; m < 10; ++m)
            late << sample(kA, kT0 + m * kMinute + 30000, 90 + m);
        bool written = false;
        PurgeOptions o = options();
        o.progress = [&](const OperationProgress &p) {
            if (p.phase == OperationProgress::CheckingSpace && !written) {
                written = true;
                if (!ingest(late))
                    qWarning("late batch not written");
                if (!sensor(kA, "a-old", kT0 - kDay, kNow))
                    qWarning("sensor not widened");
            }
            return true;
        };
        const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(written);
        QSet<QString> left;
        m_store->forEachRecord([&](const Record &x) { left << x.key().toString(); });
        foreach (const Record &l, late)
            QVERIFY2(left.contains(l.key().toString()), "a row written during the purge was removed");
        QCOMPARE(r.removed.value("sensor"), qint64(0));
        QCOMPARE(r.estimate.sensors, qint64(1));                            // the snapshot had it old
        QCOMPARE(rowsOf(QString("SELECT COUNT(*) FROM sensor WHERE subject='%1'").arg(text(kA))).first(), QString("2"));
    }

    // ---- all or nothing -------------------------------------------------------------------------

    void aStoppedPurgeLeavesTheStoreAsItWas()
    {
        seed();
        seedJournal();
        const QStringList before = everything() + journalRows();
        const qint64 stops[] = { 0, 1, 20, 40 };
        for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
            PurgeOptions o = options();
            o.subjectRefs << "ref-a";
            o.connections << "conn-a";
            const qint64 stopAt = stops[i];
            o.hook = [stopAt](qint64 done) { return done < stopAt; };
            const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
            QVERIFY(!r.ok);
            QCOMPARE(r.failure.errorClass, CompactionError::aborted());
            QCOMPARE(everything() + journalRows(), before);
            QVERIFY(!flagLeft());
        }
        QVERIFY(purgeSubject(*m_store, *m_journal, options()).ok);
    }

    // A failure late in the deleting pass (the sensor table is made to refuse) rolls back every
    // deletion made before it.
    void aWriteErrorUndoesEveryDeletion()
    {
        seed();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "saboteur");
            db.setDatabaseName(m_store->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TRIGGER sensor_no_delete"));
            QVERIFY(q.exec("CREATE TRIGGER sensor_no_delete BEFORE DELETE ON sensor"
                           " BEGIN SELECT RAISE(ABORT, 'sabotage'); END"));
        }
        QSqlDatabase::removeDatabase("saboteur");
        const QStringList before = everything();
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::io());
        QCOMPARE(everything(), before);
        QVERIFY(!flagLeft());
    }

    // Cancel in each phase (REQ-STO-15 stops a run this way when the reading queue is about to
    // overflow) leaves the store as it was; the phases come in order.
    void progressAndCancel()
    {
        seed();
        const QStringList before = everything();
        const OperationProgress::Phase phases[] = { OperationProgress::CheckingSpace, OperationProgress::Purging,
                                                    OperationProgress::Finalizing };
        for (int p = 0; p < 3; ++p) {
            PurgeOptions o = options();
            const OperationProgress::Phase stopAt = phases[p];
            o.progress = [stopAt](const OperationProgress &x) { return x.phase != stopAt; };
            const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
            QVERIFY(!r.ok);
            QCOMPARE(r.failure.errorClass, CompactionError::cancelled());
            QCOMPARE(everything(), before);
            QVERIFY(!flagLeft());
        }
        PurgeOptions o = options();
        QList<OperationProgress> seen;
        o.progress = [&](const OperationProgress &x) { seen << x; return true; };
        QVERIFY(purgeSubject(*m_store, *m_journal, o).ok);
        QCOMPARE(seen.first().phase, OperationProgress::CheckingSpace);
        QCOMPARE(seen.last().phase, OperationProgress::Done);
        QCOMPARE(seen.last().fraction, 1.0);
        int lastPhase = 0;
        double last = 0;
        QSet<int> got;
        foreach (const OperationProgress &x, seen) {
            QCOMPARE(x.steps, 3);
            QVERIFY(int(x.phase) >= lastPhase && x.fraction >= last);
            lastPhase = int(x.phase);
            last = x.fraction;
            got << int(x.phase);
        }
        QVERIFY(got.contains(int(OperationProgress::Purging)));
        QCOMPARE(phaseKey(OperationProgress::Purging), QString("purging"));
    }

    // ---- space and the estimate --------------------------------------------------------------

    // REQ-STO-16: the estimate states the need the run checks; one byte short is refused with the
    // store byte-identical; exactly the need is enough.
    void theSpaceCheckRefusesBeforeAnythingIsDeleted()
    {
        seed();
        seedJournal();
        PurgeEstimate e;
        QString error;
        QVERIFY2(estimatePurge(*m_store, *m_journal, options(), &e, &error), qPrintable(error));
        QCOMPARE(e.space, CompactionEstimate::SpaceEnough);
        QCOMPARE(e.storeBytes, m_store->sizeBytes());
        QCOMPARE(e.rebuildNeedBytes, rebuildNeedFor(e.storeBytes, e.bytes));
        QCOMPARE(e.journalRebuildNeedBytes, rebuildNeedFor(e.journalFileBytes, e.journalBytesFreed));
        QVERIFY(e.sharedDirectory);
        QCOMPARE(e.neededBytes(), qMax(e.rebuildNeedBytes, e.journalRebuildNeedBytes));   // one file after the other
        const qint64 need = e.neededBytes();

        const QByteArray digest = fileDigest();
        const QStringList before = everything();
        const QStringList journalBefore = journalRows();
        PurgeReport r = purgeSubject(*m_store, *m_journal, options(kA, 3, need - 1));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::insufficientSpace());
        QCOMPARE(r.estimate.neededBytes(), need);
        QCOMPARE(r.estimate.missingBytes(), qint64(1));
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceShort);
        QCOMPARE(fileDigest(), digest);
        QCOMPARE(everything(), before);
        QCOMPARE(journalRows(), journalBefore);

        PurgeEstimate shortOne;
        QVERIFY(estimatePurge(*m_store, *m_journal, options(kA, 3, need - 1), &shortOne, &error));
        QCOMPARE(shortOne.space, CompactionEstimate::SpaceShort);
        QCOMPARE(shortOne.missingBytes(), qint64(1));

        r = purgeSubject(*m_store, *m_journal, options(kA, 3, need));
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceEnough);
    }

    void anUnknownFreeSpaceIsSaidAndTheCallerDecides()
    {
        seed();
        const QByteArray digest = fileDigest();
        PurgeReport r = purgeSubject(*m_store, *m_journal, options(kA, 3, -1));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::freeSpaceUnknown());
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceUnknown);
        QCOMPARE(fileDigest(), digest);
        PurgeOptions o = options(kA, 3, -1);
        o.proceedWhenFreeSpaceUnknown = true;
        r = purgeSubject(*m_store, *m_journal, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(r.totalRemoved() > 0);
    }

    // The estimate changes nothing and equals what the run removes, kind by kind.
    void theEstimateEqualsTheResult()
    {
        seed();
        seedJournal();
        PurgeOptions o = options();
        o.subjectRefs << "ref-a";
        o.connections << "conn-a";
        const QStringList before = everything();
        PurgeEstimate e;
        QString error;
        QVERIFY2(estimatePurge(*m_store, *m_journal, o, &e, &error), qPrintable(error));
        QCOMPARE(everything(), before);
        QCOMPARE(e.subject, kA);
        QCOMPARE(e.months, 3);
        QCOMPARE(e.cutoff.msecsSinceEpoch(), kCutoff3);
        QCOMPARE(e.bytes, estimateBytesOfRows(*m_store, e.storeBytes, e.totalRows() - e.journalRows()));
        QVERIFY(e.journalBytesFreed > 0);
        QCOMPARE(e.totalBytesFreed(), e.bytes + e.journalBytesFreed);
        const PurgeReport r = purgeSubject(*m_store, *m_journal, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.removed.value("record"), e.records);
        QCOMPARE(r.removed.value("mark"), e.marks);
        QCOMPARE(r.removed.value("sensor"), e.sensors);
        QCOMPARE(r.removed.value("quarantine"), e.quarantine);
        QCOMPARE(r.removed.value("ingest"), e.ingest);
        QCOMPARE(r.journalRemoved, e.journal);
        QCOMPARE(e.journalRows(), qint64(3));
        QCOMPARE(r.totalRemoved(), e.totalRows());
        QCOMPARE(r.estimate.totalRows(), e.totalRows());
        QCOMPARE(r.estimate.rebuildNeedBytes, e.rebuildNeedBytes);
        QCOMPARE(r.estimate.journalRebuildNeedBytes, e.journalRebuildNeedBytes);
    }

    // After the deletion the file is rewritten, so the space really goes back.
    void theSpaceIsGivenBack()
    {
        for (int d = 0; d < 10; ++d) {
            QVector<Record> v;
            for (int i = 0; i < 1440; ++i) {
                v << sample(kA, kT0 - 30 * kDay + d * kDay + i * kMinute, 70 + (i * 7) % 110);
                if (i % 4 == 0) v << sample(kB, kT0 - 30 * kDay + d * kDay + i * kMinute, 90);
            }
            QVERIFY(ingest(v));
        }
        const qint64 before = m_store->sizeBytes();
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.removed.value("record"), qint64(14400));
        QCOMPARE(m_store->recordCount(), qint64(3600));
        const qint64 after = m_store->sizeBytes();
        QVERIFY2(after < before / 2, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        QVERIFY(r.bytesFreed > 0);
        QCOMPARE(QFileInfo(m_store->path() + "-wal").size(), qint64(0));
        QVERIFY(r.estimate.rebuildNeedBytes >= after);
    }

    // Nothing old (or a Subject the store does not know): the run succeeds and changes nothing.
    void nothingToPurgeChangesNothing()
    {
        QVector<Record> v;
        v << sample(kA, kNow - kDay, 100);
        QVERIFY(ingest(v));
        QVERIFY(alert(kA, kNow - kDay, 1));                // only newer journal entries
        const QByteArray digest = fileDigest();
        const QStringList journalBefore = journalRows();
        const PurgeReport r = purgeSubject(*m_store, *m_journal, options());
        QVERIFY(r.ok);
        QCOMPARE(r.totalRemoved(), qint64(0));
        QCOMPARE(fileDigest(), digest);
        QCOMPARE(journalRows(), journalBefore);
        const PurgeReport unknown = purgeSubject(*m_store, *m_journal, options(QUuid("{99999999-9999-4999-8999-999999999999}")));
        QVERIFY(unknown.ok);
        QCOMPARE(unknown.totalRemoved(), qint64(0));
    }

    // Outside a purge the database still refuses to delete marks and sensor records, also during
    // a compaction; compaction and reimport records never go.
    void marksAndSensorsGoOnlyInAPurge()
    {
        seed();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "writer");
            db.setDatabaseName(m_store->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(!q.exec("DELETE FROM mark"));
            QVERIFY(!q.exec("DELETE FROM sensor"));
            QVERIFY(db.transaction());
            QVERIFY(q.exec("INSERT OR REPLACE INTO meta VALUES('compacting', '1')"));
            QVERIFY(!q.exec("DELETE FROM mark"));
            QVERIFY(!q.exec("DELETE FROM sensor"));
            db.rollback();
            QVERIFY(db.transaction());
            QVERIFY(q.exec("INSERT OR REPLACE INTO meta VALUES('purging', '1')"));
            QVERIFY(q.exec("DELETE FROM mark"));
            QVERIFY(q.exec("DELETE FROM sensor"));
            QVERIFY(!q.exec("DELETE FROM compaction"));
            QVERIFY(!q.exec("DELETE FROM reimport"));
            db.rollback();
            db.close();
        }
        QSqlDatabase::removeDatabase("writer");
        QCOMPARE(m_store->marks().size(), 2);
        QCOMPARE(m_store->sensors().size(), 3);
    }
};

QTEST_APPLESS_MAIN(TstPurge)
#include "tst_purge.moc"
