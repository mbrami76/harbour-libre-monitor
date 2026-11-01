// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Store: ingest, quarantine, append-only, views, marks, sensors (REQ-DAT-*).
// Synthetic data only (REQ-DOC-03).
#include "record/registry.h"
#include "store/store.h"

#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;

Q_DECLARE_METATYPE(lm::Validation::Reason)

namespace {

const QUuid kSubject("{11111111-2222-3333-4444-555555555555}");
const QUuid kOther("{aaaaaaaa-2222-3333-4444-555555555555}");
const qint64 kNow = 1700000000000LL;

UtcTime at(qint64 ms) { return UtcTime::fromMSecsSinceEpoch(ms); }

Record sample(qint64 measuredAt, double value, const QString &source = QStringLiteral("test.current"))
{
    Record r;
    r.shape = Record::Sample;
    r.subject = kSubject;
    r.metric = metricGlucose();
    r.value = value;
    r.unit = QStringLiteral("mg/dL");
    r.measuredAt = at(measuredAt);
    r.ingestedAt = at(kNow);
    r.originalTimestamp = QStringLiteral("synthetic timestamp");
    r.source = source;
    r.origin = Origin::Adapter;
    return r;
}

Record logbook(qint64 measuredAt)
{
    Record r = sample(measuredAt, 0, QStringLiteral("test.logbook"));
    r.shape = Record::Event;
    r.metric = metricLogbookEntry();
    r.unit.clear();
    r.payload = QJsonObject();
    r.payload.insert(QStringLiteral("entry_type"), 3);
    r.payload.insert(QStringLiteral("raw"), QJsonObject());
    return r;
}

IngestItem item(const Record &r)
{
    IngestItem i;
    i.record = r;
    i.raw.insert(QStringLiteral("marker"), QStringLiteral("raw of ") + r.key().toString());
    return i;
}

IngestContext context()
{
    IngestContext c;
    c.now = at(kNow);
    c.adapter = QStringLiteral("testadapter");
    c.adapterVersion = QStringLiteral("0.0.1");
    c.sessionId = QStringLiteral("s-1");
    return c;
}

IngestResult ingestOne(Store &s, const Record &r)
{
    QVector<IngestItem> v;
    v.append(item(r));
    return s.ingest(v, context());
}

Mark mark(const Record &target, qint64 time, bool ignored, const QString &id)
{
    Mark m;
    m.id = QUuid(id);
    m.time = at(time);
    m.target = target.key();
    m.ignored = ignored;
    m.comment = QStringLiteral("synthetic comment");
    return m;
}

} // namespace

class TstStore : public QObject
{
    Q_OBJECT

    static void openMemory(Store &s) { QVERIFY2(s.open(QString()), qPrintable(s.lastError())); }

private slots:
    void opensAndRecordsSchemaVersion()
    {
        Store s;
        openMemory(s);
        QCOMPARE(s.schemaVersion(), kStoreSchemaVersion);
        QVERIFY(s.isOpen());
        s.close();
        QVERIFY(!s.isOpen());
    }

