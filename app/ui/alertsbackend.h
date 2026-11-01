// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ALERTSBACKEND_H
#define LM_UI_ALERTSBACKEND_H

// What the alert parts of the screens need from the rest of the application: the alerts that are active, acknowledged
// or snoozed, the thresholds and silent mode of every patient, and the user's actions on them.
// One abstract class with two implementations: the real one on the monitoring service's
// asynchronous D-Bus client (app/real/realalertsbackend.h, `Alerts()` and the action methods of
//) and a scripted one for development builds (fakealertsbackend.h).
//
// Pattern: a snapshot interface, like backend.h. The backend keeps the newest answer of the service
// as plain values and emits changed() when it moved; the models decide what the screens say from
// these facts and the clock. Nothing here decides a band or an alert (REQ-UI-07): the service's
// alert engine does, and this port only carries what it reported.
//
// Shaped after the service's answer as built ("The alerts"): a slot is
// `low`, `high` or `missing`, a level `LL`, `L`, `H`, `HH` or `MISSING`, a state `active`,
// `acknowledged` or `snoozed`. Times are kept as milliseconds since the epoch (0 = none). No glucose
// value and no name is ever in it: the service's answer has none, and the names of the patients
// come from the configuration file through the other backend.
//
// The thresholds edit (SetThresholds) was; the thresholds in force, the three alert
// times and their ranges come with Alerts(). The history of the edits is not here: it is read from the
// journal read-only (backend.h, requestThresholdHistory), as the service has no method for it.

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

namespace lm {
namespace ui {

// One alert slot of one patient, as the service reported it.
struct AlertFact
{
    AlertFact() : acknowledged(false), sinceMs(0), levelSinceMs(0), snoozeUntilMs(0), limitMgdl(-1) {}

    QString subject;            // the patient's UUID in braces, as in Status()
    QString slot;               // "low", "high", "missing"
    QString level;              // "LL", "L", "H", "HH", "MISSING" (internal identifiers, never shown)
    QString state;              // "active", "acknowledged", "snoozed"
    QString episode;            // the occurrence's UUID: tells a new alert from the same one
    bool acknowledged;
    qint64 sinceMs;             // when this state began
    qint64 levelSinceMs;        // when the current level began: the "since" the pages show
    qint64 snoozeUntilMs;       // 0 unless snoozed
    int limitMgdl;              // the boundary crossed in whole mg/dL; -1 for no data
    QString cause;              // the staleness cause of a no-data alert ("" otherwise)
};

// The settings in force for one followed patient (alert or not).
struct PatientAlertFacts
{
    PatientAlertFacts() : ll(-1), l(-1), h(-1), hh(-1), persistenceLowS(-1), persistenceHighS(-1),
                          missingLimitS(-1), silent(false), silentUntilMs(0) {}

    QString subject;
    int ll, l, h, hh;           // whole mg/dL; -1 when the answer did not carry them
    int persistenceLowS, persistenceHighS, missingLimitS;
    bool silent;
    qint64 silentUntilMs;       // 0 with `silent` = without limit
};

// The minimum, maximum and default of one alert time, in seconds; -1 when unknown.
struct AlertTimeRange
{
    AlertTimeRange() : minS(-1), maxS(-1), defaultS(-1) {}
    int minS, maxS, defaultS;
};

struct AlertsFacts
{
    AlertsFacts() : known(false), doNotDisturb(false) {}

    // False until a readable answer arrived; `error` then says why ("service-not-running",
    // "call-failed", "service-older" for a service without the method, "unreadable"), and the lists are empty: the pages say that the alerts are not
    // known, never that there are none.
    bool known;
    QString error;
    QList<AlertFact> alerts;
    QList<PatientAlertFacts> patients;
    AlertTimeRange persistenceLow, persistenceHigh, missingLimit;
    bool doNotDisturb;          // the service's own reading of Do not disturb (the pages read it live)

    const PatientAlertFacts *patient(const QString &subject) const
    {
        for (int i = 0; i < patients.size(); ++i)
            if (patients.at(i).subject == subject)
                return &patients.at(i);
        return 0;
    }
};

// The service's answer to SetThresholds (EditOutcome of
// core/alerts/thresholdbook.h): the result word and the numbers a page needs to explain it, all in whole
// mg/dL (-1 when the answer did not carry them).
struct ThresholdAnswer
{
    ThresholdAnswer() : ll(-1), l(-1), h(-1), hh(-1), lMin(-1), lMax(-1), hMin(-1), hMax(-1),
                        globalL(-1), globalH(-1) {}

    // "accepted", "no-change", "invalid-value", "inconsistent", "needs-confirmation",
    // "confirmation-mismatch", "unknown-subject", "not-saved", "service-not-running"; "call-failed" when
    // the bus did not answer, "service-older" when the service does not have the method, "unreadable" for a text this version cannot read.
    QString result;
    int ll, l, h, hh;           // accepted: the thresholds now in force
    QString offending;          // "L" or "H" (invalid-value, inconsistent, confirmation-mismatch), "" otherwise
    int lMin, lMax, hMin, hMax; // inconsistent: the allowed ranges
    int globalL, globalH;       // needs-confirmation, confirmation-mismatch: the standard values
};

class AlertsBackend : public QObject
{
    Q_OBJECT
public:
    explicit AlertsBackend(QObject *parent = 0) : QObject(parent) {}

    virtual AlertsFacts facts() const = 0;

    // The user's actions. Each is a request; the answer arrives as answered() with the service's own
    // word ("done", "not-active", "already-acknowledged", "not-snoozable", "bad-duration", "not-silent",
    // "unknown-subject", "unsupported", "service-not-running", "service-older", or "call-failed" when the bus did not
    // answer), and the new picture as changed().
    virtual void acknowledge(const QString &subject, const QString &slot) = 0;
    // `minutes` is one of 15, 30, 60, 120; the service refuses others.
    virtual void snooze(const QString &subject, const QString &slot, int minutes) = 0;
    // `minutes` > 0, 0 without limit, -1 until tomorrow: the service's own encoding (SetSilent). The UI
    // never computes the end: the service decides it (06:00 local) and reports it back.
    virtual void setSilent(const QString &subject, int minutes) = 0;
    virtual void endSilent(const QString &subject) = 0;
    // The low and high limits of one patient (REQ-THR-01 to 04), whole mg/dL. `typedL` and `typedH`
    // are the values typed again for the challenge of a looser limit, -1 where nothing was typed; the
    // service enforces the match. There is no comment. The
    // answer arrives as thresholdsAnswered(), and the new picture as changed().
    virtual void setThresholds(const QString &subject, int l, int h, int typedL, int typedH) = 0;

signals:
    void changed();
    // `action` is "acknowledge", "snooze", "silent" or "end-silent".
    void answered(const QString &action, const QString &subject, const QString &result);
    void thresholdsAnswered(const QString &subject, const lm::ui::ThresholdAnswer &answer);
};

} // namespace ui
} // namespace lm

// The thresholds' answer travels in a signal that tests watch with QSignalSpy.
Q_DECLARE_METATYPE(lm::ui::ThresholdAnswer)

#endif
