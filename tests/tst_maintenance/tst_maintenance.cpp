// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of the maintenance runner (REQ-STO-15):
// a run on one worker thread, progress and result as signals, cancel, the stop at the
// high-water mark, and a REAL compaction on a worker connection of its own while the main
// thread keeps its connection to the same store. Compaction is the only
// maintenance job.

#include "record/registry.h"
#include "service/maintenancerunner.h"
#include "store/compactor.h"
#include "store/store.h"

#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace lm;

namespace {

const qint64 kNow = 1700000000000LL;

// Waits for finished() with a generous bound, running the event loop so queued signals arrive.
bool waitFinished(QSignalSpy &spy, int ms = 20000)
{
    return spy.count() > 0 || spy.wait(ms);
}

Record sample(const QUuid &subject, qint64 measuredAt, double value)
{
    Record r;
    r.shape = Record::Sample;
    r.subject = subject;
    r.metric = metricGlucose();
    r.value = value;
    r.unit = QStringLiteral("mg/dL");
    r.measuredAt = UtcTime::fromMSecsSinceEpoch(measuredAt);
    r.ingestedAt = UtcTime::fromMSecsSinceEpoch(kNow);
    r.originalTimestamp = QStringLiteral("1/1/2026 11:55:00 AM");
    r.source = QStringLiteral("libre.current");
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
    c.now = UtcTime::fromMSecsSinceEpoch(kNow);
    c.adapter = QStringLiteral("testadapter");
    c.adapterVersion = QStringLiteral("0.0.1");
    c.sessionId = QStringLiteral("s-1");
    return c;
}

OperationProgress progressOf(OperationProgress::Phase phase, qint64 done, qint64 total)
{
    OperationProgress p;
    p.phase = phase;
    p.step = 2;
    p.steps = 3;
    p.done = done;
    p.total = total;
    p.fraction = total ? double(done) / total : 0;
    return p;
}

} // namespace

class TstMaintenance : public QObject
{
    Q_OBJECT

private slots:
    void aRunReportsProgressAndItsResult();
    void theJobRunsInAnotherThread();
    void aFailureIsReportedWithItsClass();
    void onlyOneRunAtATime();
    void cancelStopsTheRunCleanly();
    void theHighWaterMarkStopsTheRun();
    void cancelWithoutARunDoesNothing();
    void destroyingTheRunnerCancelsAndJoins();
    void aRunThatFinishedBeforeTheCancelKeepsItsResult();
    void aRealCompactionRunsOnItsOwnConnection();
};

void TstMaintenance::aRunReportsProgressAndItsResult()
{
    MaintenanceRunner runner;
    QSignalSpy progress(&runner, SIGNAL(progress(QString,QString,int,int,qint64,qint64,double)));
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    QVERIFY(runner.start(MaintenanceKind::compaction(), [](const ProgressFn &fn) {
        fn(progressOf(OperationProgress::Compacting, 10, 100));
        fn(progressOf(OperationProgress::Compacting, 100, 100));
        return MaintenanceResult::success();
    }));
    QVERIFY(runner.isRunning());
    QVERIFY(waitFinished(finished));
    QCOMPARE(progress.count(), 2);
    QCOMPARE(progress.at(0).at(0).toString(), MaintenanceKind::compaction());
    QCOMPARE(progress.at(0).at(1).toString(), phaseKey(OperationProgress::Compacting));
    QCOMPARE(progress.at(1).at(4).toLongLong(), qint64(100));
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    QVERIFY(finished.at(0).at(4).toString().isEmpty());
    QVERIFY(!runner.isRunning());
}

void TstMaintenance::theJobRunsInAnotherThread()
{
    MaintenanceRunner runner;
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    QThread *main = QThread::currentThread();
    bool other = false;
    runner.start(MaintenanceKind::compaction(), [&](const ProgressFn &) {
        other = (QThread::currentThread() != main);
        return MaintenanceResult::success();
    });
    QVERIFY(waitFinished(finished));
    QVERIFY(other);
}

