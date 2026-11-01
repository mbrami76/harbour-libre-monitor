// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/noticewatcher.h"

namespace lm {

void NoticeWatcher::subjectStateChanged(const SubjectStatus &s)
{
    if (!m_presenter || s.connection.isEmpty())
        return;
    switch (s.state) {
    case ScheduleState::Backoff:
    case ScheduleState::NeedsAttention:
        m_attempts[s.connection] += 1;
        break;
    case ScheduleState::Idle:
        m_attempts[s.connection] = 0;
        break;
    default:
        break;
    }
    if (s.state == ScheduleState::DisabledByGuard) {
        // One notice per connection, replaced by a newer one; the count never reads as zero.
        const int attempts = qMax(1, m_attempts.value(s.connection));
        m_presenter->notifyGuardDisabled(s.connection, attempts, s.cause);
        m_disabled.insert(s.connection);
    } else if (m_disabled.contains(s.connection)) {
        m_disabled.remove(s.connection);
        m_presenter->guardEnabled(s.connection);
    }
}

void NoticeWatcher::diagnosticReportProposed(const QString &connection)
{
    if (m_presenter)
        m_presenter->notifyReportProposed(connection);
}

void NoticeWatcher::reportSettled(const QString &connection)
{
    if (m_presenter)
        m_presenter->reportSettled(connection);
}

void NoticeWatcher::approvalRaised(const QString &connection)
{
    if (m_presenter)
        m_presenter->notifyApproval(connection);
}

void NoticeWatcher::approvalSettled(const QString &connection)
{
    if (m_presenter)
        m_presenter->approvalSettled(connection);
}

} // namespace lm
