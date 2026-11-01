// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Compaction (REQ-STO-05 to 07, REQ-STO-12, REQ-STO-13, REQ-STO-16). Synthetic data
// only. Compaction deletes permanently (there is no archive any more), so these tests protect
// what decides a deletion: the thinning rule, the window, the kinds that always stay, the
// snapshot that keeps later arrivals, the all-or-nothing transaction, and the space check that
// must refuse before anything is deleted.
#include "record/registry.h"
#include "store/compactor.h"

#include <QCryptographicHash>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace lm;

namespace {

const QUuid kA("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
const QUuid kB("{0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d}");
const qint64 kMinute = 60000;
const qint64 kDay = 86400000;

UtcTime iso(const char *s) { return UtcTime::fromIso(QString::fromLatin1(s)); }
UtcTime at(qint64 ms) { return UtcTime::fromMSecsSinceEpoch(ms); }

// The clock of the tests. The cut-off of the default window (3 calendar months) is
// 2026-03-15T12:10:00Z, itself the start of a 5-minute slot: everything measured before it
// lies in whole old slots.
const qint64 kNow = 1781525400000LL;                    // 2026-06-15T12:10:00Z
const qint64 kSnapshot = 1781524740000LL;               // 2026-06-15T11:59:00Z, the default ingest time - 1 day
const qint64 kT0 = 1769904000000LL;                     // 2026-02-01T00:00:00Z, well before the cut-off
const qint64 kCutoff = 1773576600000LL;                 // 2026-03-15T12:10:00Z
const qint64 kPlenty = qint64(1) << 40;                 // free bytes that are always enough
const qint64 kSlot = 5 * kMinute;                       // the slot of REQ-STO-07

Record sample(const QUuid &subject, qint64 measuredAt, double value, const QString &source = QStringLiteral("t.current"),
              const QString &unit = QStringLiteral("mg/dL"), qint64 ingestedAt = kSnapshot - kDay)
{
    Record r;
    r.shape = Record::Sample;
    r.subject = subject;
    r.metric = metricGlucose();
    r.value = value;
    r.unit = unit;
    r.measuredAt = at(measuredAt);
    r.ingestedAt = at(ingestedAt);
    r.originalTimestamp = QStringLiteral("synthetic");
    r.source = source;
    r.origin = Origin::Adapter;
    return r;
}

IngestItem item(const Record &r)
{
    IngestItem i;
    i.record = r;
    i.raw.insert(QStringLiteral("marker"), 1);
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

// The start of the 5-minute UTC slot that holds "t" (REQ-STO-07); times here are after 1970.
qint64 slotStart(qint64 t) { return (t / kSlot) * kSlot; }

} // namespace

class TstCompaction : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_dir;
    Store *m_store;

    QString path(const QString &name) const { return m_dir->path() + "/" + name; }

    bool ingest(const QVector<Record> &records)
    {
        QVector<IngestItem> v;
        foreach (const Record &r, records) v << item(r);
        const IngestResult res = m_store->ingest(v, context());
        return res.ok && res.quarantined == 0;
    }

    // The usual run: the test clock, the default window and always enough free space.
    static CompactionOptions options(qint64 freeBytes = kPlenty)
    {
        CompactionOptions o;
        o.now = at(kNow);
        o.freeSpace = [freeBytes](const QString &) { return freeBytes; };
        return o;
    }

    // The store's files, byte for byte: "nothing changed" means these are identical.
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

    // The raw archive columns of the compaction table, for the check that this version leaves
    // them empty.
    QStringList archiveColumns()
    {
        QStringList out;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "columns");
            db.setDatabaseName(m_store->path());
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (db.open()) {
                QSqlQuery q(db);
                q.exec("SELECT archive_id IS NULL, archive_sha256 IS NULL FROM compaction ORDER BY time, id");
                while (q.next())
                    out << QString("%1%2").arg(q.value(0).toInt()).arg(q.value(1).toInt());
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("columns");
        return out;
    }

    QMap<QString, Record> records()
    {
        QMap<QString, Record> m;
        m_store->forEachRecord([&](const Record &r) { m.insert(r.key().toString(), r); });
        return m;
    }

    QMap<QString, qint64> state()
    {
        QMap<QString, qint64> m;
        const char *types[] = { "subject", "record", "mark", "sensor", "quarantine", "ingest", "compaction" };
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
            m[types[i]] = m_store->rowCount(types[i]);
        return m;
    }

    // Is the flag row of a compaction transaction still there? It must never survive.
    bool flagLeft()
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "flag");
        db.setDatabaseName(m_store->path());
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        bool left = true;
        if (db.open()) {
            QSqlQuery q(db);
            left = q.exec("SELECT COUNT(*) FROM meta WHERE key='compacting'") && q.next() && q.value(0).toInt() > 0;
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("flag");
        return left;
    }

    QSet<QString> keys()
    {
        QSet<QString> out;
        m_store->forEachRecord([&](const Record &r) { out.insert(r.key().toString()); });
        return out;
    }

private slots:
    void init()
    {
        m_dir = new QTemporaryDir;
        m_store = new Store;
        QVERIFY(m_store->open(path("store.db")));
    }
    void cleanup()
    {
        delete m_store;
        delete m_dir;
    }

    // ---- the window ----------------------------------------------------------------------------

