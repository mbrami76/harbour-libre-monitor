// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "guard/statekeeper.h"

namespace lm {

StateKeeper::StateKeeper(StateLog *log, Clock *clock)
    : m_log(log), m_clock(clock), m_lastStampMs(0) {}

QString StateKeeper::keyOf(const StateEntry &e)
{
    if (e.scope == StateScope::adapterVersion())
        return e.scope + QLatin1Char('|') + e.adapter + QLatin1Char('|') + e.adapterVersion;
    return e.scope + QLatin1Char('|') + e.connection;
}

void StateKeeper::applyToFold(const StateEntry &entry)
{
    const QString key = keyOf(entry);
    if (entry.state == StateKind::clear())
        m_fold.remove(key);
    else
        m_fold.insert(key, entry);
}

void StateKeeper::load()
{
    m_history.clear();
    m_fold.clear();
    if (!m_log)
        return;
    const QVector<StateEntry> all = m_log->load();
    for (int i = 0; i < all.size(); ++i) {
        m_history.append(all.at(i));
        applyToFold(all.at(i));
        if (all.at(i).time.isValid() && all.at(i).time.msecsSinceEpoch() > m_lastStampMs)
            m_lastStampMs = all.at(i).time.msecsSinceEpoch();
    }
}

void StateKeeper::record(StateEntry entry)
{
    qint64 ms = m_clock ? m_clock->now().msecsSinceEpoch() : 0;
    if (ms <= m_lastStampMs)
        ms = m_lastStampMs + 1;
    m_lastStampMs = ms;
    entry.time = UtcTime::fromMSecsSinceEpoch(ms);

    m_history.append(entry);
    applyToFold(entry);
    // Writes keep their order: while an older one is waiting, a newer one waits behind it.
    if (m_pending.isEmpty() && m_log && m_log->append(entry))
        return;
    m_pending.append(entry);
}

void StateKeeper::retryPending()
{
    while (!m_pending.isEmpty()) {
        if (!m_log || !m_log->append(m_pending.first()))
            return;
        m_pending.removeFirst();
    }
}

void StateKeeper::disableConnection(const QString &connection, const QString &adapter,
                                    const QString &adapterVersion, const QString &cause,
                                    const QString &detail)
{
    StateEntry e;
    e.scope = StateScope::connection();
    e.connection = connection;
    e.adapter = adapter;
    e.adapterVersion = adapterVersion;
    e.state = StateKind::disabled();
    e.cause = cause;
    e.detail = detail;
    record(e);
}

void StateKeeper::disableVersion(const QString &adapter, const QString &adapterVersion,
                                 const QString &detail)
{
    StateEntry e;
    e.scope = StateScope::adapterVersion();
    e.adapter = adapter;
    e.adapterVersion = adapterVersion;
    e.state = StateKind::disabled();
    e.cause = StateCause::adapterVersion();
    e.detail = detail;
    record(e);
}

void StateKeeper::setAttention(const QString &connection, const QString &cause,
                               const QString &detail, const QString &adapter,
                               const QString &adapterVersion)
{
    StateEntry e;
    e.adapter = adapter;
    e.adapterVersion = adapterVersion;
    e.scope = StateScope::connection();
    e.connection = connection;
    e.state = StateKind::attention();
    e.cause = cause;
    e.detail = detail;
    record(e);
}

void StateKeeper::setHold(const QString &connection, const UtcTime &until, const QString &detail)
{
    StateEntry e;
    e.scope = StateScope::connection();
    e.connection = connection;
    e.state = StateKind::hold();
    e.cause = StateCause::rateLimited();
    e.until = until;
    e.detail = detail;
    record(e);
}

void StateKeeper::clearConnection(const QString &connection, const QString &detail)
{
    StateEntry e;
    e.scope = StateScope::connection();
    e.connection = connection;
    e.state = StateKind::clear();
    e.detail = detail;
    record(e);
}

void StateKeeper::clearVersion(const QString &adapter, const QString &adapterVersion,
                               const QString &detail)
{
    StateEntry e;
    e.scope = StateScope::adapterVersion();
    e.adapter = adapter;
    e.adapterVersion = adapterVersion;
    e.state = StateKind::clear();
    e.detail = detail;
    record(e);
}

bool StateKeeper::connectionDisabled(const QString &connection) const
{
    const StateEntry e = m_fold.value(StateScope::connection() + QLatin1Char('|') + connection);
    return e.state == StateKind::disabled();
}

QString StateKeeper::connectionDisabledCause(const QString &connection) const
{
    return m_fold.value(StateScope::connection() + QLatin1Char('|') + connection).cause;
}

bool StateKeeper::versionDisabled(const QString &adapter, const QString &adapterVersion) const
{
    if (adapterVersion.isEmpty())
        return false;
    return m_fold.contains(StateScope::adapterVersion() + QLatin1Char('|') + adapter
                           + QLatin1Char('|') + adapterVersion);
}

AttentionState StateKeeper::attention(const QString &connection) const
{
    AttentionState a;
    const StateEntry e = m_fold.value(StateScope::connection() + QLatin1Char('|') + connection);
    if (e.state == StateKind::attention()) {
        a.cause = e.cause;
        a.detail = e.detail;
        a.since = e.time;
    }
    return a;
}

UtcTime StateKeeper::holdUntil(const QString &connection) const
{
    const StateEntry e = m_fold.value(StateScope::connection() + QLatin1Char('|') + connection);
    return e.state == StateKind::hold() ? e.until : UtcTime();
}

QString StateKeeper::attentionVersion(const QString &connection) const
{
    const StateEntry e = m_fold.value(StateScope::connection() + QLatin1Char('|') + connection);
    return e.state == StateKind::attention() ? e.adapterVersion : QString();
}

QString StateKeeper::lastVersionOf(const QString &connection) const
{
    for (int i = m_history.size() - 1; i >= 0; --i) {
        const StateEntry &e = m_history.at(i);
        if (e.scope == StateScope::connection() && e.connection == connection
            && !e.adapterVersion.isEmpty())
            return e.adapterVersion;
    }
    return QString();
}

int StateKeeper::connectionsTrippedWithin(const QString &adapter, const QString &adapterVersion,
                                          const UtcTime &now, qint64 windowMs) const
{
    QSet<QString> seen;
    const qint64 from = now.msecsSinceEpoch() - windowMs;
    for (int i = 0; i < m_history.size(); ++i) {
        const StateEntry &e = m_history.at(i);
        if (e.scope == StateScope::connection() && e.state == StateKind::disabled()
            && e.cause == StateCause::score() && e.adapter == adapter
            && e.adapterVersion == adapterVersion && e.time.msecsSinceEpoch() >= from)
            seen.insert(e.connection);
    }
    return seen.size();
}

QStringList StateKeeper::disabledConnections() const
{
    QStringList out;
    for (QHash<QString, StateEntry>::const_iterator it = m_fold.begin(); it != m_fold.end(); ++it)
        if (it.value().scope == StateScope::connection() && it.value().state == StateKind::disabled())
            out.append(it.value().connection);
    out.sort();
    return out;
}

} // namespace lm
