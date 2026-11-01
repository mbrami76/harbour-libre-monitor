// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_INGESTLOG_H
#define LM_INGESTLOG_H

// Bounded in-memory queue in front of the ingest log (
// REQ-GRD-10, REQ-GRD-11, REQ-STO-15). Why a queue: while the store runs export, reimport
// or compaction it can refuse writes for tens of seconds, yet monitoring and the guard must
// carry on and "every violation is written" must stay true. Entries wait here and are
// written when the store is free again, in order. The queue is bounded: on overflow the
// oldest entries are dropped and counted, and one `entries-lost` entry with the count is
// written before the next entry, so the loss is itself visible and never silent.

#include "guard/ports.h"

#include <QList>

namespace lm {

class IngestLog
{
public:
    // The sink is not owned. maxQueued is kIngestQueueMax.
    IngestLog(IngestLogSink *sink, int maxQueued);

    // Queues the entry and tries to write everything queued.
    void append(const IngestLogEntry &entry);
    // Tries to write the queue now (called on every tick of the scheduler).
    void flush();

    int queued() const { return m_queue.size(); }
    int lost() const { return m_lostPending; }
    qint64 lostTotal() const { return m_lostTotal; }

private:
    IngestLogSink *m_sink;
    int m_max;
    QList<IngestLogEntry> m_queue;
    int m_lostPending;
    qint64 m_lostTotal;
};

} // namespace lm

#endif