    void theWindowStartsOnTheCalendarAndNeverBelowFourteenDays()
    {
        QCOMPARE(FullResolutionWindow().cutoff(at(kNow)).toIso(), QString("2026-03-15T12:10:00.000Z"));
        QCOMPARE(FullResolutionWindow::ofMonths(1).cutoff(iso("2026-03-31T12:00:00.000Z")).toIso(),
                 QString("2026-02-28T12:00:00.000Z"));
        QCOMPARE(FullResolutionWindow::ofMonths(6).cutoff(iso("2026-08-31T00:00:00.000Z")).toIso(),
                 QString("2026-02-28T00:00:00.000Z"));
        QCOMPARE(FullResolutionWindow::ofDays(14).cutoff(at(kNow)).msecsSinceEpoch(), kNow - 14 * kDay);

        QVERIFY(FullResolutionWindow().isAllowed(at(kNow)));
        QVERIFY(FullResolutionWindow::ofMonths(1).isAllowed(at(kNow)));
        QVERIFY(FullResolutionWindow::ofDays(14).isAllowed(at(kNow)));
        QVERIFY(!FullResolutionWindow::ofDays(13).isAllowed(at(kNow)));
        QVERIFY(!FullResolutionWindow::ofMonths(0).isAllowed(at(kNow)));
        QVERIFY(!FullResolutionWindow().isAllowed(UtcTime()));
    }

