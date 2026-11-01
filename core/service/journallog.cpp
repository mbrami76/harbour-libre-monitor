// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/journallog.h"

namespace lm {

bool JournalLifecycleLog::lastLifecycleEntry(JournalEntry *entry)
{
    const QString kinds[3] = { JournalKind::heartbeat(), JournalKind::start(), JournalKind::stop() };
    bool found = false;
    for (int k = 0; k < 3; ++k) {
        const QVector<JournalEntry> list = m_journal->entries(kinds[k]);
        if (list.isEmpty())
            continue;
        const JournalEntry &last = list.last();     // ordered by wall time and id
        // Strictly later wins; on a tie a stop beats a heartbeat beats a start, because the
        // stop is written last in a run and the start first.
        if (!found || last.wallTime.msecsSinceEpoch() > entry->wallTime.msecsSinceEpoch()
            || (last.wallTime == entry->wallTime && kinds[k] == JournalKind::stop())) {
            *entry = last;
            found = true;
        }
    }
    return found;
}

bool JournalLifecycleLog::hasStartRecord()
{
    return !m_journal->entries(JournalKind::start()).isEmpty();
}

} // namespace lm
