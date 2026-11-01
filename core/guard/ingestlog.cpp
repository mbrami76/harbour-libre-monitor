// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "guard/ingestlog.h"

namespace lm {

IngestLog::IngestLog(IngestLogSink *sink, int maxQueued)
    : m_sink(sink), m_max(maxQueued > 0 ? maxQueued : 1), m_lostPending(0), m_lostTotal(0) {}

void IngestLog::append(const IngestLogEntry &entry)
{
    m_queue.append(entry);
    while (m_queue.size() > m_max) {
        m_queue.removeFirst();
        ++m_lostPending;
        ++m_lostTotal;
    }
    flush();
}

void IngestLog::flush()
{
    if (!m_sink)
        return;
    while (!m_queue.isEmpty()) {
        if (m_lostPending > 0) {
            // The count of what was dropped is written first, stamped like the entry that
            // follows it, so the log shows where the hole is.
            IngestLogEntry lost = m_queue.first();
            lost.event = QStringLiteral("entries-lost");
            lost.detail = QStringLiteral("%1 entries dropped while the store was busy").arg(m_lostPending);
            if (!m_sink->append(lost))
                return;
            m_lostPending = 0;
        }
        if (!m_sink->append(m_queue.first()))
            return;
        m_queue.removeFirst();
    }
}

} // namespace lm
