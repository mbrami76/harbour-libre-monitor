// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The weekly storage check (REQ-STO-01, REQ-STO-02, REQ-SCH-14). Synthetic data only.
#include "record/registry.h"
#include "store/compactor.h"
#include "store/storagecheck.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;

namespace {

const qint64 kMinute = 60000;
const qint64 kHour = 60 * kMinute;
const qint64 kDay = 24 * kHour;
const qint64 kNow = 1781525400000LL;                    // 2026-06-15T12:10:00Z
const QUuid kA("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");

UtcTime at(qint64 ms) { return UtcTime::fromMSecsSinceEpoch(ms); }

JournalEntry beat(const QString &kind, qint64 t)
{
    JournalEntry e;
    e.kind = kind;
    e.wallTime = at(t);
    if (kind == JournalKind::start()) e.cause = StartCause::firstStart();
    e.bootTimeMs = 5000 + t % 1000;
    e.monotonicMs = 4200 + t % 1000;
    return e;
}

} // namespace

class TstStorageCheck : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_dir;
    Store *m_store;
    Journal *m_journal;

    QString path(const QString &name) const { return m_dir->path() + "/" + name; }

    StorageCheckOptions at_(qint64 now, qint64 free)
    {
        StorageCheckOptions o;
        o.now = at(now);
        o.freeSpace = [free](const QString &) { return free; };
        return o;
    }

    void ingestSome(int n)
    {
        QVector<IngestItem> v;
        for (int i = 0; i < n; ++i) {
            Record r;
            r.shape = Record::Sample;
            r.subject = kA;
            r.metric = metricGlucose();
            r.value = 70 + i % 100;
            r.unit = "mg/dL";
            r.measuredAt = at(kNow - kDay + i * kMinute);
            r.ingestedAt = at(kNow - kDay);
            r.originalTimestamp = "synthetic";
            r.source = "t.current";
            r.origin = Origin::Adapter;
            IngestItem it;
            it.record = r;
            v << it;
        }
        IngestContext c;
        c.now = at(kNow);
        c.adapter = "a";
        c.adapterVersion = "1";
        c.sessionId = "s";
        QVERIFY(m_store->ingest(v, c).ok);
    }