    void aWindowBelowFourteenDaysIsRefusedAndNothingChanges()
    {
        QVector<Record> v;
        for (int i = 0; i < 60; ++i) v << sample(kA, kT0 + i * kMinute, 100 + i % 9);
        QVERIFY(ingest(v));
        const QMap<QString, qint64> before = state();
        CompactionOptions o = options();
        o.window = FullResolutionWindow::ofDays(13);
        const CompactionReport r = compactStore(*m_store, o);
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::windowTooShort());
        QCOMPARE(state(), before);
        QVERIFY(!flagLeft());
    }

    // ---- the thinning rule (REQ-STO-07) ---------------------------------------------

    // In each old 5-minute slot of the `current` feed only the sample nearest to the slot start
    // stays, unchanged. A dip or a peak inside the slot can go.
    void theRuleKeepsTheSampleNearestTheSlotStart()
    {
        const qint64 slot = slotStart(kT0);
        QCOMPARE(slot % kSlot, qint64(0));
        QVector<Record> all;
        QSet<QString> kept;
        // Slot 0: one-minute samples at minutes 0 to 4, with a dip at minute 2: minute 0 stays.
        const double s0[] = { 120, 118, 55, 117, 121 };
        for (int m = 0; m < 5; ++m) all << sample(kA, slot + m * kMinute, s0[m]);
        kept << sample(kA, slot, 0).key().toString();
        // Slot 1: the first sample is at minute 7 (the source skipped two minutes): it stays.
        for (int m = 7; m < 10; ++m) all << sample(kA, slot + m * kMinute, 100 + m);
        kept << sample(kA, slot + 7 * kMinute, 0).key().toString();
        // Slot 2: off the minute grid, 40 s past each minute: the one at 10:40 stays; the sample at
        // exactly 15:00 belongs to slot 3, where it is the only one and stays.
        for (int m = 10; m < 15; ++m) all << sample(kA, slot + m * kMinute + 40000, 90 + m);
        kept << sample(kA, slot + 10 * kMinute + 40000, 0).key().toString();
        all << sample(kA, slot + 15 * kMinute, 140);
        kept << sample(kA, slot + 15 * kMinute, 0).key().toString();
        // Another Subject in slot 0 is thinned on its own.
        for (int m = 1; m < 5; ++m) all << sample(kB, slot + m * kMinute, 100);
        kept << sample(kB, slot + kMinute, 0).key().toString();
        QVERIFY(ingest(all));

        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.errorClass + ": " + r.failure.message));
        QCOMPARE(keys(), kept);
        QCOMPARE(r.removed.value("record"), qint64(all.size() - kept.size()));
        QCOMPARE(r.slotCount, qint64(5));
        QCOMPARE(r.kept, qint64(5));
        // Kept samples are the original adapter records, unchanged.
        const QMap<QString, Record> now = records();
        foreach (const Record &orig, all)
            if (now.contains(orig.key().toString())) {
                const Record &k = now[orig.key().toString()];
                QCOMPARE(k.value, orig.value);
                QCOMPARE(k.unit, orig.unit);
                QCOMPARE(k.ingestedAt, orig.ingestedAt);
                QCOMPARE(k.originalTimestamp, orig.originalTimestamp);
                QCOMPARE(k.origin, orig.origin);
            }
        QVERIFY(!flagLeft());
    }

    // Two samples at the same time (two entry ids): the earlier one in key order stays.
    void aTieGoesToTheEarlierSample()
    {
        const qint64 slot = slotStart(kT0);
        Record a = sample(kA, slot + kMinute, 100); a.entryId = "a";
        Record b = sample(kA, slot + kMinute, 101); b.entryId = "b";
        Record c = sample(kA, slot + 2 * kMinute, 102);
        QVERIFY(ingest(QVector<Record>() << b << c << a));
        QVERIFY(compactStore(*m_store, options()).ok);
        QCOMPARE(keys(), QSet<QString>() << a.key().toString());
    }

    // Units never mix: a slot with samples in mg/dL and in mmol/L keeps one of each.
    void aSlotWithTwoUnitsKeepsOneSamplePerUnit()
    {
        const qint64 slot = slotStart(kT0);
        QVector<Record> all;
        all << sample(kA, slot + 1 * kMinute, 100) << sample(kA, slot + 2 * kMinute, 110)
            << sample(kA, slot + 3 * kMinute, 120);
        all << sample(kA, slot + 0 * kMinute, 5.5, "t.current", "mmol/L")
            << sample(kA, slot + 4 * kMinute, 6.0, "t.current", "mmol/L");
        QVERIFY(ingest(all));
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(keys(), QSet<QString>() << all[0].key().toString() << all[3].key().toString());
        const QMap<QString, Record> now = records();
        QCOMPARE(now[all[0].key().toString()].unit, QString("mg/dL"));
        QCOMPARE(now[all[0].key().toString()].value, 100.0);
        QCOMPARE(now[all[3].key().toString()].unit, QString("mmol/L"));
        QCOMPARE(now[all[3].key().toString()].value, 5.5);
        QCOMPARE(r.slotCount, qint64(2));
    }

    // Only the `current` feed is thinned: the graph (history) feed, logbook entries, manual
    // records stay in full, also when old and dense; derived records go (REQ-STO-06).
    void theGraphFeedAndEverythingElseStay()
    {
        const qint64 slot = slotStart(kT0);
        QVector<Record> all;
        QSet<QString> kept;
        for (int m = 0; m < 60; ++m) {
            all << sample(kA, slot + m * kMinute, 100 + m % 9, "t.graph");     // even one a minute
            kept << all.last().key().toString();
        }
        for (int m = 0; m < 60; m += 5) {
            all << sample(kB, slot + m * kMinute + 30000, 90, "t.graph");
            kept << all.last().key().toString();
        }
        Record e = sample(kA, slot + 7 * kMinute, 0, "t.logbook");
        e.shape = Record::Event;
        e.metric = metricLogbookEntry();
        e.unit.clear();
        e.payload.insert("entry_type", 3);
        e.payload.insert("raw", QJsonObject());
        Record e2 = e;
        e2.measuredAt = at(slot + 8 * kMinute);
        Record m1 = sample(kA, slot + 8 * kMinute, 140, "manual.entry");
        m1.origin = Origin::Manual; m1.entryId = "entry-1";
        Record m2 = sample(kA, slot + 9 * kMinute, 141, "manual.entry");
        m2.origin = Origin::Manual; m2.entryId = "entry-2";
        all << e << e2 << m1 << m2;
        kept << e.key().toString() << e2.key().toString() << m1.key().toString() << m2.key().toString();
        Record d = sample(kA, slot + 9 * kMinute, 141, "derived.rule");
        d.origin = Origin::Derived; d.ruleVersion = "r1";
        all << d;
        // And a current feed in the same slots, which is thinned.
        for (int m = 0; m < 60; ++m) all << sample(kA, slot + m * kMinute, 100 + m % 7);
        for (int m = 0; m < 60; m += 5) kept << sample(kA, slot + m * kMinute, 0).key().toString();
        QVERIFY(ingest(all));
        Mark mk;
        mk.time = at(kNow); mk.target = all[3].key(); mk.comment = "on a graph point";
        QVERIFY(m_store->appendMark(mk));

        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(keys(), kept);
        QCOMPARE(r.derivedRemoved, qint64(1));
        QCOMPARE(r.removed.value("record"), qint64(48 + 1));
        QCOMPARE(m_store->marks().size(), 1);
    }

    // The rule against an independent statement of it (distance to the slot start), on random
    // series with gaps, off-grid times, two Subjects and two feeds.
    void theRuleAgreesWithAReferenceOnRandomSeries()
    {
        qsrand(12345);
        QVector<Record> all;
        QSet<QString> expected;
        QMap<QString, QPair<qint64, QString> > best;           // Subject + slot -> (distance to the start, key)
        const qint64 base = slotStart(kT0);
        for (int subject = 0; subject < 2; ++subject) {
            const QUuid sub = subject ? kB : kA;
            for (int minute = 0; minute < 600; ++minute) {
                if (qrand() % 4 == 0)
                    continue;                                   // gaps
                const qint64 t = base + minute * kMinute + (qrand() % 60) * 1000;
                const Record c = sample(sub, t, 80 + qrand() % 50);
                all << c;
                const qint64 s = (t / kSlot) * kSlot;
                const QString group = sub.toString() + QString::number(s);
                if (!best.contains(group) || t - s < best[group].first)
                    best[group] = qMakePair(t - s, c.key().toString());
                if (qrand() % 3 == 0) {
                    const Record g = sample(sub, t, 80, "t.graph");
                    all << g;
                    expected << g.key().toString();         // the graph feed is never thinned
                }
            }
        }
        for (QMap<QString, QPair<qint64, QString> >::const_iterator it = best.begin(); it != best.end(); ++it)
            expected << it.value().second;
        QVERIFY(ingest(all));
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(keys(), expected);
        QVERIFY(r.removed.value("record") > 500);
    }

    void theOrderOfArrivalDoesNotMatter()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 200; ++i)
            all << sample(kA, base + i * kMinute + (i % 3) * 20000, 70 + (i * 37) % 11 * 9);
        QVERIFY(ingest(all));
        QVERIFY(compactStore(*m_store, options()).ok);
        const QSet<QString> first = keys();

        Store other;
        QVERIFY(other.open(path("other.db")));
        QVector<IngestItem> v;
        for (int i = all.size() - 1; i >= 0; --i) v << item(all[i]);          // backwards
        QVERIFY(other.ingest(v, context()).ok);
        QVERIFY(compactStore(other, options()).ok);
        QSet<QString> second;
        other.forEachRecord([&](const Record &r) { second.insert(r.key().toString()); });
        QCOMPARE(second, first);
        QCOMPARE(first.size(), 40);
    }

    // ---- what the window protects --------------------------------------------------------------

    // Whole slots only: with the cut-off at 12:12:30 the slot 12:10 to 12:15 crosses it and is
    // left alone, old samples included; the slot before it is thinned; recent data stays.
    void newerDataAndTheSlotOfTheCutoffAreLeftAlone()
    {
        CompactionOptions o = options();
        o.now = at(kNow + 150000);                                         // cut-off 2026-03-15T12:12:30Z
        const qint64 cutSlot = kCutoff;                                    // 12:10, a slot start
        QCOMPARE(cutSlot % kSlot, qint64(0));
        QVector<Record> all;
        for (int m = 0; m < 5; ++m) all << sample(kA, cutSlot + m * kMinute, 100 + m);
        const qint64 before = cutSlot - kSlot;
        for (int m = 0; m < 5; ++m) all << sample(kA, before + m * kMinute, 100 + m);
        for (int m = 0; m < 120; ++m) all << sample(kA, kNow - kDay + m * kMinute, 100 + m % 5);
        QVERIFY(ingest(all));
        const CompactionReport r = compactStore(*m_store, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        int inCutSlot = 0, inBefore = 0, recent = 0;
        const QMap<QString, Record> now = records();
        for (QMap<QString, Record>::const_iterator it = now.begin(); it != now.end(); ++it) {
            const qint64 t = it.value().measuredAt.msecsSinceEpoch();
            if (t >= cutSlot && t < cutSlot + kSlot) ++inCutSlot;
            else if (t >= before && t < cutSlot) ++inBefore;
            else if (t >= kNow - kDay) ++recent;
        }
        QCOMPARE(inCutSlot, 5);
        QCOMPARE(inBefore, 1);
        QCOMPARE(recent, 120);
    }

    void aShortWindowThinsOnlyWhatIsOlderThanIt()
    {
        QVector<Record> all;
        const qint64 s13 = slotStart(kNow - 13 * kDay);                   // 13 days ago: protected
        const qint64 s20 = slotStart(kNow - 20 * kDay);                   // 20 days ago: old
        for (int m = 0; m < 5; ++m) { all << sample(kA, s13 + m * kMinute, 100 + m); all << sample(kA, s20 + m * kMinute, 100 + m); }
        QVERIFY(ingest(all));
        CompactionOptions o = options();
        o.window = FullResolutionWindow::ofDays(14);
        const CompactionReport r = compactStore(*m_store, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        int a13 = 0, a20 = 0;
        const QMap<QString, Record> now = records();
        for (QMap<QString, Record>::const_iterator it = now.begin(); it != now.end(); ++it) {
            const qint64 t = it.value().measuredAt.msecsSinceEpoch();
            if (t >= s13 && t < s13 + kSlot) ++a13;
            if (t >= s20 && t < s20 + kSlot) ++a20;
        }
        QCOMPARE(a13, 5);
        QCOMPARE(a20, 1);
    }

    // ---- what arrives while compaction runs ----------------------------------------------------

    // The decisions come from a snapshot taken at the start. A row the service writes while the
    // dry run is going (here: from the progress callback of the checking-space phase, before
    // the deleting pass takes the write lock) is not in that snapshot and must never be
    // removed, even when it is old and lies in a slot that is being thinned: compaction deletes
    // only what it has looked at.
    void rowsWrittenDuringTheRunAreNeverRemoved()
    {
        const qint64 slot = slotStart(kT0);
        QVector<Record> all;
        for (int m = 0; m < 30; ++m) all << sample(kA, slot + m * kMinute, 100 + (m % 7));
        QVERIFY(ingest(all));

        QVector<Record> late;
        for (int m = 0; m < 30; ++m)
            late << sample(kA, slot + m * kMinute + 30000, 61 + m, "t.current", "mg/dL", kSnapshot + 5 * kMinute);
        bool written = false;
        CompactionOptions o = options();
        o.progress = [&](const OperationProgress &p) {
            if (p.phase == OperationProgress::CheckingSpace && !written) {
                written = true;
                QVector<IngestItem> v;
                foreach (const Record &r, late) v << item(r);
                if (!m_store->ingest(v, context()).ok)
                    qWarning("the late batch could not be written");
            }
            return true;
        };
        const CompactionReport r = compactStore(*m_store, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(written);
        const QMap<QString, Record> now = records();
        foreach (const Record &l, late)
            QVERIFY2(now.contains(l.key().toString()), "a row written during the run was removed");
        // Of the rows the run looked at, each of the six slots keeps one; the rest went.
        int earlyLeft = 0;
        foreach (const Record &x, all)
            if (now.contains(x.key().toString())) ++earlyLeft;
        QCOMPARE(earlyLeft, 6);
        QCOMPARE(r.removed.value("record"), qint64(24));
        QCOMPARE(r.estimate.records, qint64(24));       // the dry run saw the same rows
    }

    void derivedQuarantineAndTheIngestLogGoOnlyWhenOld()
    {
        QVector<Record> all;
        Record oldDerived = sample(kA, kT0, 120, "derived.rule");
        oldDerived.origin = Origin::Derived; oldDerived.ruleVersion = "r1";
        Record newDerived = sample(kA, kNow - kDay, 120, "derived.rule");
        newDerived.origin = Origin::Derived; newDerived.ruleVersion = "r1";
        Record oldManual = sample(kA, kT0 + kMinute, 130, "manual.entry");
        oldManual.origin = Origin::Manual; oldManual.entryId = "e1";
        Record oldEvent = sample(kA, kT0 + 2 * kMinute, 0, "t.logbook");
        oldEvent.shape = Record::Event; oldEvent.metric = metricLogbookEntry(); oldEvent.unit.clear();
        oldEvent.payload.insert("entry_type", 3); oldEvent.payload.insert("raw", QJsonObject());
        all << oldDerived << newDerived << oldManual << oldEvent;
        QVERIFY(ingest(all));

        // Quarantine: two old (received about four months ago), one recent.
        IngestContext oldCtx = context();
        oldCtx.now = at(kT0 + 10 * kMinute);
        QVector<IngestItem> bad;
        IngestItem b1;
        b1.rejected.reason = Validation::UnknownMetric; b1.rejected.detail = "metric"; b1.raw.insert("a", 1);
        bad << b1;
        QVERIFY(m_store->ingest(bad, oldCtx).ok);
        QVERIFY(m_store->ingest(bad, oldCtx).ok);           // a new quarantine id each time
        QVERIFY(m_store->ingest(bad, context()).ok);
        // Ingest log: an old and a recent entry.
        IngestEntry le;
        le.time = at(kT0 + 20 * kMinute); le.event = "violation"; le.adapter = "libre"; le.adapterVersion = "1"; le.sessionId = "s"; le.detail = "old";
        QVERIFY(m_store->appendIngestEntry(le));
        le.time = at(kNow - kDay); le.detail = "new";
        QVERIFY(m_store->appendIngestEntry(le));
        // Marks and sensors, which stay in full.
        Mark mk;
        mk.time = at(kNow); mk.target = oldDerived.key(); mk.comment = "on a record that will go";
        QVERIFY(m_store->appendMark(mk));
        Sensor sn;
        sn.subject = kA; sn.sensorId = "abc"; sn.firstSeen = at(kT0); sn.lastSeen = at(kT0);
        QVERIFY(m_store->noteSensor(sn));
        const qint64 quarantineBefore = m_store->rowCount("quarantine");

        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.removed.value("quarantine"), qint64(2));
        QCOMPARE(m_store->rowCount("quarantine"), quarantineBefore - 2);
        QCOMPARE(r.removed.value("ingest"), qint64(1));
        QCOMPARE(m_store->ingestLog().size(), 1);
        QCOMPARE(m_store->ingestLog().first().detail, QString("new"));
        const QMap<QString, Record> now = records();
        QVERIFY(!now.contains(oldDerived.key().toString()));
        QVERIFY(now.contains(newDerived.key().toString()));
        QVERIFY(now.contains(oldManual.key().toString()));
        QVERIFY(now.contains(oldEvent.key().toString()));
        QCOMPARE(m_store->marks().size(), 1);
        QCOMPARE(m_store->sensors().size(), 1);
        QCOMPARE(m_store->subjects().size(), 1);
    }

    // ---- the compaction record -----------------------------------------------------------------

    // REQ-STO-06 as rewritten: one record per run that removed something, with time,
    // range, counts and rule version, and no archive.
    void oneCompactionRecordIsAppendedAndKept()
    {
        const qint64 slot = slotStart(kT0);
        QVector<Record> all;
        for (int m = 0; m < 30; ++m) all << sample(kA, slot + m * kMinute, 100 + (m % 7));
        QVERIFY(ingest(all));
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));

        QVector<CompactionEntry> entries;
        m_store->forEachCompaction([&](const CompactionEntry &e) { entries << e; });
        QCOMPARE(entries.size(), 1);
        const CompactionEntry &e = entries[0];
        QCOMPARE(e.time.msecsSinceEpoch(), kNow);
        // The earliest removed sample: minute 0 holds the lowest value and is kept, minute 1 goes.
        QCOMPARE(e.rangeFrom.msecsSinceEpoch(), slot + kMinute);
        QCOMPARE(e.rangeTo.msecsSinceEpoch(), kCutoff);
        QCOMPARE(qint64(e.removed.value("record").toDouble()), r.removed.value("record"));
        QCOMPARE(e.ruleVersion, QString("2"));                       // the rule
        QVERIFY(r.cutoff.msecsSinceEpoch() == kCutoff);
        QCOMPARE(archiveColumns(), QStringList() << "11");          // no archive is named

        // Nothing more to remove: a second run writes no second record.
        const CompactionReport again = compactStore(*m_store, options());
        QVERIFY2(again.ok, qPrintable(again.failure.message));
        QCOMPARE(again.totalRemoved(), qint64(0));
        QCOMPARE(m_store->rowCount("compaction"), qint64(1));
    }

    // ---- atomicity and cancel ---------------------------------------------------------------

    void aStoppedCompactionLeavesTheStoreAsItWas()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 300; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        QVERIFY(ingest(all));
        const QMap<QString, qint64> before = state();
        const qint64 stops[] = { 1, 50, 299, 300 };
        for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
            CompactionOptions o = options();
            const qint64 stopAt = stops[i];
            o.hook = [stopAt](qint64 scanned) { return scanned < stopAt; };
            const CompactionReport r = compactStore(*m_store, o);
            QVERIFY(!r.ok);
            QCOMPARE(r.failure.errorClass, CompactionError::aborted());
            QCOMPARE(state(), before);
            QVERIFY(!flagLeft());
        }
        QVERIFY(compactStore(*m_store, options()).ok);              // and it works afterwards
        QVERIFY(m_store->rowCount("record") < before.value("record"));
    }

    void aWriteErrorAtTheEndUndoesEveryDeletion()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 300; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        QVERIFY(ingest(all));
        const qint64 records = m_store->rowCount("record");
        {
            // Failure injection: the table for the compaction record is gone, so the run fails
            // after all the deletions were made inside its transaction.
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "saboteur");
            db.setDatabaseName(m_store->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TRIGGER compaction_no_delete"));
            QVERIFY(q.exec("DROP TABLE compaction"));
        }
        QSqlDatabase::removeDatabase("saboteur");
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::io());
        QCOMPARE(m_store->rowCount("record"), records);
        QVERIFY(!flagLeft());
    }

    // The database itself refuses deletes outside compaction, so no other code path can
    // remove data by mistake; inside compaction only records, quarantine and the ingest log may go.
    void deletesAreRefusedExceptByCompaction()
    {
        QVector<Record> all;
        all << sample(kA, kT0, 100);
        QVERIFY(ingest(all));
        Mark mk;
        mk.time = at(kNow); mk.target = all[0].key(); mk.comment = "x";
        QVERIFY(m_store->appendMark(mk));
        Sensor sn;
        sn.subject = kA; sn.sensorId = "abc"; sn.firstSeen = at(kT0); sn.lastSeen = at(kT0);
        QVERIFY(m_store->noteSensor(sn));
        IngestEntry ie;
        ie.time = at(kT0); ie.event = "violation"; ie.adapter = "a"; ie.adapterVersion = "1"; ie.sessionId = "s"; ie.detail = "d";
        QVERIFY(m_store->appendIngestEntry(ie));
        QVector<IngestItem> bad;
        IngestItem b;
        b.rejected.reason = Validation::UnknownMetric; b.rejected.detail = "m"; b.raw.insert("a", 1);
        bad << b;
        QVERIFY(m_store->ingest(bad, context()).ok);

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "writer");
        db.setDatabaseName(m_store->path());
        QVERIFY(db.open());
        {
            // A compaction record of this version and a reimport record as schema 1 wrote them.
            QSqlQuery q(db);
            QVERIFY2(q.exec("INSERT INTO compaction(id, time, range_from, range_to, removed, rule_version)"
                            " VALUES('c1', 1, 1, 1, '{}', '1')"), qPrintable(q.lastError().text()));
            QVERIFY2(q.exec("INSERT INTO reimport VALUES('r1', 1, 'x', 'y', '1.0', '{}')"), qPrintable(q.lastError().text()));
        }
        const char *tables[] = { "record", "mark", "sensor", "quarantine", "ingest_log", "compaction", "reimport" };
        for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) {
            QSqlQuery q(db);
            QVERIFY2(!q.exec(QString("DELETE FROM %1").arg(tables[i])), tables[i]);
            QVERIFY(q.lastError().text().contains("compaction") || q.lastError().text().contains("never")
                    || q.lastError().text().contains("purge"));
        }
        // Inside the transaction of a compaction (the flag row) three tables may be emptied; the
        // others never.
        QVERIFY(db.transaction());
        QSqlQuery q(db);
        QVERIFY(q.exec("INSERT OR REPLACE INTO meta VALUES('compacting', '1')"));
        QVERIFY(q.exec("DELETE FROM record"));
        QVERIFY(q.exec("DELETE FROM quarantine"));
        QVERIFY(q.exec("DELETE FROM ingest_log"));
        QVERIFY(!q.exec("DELETE FROM mark"));
        QVERIFY(!q.exec("DELETE FROM sensor"));
        QVERIFY(!q.exec("DELETE FROM compaction"));
        QVERIFY(!q.exec("DELETE FROM reimport"));
        db.rollback();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("writer");
        QCOMPARE(m_store->rowCount("record"), qint64(1));
        QVERIFY(!flagLeft());
    }

    // ---- space ----------------------------------------------------------------------------------

    void theSpaceIsGivenBackToTheFileSystem()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int d = 0; d < 20; ++d) {
            all.clear();
            for (int i = 0; i < 1440; ++i)
                all << sample(kA, base + d * kDay + i * kMinute, 70 + ((d * 1440 + i) * 7) % 110);
            QVERIFY(ingest(all));
        }
        QCOMPARE(m_store->rowCount("record"), qint64(28800));
        const qint64 before = m_store->sizeBytes();
        QVERIFY(before > 2000000);
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        const qint64 after = m_store->sizeBytes();
        QVERIFY2(after < before / 3, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        QVERIFY(r.bytesFreed > 0);
        QCOMPARE(QFileInfo(m_store->path() + "-wal").size(), qint64(0));
        // 5-minute slots: 20 days * 288 slots, one sample each.
        QCOMPARE(m_store->rowCount("record"), qint64(20 * 288));
        QCOMPARE(r.slotCount, qint64(20 * 288));
        // The rebuild need the run checked was about what remained afterwards: the
        // estimate of what remains is not below the real remainder by more than the margin.
        QVERIFY2(r.estimate.rebuildNeedBytes >= after,
                 qPrintable(QString("need %1, remained %2").arg(r.estimate.rebuildNeedBytes).arg(after)));
    }

    void theRebuildNeedIsWhatRemainsPlusTheMargin()
    {
        QCOMPARE(rebuildNeedFor(0, 0), kRebuildMarginMinimumBytes);
        QCOMPARE(rebuildNeedFor(5000000, 1000000), qint64(4000000) + kRebuildMarginMinimumBytes);   // a tenth is below 1 MiB
        QCOMPARE(rebuildNeedFor(80000000, 20000000), qint64(60000000) + qint64(6000000));          // a tenth of what remains
        QCOMPARE(rebuildNeedFor(1000, 5000), kRebuildMarginMinimumBytes);                           // never below the margin
        QCOMPARE(rebuildNeedFor(1000, -5), qint64(1000) + kRebuildMarginMinimumBytes);              // a negative estimate counts as nothing
    }

    // REQ-STO-16 and the test list: the estimate states the same need the run checks,
    // the exact boundary is allowed, one byte below is refused before anything changes.
    void theSpaceCheckRefusesBeforeAnythingIsDeleted()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 3000; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        QVERIFY(ingest(all));

        CompactionEstimate e;
        QString error;
        QVERIFY2(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &e, &error,
                                    [](const QString &) { return kPlenty; }), qPrintable(error));
        QCOMPARE(e.space, CompactionEstimate::SpaceEnough);
        QCOMPARE(e.freeBytes, kPlenty);
        QCOMPARE(e.storeBytes, m_store->sizeBytes());
        QCOMPARE(e.rebuildNeedBytes, rebuildNeedFor(e.storeBytes, e.bytes));
        QCOMPARE(e.missingBytes(), qint64(0));
        const qint64 need = e.rebuildNeedBytes;

        // One byte short: refused with the numbers, the files byte-identical, no flag row.
        const QMap<QString, qint64> before = state();
        const QByteArray digest = fileDigest();
        CompactionReport r = compactStore(*m_store, options(need - 1));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::insufficientSpace());
        QCOMPARE(r.estimate.rebuildNeedBytes, need);                 // the same calculation as the estimate
        QCOMPARE(r.estimate.freeBytes, need - 1);
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceShort);
        QCOMPARE(r.estimate.missingBytes(), qint64(1));
        QCOMPARE(r.estimate.records, e.records);
        QCOMPARE(state(), before);
        QCOMPARE(fileDigest(), digest);
        QVERIFY(!flagLeft());

        // The estimate says the same for the same free space.
        CompactionEstimate shortOne;
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &shortOne, &error,
                                   [need](const QString &) { return need - 1; }));
        QCOMPARE(shortOne.space, CompactionEstimate::SpaceShort);
        QCOMPARE(shortOne.missingBytes(), qint64(1));

        // Exactly the need: allowed, and it runs.
        r = compactStore(*m_store, options(need));
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceEnough);
        QCOMPARE(r.removed.value("record"), e.records);
    }

    // REQ-STO-16: an unknown free space is its own outcome; the caller decides.
    void anUnknownFreeSpaceIsSaidAndTheCallerDecides()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 300; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        QVERIFY(ingest(all));
        const QMap<QString, qint64> before = state();
        const QByteArray digest = fileDigest();

        CompactionEstimate e;
        QString error;
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &e, &error,
                                   [](const QString &) { return qint64(-1); }));
        QCOMPARE(e.space, CompactionEstimate::SpaceUnknown);
        QCOMPARE(e.freeBytes, qint64(-1));
        QVERIFY(e.rebuildNeedBytes > 0);
        QCOMPARE(e.missingBytes(), qint64(0));

        CompactionReport r = compactStore(*m_store, options(-1));
        QVERIFY(!r.ok);
        QCOMPARE(r.failure.errorClass, CompactionError::freeSpaceUnknown());
        QCOMPARE(r.estimate.space, CompactionEstimate::SpaceUnknown);
        QCOMPARE(state(), before);
        QCOMPARE(fileDigest(), digest);

        CompactionOptions o = options(-1);
        o.proceedWhenFreeSpaceUnknown = true;
        r = compactStore(*m_store, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QVERIFY(r.removed.value("record") > 0);
    }

    // The free space is asked for the directory of the store's own file, and without an
    // injected answer the file system is asked (a real answer, never "unknown", on the host).
    void theFreeSpaceIsAskedWhereTheStoreIs()
    {
        QVector<Record> all;
        all << sample(kA, kT0, 100);
        QVERIFY(ingest(all));
        QString asked;
        CompactionEstimate e;
        QString error;
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &e, &error,
                                   [&](const QString &dir) { asked = dir; return kPlenty; }));
        QCOMPARE(asked, QFileInfo(m_store->path()).absolutePath());
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &e, &error));
        QVERIFY(e.freeBytes > 0);
        QVERIFY(e.space != CompactionEstimate::SpaceUnknown);
    }

    void theDryRunEstimatesWhatTheRealRunRemoves()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 2000; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        Record d = sample(kA, base + 5 * kMinute, 120, "derived.rule");
        d.origin = Origin::Derived; d.ruleVersion = "r1";
        all << d;
        for (int i = 0; i < 500; ++i) all << sample(kA, kNow - 2 * kDay + i * kMinute, 100 + i % 9);
        // A month old: inside the default window, outside a window of 14 days.
        for (int i = 0; i < 600; ++i) all << sample(kA, kNow - 30 * kDay + i * kMinute, 90 + (i * 7) % 13);
        QVERIFY(ingest(all));
        QVector<IngestItem> bad;
        IngestItem b;
        b.rejected.reason = Validation::UnknownMetric; b.rejected.detail = "m"; b.raw.insert("a", 1);
        bad << b;
        IngestContext oldCtx = context();
        oldCtx.now = at(kT0 + kMinute);
        QVERIFY(m_store->ingest(bad, oldCtx).ok);

        const QMap<QString, qint64> before = state();
        CompactionEstimate e;
        QString error;
        QVERIFY2(estimateCompaction(*m_store, FullResolutionWindow(), at(kNow), &e, &error), qPrintable(error));
        QCOMPARE(state(), before);                                              // a dry run changes nothing
        QVERIFY(!flagLeft());
        QCOMPARE(e.cutoff.msecsSinceEpoch(), kCutoff);
        QVERIFY(e.records > 1500);
        QCOMPARE(e.quarantine, qint64(1));
        QVERIFY(e.bytes > 0);
        const double average = double(m_store->sizeBytes()) / double(before.value("record") + before.value("quarantine")
                                                                     + before.value("ingest") + before.value("mark") + before.value("sensor"));
        QVERIFY(std::fabs(double(e.bytes) - double(e.records + e.quarantine + e.ingest) * average) <= 1.0);

        // A shorter window removes more and leaves less to rebuild; a longer one the opposite;
        // a window below 14 days is refused.
        CompactionEstimate shorter, longer;
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow::ofDays(14), at(kNow), &shorter, &error));
        QVERIFY(estimateCompaction(*m_store, FullResolutionWindow::ofMonths(6), at(kNow), &longer, &error));
        QVERIFY(shorter.records > e.records);
        QVERIFY(shorter.bytes > e.bytes);
        QVERIFY(shorter.rebuildNeedBytes < e.rebuildNeedBytes);
        QCOMPARE(longer.records, qint64(0));
        QVERIFY(longer.rebuildNeedBytes > e.rebuildNeedBytes);
        QCOMPARE(longer.rebuildNeedBytes, rebuildNeedFor(longer.storeBytes, 0));
        QVERIFY(!estimateCompaction(*m_store, FullResolutionWindow::ofDays(7), at(kNow), &shorter, &error));

        // The real run removes exactly what the estimate said.
        const CompactionReport r = compactStore(*m_store, options());
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(r.removed.value("record"), e.records);
        QCOMPARE(r.removed.value("quarantine"), e.quarantine);
        QCOMPARE(r.removed.value("ingest"), e.ingest);
    }

    // ---- progress and cancel -------------------------------------------------------------------

    // Cancel in every phase leaves the store as it was (REQ-STO-15 stops a run this way when the
    // reading queue is about to overflow); the phases come in order and the bar never goes back.
    void progressAndCancel()
    {
        QVector<Record> all;
        const qint64 base = slotStart(kT0);
        for (int i = 0; i < 400; ++i) all << sample(kA, base + i * kMinute, 70 + (i * 13) % 17 * 7);
        QVERIFY(ingest(all));
        const QMap<QString, qint64> before = state();

        const OperationProgress::Phase phases[] = { OperationProgress::CheckingSpace, OperationProgress::Compacting,
                                                    OperationProgress::Finalizing };
        for (int p = 0; p < 3; ++p) {
            CompactionOptions o = options();
            bool cancelled = false;
            int after = 0;
            o.progress = [&](const OperationProgress &x) {
                if (cancelled) ++after;
                if (x.phase == phases[p]) { cancelled = true; return false; }
                return true;
            };
            const CompactionReport r = compactStore(*m_store, o);
            QVERIFY(!r.ok);
            QCOMPARE(r.failure.errorClass, CompactionError::cancelled());
            QCOMPARE(after, 0);
            QCOMPARE(state(), before);
            QVERIFY(!flagLeft());
        }

        CompactionOptions o = options();
        QList<OperationProgress> seen;
        o.progress = [&](const OperationProgress &x) { seen << x; return true; };
        const CompactionReport r = compactStore(*m_store, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        QCOMPARE(seen.first().phase, OperationProgress::CheckingSpace);
        QCOMPARE(seen.last().phase, OperationProgress::Done);
        QCOMPARE(seen.last().fraction, 1.0);
        double last = 0;
        int lastPhase = 0;
        QSet<int> got;
        foreach (const OperationProgress &x, seen) {
            QCOMPARE(x.steps, 3);
            QVERIFY(x.fraction >= last && x.fraction <= 1);
            QVERIFY(int(x.phase) >= lastPhase);
            last = x.fraction;
            lastPhase = int(x.phase);
            got << int(x.phase);
        }
        QVERIFY(got.contains(int(OperationProgress::Compacting)));
        QVERIFY(got.contains(int(OperationProgress::Finalizing)));
        QCOMPARE(phaseKey(OperationProgress::CheckingSpace), QString("checking-space"));
        QCOMPARE(phaseKey(OperationProgress::Compacting), QString("compacting"));
        QCOMPARE(phaseKey(OperationProgress::Finalizing), QString("finalizing"));
        QCOMPARE(phaseKey(OperationProgress::Done), QString("done"));
    }
};

QTEST_APPLESS_MAIN(TstCompaction)
#include "tst_compaction.moc"
