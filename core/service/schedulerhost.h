// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDULERHOST_H
#define LM_SCHEDULERHOST_H

// The real SchedulerHost: the lifecycle's narrow view Scheduler. Pattern:
// adapter. It forwards unchanged, except for two things the scheduler does not offer today
// there is no "have all links ended" question, so the links are
// counted by a decorating factory (TrackingLinkFactory) that the service puts between the
// scheduler and the real factory; and there is no way to flush the
// held readings from outside, so flushQueuedReadings() can only report how many are still
// held. The lifecycle then waits its bounded time and counts them as lost. Until the
// scheduler gets a public flush this is the honest behaviour: readings held at stop are
// lost and logged, and the next start refetches the overlapping windows.

#include "service/serviceports.h"

namespace lm {

// Decorates the LinkFactory of the scheduler so the service can ask "is any adapter process
// still alive?". The scheduler retires a link at shutdown() but only deletes it at a later
// tick(), which never comes once it is stopped; the process behind it is what the stop
// sequence must wait for (step 1), so it is asked through isAlive().
class TrackingLinkFactory : public LinkFactory
{
public:
    explicit TrackingLinkFactory(LinkFactory *inner) : m_inner(inner) {}
    ~TrackingLinkFactory();

    LinkPort *create(const QString &connectionId);
    bool anyAlive() const;

private:
    class Tracked;
    friend class Tracked;
    LinkFactory *m_inner;
    QList<Tracked *> m_live;
};

class SchedulerHostAdapter : public SchedulerHost
{
public:
    // Neither is owned. `links` is the factory the scheduler was given.
    SchedulerHostAdapter(Scheduler *scheduler, const TrackingLinkFactory *links)
        : m_scheduler(scheduler), m_links(links) {}

    void start(const UtcTime &serviceDownSince) { m_scheduler->start(serviceDownSince); }
    void tick() { m_scheduler->tick(); }
    UtcTime nextWakeAt() const { return m_scheduler->nextWakeAt(); }
    void shutdown() { m_scheduler->shutdown(); }
    bool linksEnded() const { return !m_links->anyAlive(); }
    int queuedReadings() const { return m_scheduler->queuedReadings(); }
    int flushQueuedReadings() { return m_scheduler->queuedReadings(); }
    void secretsAvailable(const QString &connectionId) { m_scheduler->secretsAvailable(connectionId); }

private:
    Scheduler *m_scheduler;
    const TrackingLinkFactory *m_links;
};

} // namespace lm

#endif
