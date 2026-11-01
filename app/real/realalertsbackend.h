// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_REALALERTSBACKEND_H
#define LM_REAL_REALALERTSBACKEND_H

// The real alerts source of the screens: the monitoring service's `Alerts()` and its action methods
// (AcknowledgeAlert, SnoozeAlert, SetSilent, EndSilent, SetThresholds) over the
// asynchronous client (ServiceLink), so the window never waits for the bus.
//
// Pattern: an event-driven cache, like RealBackend. Nothing here polls (REQ-ARC-03): the UI asks `Alerts()` when it begins, after each `AlertChanged` or
// `SilentChanged` signal of the service (a burst of signals makes one call after a short pause), after
// the service's name reappeared, after an action was answered, and when the application comes back to
// the front (refresh()). The newest readable answer is kept; a failed call or a service that is not
// running makes the alerts "not known", so a page never says "no alerts" when it cannot know.
//
// Acknowledging from the application closes the notification: the service does it (//); this class only sends the request.
//
// An older service: a service built before the alerts (0.1-1) answers
// these calls with the D-Bus error "unknown method". The client tells that reason just before the failed
// answer (ServiceLink::callFailed); this class keeps it for that one answer, so the failure becomes the word
// "service-older" (the alerts are not known because the service is an older version) instead of
// "call-failed" (the service did not answer). The pages then show the text of the "older service" state.

#include "servicelink.h"
#include "ui/alertsbackend.h"

#include <QTimer>

namespace lm {
namespace real {

class RealAlertsBackend : public ui::AlertsBackend
{
    Q_OBJECT
public:
    explicit RealAlertsBackend(ServiceLink *link, QObject *parent = 0);

    // Asks the service at once (the start of the application, its return to the front).
    Q_INVOKABLE void refresh();

    ui::AlertsFacts facts() const { return m_facts; }
    void acknowledge(const QString &subject, const QString &slot);
    void snooze(const QString &subject, const QString &slot, int minutes);
    void setSilent(const QString &subject, int minutes);
    void endSilent(const QString &subject);
    void setThresholds(const QString &subject, int l, int h, int typedL, int typedH);

private slots:
    void onAlertsAnswered(bool ok, const QByteArray &json);
    void onSignal();

private:
    void answer(const QString &action, const QString &subject, bool ok, const QString &result,
                const char *method);
    // The word of a failed answer of `method`: "service-older" when the reason just received for that very
    // method was "unknown method" (the reason is used once and cleared), "call-failed" otherwise.
    QString failureWord(const char *method);

    ServiceLink *m_link;
    ui::AlertsFacts m_facts;
    QTimer m_coalesce;
    QString m_olderFailure;     // the D-Bus method whose failure was "unknown method", until its answer
};

} // namespace real
} // namespace lm

#endif