private slots:
    void init()
    {
        m_dir = new QTemporaryDir;
        m_store = new Store;
        m_journal = new Journal;
        QVERIFY(m_store->open(path("store.db")));
        QVERIFY(m_journal->open(path("journal.db")));
        ingestSome(300);
    }
    void cleanup()
    {
        delete m_store;
        delete m_journal;
        delete m_dir;
    }

    void theThresholdIsThreeTimesTheSizeOfStoreAndJournal()
    {
        StorageCheckOptions probe = at_(kNow, 1000000000);
        probe.prune = false;
        const StorageCheckResult first = runStorageCheck(*m_store, *m_journal, probe);
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(first.recorded && !first.skipped);
        QCOMPARE(first.storeBytes, m_store->sizeBytes());
        QVERIFY(first.storeBytes > 0);
        QVERIFY(first.journalBytes > 0);
        QCOMPARE(first.thresholdBytes, 3 * (first.storeBytes + first.journalBytes));
        QVERIFY(first.freeKnown);
        QCOMPARE(first.freeBytes, qint64(1000000000));
        QVERIFY(!first.low);
    }

    void lowIsStrictlyBelowThreeTimes()
    {
        // The comparison itself, with the sizes pinned: free = threshold is not low, one less is.
        StorageCheckOptions o = at_(kNow, 0);
        o.prune = false;
        qint64 threshold = 0;
        o.freeSpace = [&](const QString &) {
            threshold = 3 * (m_store->sizeBytes() + m_journal->sizeBytes());
            return threshold;
        };
        StorageCheckResult r = runStorageCheck(*m_store, *m_journal, o);
        QVERIFY(r.ok);
        QCOMPARE(r.thresholdBytes, threshold);
        QCOMPARE(r.freeBytes, r.thresholdBytes);
        QVERIFY(!r.low);

        o.now = at(kNow + 7 * kDay);
        o.freeSpace = [&](const QString &) {
            threshold = 3 * (m_store->sizeBytes() + m_journal->sizeBytes());
            return threshold - 1;
        };
        r = runStorageCheck(*m_store, *m_journal, o);
        QVERIFY(r.ok);
        QCOMPARE(r.freeBytes, r.thresholdBytes - 1);
        QVERIFY(r.low);
    }

    void unknownFreeSpaceIsNotFlaggedAndSaysSo()
    {
        StorageCheckOptions o = at_(kNow, -1);
        o.prune = false;
        const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, o);
        QVERIFY(r.ok);
        QVERIFY(!r.freeKnown);
        QVERIFY(!r.low);
        QCOMPARE(r.freeBytes, qint64(-1));
        const JournalEntry e = m_journal->entries(JournalKind::storageCheck()).last();
        QVERIFY(!e.extra.contains("free_bytes"));
        StorageCheckResult back;
        QVERIFY(lastStorageCheck(*m_journal, &back));
        QVERIFY(!back.freeKnown);
        QCOMPARE(back.freeBytes, qint64(-1));
    }

    void theResultIsRecordedInTheJournal()
    {
        StorageCheckOptions o = at_(kNow, 1);
        o.prune = false;
        const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, o);
        QVERIFY(r.ok && r.recorded);
        QVERIFY(r.low);                                     // 1 byte free
        const QVector<JournalEntry> entries = m_journal->entries(JournalKind::storageCheck());
        QCOMPARE(entries.size(), 1);
        const JournalEntry &e = entries[0];
        QCOMPARE(e.wallTime.msecsSinceEpoch(), kNow);
        QVERIFY(e.cause.isNull());
        QCOMPARE(qint64(e.extra.value("store_bytes").toDouble()), r.storeBytes);
        QCOMPARE(qint64(e.extra.value("journal_bytes").toDouble()), r.journalBytes);
        QCOMPARE(qint64(e.extra.value("free_bytes").toDouble()), qint64(1));
        QCOMPARE(qint64(e.extra.value("threshold_bytes").toDouble()), r.thresholdBytes);
        QVERIFY(e.extra.value("low").toBool());

        // It reads back for the UI.
        StorageCheckResult back;
        QVERIFY(lastStorageCheck(*m_journal, &back));
        QCOMPARE(back.time.msecsSinceEpoch(), kNow);
        QCOMPARE(back.storeBytes, r.storeBytes);
        QCOMPARE(back.journalBytes, r.journalBytes);
        QCOMPARE(back.thresholdBytes, r.thresholdBytes);
        QCOMPARE(back.freeBytes, qint64(1));
        QVERIFY(back.low && back.freeKnown);
    }

    void noCheckYetMeansNothingToRead()
    {
        StorageCheckResult r;
        QVERIFY(!lastStorageCheck(*m_journal, &r));
        QVERIFY(storageCheckDue(*m_journal, at(kNow)));
    }

    void itRunsOnceAWeekAndNotMoreOften()
    {
        QVERIFY(runStorageCheck(*m_store, *m_journal, at_(kNow, 1000000000)).recorded);

        // Asked again within the week, by the service: nothing is done, nothing recorded.
        const qint64 times[] = { kNow + 1000, kNow + kHour, kNow + 3 * kDay, kNow + 7 * kDay - 1 };
        for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
            const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, at_(times[i], 1000000000));
            QVERIFY(r.ok && r.skipped && !r.recorded);
            QVERIFY(!storageCheckDue(*m_journal, at(times[i])));
        }
        QCOMPARE(m_journal->entries(JournalKind::storageCheck()).size(), 1);

        // A week later it runs again, and so on: the notice repeats at each check while the
        // condition holds (REQ-STO-02).
        for (int week = 1; week <= 3; ++week) {
            const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, at_(kNow + week * 7 * kDay, 1));
            QVERIFY(r.ok && !r.skipped && r.recorded);
            QVERIFY(r.low);
        }
        QCOMPARE(m_journal->entries(JournalKind::storageCheck()).size(), 4);
        StorageCheckResult last;
        QVERIFY(lastStorageCheck(*m_journal, &last));
        QCOMPARE(last.time.msecsSinceEpoch(), kNow + 3 * 7 * kDay);
        QVERIFY(last.low);
    }

    void aUserActionCanForceACheck()
    {
        QVERIFY(runStorageCheck(*m_store, *m_journal, at_(kNow, 1000000000)).recorded);
        StorageCheckOptions o = at_(kNow + kHour, 1000000000);
        o.force = true;
        const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, o);
        QVERIFY(r.ok && !r.skipped && r.recorded);
        QCOMPARE(m_journal->entries(JournalKind::storageCheck()).size(), 2);
    }

    void aClockSetBackIsHandled()
    {
        QVERIFY(runStorageCheck(*m_store, *m_journal, at_(kNow, 1000000000)).recorded);
        // A little earlier than the last check (clock off by a little): not due.
        QVERIFY(!storageCheckDue(*m_journal, at(kNow - kHour)));
        QVERIFY(!storageCheckDue(*m_journal, at(kNow - 6 * kDay)));
        // Set back by a week or more: the old entry would block the checks for a long time.
        QVERIFY(storageCheckDue(*m_journal, at(kNow - 7 * kDay)));
        QVERIFY(storageCheckDue(*m_journal, at(kNow - 100 * kDay)));
    }

    void theJournalIsPrunedAtTheCheck()
    {
        // A run from 100 days ago with plenty of heartbeats, and a recent run.
        const qint64 old = kNow - 100 * kDay;
        QVERIFY(m_journal->append(beat(JournalKind::start(), old)));
        for (int i = 1; i <= 200; ++i)
            QVERIFY(m_journal->append(beat(JournalKind::heartbeat(), old + i * kMinute)));
        QVERIFY(m_journal->append(beat(JournalKind::start(), kNow - kDay)));
        for (int i = 1; i <= 20; ++i)
            QVERIFY(m_journal->append(beat(JournalKind::heartbeat(), kNow - kDay + i * kMinute)));
        const QVector<JournalGap> gaps = m_journal->gaps();

        const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, at_(kNow, 1000000000));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.pruned, qint64(199));                        // all but the last of the old run
        QVERIFY(r.pruneError.isNull());
        QCOMPARE(m_journal->entries(JournalKind::prune()).size(), 1);
        QCOMPARE(m_journal->entries(JournalKind::heartbeat()).size(), 1 + 20);
        QCOMPARE(m_journal->gaps(), gaps);                      // no gap lost its start
        const JournalEntry e = m_journal->entries(JournalKind::storageCheck()).last();
        QCOMPARE(qint64(e.extra.value("pruned").toDouble()), qint64(199));

        // With pruning off the heartbeats stay.
        QVERIFY(m_journal->append(beat(JournalKind::heartbeat(), old - 5 * kDay)));
        StorageCheckOptions o = at_(kNow + 7 * kDay, 1000000000);
        o.prune = false;
        const StorageCheckResult again = runStorageCheck(*m_store, *m_journal, o);
        QCOMPARE(again.pruned, qint64(0));
        QCOMPARE(m_journal->entries(JournalKind::prune()).size(), 1);
    }

    void aCheckAfterCompactionShowsTheSpaceThatCameBack()
    {
        // 20 days of one-minute samples, old enough for the default window.
        const qint64 base = 1769904000000LL;                    // 2026-02-01
        for (int d = 0; d < 20; ++d) {
            QVector<IngestItem> v;
            for (int i = 0; i < 1440; ++i) {
                Record r;
                r.shape = Record::Sample; r.subject = kA; r.metric = metricGlucose();
                r.value = 70 + ((d * 1440 + i) * 7) % 110; r.unit = "mg/dL";
                r.measuredAt = at(base + d * kDay + i * kMinute);
                r.ingestedAt = at(base + 25 * kDay);
                r.originalTimestamp = "synthetic"; r.source = "t.current"; r.origin = Origin::Adapter;
                IngestItem it; it.record = r; v << it;
            }
            IngestContext c;
            c.now = at(kNow); c.adapter = "a"; c.adapterVersion = "1"; c.sessionId = "s";
            QVERIFY(m_store->ingest(v, c).ok);
        }
        const StorageCheckResult before = runStorageCheck(*m_store, *m_journal, at_(kNow, 1000000000));
        QVERIFY(before.ok);

        CompactionOptions co;
        co.now = at(kNow);
        co.freeSpace = [](const QString &) { return qint64(1) << 40; };
        QVERIFY(compactStore(*m_store, co).ok);

        const StorageCheckResult after = runStorageCheck(*m_store, *m_journal, at_(kNow + 7 * kDay, 1000000000));
        QVERIFY(after.ok && after.recorded);
        QVERIFY2(after.storeBytes < before.storeBytes / 3, qPrintable(QString("%1 -> %2").arg(before.storeBytes).arg(after.storeBytes)));
        QVERIFY(after.thresholdBytes < before.thresholdBytes);
    }

    void theFileSystemIsAskedAtTheStoresDirectoryByDefault()
    {
        StorageCheckOptions o;
        o.now = at(kNow);
        o.prune = false;
        const StorageCheckResult r = runStorageCheck(*m_store, *m_journal, o);       // QStorageInfo
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.freeKnown);
        QVERIFY(r.freeBytes > 0);

        // The provider sees the directory of the store.
        QString asked;
        StorageCheckOptions p = at_(kNow + 7 * kDay, 1000000000);
        p.freeSpace = [&](const QString &dir) { asked = dir; return qint64(1000000000); };
        QVERIFY(runStorageCheck(*m_store, *m_journal, p).ok);
        QCOMPARE(asked, m_dir->path());
    }

    void closedStoresAreRefused()
    {
        Store closed;
        const StorageCheckResult r = runStorageCheck(closed, *m_journal, at_(kNow, 1));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(m_journal->entries(JournalKind::storageCheck()).size(), 0);
    }
};

QTEST_APPLESS_MAIN(TstStorageCheck)
#include "tst_storagecheck.moc"
