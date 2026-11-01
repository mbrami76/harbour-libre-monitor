// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ALERTVIEW_H
#define LM_UI_ALERTVIEW_H

// The pure decisions of the alert parts of the screens:
// reading the service's `Alerts()` answer, the order of the alerts list, which buttons an alert has,
// what silent mode says, and the words for the service's answers. No QML, no D-Bus and no clock of its
// own (the time is an argument), so the host tests can try every case (REQ-UI-07: the service decides
// the alerts, these functions only decide how they are presented).

#include "alertsbackend.h"
#include "backend.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>

namespace lm {
namespace ui {

// Reads the JSON of `Alerts()`. A service that is not running answers
// {"format":1,"error":"service-not-running"}; a text this version cannot read (not JSON, another
// format number) gives known=false with error "unreadable". Unknown keys are ignored (the service
// adds keys without changing the format number).
AlertsFacts parseAlertsJson(const QByteArray &json);

// Reads the JSON answer of `SetSilent`: returns the `result` word ("unreadable" for a text this
// version cannot read) and, when there is one, the end time in `*untilMs` (0 = without limit).
QString parseSilentAnswer(const QByteArray &json, qint64 *untilMs);

// The rank that orders the alerts list, the same as the service's arbiter: 1 very low, 2 very high, 3 no data, 4 low, 5 high; 6 for a level this version does
// not know (it is still listed, last).
int alertRank(const QString &level);
// Snooze exists for low, high and no data only; never for very low and very high (REQ-THR-01).
bool alertCanSnooze(const QString &level);
// The level's identifier for the pages: "ll", "l", "h", "hh" (the band keys, so a glucose alert can
// show its round band badge) or "missing"; "" for an unknown level.
QString alertLevelKey(const QString &level);
// The UI's own words for the cause of a no-data alert: the same text keys as the stale states
// (StateText.js), the same mapping as the notification texts (core/service/notificationtexts.cpp).
QString alertCauseKey(const QString &cause);
// The text key for an answer of an action, "" for "done" (nothing to say: the picture changes).
QString alertResultKey(const QString &result);

// The snooze durations the service accepts, in minutes. The one
// place the numbers are written in the interface; the service refuses any other.
QList<int> snoozeChoicesMinutes();

// The four silent-mode choices of the pages (REQ-UI-15): "hour", "four-hours", "tomorrow",
// "no-limit". The minutes argument of SetSilent for each (60, 240, -1, 0), or -2 for an unknown choice.
// "tomorrow" is the service's own constant: the UI never computes 06:00.
int silentMinutesForChoice(const QString &choice);
// Only "without limit" asks for the confirmation that names the very low and very high alerts.
bool silentChoiceNeedsConfirmation(const QString &choice);

// Silent mode of one patient as the pages show it: the struck-through speaker and "Silent until
// 14:30" or "Silent" (REQ-UI-15). `untilToday` is true when the end falls on today's local date, so
// the page writes the time alone; otherwise the page writes the day too.
struct SilentView
{
    SilentView() : silent(false), noLimit(false), untilMs(0), untilToday(false) {}
    bool silent, noLimit;
    qint64 untilMs;
    bool untilToday;
};
SilentView silentViewOf(const AlertsFacts &facts, const QString &subject, qint64 nowMs);

// One line of the alerts list, decided.
struct AlertRow
{
    AlertRow() : rank(6), position(-1), minutesSince(-1), canAcknowledge(false), canSnooze(false),
                 isPause(false), snoozed(false) {}

    AlertFact fact;
    QString name;               // the patient's name from the configuration (personal data, screen only)
    QString shortLabel;         // lm::patientLabel: the label or P1, P2 by the place in the file
    QString levelKey;           // "ll", "l", "h", "hh", "missing"
    // The band badge with its colour ("ll" to "hh") only for a glucose alert that is still active;
    // "" otherwise: acknowledged, snoozed and no-data lines are neutral, pictogram and text (REQ-UI-04).
    QString bandKey;
    QString causeKey;           // the no-data cause in the UI's words, "" for a glucose alert
    int rank;
    int position;               // the patient's place in the configuration file, -1 if not there
    int minutesSince;           // whole minutes since the level began, -1 when unknown
    bool canAcknowledge;        // active and not acknowledged yet
    // Low, high and no data while active and not acknowledged: after an acknowledgement the reminders
    // have stopped already, so a snooze would add nothing.
    bool canSnooze;
    // An acknowledged very low or very high alert: the page says it is a pause and that the alert
    // returns if the level persists, with no number of minutes (the service owns them).
    bool isPause;
    bool snoozed;

    QVariantMap toMap() const;
};

// Every alert of the answer, in the arbiter's order: by rank, then the earlier onset first, then
// by the place in the file so the order never jumps. The patients' names and places come from
// `subjects` (the other backend keeps the configuration file's order).
QList<AlertRow> buildAlertRows(const AlertsFacts &facts, const QList<SubjectSnapshot> &subjects, qint64 nowMs);

} // namespace ui
} // namespace lm

#endif
