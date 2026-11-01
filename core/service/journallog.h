// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_JOURNALLOG_H
#define LM_JOURNALLOG_H

// The real ServiceJournal: the lifecycle's view Journal. Pattern: adapter. The
// journal API offers no "newest entry of these kinds" query, so the newest of each of the
// three kinds is taken from entries(kind), which are ordered by wall time and id. That reads
// a month of heartbeats once at start (about 43000 small entries at one a minute, REQ-SCH-14
// prunes older ones): acceptable once per start, not per wake-up. A direct query in the
// journal would be cheaper (optional).

#include "service/serviceports.h"

namespace lm {

class JournalLifecycleLog : public ServiceJournal
{
public:
    // The journal is not owned and must be open.
    explicit JournalLifecycleLog(Journal *journal) : m_journal(journal) {}

    bool lastLifecycleEntry(JournalEntry *entry);
    bool hasStartRecord();
    bool append(const JournalEntry &entry) { return m_journal->append(entry); }

private:
    Journal *m_journal;
};

} // namespace lm

#endif
