// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_JOURNALSTATELOG_H
#define LM_JOURNALSTATELOG_H

// StateLog on the service journal: each state change is one journal entry of the kind
// `connection-state`. Why the journal: it is a database file of its own, so store
// maintenance cannot block a trip from being persisted (REQ-STO-15); it is append-only, its
// connection-state entries are never pruned, and it travels in the archive. The journal is
// not owned.

#include "guard/statelog.h"
#include "store/journal.h"

namespace lm {

class JournalStateLog : public StateLog
{
public:
    explicit JournalStateLog(Journal *journal) : m_journal(journal) {}

    bool append(const StateEntry &entry);
    QVector<StateEntry> load();

private:
    Journal *m_journal;
};

} // namespace lm

#endif
