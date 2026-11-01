// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "guard/journalstatelog.h"

namespace lm {

bool JournalStateLog::append(const StateEntry &s)
{
    if (!m_journal || !m_journal->isOpen())
        return false;
    JournalEntry e;
    e.kind = JournalKind::connectionState();
    e.wallTime = s.time;
    e.cause = s.cause.isEmpty() ? QString() : s.cause;
    e.extra.insert(ConnectionStateKey::scope(), s.scope);
    e.extra.insert(ConnectionStateKey::connection(), s.connection);
    e.extra.insert(ConnectionStateKey::adapter(), s.adapter);
    e.extra.insert(ConnectionStateKey::adapterVersion(), s.adapterVersion);
    e.extra.insert(ConnectionStateKey::state(), s.state);
    if (s.until.isValid())
        e.extra.insert(ConnectionStateKey::until(), s.until.toIso());
    if (!s.detail.isEmpty())
        e.extra.insert(ConnectionStateKey::detail(), s.detail);
    return m_journal->append(e);
}

QVector<StateEntry> JournalStateLog::load()
{
    QVector<StateEntry> out;
    if (!m_journal || !m_journal->isOpen())
        return out;
    const QVector<JournalEntry> all = m_journal->entries(JournalKind::connectionState());
    for (int i = 0; i < all.size(); ++i) {
        const JournalEntry &e = all.at(i);
        StateEntry s;
        s.time = e.wallTime;
        s.cause = e.cause;
        s.scope = e.extra.value(ConnectionStateKey::scope()).toString();
        s.connection = e.extra.value(ConnectionStateKey::connection()).toString();
        s.adapter = e.extra.value(ConnectionStateKey::adapter()).toString();
        s.adapterVersion = e.extra.value(ConnectionStateKey::adapterVersion()).toString();
        s.state = e.extra.value(ConnectionStateKey::state()).toString();
        s.until = UtcTime::fromIso(e.extra.value(ConnectionStateKey::until()).toString());
        s.detail = e.extra.value(ConnectionStateKey::detail()).toString();
        out.append(s);
    }
    return out;
}

} // namespace lm
