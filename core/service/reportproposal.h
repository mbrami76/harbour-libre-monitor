// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REPORTPROPOSAL_H
#define LM_REPORTPROPOSAL_H

// Remembers, per connection, whether the user should now be offered the diagnostic report for
// the developer (REQ-GRD-15: "after a second consecutive guard kill of the same
// connection"), so that `Status()` can say so. The D-Bus signal
// `DiagnosticReportProposed` alone is not enough: a user interface that was closed at that
// moment would never learn of it.
//
// Why it lives here and not in the scheduler: the scheduler shows a guard kill only
// as a Subject entering backoff with the cause "guard-kill" and does not count them, and it
// does not raise `diagnosticReportProposed` yet. The service already compensates for
// such gaps in this layer (see schedulerhost.h), so this class derives the same fact from the
// state events the scheduler does send. It is a small state machine, driven by those events,
// that never reads the system clock: the time it is given comes from the service's clock port.
//
// Pattern: observer (a ServiceEventSink) with an output sink. It is registered with the
// service's event fan-out to receive `subjectStateChanged`, and it calls
// `diagnosticReportProposed` on the output sink once per proposal.
//
// Rules (all provisional):
//  - A Subject entering backoff with the cause "guard-kill" counts one kill for its connection.
//  - The second kill without a successful contact in between proposes the report, once. A third
//    kill does not propose again while the proposal is open.
//  - A successful contact with the source (`lastContactOkAt` later than the latest kill) ends the
//    series: the count and the proposal are cleared, because the problem did not repeat.
//  - A manual re-enable (REQ-GRD-08) or the user's answer clears it too (clear()).
//  - Nothing is stored: after a restart of the service the count starts again at zero.
//
// Assumption: a guard kill that leads to a trip (DisabledByGuard) is not counted here; the user
// sees the Re-enable button for it.

#include "guard/ports.h"
#include "service/serviceevents.h"

#include <QHash>
#include <QString>

namespace lm {

class ReportProposalTracker : public ServiceEventSink
{
public:
    // Neither pointer is owned. `out` receives diagnosticReportProposed (the event fan-out).
    ReportProposalTracker(Clock *clock, ServiceEventSink *out) : m_clock(clock), m_out(out) {}

    void subjectStateChanged(const SubjectStatus &status);

    // True while a proposal for the connection is open (the user has not yet been released
    // from it by a success, a re-enable or clear()).
    bool proposed(const QString &connection) const;
    int consecutiveKills(const QString &connection) const;

    void clear(const QString &connection);
    void clearAll();

private:
    struct Series
    {
        Series() : kills(0), proposed(false) {}
        int kills;
        bool proposed;
        UtcTime lastKill;
    };

    Clock *m_clock;
    ServiceEventSink *m_out;
    QHash<QString, Series> m_series;
};

} // namespace lm

#endif
