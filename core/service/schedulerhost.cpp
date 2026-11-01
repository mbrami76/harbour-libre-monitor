// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/schedulerhost.h"

namespace lm {

// A link that forwards everything to the real one and tells the factory when it is deleted.
class TrackingLinkFactory::Tracked : public LinkPort
{
public:
    Tracked(TrackingLinkFactory *factory, LinkPort *inner) : m_owner(factory), m_inner(inner) {}
    ~Tracked()
    {
        if (m_owner)
            m_owner->m_live.removeAll(this);
        delete m_inner;
    }
    void orphan() { m_owner = 0; }

    void start(const QString &adapterId, const QString &connectionId, const QJsonObject &settings,
               const StartCallback &done) { m_inner->start(adapterId, connectionId, settings, done); }
    void authenticate(const Credentials &c, const QString &session, const AuthCallback &done)
    { m_inner->authenticate(c, session, done); }
    void discoverSubjects(const DiscoverCallback &done) { m_inner->discoverSubjects(done); }
    void fetchCurrent(const QString &ref, const FetchCallback &done) { m_inner->fetchCurrent(ref, done); }
    void fetchRange(const QString &ref, const UtcTime &from, const UtcTime &to,
                    const QStringList &feeds, const FetchCallback &done)
    { m_inner->fetchRange(ref, from, to, feeds, done); }
    void stop(const StopCallback &done) { m_inner->stop(done); }
    bool isAlive() const { return m_inner->isAlive(); }
    LinkId linkId() const { return m_inner->linkId(); }

private:
    TrackingLinkFactory *m_owner;
    LinkPort *m_inner;
};

TrackingLinkFactory::~TrackingLinkFactory()
{
    for (int i = 0; i < m_live.size(); ++i)
        m_live.at(i)->orphan();     // the scheduler still owns and deletes them
}

LinkPort *TrackingLinkFactory::create(const QString &connectionId)
{
    Tracked *link = new Tracked(this, m_inner->create(connectionId));
    m_live.append(link);
    return link;
}

bool TrackingLinkFactory::anyAlive() const
{
    for (int i = 0; i < m_live.size(); ++i)
        if (m_live.at(i)->isAlive())
            return true;
    return false;
}

} // namespace lm
