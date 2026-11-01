// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_FAKEBACKEND_H
#define LM_UI_FAKEBACKEND_H

// A scripted data source for DEVELOPMENT builds only (REQ-SIM-06): the whole interface
// runs on the emulator with no service, no credentials and no real source. The whole file is
// compiled only with the flag `devsource`, so a release or Harbour build contains neither the
// code nor a way to switch to it (there is no runtime switch).
//
// Scenarios, cycled by the development menu of the overview page: 0 everything normal, 1 problems (a low, a guard stop, terms to accept),
// 2 the service is stopped, 3 one fresh patient in each band (for the colour review on the
// emulator), 4 no account set up (the empty state). Series are generated from the clock at each call, so ages tick
// naturally and the plot always ends where the scenario says. The data are synthetic and
// invented; nothing here is a recording of anyone's glucose.

#ifdef LIBREMON_DEVSOURCE

#include "backend.h"

#include <QHash>

namespace lm {
namespace ui {

class FakeBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(int scenario READ scenario WRITE setScenario NOTIFY scenarioChanged)
    Q_PROPERTY(QString scenarioName READ scenarioName NOTIFY scenarioChanged)

public:
    explicit FakeBackend(QObject *parent = 0);

    int scenario() const { return m_scenario; }
    void setScenario(int s);
    QString scenarioName() const;
    Q_INVOKABLE void nextScenario();

    QList<SubjectSnapshot> subjects() const;
    ServiceStatus serviceStatus() const;
    qint64 lastHeartbeatMs() const;

    void retry(const QString &subjectId);
    void reenable(const QString &subjectId);
    void reportHandled(const QString &subjectId);
    void startMonitoring();
    void askAgain();
    // Development builds (LM_DEV_SERVICE): the strip shows this state instead of the script's own, so the
    // "older" and "newer" service states can be looked at on the emulator. Connecting = no override.
    void setServiceStatusOverride(ServiceStatus status) { m_statusOverride = status; emit changed(); }
    // The scripted journal's threshold history (kept by FakeAlertsBackend, which plays SetThresholds with
    // the core's own ThresholdBook): answered on the next turn of the event loop, as the store would.
    void requestThresholdHistory(const QString &subjectId);
    void setThresholdHistory(const QString &subjectId, const QList<ThresholdHistoryEntry> &entries);

signals:
    void scenarioChanged();

private:
    int m_scenario;
    // Buttons change the script: a re-enabled Subject becomes normal until the scenario changes.
    bool m_reenabled, m_retried, m_started, m_reportHandled;
    QHash<QString, QList<ThresholdHistoryEntry> > m_thresholdHistory;
    ServiceStatus m_statusOverride = ServiceStatus::Connecting;
};

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE

#endif
