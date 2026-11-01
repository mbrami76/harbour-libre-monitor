// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ADAPTERLINKPORT_H
#define LM_ADAPTERLINKPORT_H

// The real link behind the scheduler's LinkPort (the AdapterLink). Pattern: adapter.
// The scheduler is written against the abstract LinkPort so that tests use a scripted fake;
// this thin wrapper forwards each call to the driver unchanged. The service wires the real
// driver; the wrapper exists here so that wiring is one line, and it is
// the only place that knows both worlds.

#include "scheduler/schedulerports.h"
#include "session/adapterlink.h"

namespace lm {

class AdapterLinkPort : public LinkPort
{
public:
    // Takes ownership of the link.
    explicit AdapterLinkPort(AdapterLink *link) : m_link(link) {}
    ~AdapterLinkPort() { delete m_link; }

    void start(const QString &adapterId, const QString &connectionId, const QJsonObject &settings,
               const StartCallback &done) { m_link->start(adapterId, connectionId, settings, done); }
    void authenticate(const Credentials &credentials, const QString &session, const AuthCallback &done)
    { m_link->authenticate(credentials, session, done); }
    void discoverSubjects(const DiscoverCallback &done) { m_link->discoverSubjects(done); }
    void fetchCurrent(const QString &subjectRef, const FetchCallback &done)
    { m_link->fetchCurrent(subjectRef, done); }
    void fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                    const QStringList &feeds, const FetchCallback &done)
    { m_link->fetchRange(subjectRef, from, to, feeds, done); }
    void stop(const StopCallback &done) { m_link->stop(done); }
    bool isAlive() const { return m_link->isAlive(); }
    LinkId linkId() const { return m_link->linkId(); }

private:
    AdapterLink *m_link;
};

// Makes AdapterLinks for the scheduler. `maker` builds one fully configured link (policy,
// sinks, channel factory, clock); the service supplies it.
class AdapterLinkFactory : public LinkFactory
{
public:
    typedef std::function<AdapterLink *(const QString &connectionId)> Maker;

    explicit AdapterLinkFactory(const Maker &maker) : m_maker(maker) {}
    LinkPort *create(const QString &connectionId) { return new AdapterLinkPort(m_maker(connectionId)); }

private:
    Maker m_maker;
};

} // namespace lm

#endif
