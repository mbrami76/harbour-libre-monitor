// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/serviceevents.h"

namespace lm {

void ServiceEventFanOut::add(ServiceEventSink *sink)
{
    if (sink && !m_sinks.contains(sink))
        m_sinks.append(sink);
}

void ServiceEventFanOut::remove(ServiceEventSink *sink)
{
    m_sinks.removeAll(sink);
}

// A sink may remove itself while it is being called, so the loop works on a copy.
#define LM_FANOUT(call)                                                                 \
    do {                                                                                \
        const QList<ServiceEventSink *> sinks = m_sinks;                                \
        for (int i = 0; i < sinks.size(); ++i)                                          \
            sinks.at(i)->call;                                                          \
    } while (0)

void ServiceEventFanOut::subjectStateChanged(const SubjectStatus &s) { LM_FANOUT(subjectStateChanged(s)); }
void ServiceEventFanOut::stalenessChanged(const SubjectStatus &s) { LM_FANOUT(stalenessChanged(s)); }
void ServiceEventFanOut::connectionWarning(const QString &c, const QString &k, const QString &d)
{ LM_FANOUT(connectionWarning(c, k, d)); }
void ServiceEventFanOut::readingQueueFull(int n) { LM_FANOUT(readingQueueFull(n)); }
void ServiceEventFanOut::gapDetected(const UtcTime &f, const UtcTime &t) { LM_FANOUT(gapDetected(f, t)); }
void ServiceEventFanOut::readingQueueHighWater(int n) { LM_FANOUT(readingQueueHighWater(n)); }
void ServiceEventFanOut::diagnosticReportProposed(const QString &c) { LM_FANOUT(diagnosticReportProposed(c)); }
void ServiceEventFanOut::dataChanged(const QUuid &s) { LM_FANOUT(dataChanged(s)); }
void ServiceEventFanOut::approvalRaised(const QString &c) { LM_FANOUT(approvalRaised(c)); }
void ServiceEventFanOut::approvalSettled(const QString &c) { LM_FANOUT(approvalSettled(c)); }
void ServiceEventFanOut::alertChanged(const QUuid &s, const QString &slot, const QString &state)
{ LM_FANOUT(alertChanged(s, slot, state)); }

#undef LM_FANOUT

int NotifyingReadingStore::ingest(const QUuid &subject, const LinkId &link,
                                  const QVector<Record> &records)
{
    const int added = m_inner->ingest(subject, link, records);
    // -1 means the store was busy and nothing was written: there is nothing new to see.
    if (added > 0 && m_sink)
        m_sink->dataChanged(subject);
    return added;
}

} // namespace lm
