// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_FAKEALERTSBACKEND_H
#define LM_UI_FAKEALERTSBACKEND_H

// A scripted alerts source for DEVELOPMENT builds only (REQ-SIM-06), the companion of
// fakebackend.h: the alert parts of the screens run on the emulator with no service. Compiled only with
// the flag `devsource`; a release build has neither the code nor a switch to it.
//
// It plays the service's part for the scripted patients of each scenario of FakeBackend: 0 a very low
// alert not acknowledged, 1 a low alert and a no-data alert (the guard stop), 3 one alert in each of
// the four glucose levels in different states, 2 and 4 none. The actions change the script like the
// service would (an acknowledged very low becomes a pause, a snooze sets its end, silent mode takes the
// service's encoding, "until tomorrow" is the next local 06:00 or the one after when it is less than an
// hour away, as the service does); SetThresholds is answered by the core's ThresholdBook, the same
// checks as the service (scenario 3 starts with one edit in the history); a change of scenario starts
// again. Synthetic and invented.

#ifdef LIBREMON_DEVSOURCE

#include "alertsbackend.h"
#include "fakebackend.h"
#include "alerts/thresholdbook.h"

#include <QHash>

namespace lm {
namespace ui {

class FakeAlertsBackend : public AlertsBackend
{
    Q_OBJECT
public:
    explicit FakeAlertsBackend(FakeBackend *subjects, QObject *parent = 0);

    AlertsFacts facts() const;
    void acknowledge(const QString &subject, const QString &slot);
    void snooze(const QString &subject, const QString &slot, int minutes);
    void setSilent(const QString &subject, int minutes);
    void endSilent(const QString &subject);
    void setThresholds(const QString &subject, int l, int h, int typedL, int typedH);

private:
    void reset();
    AlertFact *find(const QString &subject, const QString &slot);

    FakeBackend *m_subjects;
    QList<AlertFact> m_alerts;
    QHash<QString, qint64> m_silent;        // subject -> end (0 = without limit)
    // The thresholds play the service's part with the core's own book (the same checks, the same
    // answers), so the page's every refusal can be seen on the emulator without a service.
    ThresholdBook m_book;
    void publishHistory(const QString &subject);
};

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE

#endif
