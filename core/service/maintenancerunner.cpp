// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/maintenancerunner.h"

#include <QThread>

namespace lm {

// The thread body. A QThread subclass with run() overridden, because nothing else needs to
// live in the worker: no event loop, no objects, just the job.
class MaintenanceRunner::Worker : public QThread
{
public:
    Worker(MaintenanceRunner *runner, const Job &job) : m_owner(runner), m_job(job) {}

protected:
    void run()
    {
        MaintenanceRunner *runner = m_owner;
        const QString kind = runner->m_kind;
        ProgressFn fn = [runner, kind](const OperationProgress &p) -> bool {
            // A queued signal: the slots live in the main thread.
            emit runner->progress(kind, phaseKey(p.phase), p.step, p.steps, p.done, p.total,
                                 p.fraction);
            return !runner->m_cancel.load();
        };
        MaintenanceResult r = m_job(fn);
        runner->m_result = r;    // read by the main thread only after the thread has finished
    }

private:
    MaintenanceRunner *m_owner;
    Job m_job;
};

MaintenanceRunner::MaintenanceRunner(QObject *parent)
    : QObject(parent), m_thread(0), m_worker(0), m_cancel(false)
{
}

MaintenanceRunner::~MaintenanceRunner()
{
    cancelAndWait();
}

bool MaintenanceRunner::start(const QString &kind, const Job &job)
{
    if (m_thread)
        return false;
    m_kind = kind;
    m_cancelReason.clear();
    m_cancel.store(false);
    m_result = MaintenanceResult();
    m_worker = new Worker(this, job);
    m_thread = m_worker;
    connect(m_worker, SIGNAL(finished()), this, SLOT(onThreadFinished()));
    m_worker->start();
    return true;
}

void MaintenanceRunner::cancel(const QString &reason)
{
    if (!m_thread)
        return;
    if (m_cancelReason.isEmpty())
        m_cancelReason = reason;
    m_cancel.store(true);
}

void MaintenanceRunner::cancelAndWait()
{
    if (!m_thread)
        return;
    cancel(CancelReason::shutdown());
    m_thread->wait();
    onThreadFinished();     // the finished() signal may not be delivered any more
}

void MaintenanceRunner::onThreadFinished()
{
    if (!m_thread)
        return;
    QThread *thread = m_thread;
    m_thread = 0;
    m_worker = 0;
    thread->wait();
    const MaintenanceResult result = m_result;
    const QString kind = m_kind;
    // A run that finished before the cancel took effect keeps its own result; the reason is
    // reported only when the operation ended as cancelled.
    const QString reason = (result.errorClass == QLatin1String("cancelled")) ? m_cancelReason : QString();
    thread->deleteLater();
    m_lastResult = result;
    emit finished(kind, result.ok, result.errorClass, result.message, reason);
}

} // namespace lm
