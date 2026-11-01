// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICEEVENTS_H
#define LM_SERVICEEVENTS_H

// The events the service passes on to the other modules and to the UI instance
// The service forwards; it does not interpret them:
// The scheduler decided what they mean, the alert engine turns them into alerts, the user interface into words.
//
// Pattern: observer with a fan-out. The scheduler accepts ONE listener, but several parties
// want the events (the lifecycle itself, the maintenance runner, the D-Bus adaptor, later the
// alert engine), so ServiceEventFanOut is the one listener and passes each call to every
// registered sink. Everything runs on the Qt main thread; no locking.

#include "scheduler/scheduler.h"

#include <QList>
#include <QUuid>

namespace lm {

// All callbacks have empty defaults. The scheduler-originated ones keep the signatures of
// SchedulerListener so the fan-out is a plain pass-through.
class ServiceEventSink
{
public:
    virtual ~ServiceEventSink() {}
    // Monitoring was not running from `from` to `to` (REQ-ALR-07).
    virtual void gapDetected(const UtcTime & /*from*/, const UtcTime & /*to*/) {}
    virtual void subjectStateChanged(const SubjectStatus &) {}
    virtual void stalenessChanged(const SubjectStatus &) {}
    virtual void connectionWarning(const QString & /*connection*/, const QString & /*kind*/,
                                   const QString & /*detail*/) {}
    // The reading queue reached its high-water mark: stop the maintenance run.
    virtual void readingQueueHighWater(int /*queuedRecords*/) {}
    // The queue overflowed and records were dropped (last resort, REQ-STO-15).
    virtual void readingQueueFull(int /*droppedRecords*/) {}
    // The guard ended an adapter a second time in a row: the UI may offer the diagnostic
    // report (REQ-GRD-15). The scheduler does not raise it yet, so nothing calls
    // this today; the interface is fixed here so that the D-Bus signal can be too.
    virtual void diagnosticReportProposed(const QString & /*connection*/) {}
    // New readings were stored for the Subject (REQ-ARC-02): a light signal, no data.
    virtual void dataChanged(const QUuid & /*subject*/) {}
    // The service asked for the one-time approval to read the stored login of a connection (at the
    // start and at each 15-minute re-raise), and the approval was given (or the wait ended).
    virtual void approvalRaised(const QString & /*connection*/) {}
    virtual void approvalSettled(const QString & /*connection*/) {}
    // An alert slot of a Subject changed what the user sees: raised, escalated, acknowledged,
    // snoozed, lowered or cleared. `slot` is the slot's name (low,
    // high, missing) and `state` the state's name after the change (active, acknowledged,
    // snoozed, cleared). Never a value, a name or a comment. A reminder is not a change.
    virtual void alertChanged(const QUuid & /*subject*/, const QString & /*slot*/,
                              const QString & /*state*/) {}
};

class ServiceEventFanOut : public SchedulerListener, public ServiceEventSink
{
public:
    // Sinks are not owned. Adding the same sink twice is ignored.
    void add(ServiceEventSink *sink);
    void remove(ServiceEventSink *sink);

    // SchedulerListener (called by the scheduler) and ServiceEventSink (called by the
    // service itself) both end in the same pass-through.
    void subjectStateChanged(const SubjectStatus &status);
    void stalenessChanged(const SubjectStatus &status);
    void connectionWarning(const QString &connection, const QString &kind, const QString &detail);
    void readingQueueFull(int droppedRecords);
    void gapDetected(const UtcTime &from, const UtcTime &to);
    void readingQueueHighWater(int queuedRecords);
    void diagnosticReportProposed(const QString &connection);
    void dataChanged(const QUuid &subject);
    void alertChanged(const QUuid &subject, const QString &slot, const QString &state);
    void approvalRaised(const QString &connection);
    void approvalSettled(const QString &connection);

private:
    QList<ServiceEventSink *> m_sinks;
};

// A ReadingStore that tells the sinks when readings were stored, so the UI instance can
// refresh without polling the store (REQ-ARC-02). Pattern: decorator; everything else is
// passed to the wrapped store unchanged. The scheduler never knows it is there.
class NotifyingReadingStore : public ReadingStore
{
public:
    // Neither is owned.
    NotifyingReadingStore(ReadingStore *inner, ServiceEventSink *sink)
        : m_inner(inner), m_sink(sink) {}

    UtcTime newestMeasurement(const QUuid &subject, const QString &source)
    { return m_inner->newestMeasurement(subject, source); }
    int ingest(const QUuid &subject, const LinkId &link, const QVector<Record> &records);
    void quarantine(const LinkId &link, const QString &reason, const QString &detail,
                    const QJsonObject &rawRecord)
    { m_inner->quarantine(link, reason, detail, rawRecord); }
    bool latestSensor(const QUuid &subject, UtcTime *activatedAt, int *warmupMinutes)
    { return m_inner->latestSensor(subject, activatedAt, warmupMinutes); }
    void noteSensor(const QUuid &subject, const QJsonObject &sensor)
    { m_inner->noteSensor(subject, sensor); }

private:
    ReadingStore *m_inner;
    ServiceEventSink *m_sink;
};

} // namespace lm

#endif
