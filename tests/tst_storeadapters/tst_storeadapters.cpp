// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The two adapters between the scheduler and the store (StoreReadingStore, StoreIngestLogSink),
// against a real store in memory: what the scheduler reads and writes through its ports ends
// up where the store keeps it, and the connection id travels as an extra key of the ingest
// log. Synthetic data only.
#include "modtest.h"
#include "scheduler/storereadingstore.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

LinkId linkOf()
{
    LinkId l;
    l.connectionId = "c1";
    l.adapterId = "libre";
    l.adapterVersion = "0.1.0";
    l.sessionId = "s1";
    return l;
}

Record sample(const QUuid &subject, const QString &feed, qint64 ms, double value = 110)
{
    Record r;
    r.shape = Record::Sample;
    r.subject = subject;
    r.metric = "glucose";
    r.value = value;
    r.unit = "mg/dL";
    r.measuredAt = UtcTime::fromMSecsSinceEpoch(ms);
    r.originalTimestamp = "synthetic";
    r.source = "libre." + feed;
    return r;
}

} // namespace

class TstStoreAdapters : public QObject
{
    Q_OBJECT

private slots:
    void readingsGoInAndTheNewestComesBackPerSource()
    {
        Store store;
        QVERIFY(store.open(QString()));
        ManualClock clock;
        StoreReadingStore rs(&store, &clock);
        const QUuid subject("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
        const qint64 t = clock.ms() - 120000;

        QVERIFY(!rs.newestMeasurement(subject, "libre.current").isValid());
        QVector<Record> v;
        v.append(sample(subject, "current", t));
        v.append(sample(subject, "current", t + 60000, 115));
        QCOMPARE(rs.ingest(subject, linkOf(), v), 2);
        QCOMPARE(rs.newestMeasurement(subject, "libre.current").msecsSinceEpoch(), t + 60000);
        QVERIFY(!rs.newestMeasurement(subject, "libre.graph").isValid());       // another feed

        QCOMPARE(rs.ingest(subject, linkOf(), v), 0);                            // duplicates: nothing added, no error
        QCOMPARE(store.recordCount(), qint64(2));
    }

    void aBatchTheStoreRefusesIsReportedAsNotWritten()
    {
        Store store;                                                             // never opened: every write fails
        ManualClock clock;
        StoreReadingStore rs(&store, &clock);
        const QUuid subject("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
        QVector<Record> v;
        v.append(sample(subject, "current", clock.ms() - 60000));
        QCOMPARE(rs.ingest(subject, linkOf(), v), -1);                           // the scheduler keeps it queued
    }

    void aRejectedRecordGoesToQuarantineUnmodified()
    {
        Store store;
        QVERIFY(store.open(QString()));
        ManualClock clock;
        StoreReadingStore rs(&store, &clock);
        QJsonObject raw;
        raw.insert("metric", "glucose");
        raw.insert("value", 9999);
        rs.quarantine(linkOf(), "ValueOutOfRange", "value above the plausible range", raw);
        const QVector<QuarantineEntry> q = store.quarantine();
        QCOMPARE(q.size(), 1);
        QCOMPARE(q[0].reason, QStringLiteral("ValueOutOfRange"));
        QCOMPARE(q[0].adapter, QStringLiteral("libre"));
        QCOMPARE(q[0].adapterVersion, QStringLiteral("0.1.0"));
    }

    void aSensorIsKeptAndFoundAgain()
    {
        Store store;
        QVERIFY(store.open(QString()));
        ManualClock clock;
        StoreReadingStore rs(&store, &clock);
        const QUuid subject("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
        UtcTime a;
        int w = 0;
        QVERIFY(!rs.latestSensor(subject, &a, &w));
        QJsonObject s;
        s.insert("id", "3f99d7824fd77d683ba2d54a9b92ae567e66a0fa3454758f73fe74597a41793e");
        s.insert("activated_at", "2026-01-01T08:00:00.000Z");
        s.insert("warmup_minutes", 60);
        rs.noteSensor(subject, s);
        QVERIFY(rs.latestSensor(subject, &a, &w));
        QCOMPARE(a.toIso(), QStringLiteral("2026-01-01T08:00:00.000Z"));
        QCOMPARE(w, 60);
    }

    void ingestLogEntriesCarryTheSessionAndTheConnection()
    {
        Store store;
        QVERIFY(store.open(QString()));
        StoreIngestLogSink sink(&store);
        IngestLogEntry e;
        e.time = UtcTime::fromMSecsSinceEpoch(1768000000000LL);
        e.event = "violation";
        e.adapter = "libre";
        e.adapterVersion = "0.1.0";
        e.sessionId = "s1";
        e.connection = "c1";
        e.detail = "kind=unknown-type severity=hard weight=10.0 score=10.0 action=kill";
        QVERIFY(sink.append(e));
        const QVector<IngestEntry> log = store.ingestLog();
        QCOMPARE(log.size(), 1);
        QCOMPARE(log[0].event, QStringLiteral("violation"));
        QCOMPARE(log[0].sessionId, QStringLiteral("s1"));
        QCOMPARE(log[0].extra.value("connection").toString(), QStringLiteral("c1"));

        Store closed;                                                            // a store that cannot take it
        StoreIngestLogSink failing(&closed);
        QVERIFY(!failing.append(e));
    }
};

QTEST_APPLESS_MAIN(TstStoreAdapters)
#include "tst_storeadapters.moc"
