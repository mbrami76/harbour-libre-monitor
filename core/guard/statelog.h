// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STATELOG_H
#define LM_STATELOG_H

// Persistent memory of connection states (REQ-GRD-06).
// Three facts must survive a restart of the core and of the phone: a connection or an
// adapter version that the guard disabled (and that never comes back by itself), a
// connection that needs the caregiver (so a restart does not repeat a refused login and risk
// locking the account, REQ-SCH-05), and a rate-limit hold (so a restart does not hammer a
// source that has just refused). Pattern: an append-only log of state changes folded into
// the current state; the real implementation writes journal entries of the kind
// `connection-state` (JournalStateLog), the tests use MemoryStateLog.

#include "time/utctime.h"

#include <QString>
#include <QVector>

namespace lm {

namespace StateScope {
inline QString connection() { return QStringLiteral("connection"); }
inline QString adapterVersion() { return QStringLiteral("adapter-version"); }
}

namespace StateKind {
inline QString disabled() { return QStringLiteral("disabled"); }
inline QString attention() { return QStringLiteral("attention"); }
inline QString hold() { return QStringLiteral("hold"); }
inline QString clear() { return QStringLiteral("clear"); }
}

// Causes written to `cause`. Plain words, no personal data.
namespace StateCause {
inline QString score() { return QStringLiteral("score"); }                     // guard trip
inline QString adapterVersion() { return QStringLiteral("adapter-version"); }  // version trip
inline QString authFailed() { return QStringLiteral("auth-failed"); }
inline QString authFailedSuspected() { return QStringLiteral("auth-failed-suspected"); }
inline QString terms() { return QStringLiteral("terms"); }
inline QString protocolChanged() { return QStringLiteral("protocol-changed"); }
inline QString credentialsMissing() { return QStringLiteral("credentials-missing"); }
inline QString adapterRejected() { return QStringLiteral("adapter-rejected"); }
inline QString subjectUnavailable() { return QStringLiteral("subject-unavailable"); }
inline QString internal() { return QStringLiteral("internal"); }
inline QString rateLimited() { return QStringLiteral("rate-limited"); }
}

struct StateEntry
{
    StateEntry() {}

    UtcTime time;
    QString scope;              // StateScope
    QString connection;         // opaque id; empty for the version scope
    QString adapter;
    QString adapterVersion;
    QString state;              // StateKind
    QString cause;              // StateCause
    UtcTime until;              // hold only
    QString detail;             // no credentials, tokens, names or glucose values
};

class StateLog
{
public:
    virtual ~StateLog() {}
    // Appends one change. False = could not be written now; the caller retries.
    virtual bool append(const StateEntry &entry) = 0;
    // All entries in the order they were written (by time, then writing order).
    virtual QVector<StateEntry> load() = 0;
};

// In-memory log for tests and for the fallback when no journal is attached.
class MemoryStateLog : public StateLog
{
public:
    MemoryStateLog() : m_failWrites(false) {}

    bool append(const StateEntry &entry)
    {
        if (m_failWrites)
            return false;
        m_entries.append(entry);
        return true;
    }
    QVector<StateEntry> load() { return m_entries; }

    // Test hook: makes append() fail, as a journal that is not writable would.
    void setFailWrites(bool fail) { m_failWrites = fail; }
    int size() const { return m_entries.size(); }

private:
    QVector<StateEntry> m_entries;
    bool m_failWrites;
};

} // namespace lm

#endif
