// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/reportproposal.h"

namespace lm {

void ReportProposalTracker::subjectStateChanged(const SubjectStatus &status)
{
    if (status.connection.isEmpty())
        return;
    Series &series = m_series[status.connection];

    // A success after the latest kill ends the series. Checked first, so that the state change
    // that follows a good poll (back to idle) is what clears it.
    if (series.kills > 0 && status.lastContactOkAt.isValid() && series.lastKill.isValid()
        && series.lastKill < status.lastContactOkAt) {
        series.kills = 0;
        series.proposed = false;
    }

    if (status.state == ScheduleState::Backoff && status.cause == QLatin1String("guard-kill")) {
        ++series.kills;
        series.lastKill = m_clock->now();
        if (series.kills >= 2 && !series.proposed) {
            series.proposed = true;
            if (m_out)
                m_out->diagnosticReportProposed(status.connection);
        }
    }
}

bool ReportProposalTracker::proposed(const QString &connection) const
{
    return m_series.value(connection).proposed;
}

int ReportProposalTracker::consecutiveKills(const QString &connection) const
{
    return m_series.value(connection).kills;
}

void ReportProposalTracker::clear(const QString &connection)
{
    m_series.remove(connection);
}

void ReportProposalTracker::clearAll()
{
    m_series.clear();
}

} // namespace lm
