// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_MAINTENANCERUNNER_H
#define LM_MAINTENANCERUNNER_H

// Runs the store's maintenance (since only compaction: no export, no import) inside the
// service without stopping the polling (REQ-STO-15).
// Compaction can hold the store for tens of seconds; the main thread must keep ticking meanwhile, and the scheduler
// holds readings in its queue while the store is busy.
//
// Pattern: one worker thread per run, started and joined by this class, and the ONLY thread
// besides the Qt main thread in the whole service (, documented
// here). The job is a function the caller builds; it opens its OWN store connection inside
// the worker (a Qt SQL connection must be used only in the thread that opened it) and calls
// the progress function now and then. Progress and the result reach the main thread as
// queued signals, so no state is shared except one atomic cancel flag. At most one run at a
// time.
//
// Cancel: the progress function the job receives returns false once cancel() was called; the
// compaction then rolls back (the store is untouched) and returns the class
// "cancelled". A job that never calls the progress function cannot be
// cancelled; compaction does.
//
// Assumptions: a second store connection on a worker thread works with the store's busy
// handling; the
// job does not touch any object of the main thread.

#include "service/serviceevents.h"
#include "store/progress.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>

class QThread;

namespace lm {

struct MaintenanceResult
{
    MaintenanceResult() : ok(false) {}
    static MaintenanceResult success() { MaintenanceResult r; r.ok = true; return r; }
    static MaintenanceResult failure(const QString &errorClass, const QString &message)
    { MaintenanceResult r; r.errorClass = errorClass; r.message = message; return r; }

    bool ok;
    QString errorClass;         // CompactionError::* (store/compactor.h), "cancelled" among them; a purge
                                // can also give PurgeError::* (store/purge.h)
    QString message;            // no names, no paths, no record content
    QJsonObject details;        // the estimate of the space check and the numbers removed (maintenancejobs.h)
};

namespace MaintenanceKind {
inline QString compaction() { return QStringLiteral("compaction"); }
// The purge of one Subject's old data (REQ-STO-17) and its dry run, which changes
// nothing and only reports the estimate. Two kinds, so that a window reading MaintenanceFinished
// never mistakes an estimate for a purge that was done.
inline QString purge() { return QStringLiteral("purge"); }
inline QString purgeEstimate() { return QStringLiteral("purge-estimate"); }
}

// Why a run was cancelled (reported with the result).
namespace CancelReason {
inline QString user() { return QStringLiteral("user"); }
inline QString queueHighWater() { return QStringLiteral("queue-high-water"); }
inline QString shutdown() { return QStringLiteral("shutdown"); }
}

class MaintenanceRunner : public QObject
{
    Q_OBJECT
public:
    // The job runs in the worker thread. `progress` returns false when the run is cancelled.
    typedef std::function<MaintenanceResult(const ProgressFn &progress)> Job;

    explicit MaintenanceRunner(QObject *parent = 0);
    ~MaintenanceRunner();

    // False when a run is active. Main thread only.
    bool start(const QString &kind, const Job &job);
    bool isRunning() const { return m_thread != 0; }
    QString runningKind() const { return m_kind; }
    // The full result of the run that finished last (the details are not in the signal).
    MaintenanceResult lastResult() const { return m_lastResult; }

    // Asks the run to stop cleanly; the result arrives through finished(). No effect when
    // nothing runs. The first reason wins.
    void cancel(const QString &reason = CancelReason::user());
    // Cancel and wait until the worker is gone; used when the service stops.
    void cancelAndWait();

signals:
    // The phase key is stable (progress.h phaseKey) and translated by the UI. Throttled by
    // the operation (at most one per 100 ms within a phase).
    void progress(const QString &kind, const QString &phase, int step, int steps, qint64 done,
                  qint64 total, double fraction);
    // `cancelReason` is empty unless the run was cancelled.
    void finished(const QString &kind, bool ok, const QString &errorClass, const QString &message,
                  const QString &cancelReason);

private slots:
    void onThreadFinished();

private:
    class Worker;

    QThread *m_thread;
    Worker *m_worker;
    QString m_kind;
    QString m_cancelReason;
    std::atomic<bool> m_cancel;
    MaintenanceResult m_result;
    MaintenanceResult m_lastResult;
};

// Stops the running maintenance at the high-water mark of the reading queue (REQ-STO-15):
// the readings the scheduler holds while the store is busy must never overflow, and
// the user is told by the UI through the same event. Register it with the event fan-out.
class MaintenanceHighWaterGuard : public ServiceEventSink
{
public:
    explicit MaintenanceHighWaterGuard(MaintenanceRunner *runner) : m_runner(runner) {}
    void readingQueueHighWater(int) { m_runner->cancel(CancelReason::queueHighWater()); }

private:
    MaintenanceRunner *m_runner;
};

} // namespace lm

#endif