void TstMaintenance::aFailureIsReportedWithItsClass()
{
    MaintenanceRunner runner;
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    runner.start(MaintenanceKind::compaction(), [](const ProgressFn &) {
        return MaintenanceResult::failure(QStringLiteral("io"), QStringLiteral("no space"));
    });
    QVERIFY(waitFinished(finished));
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("io"));
    QCOMPARE(finished.at(0).at(3).toString(), QStringLiteral("no space"));
}

void TstMaintenance::onlyOneRunAtATime()
{
    MaintenanceRunner runner;
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    std::atomic<bool> release(false);
    QVERIFY(runner.start(MaintenanceKind::compaction(), [&](const ProgressFn &) {
        while (!release.load()) QThread::msleep(5);
        return MaintenanceResult::success();
    }));
    QVERIFY(!runner.start(MaintenanceKind::compaction(), [](const ProgressFn &) { return MaintenanceResult::success(); }));
    QCOMPARE(runner.runningKind(), MaintenanceKind::compaction());
    release.store(true);
    QVERIFY(waitFinished(finished));
    // Free again afterwards.
    QVERIFY(runner.start(MaintenanceKind::compaction(), [](const ProgressFn &) { return MaintenanceResult::success(); }));
    QSignalSpy second(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    QVERIFY(waitFinished(second));
}

void TstMaintenance::cancelStopsTheRunCleanly()
{
    MaintenanceRunner runner;
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    runner.start(MaintenanceKind::compaction(), [](const ProgressFn &fn) {
        for (int i = 0; i < 100000; ++i) {
            if (!fn(progressOf(OperationProgress::Compacting, i, 100000)))
                return MaintenanceResult::failure(QStringLiteral("cancelled"), QStringLiteral("cancelled"));
            QThread::msleep(1);
        }
        return MaintenanceResult::success();
    });
    QTest::qWait(30);
    runner.cancel();
    QVERIFY(waitFinished(finished));
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cancelled"));
    QCOMPARE(finished.at(0).at(4).toString(), CancelReason::user());
}

void TstMaintenance::theHighWaterMarkStopsTheRun()
{
    MaintenanceRunner runner;
    MaintenanceHighWaterGuard guard(&runner);
    ServiceEventFanOut fan;
    fan.add(&guard);
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    runner.start(MaintenanceKind::compaction(), [](const ProgressFn &fn) {
        for (int i = 0; i < 100000; ++i) {
            if (!fn(progressOf(OperationProgress::Compacting, i, 100000)))
                return MaintenanceResult::failure(QStringLiteral("cancelled"), QString());
            QThread::msleep(1);
        }
        return MaintenanceResult::success();
    });
    QTest::qWait(20);
    fan.readingQueueHighWater(16000);               // the lifecycle's event
    QVERIFY(waitFinished(finished));
    QCOMPARE(finished.at(0).at(4).toString(), CancelReason::queueHighWater());
}

void TstMaintenance::cancelWithoutARunDoesNothing()
{
    MaintenanceRunner runner;
    runner.cancel();
    QVERIFY(!runner.isRunning());
    // A later run is not affected by the earlier cancel.
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    runner.start(MaintenanceKind::compaction(), [](const ProgressFn &fn) {
        return fn(progressOf(OperationProgress::Compacting, 1, 2)) ? MaintenanceResult::success()
            : MaintenanceResult::failure(QStringLiteral("cancelled"), QString());
    });
    QVERIFY(waitFinished(finished));
    QVERIFY(finished.at(0).at(1).toBool());
}

void TstMaintenance::destroyingTheRunnerCancelsAndJoins()
{
    std::atomic<bool> sawCancel(false);
    {
        MaintenanceRunner runner;
        runner.start(MaintenanceKind::compaction(), [&](const ProgressFn &fn) {
            while (fn(progressOf(OperationProgress::Compacting, 0, 1)))
                QThread::msleep(1);
            sawCancel.store(true);
            return MaintenanceResult::failure(QStringLiteral("cancelled"), QString());
        });
        QTest::qWait(20);
    }
    QVERIFY(sawCancel.load());
}

void TstMaintenance::aRunThatFinishedBeforeTheCancelKeepsItsResult()
{
    MaintenanceRunner runner;
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    runner.start(MaintenanceKind::compaction(), [](const ProgressFn &) { return MaintenanceResult::success(); });
    runner.cancel(CancelReason::user());            // too late, maybe
    QVERIFY(waitFinished(finished));
    if (finished.at(0).at(1).toBool())
        QVERIFY(finished.at(0).at(4).toString().isEmpty());     // no reason for a run that completed
}

void TstMaintenance::aRealCompactionRunsOnItsOwnConnection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storePath = dir.path() + "/store.db";
    const QUuid subject("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
    const qint64 kDay = 86400000LL;

    // The service's own connection, in the main thread: 2,000 old one-minute samples of the
    // current feed (five per 5-minute slot, so compaction thins them), 400 old points of
    // the graph feed (never thinned) and 100 recent current samples.
    Store store;
    QVERIFY(store.open(storePath));
    for (int batch = 0; batch < 20; ++batch) {
        QVector<IngestItem> items;
        for (int i = 0; i < 100; ++i)
            items.append(item(sample(subject, kNow - 200 * kDay + (batch * 100 + i) * 60000, 100 + i % 50)));
        QVERIFY(store.ingest(items, context()).ok);
    }
    QVector<IngestItem> graph;
    for (int i = 0; i < 400; ++i) {
        Record g = sample(subject, kNow - 200 * kDay + i * 300000, 110);
        g.source = QStringLiteral("libre.graph");
        graph.append(item(g));
    }
    QVERIFY(store.ingest(graph, context()).ok);
    QVector<IngestItem> recent;
    for (int i = 0; i < 100; ++i)
        recent.append(item(sample(subject, kNow - kDay + i * 60000, 120)));
    QVERIFY(store.ingest(recent, context()).ok);
    const qint64 before = store.recordCount();

    MaintenanceRunner runner;
    QSignalSpy progress(&runner, SIGNAL(progress(QString,QString,int,int,qint64,qint64,double)));
    QSignalSpy finished(&runner, SIGNAL(finished(QString,bool,QString,QString,QString)));
    // The job opens its OWN writable connection inside the worker thread (a Qt SQL connection
    // belongs to the thread that opened it), as the service's job does.
    QVERIFY(runner.start(MaintenanceKind::compaction(), [=](const ProgressFn &fn) {
        Store s;
        if (!s.open(storePath))
            return MaintenanceResult::failure(QStringLiteral("io"), QStringLiteral("open"));
        CompactionOptions o;
        o.now = UtcTime::fromMSecsSinceEpoch(kNow);
        o.freeSpace = [](const QString &) { return qint64(1) << 40; };
        o.progress = fn;
        const CompactionReport r = compactStore(s, o);
        return r.ok ? MaintenanceResult::success() : MaintenanceResult::failure(r.failure.errorClass, r.failure.message);
    }));
    // Meanwhile the main thread keeps reading through its own connection, as the window does.
    Record latest;
    QVERIFY(store.latest(subject, metricGlucose(), QStringLiteral("libre.current"), true, &latest));
    QVERIFY(waitFinished(finished));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(3).toString()));
    QVERIFY(progress.count() > 0);
    QCOMPARE(progress.last().at(1).toString(), phaseKey(OperationProgress::Done));

    // The main connection sees the thinned store and writes again afterwards.
    const qint64 after = store.recordCount();
    QVERIFY2(after < before, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
    QVERIFY(after >= 100 + 400);
    RecordQuery graphQuery;
    graphQuery.source = QStringLiteral("libre.graph");
    QCOMPARE(store.records(graphQuery).size(), 400);           // the graph feed is untouched
    QVector<IngestItem> more;
    more.append(item(sample(subject, kNow, 130)));
    QVERIFY(store.ingest(more, context()).ok);
}

QTEST_GUILESS_MAIN(TstMaintenance)
#include "tst_maintenance.moc"
