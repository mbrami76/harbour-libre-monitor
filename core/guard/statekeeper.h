// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STATEKEEPER_H
#define LM_STATEKEEPER_H

// The current persistent state of every connection and adapter version, shared by the guard
// and the scheduler. Pattern: event log plus fold. The
// StateLog holds the history of changes; the keeper loads it once, folds it into "the last
// entry per key wins, `clear` ends it" and keeps that view in memory, writing every further
// change through to the log.
//
// A write that fails (the journal is not writable right now) never changes the decision: the
// state applies in memory at once and the write is retried at every tick, so a trip cannot be
// lost to a transient fault while the process runs (REQ-GRD-06). The price, accepted and
// documented: if the process dies before the retry succeeds the state is lost.
//
// Time stamps are made strictly increasing (at least one millisecond apart), because two
// entries of the same millisecond would be ordered by a random id on reading and the fold
// would become ambiguous.

#include "guard/ports.h"
#include "guard/statelog.h"

#include <QHash>
#include <QSet>
#include <QVector>

namespace lm {

struct AttentionState
{
    AttentionState() {}
    bool active() const { return !cause.isEmpty(); }

    QString cause;
    QString detail;
    UtcTime since;
};

class StateKeeper
{
public:
    // Neither pointer is owned.
    StateKeeper(StateLog *log, Clock *clock);

    // Reads the log and folds it. Call once before use; false if the log could not be read.
    void load();

    // ---- changes (each is written through to the log) -----------------------------------
    void disableConnection(const QString &connection, const QString &adapter,
                           const QString &adapterVersion, const QString &cause,
                           const QString &detail);
    void disableVersion(const QString &adapter, const QString &adapterVersion,
                        const QString &detail);
    // `adapter` and `adapterVersion` record which version was running when the connection
    // needed attention, so that a later start with another version can try once more.
    void setAttention(const QString &connection, const QString &cause, const QString &detail,
                      const QString &adapter = QString(), const QString &adapterVersion = QString());
    void setHold(const QString &connection, const UtcTime &until, const QString &detail);
    // Ends the state of one connection (disabled, attention or hold), or of one version.
    void clearConnection(const QString &connection, const QString &detail);
    void clearVersion(const QString &adapter, const QString &adapterVersion,
                      const QString &detail);

    // ---- reading the folded state -------------------------------------------------------
    bool connectionDisabled(const QString &connection) const;
    QString connectionDisabledCause(const QString &connection) const;
    bool versionDisabled(const QString &adapter, const QString &adapterVersion) const;
    AttentionState attention(const QString &connection) const;
    // Invalid when no hold is active.
    UtcTime holdUntil(const QString &connection) const;
    // The adapter version recorded with the connection's attention entry; empty if none.
    QString attentionVersion(const QString &connection) const;
    // The adapter version recorded with the connection's latest persisted entry; empty if none.
    QString lastVersionOf(const QString &connection) const;
    // Distinct connections with a score trip of this adapter version since `now - windowMs`
    // (REQ-GRD-07). Reads the whole history, not only the fold, because a connection that
    // was acknowledged since still counts as having tripped.
    int connectionsTrippedWithin(const QString &adapter, const QString &adapterVersion,
                                 const UtcTime &now, qint64 windowMs) const;
    QStringList disabledConnections() const;

    // ---- persistence of failed writes ---------------------------------------------------
    void retryPending();
    int pendingWrites() const { return m_pending.size(); }

private:
    void record(StateEntry entry);
    void applyToFold(const StateEntry &entry);
    static QString keyOf(const StateEntry &entry);

    StateLog *m_log;
    Clock *m_clock;
    QVector<StateEntry> m_history;              // in written order
    QHash<QString, StateEntry> m_fold;          // key -> last entry whose state is not clear
    QVector<StateEntry> m_pending;              // not yet in the log, in order
    qint64 m_lastStampMs;
};

} // namespace lm

#endif