    void refusesANewerSchema()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        {
            Store s;
            QVERIFY(s.open(path));
        }
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "bump");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE meta SET value='99' WHERE key='store_schema'"));
        }
        QSqlDatabase::removeDatabase("bump");
        Store s;
        QVERIFY(!s.open(path));
        QVERIFY(!s.isOpen());
        QVERIFY(s.lastError().contains("newer"));
    }

    void reopenKeepsTheData()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        {
            Store s;
            QVERIFY(s.open(path));
            QCOMPARE(ingestOne(s, sample(kNow - 60000, 110)).added, 1);
        }
        Store s;
        QVERIFY(s.open(path));
        QCOMPARE(s.recordCount(), qint64(1));
        Record r;
        QVERIFY(s.latest(kSubject, metricGlucose(), QString(), true, &r));
        QCOMPARE(r.value, 110.0);
    }

    void ingestAddsDuplicatesAndConflicts()
    {
        Store s;
        openMemory(s);
        QVector<IngestItem> batch;
        batch << item(sample(kNow - 120000, 100)) << item(sample(kNow - 60000, 105));
        IngestResult r = s.ingest(batch, context());
        QVERIFY(r.ok);
        QCOMPARE(r.added, 2);
        QCOMPARE(s.recordCount(), qint64(2));

        // Same key, same value and unit: a duplicate, nothing written.
        r = ingestOne(s, sample(kNow - 60000, 105));
        QCOMPARE(r.duplicate, 1);
        QCOMPARE(r.added, 0);
        QCOMPARE(s.recordCount(), qint64(2));

        // Same key, different value: first record kept, conflict logged.
        r = ingestOne(s, sample(kNow - 60000, 140));
        QCOMPARE(r.conflict, 1);
        QCOMPARE(r.added, 0);
        Record kept;
        QVERIFY(s.latest(kSubject, metricGlucose(), QString(), false, &kept));
        QCOMPARE(kept.value, 105.0);
        QCOMPARE(s.ingestLog().size(), 1);
        QCOMPARE(s.ingestLog().first().event, QString("conflict"));
        QVERIFY(!s.ingestLog().first().detail.contains("140"));

        // Same key, different unit: conflict too.
        Record u = sample(kNow - 60000, 105);
        u.unit = "mmol/L";
        u.value = 5.8;
        QCOMPARE(ingestOne(s, u).conflict, 1);

        // The feed is part of the source tag: the same time in another feed coexists.
        QCOMPARE(ingestOne(s, sample(kNow - 60000, 111, "test.graph")).added, 1);
        QCOMPARE(s.recordCount(), qint64(3));
    }

    void ingestKeepsEveryField()
    {
        Store s;
        openMemory(s);
        Record r = sample(kNow - 1000, 5.6);
        r.unit = "mmol/L";
        r.ingestedAt = at(kNow - 5);
        r.originalTimestamp = "1/1/2026 11:55:00 AM";
        QCOMPARE(ingestOne(s, r).added, 1);
        Record e = logbook(kNow - 2000);
        e.durationMs = 1500;
        QCOMPARE(ingestOne(s, e).added, 1);
        Record m = sample(kNow - 3000, 90, "manual.entry");
        m.origin = Origin::Manual;
        m.entryId = "entry-1";
        QCOMPARE(ingestOne(s, m).added, 1);
        Record d = sample(kNow - 4000, 91, "derived.rule");
        d.origin = Origin::Derived;
        d.ruleVersion = "r1";
        QCOMPARE(ingestOne(s, d).added, 1);

        RecordQuery q;
        q.effective = false;
        const QVector<Record> all = s.records(q);
        QCOMPARE(all.size(), 4);
        // Ordered by measurement time: derived, manual, event, sample.
        QCOMPARE(all[0].origin, Origin::Derived);
        QCOMPARE(all[0].ruleVersion, QString("r1"));
        QCOMPARE(all[1].entryId, QString("entry-1"));
        QCOMPARE(all[2].shape, Record::Event);
        QCOMPARE(all[2].payload, e.payload);
        QCOMPARE(all[2].durationMs, qint64(1500));
        QVERIFY(all[2].unit.isEmpty());
        QCOMPARE(all[3].value, 5.6);
        QCOMPARE(all[3].unit, QString("mmol/L"));
        QCOMPARE(all[3].ingestedAt.msecsSinceEpoch(), kNow - 5);
        QCOMPARE(all[3].originalTimestamp, QString("1/1/2026 11:55:00 AM"));
        QCOMPARE(all[3].subject, kSubject);
        QCOMPARE(s.subjects().size(), 1);
    }

    void manualRecordsWithTheSameTimeCoexist()
    {
        Store s;
        openMemory(s);
        Record a = sample(kNow - 1000, 90, "manual.entry");
        a.origin = Origin::Manual;
        a.entryId = "one";
        Record b = a;
        b.entryId = "two";
        QCOMPARE(ingestOne(s, a).added, 1);
        QCOMPARE(ingestOne(s, b).added, 1);
        QCOMPARE(ingestOne(s, a).duplicate, 1);
    }

    void quarantineHoldsEveryReasonUnmodified_data()
    {
        QTest::addColumn<Validation::Reason>("reason");
        QTest::addColumn<QString>("what");

        QTest::newRow("MissingSubject") << Validation::MissingSubject << "subject";
        QTest::newRow("UnknownMetric") << Validation::UnknownMetric << "metric";
        QTest::newRow("ShapeMismatch") << Validation::ShapeMismatch << "shape";
        QTest::newRow("MissingUnit") << Validation::MissingUnit << "unit";
        QTest::newRow("UnitNotAccepted") << Validation::UnitNotAccepted << "unit-bad";
        QTest::newRow("ValueNotFinite") << Validation::ValueNotFinite << "nan";
        QTest::newRow("ValueOutOfRange") << Validation::ValueOutOfRange << "range";
        QTest::newRow("InvalidPayload") << Validation::InvalidPayload << "payload";
        QTest::newRow("InvalidDuration") << Validation::InvalidDuration << "duration";
        QTest::newRow("MissingMeasurementTime") << Validation::MissingMeasurementTime << "mtime";
        QTest::newRow("MeasurementTimeInFuture") << Validation::MeasurementTimeInFuture << "future";
        QTest::newRow("MissingIngestTime") << Validation::MissingIngestTime << "itime";
        QTest::newRow("MissingOriginalTimestamp") << Validation::MissingOriginalTimestamp << "orig";
        QTest::newRow("MissingSource") << Validation::MissingSource << "source";
        QTest::newRow("MissingEntryId") << Validation::MissingEntryId << "entry";
        QTest::newRow("MissingRuleVersion") << Validation::MissingRuleVersion << "rule";
        QTest::newRow("UndeclaredMetric") << Validation::UndeclaredMetric << "undeclared-metric";
        QTest::newRow("UndeclaredUnit") << Validation::UndeclaredUnit << "undeclared-unit";
        QTest::newRow("UndeclaredFeed") << Validation::UndeclaredFeed << "undeclared-feed";
        QTest::newRow("MalformedMeasurementTime") << Validation::MalformedMeasurementTime << "malformed-time";
    }
    void quarantineHoldsEveryReasonUnmodified()
    {
        QFETCH(Validation::Reason, reason);
        QFETCH(QString, what);

        Record r = sample(kNow - 60000, 110);
        if (what == "subject") r.subject = QUuid();
        else if (what == "metric") r.metric = "nonsense";
        else if (what == "shape") r.shape = Record::Event;
        else if (what == "unit") r.unit.clear();
        else if (what == "unit-bad") r.unit = "mg/dl";
        else if (what == "nan") r.value = qQNaN();
        else if (what == "range") r.value = 9999;
        else if (what == "payload") { r = logbook(kNow - 60000); r.payload.insert("bogus", 1); }
        else if (what == "duration") { r = logbook(kNow - 60000); r.durationMs = -2; }
        else if (what == "mtime") r.measuredAt = UtcTime();
        else if (what == "future") r.measuredAt = at(kNow + 3600000);
        else if (what == "itime") r.ingestedAt = UtcTime();
        else if (what == "orig") r.originalTimestamp.clear();
        else if (what == "source") r.source.clear();
        else if (what == "entry") r.origin = Origin::Manual;
        else if (what == "rule") r.origin = Origin::Derived;
        // The four reasons come from the session driver, not from validateRecord:
        // the driver hands the record in as a caller rejection.
        const bool fromDriver = what.startsWith("undeclared") || what == "malformed-time";
        if (!fromDriver)
            QCOMPARE(validateRecord(r, at(kNow)).reason, reason);

        Store s;
        openMemory(s);
        IngestItem it = item(r);
        if (fromDriver) {
            it.rejected.reason = reason;
            it.rejected.detail = "from the driver";
        }
        it.raw = QJsonObject();
        it.raw.insert("odd", QJsonObject());     // keys the record model does not know
        it.raw.insert("value", "not a number");
        QVector<IngestItem> v;
        v << it;
        const IngestResult res = s.ingest(v, context());
        QVERIFY(res.ok);
        QCOMPARE(res.quarantined, 1);
        QCOMPARE(res.added, 0);
        QCOMPARE(s.recordCount(), qint64(0));
        const QVector<QuarantineEntry> q = s.quarantine();
        QCOMPARE(q.size(), 1);
        QCOMPARE(q[0].reason, validationReasonName(reason));
        QCOMPARE(q[0].record, it.raw);                       // unmodified
        QCOMPARE(q[0].adapter, QString("testadapter"));
        QCOMPARE(q[0].receivedAt.msecsSinceEpoch(), kNow);
        QVERIFY(!q[0].detail.isEmpty());
        Validation::Reason back;
        QVERIFY(validationReasonFromName(q[0].reason, &back));
        QCOMPARE(back, reason);
    }

    void everyReasonHasAName()
    {
        QSet<QString> names;
        for (int i = int(Validation::MissingSubject); i <= int(Validation::MalformedMeasurementTime); ++i) {
            const QString n = validationReasonName(Validation::Reason(i));
            QVERIFY2(!n.isEmpty(), qPrintable(QString::number(i)));
            names.insert(n);
        }
        QCOMPARE(names.size(), 20);
        QVERIFY(validationReasonName(Validation::Ok).isEmpty());
    }

    void callerRejectionGoesToQuarantineWithoutValidation()
    {
        Store s;
        openMemory(s);
        IngestItem it;
        it.raw.insert("junk", 1);
        it.rejected.reason = Validation::MissingMeasurementTime;
        it.rejected.detail = "measured_at";
        QVector<IngestItem> v;
        v << it;
        QCOMPARE(s.ingest(v, context()).quarantined, 1);
        QCOMPARE(s.quarantine().first().record, it.raw);
    }

    void aBatchMixesAllOutcomes()
    {
        Store s;
        openMemory(s);
        ingestOne(s, sample(kNow - 300000, 100));
        Record bad = sample(kNow - 60000, 9999);
        QVector<IngestItem> v;
        v << item(sample(kNow - 300000, 100))        // duplicate
          << item(sample(kNow - 300000, 101))        // conflict
          << item(sample(kNow - 240000, 102))        // added
          << item(bad);                              // quarantined
        const IngestResult r = s.ingest(v, context());
        QCOMPARE(r.added, 1);
        QCOMPARE(r.duplicate, 1);
        QCOMPARE(r.conflict, 1);
        QCOMPARE(r.quarantined, 1);
        QCOMPARE(s.recordCount(), qint64(2));
    }

    void aFailingBatchChangesNothing()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        Store s;
        QVERIFY(s.open(path));
        ingestOne(s, sample(kNow - 300000, 100));

        // Failure injection: another connection removes the ingest log, so the conflict of
        // the second item cannot be logged and the batch fails after its first item.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "saboteur");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TABLE ingest_log"));
        }
        QSqlDatabase::removeDatabase("saboteur");

        QVector<IngestItem> v;
        v << item(sample(kNow - 240000, 102)) << item(sample(kNow - 300000, 150));
        const IngestResult r = s.ingest(v, context());
        QVERIFY(!r.ok);
        QVERIFY(!s.lastError().isEmpty());
        QCOMPARE(s.recordCount(), qint64(1));
    }

    void existingRowsCannotBeChanged()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/store.db";
        Store s;
        QVERIFY(s.open(path));
        const Record r = sample(kNow - 60000, 105);
        ingestOne(s, r);
        Mark m = mark(r, kNow, true, "{00000000-0000-0000-0000-000000000001}");
        QVERIFY(s.appendMark(m));
        ingestOne(s, sample(kNow - 1, 9999));    // quarantine

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "writer");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        const char *updates[] = {
            "UPDATE record SET value=1", "UPDATE mark SET comment='x'",
            "UPDATE quarantine SET detail='x'"
        };
        for (size_t i = 0; i < sizeof(updates) / sizeof(updates[0]); ++i) {
            QSqlQuery q(db);
            QVERIFY2(!q.exec(updates[i]), updates[i]);
            QVERIFY(q.lastError().text().contains("append-only"));
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("writer");
        Record kept;
        QVERIFY(s.latest(kSubject, metricGlucose(), QString(), false, &kept));
        QCOMPARE(kept.value, 105.0);
    }

    void rangeFeedAndLatest()
    {
        Store s;
        openMemory(s);
        for (int i = 0; i < 10; ++i)
            ingestOne(s, sample(kNow - 600000 + i * 60000, 100 + i));
        ingestOne(s, sample(kNow - 600000, 150, "test.graph"));

        RecordQuery q;
        q.subject = kSubject;
        q.metric = metricGlucose();
        q.source = "test.current";
        q.from = at(kNow - 600000 + 2 * 60000);
        q.to = at(kNow - 600000 + 4 * 60000);
        QVector<Record> r = s.records(q);
        QCOMPARE(r.size(), 3);               // both ends inclusive
        QCOMPARE(r.first().value, 102.0);
        QCOMPARE(r.last().value, 104.0);

        q.source.clear();
        q.from = UtcTime();
        q.to = at(kNow - 600000);
        QCOMPARE(s.records(q).size(), 2);    // one per feed at the first minute

        Record last;
        QVERIFY(s.latest(kSubject, metricGlucose(), "test.current", true, &last));
        QCOMPARE(last.value, 109.0);
        QVERIFY(!s.latest(kOther, metricGlucose(), QString(), true, &last));

        q = RecordQuery();
        q.limit = 3;
        q.newestFirst = true;
        r = s.records(q);
        QCOMPARE(r.size(), 3);
        QVERIFY(r[0].measuredAt.msecsSinceEpoch() >= r[1].measuredAt.msecsSinceEpoch());

        const SubjectStats st = s.subjectStats(kSubject);
        QCOMPARE(st.records, qint64(11));
        QCOMPARE(st.first.msecsSinceEpoch(), kNow - 600000);
        QCOMPARE(st.last.msecsSinceEpoch(), kNow - 600000 + 9 * 60000);
        QCOMPARE(s.subjectStats(kOther).records, qint64(0));
    }

    void rawAndEffectiveViews()
    {
        Store s;
        openMemory(s);
        const Record a = sample(kNow - 180000, 100);
        const Record b = sample(kNow - 120000, 101);
        ingestOne(s, a);
        ingestOne(s, b);
        QVERIFY(s.appendMark(mark(a, kNow - 1000, true, "{00000000-0000-0000-0000-000000000001}")));

        RecordQuery q;
        q.effective = false;
        QCOMPARE(s.records(q).size(), 2);      // raw view keeps everything
        q.effective = true;
        QCOMPARE(s.records(q).size(), 1);
        QCOMPARE(s.records(q).first().value, 101.0);

        // Un-ignoring is a later mark with state active.
        QVERIFY(s.appendMark(mark(a, kNow, false, "{00000000-0000-0000-0000-000000000002}")));
        QCOMPARE(s.records(q).size(), 2);
        // And ignoring again supersedes that.
        QVERIFY(s.appendMark(mark(a, kNow + 1000, true, "{00000000-0000-0000-0000-000000000003}")));
        QCOMPARE(s.records(q).size(), 1);
        QCOMPARE(s.marks().size(), 3);
    }

    void markTieGoesToTheGreaterId()
    {
        Store s;
        openMemory(s);
        const Record a = sample(kNow - 180000, 100);
        ingestOne(s, a);
        // Same time: ignored with the smaller id, active with the greater id -> active wins.
        QVERIFY(s.appendMark(mark(a, kNow, true, "{00000000-0000-0000-0000-00000000000a}")));
        QVERIFY(s.appendMark(mark(a, kNow, false, "{00000000-0000-0000-0000-00000000000b}")));
        RecordQuery q;
        QCOMPARE(s.records(q).size(), 1);

        // Other way round, on another record.
        const Record b = sample(kNow - 120000, 101);
        ingestOne(s, b);
        QVERIFY(s.appendMark(mark(b, kNow, false, "{00000000-0000-0000-0000-00000000000c}")));
        QVERIFY(s.appendMark(mark(b, kNow, true, "{00000000-0000-0000-0000-00000000000d}")));
        QCOMPARE(s.records(q).size(), 1);
        QCOMPARE(s.records(q).first().value, 100.0);

        // The order of insertion does not matter.
        const Record c = sample(kNow - 60000, 102);
        ingestOne(s, c);
        QVERIFY(s.appendMark(mark(c, kNow, true, "{00000000-0000-0000-0000-00000000000f}")));
        QVERIFY(s.appendMark(mark(c, kNow, false, "{00000000-0000-0000-0000-00000000000e}")));
        QCOMPARE(s.records(q).size(), 1);       // c ignored (f > e), a visible, b ignored
    }

    void marksNeedACommentAndTargetAndSurviveMissingTargets()
    {
        Store s;
        openMemory(s);
        const Record a = sample(kNow - 180000, 100);
        Mark m = mark(a, kNow, true, "{00000000-0000-0000-0000-000000000001}");
        m.comment = "  ";
        QVERIFY(!s.appendMark(m));
        m.comment = "ok";
        m.author = "someone";
        QVERIFY(s.appendMark(m));              // the target need not be in the store (thinned away)
        QVERIFY(s.appendMark(m));              // a repeated id is ignored
        QCOMPARE(s.marks().size(), 1);
        QCOMPARE(s.marks().first().author, QString("someone"));
        QVERIFY(s.marks().first().target == a.key());
    }

    void sensorsWidenFirstAndLastSeen()
    {
        Store s;
        openMemory(s);
        Sensor x;
        x.subject = kSubject;
        x.sensorId = "3f99d7824fd77d683ba2d54a9b92ae567e66a0fa3454758f73fe74597a41793e";
        x.activatedAt = at(kNow - 86400000);
        x.warmupMinutes = 60;
        x.firstSeen = at(kNow - 1000);
        x.lastSeen = at(kNow - 1000);
        QVERIFY(s.noteSensor(x));
        x.firstSeen = at(kNow - 5000);
        x.lastSeen = at(kNow);
        x.warmupMinutes = 99;                  // a later report does not rewrite the entity
        QVERIFY(s.noteSensor(x));
        const QVector<Sensor> all = s.sensors();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].firstSeen.msecsSinceEpoch(), kNow - 5000);
        QCOMPARE(all[0].lastSeen.msecsSinceEpoch(), kNow);
        QCOMPARE(all[0].warmupMinutes, 60);
        QVERIFY(all[0].model.isNull());
        QCOMPARE(all[0].activatedAt.msecsSinceEpoch(), kNow - 86400000);
    }

    void ingestLogKeepsEntries()
    {
        Store s;
        openMemory(s);
        IngestEntry e;
        e.time = at(kNow);
        e.event = "violation";
        e.adapter = "libre";
        e.adapterVersion = "0.1.0";
        e.sessionId = "s-0001";
        e.detail = "soft: batch above max_records_per_batch";
        e.extra.insert("limit", 100);
        QVERIFY(s.appendIngestEntry(e));
        const QVector<IngestEntry> log = s.ingestLog();
        QCOMPARE(log.size(), 1);
        QCOMPARE(log[0].event, QString("violation"));
        QCOMPARE(log[0].extra, e.extra);
        QVERIFY(!log[0].id.isNull());
        e.time = UtcTime();
        QVERIFY(!s.appendIngestEntry(e));
    }
};

QTEST_APPLESS_MAIN(TstStore)
#include "tst_store.moc"
