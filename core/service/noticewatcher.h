// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_NOTICEWATCHER_H
#define LM_NOTICEWATCHER_H

// THE NOTICES THAT ARE NOT ALERTS: waiting for approval, a
// connection disabled by the guard, a diagnostic report proposed. They come from the service's own
// events, not from the alert engine, so this small sink listens to the events and asks the
// presenter to post or withdraw the notice (the interruption notice and the "ended" entries come
// from the engine's effects and are the presenter's own).
//
// Pattern: observer (a ServiceEventSink) with one memory per connection: whether the guard has it
// disabled, and how many failed attempts led to it. The guard does not count attempts for the
// notice, so the count is taken here from the scheduler's own state events: every time a Subject of
// the connection goes into backoff or needs attention, one more, and a return to idle starts the
// count again. It is the number the user is told ("disabled after N failed attempts"); it is
// best-effort and never below 1. Nothing here is a name or a credential: the words are the
// presenter's, built from opaque ids.
//
// None of these notices is ever covered by a Subject's silent mode.

#include "service/alertnotification.h"

#include <QHash>
#include <QSet>

namespace lm {

class NoticeWatcher : public ServiceEventSink
{
public:
    // The presenter is not owned and must outlive the watcher.
    explicit NoticeWatcher(AlertNotificationPresenter *presenter) : m_presenter(presenter) {}

    void subjectStateChanged(const SubjectStatus &status);
    void diagnosticReportProposed(const QString &connection);
    void approvalRaised(const QString &connection);
    void approvalSettled(const QString &connection);
    // The user handled the report or re-enabled the connection (the runtime calls these).
    void reportSettled(const QString &connection);

private:
    AlertNotificationPresenter *m_presenter;
    QHash<QString, int> m_attempts;         // failures since the last return to idle, per connection
    QSet<QString> m_disabled;               // connections with a guard notice up
};

} // namespace lm

#endif
