// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTSETTINGS_H
#define LM_ALERTSETTINGS_H

// Every constant of the alert engine, in one place.
// All of them are PROVISIONAL assumptions; a
// change of a value is a change of this struct, never of the engine's code.
//
// What is deliberately NOT here: the persistence time of LL and HH, and any switch that would
// delay or silence them. LL and HH raise an alert the moment a reading is beyond the threshold
// and no setting can change that (REQ-THR-01). Because the struct has no field for it, no
// configuration file, test fixture or future settings page can set one by mistake.

#include "alerts/alerttypes.h"

#include <QtGlobal>

namespace lm {

const qint64 kAlertSecondMs = 1000;
const qint64 kAlertMinuteMs = 60 * kAlertSecondMs;

// The snooze durations offered for L, H and MISSING, in minutes.
const int kSnoozeMinutes[] = { 15, 30, 60, 120 };
const int kSnoozeMinutesCount = 4;

struct AlertSettings
{
    AlertSettings()
        : persistenceHighDefaultMs(15 * kAlertMinuteMs),
          persistenceLowDefaultMs(0),
          persistenceLowMaxMs(15 * kAlertMinuteMs),
          persistenceHighMaxMs(60 * kAlertMinuteMs),
          reminderLLMs(2 * kAlertMinuteMs),
          reminderLMs(15 * kAlertMinuteMs),
          reminderHHMs(2 * kAlertMinuteMs),
          reminderHMs(30 * kAlertMinuteMs),
          reminderMissingMs(30 * kAlertMinuteMs),
          hysteresisMgDl(5),
          confirmReadings(2),
          reAlertLLMs(5 * kAlertMinuteMs),
          reAlertHHMs(15 * kAlertMinuteMs),
          missingLimitDefaultMs(15 * kAlertMinuteMs),
          missingLimitMinMs(5 * kAlertMinuteMs),
          missingLimitMaxMs(60 * kAlertMinuteMs),
          staleLimitDefaultMs(3 * kAlertMinuteMs),
          interruptionLimitMs(15 * kAlertMinuteMs),
          silentTomorrowHour(6),
          silentTomorrowMinAheadMs(60 * kAlertMinuteMs)
    {}

    // Persistence: how long a reading must stay beyond the threshold before the alert is active.
    // Only L and H have one, and only these two are editable per Subject: L at
    // most 15 minutes. The bound for H is the module's own.
    qint64 persistenceHighDefaultMs;    // H: 15 min
    qint64 persistenceLowDefaultMs;     // L: none
    qint64 persistenceLowMaxMs;
    qint64 persistenceHighMaxMs;

    // Reminders while an alert is active and not acknowledged.
    qint64 reminderLLMs;                // every 2 min
    qint64 reminderLMs;                 // 15 min
    qint64 reminderHHMs;                // 2 min, like LL
    qint64 reminderHMs;                 // 30 min
    qint64 reminderMissingMs;           // 30 min

    // Hysteresis: an alert ends only when the value is back inside the threshold by this many
    // mg/dL for this many consecutive readings.
    int hysteresisMgDl;
    int confirmReadings;

    // An acknowledged LL or HH becomes active again after this long if the condition still holds
    // and the reminders resume. LL after 5 minutes, HH after 15. The two
    // times are separate fields so that a later change of one never moves the other.
    qint64 reAlertLLMs;
    qint64 reAlertHHMs;

    // Missing data: the limit is per Subject; the engine keeps it inside this range.
    qint64 missingLimitDefaultMs;
    qint64 missingLimitMinMs;
    qint64 missingLimitMaxMs;

    // The display staleness limit used until the scheduler tells the engine its own (3 times the
    // current-feed interval). A missing-data alert starts pending when the newest reading is
    // older than this.
    qint64 staleLimitDefaultMs;

    // A gap in monitoring longer than this produces the interruption notice at the next start
    // (REQ-ALR-07 says "a configurable limit" and gives no number).
    qint64 interruptionLimitMs;

    // Silent mode "until tomorrow" (the boundary is the module's): the next local
    // `silentTomorrowHour`:00, or the one after when the next is less than `silentTomorrowMinAheadMs`
    // away. Not scaled by --alert-scale.
    int silentTomorrowHour;
    qint64 silentTomorrowMinAheadMs;

    // The quiet time after an acknowledgement of an urgent alert; 0 for the others, which have no
    // re-alert.
    qint64 reAlertFor(AlertType type) const
    {
        return type == AlertType::LL ? reAlertLLMs : type == AlertType::HH ? reAlertHHMs : 0;
    }

    qint64 reminderMs(AlertType type) const
    {
        switch (type) {
        case AlertType::LL: return reminderLLMs;
        case AlertType::L: return reminderLMs;
        case AlertType::H: return reminderHMs;
        case AlertType::HH: return reminderHHMs;
        case AlertType::Missing: break;
        }
        return reminderMissingMs;
    }
};

// Every duration of the settings divided by `divisor` (at least 1 ms each), for the host
// executable of the integration run only, so that a run against the simulator in real time takes
// minutes instead of hours. What it cannot do: it has no way to delay or silence LL or HH (there
// is nothing to scale), and the snooze durations stay as they are. Release and Harbour builds never
// call it: the only caller is daemon/host, which exists in host builds only (REQ-SIM-06).
inline AlertSettings scaledAlertSettings(const AlertSettings &s, int divisor)
{
    if (divisor < 1)
        divisor = 1;
    AlertSettings r = s;
    qint64 *durations[] = { &r.persistenceHighDefaultMs, &r.persistenceLowDefaultMs, &r.persistenceLowMaxMs,
                            &r.persistenceHighMaxMs, &r.reminderLLMs, &r.reminderLMs, &r.reminderHHMs,
                            &r.reminderHMs, &r.reminderMissingMs, &r.reAlertLLMs, &r.reAlertHHMs, &r.missingLimitDefaultMs,
                            &r.missingLimitMinMs, &r.missingLimitMaxMs, &r.interruptionLimitMs };
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i)
        *durations[i] = qMax<qint64>(1, *durations[i] / divisor);
    // The staleness limit is not scaled: it follows the polling interval, which the source fixes
    // at one reading a minute.
    return r;
}

inline bool isSnoozeMinutes(int minutes)
{
    for (int i = 0; i < kSnoozeMinutesCount; ++i)
        if (kSnoozeMinutes[i] == minutes)
            return true;
    return false;
}

} // namespace lm

#endif
