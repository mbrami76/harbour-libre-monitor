// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDULEMATH_H
#define LM_SCHEDULEMATH_H

// The pure arithmetic of the scheduler: backoff with jitter, the effective polling interval,
// the next run of the `current` feed, and the staleness cause. No state, no clock, no
// randomness of their own: every input is a parameter, so each rule is a table row in the
// tests.

#include "scheduler/schedulersettings.h"
#include "time/utctime.h"

#include <QString>

namespace lm {

// Delay after the `failures`-th consecutive transient failure (0 = the first):
// min(cap, base * factor^failures) times a jitter factor in [1 - j, 1 + j], never shorter
// than the minimum gap and never shorter than a retry_after hint (REQ-SCH-04). `random01` is
// in [0, 1); 0.5 gives no jitter.
qint64 backoffDelayMs(int failures, int retryAfterS, const SchedulerSettings &s, double random01);

// Effective interval of a feed: the nominal one, raised by the connection's rate floor
// (REQ-SCH-13) and, for the `current` feed, stretched in steps while no reading has come for
// a long time (quiet stretch). `lastReadingMs` < 0 = never a reading.
qint64 effectiveIntervalMs(qint64 nominalMs, qint64 floorMs, bool current, qint64 nowMs,
                           qint64 lastReadingMs, const SchedulerSettings &s);

struct NextRun
{
    NextRun() : atMs(0), recheckN(0) {}
    qint64 atMs;
    int recheckN;
};

// Next run of the `current` feed after a successful command that ended at `nowMs`
// (REQ-SCH-02). `newReading`: a record newer than before was stored, with
// measurement time `readingMs`.
NextRun nextCurrentRun(qint64 nowMs, qint64 effMs, bool newReading, qint64 readingMs,
                       int recheckN, const SchedulerSettings &s);

// ---- staleness (REQ-SCH-06, REQ-SCH-07) ---------------------------------------------------

namespace StaleCause {
inline QString none() { return QString(); }
inline QString disabledByGuard() { return QStringLiteral("disabled-by-guard"); }     // (4)
inline QString credentialsLocked() { return QStringLiteral("credentials-locked"); }  // (6)
inline QString needsAttention() { return QStringLiteral("needs-attention"); }        // (7)
inline QString serviceNotRunning() { return QStringLiteral("service-not-running"); } // (5)
inline QString unreachable() { return QStringLiteral("source-unreachable"); }        // (1)
inline QString warmUp() { return QStringLiteral("sensor-warm-up"); }                 // (3)
inline QString noNewReadings() { return QStringLiteral("no-new-readings"); }         // (2)
}

struct StalenessInput
{
    StalenessInput()
        : nowMs(0), lastSuccessMs(-1), lastContactOkMs(-1), effCurrentMs(60000), staleFactor(3),
          disabledByGuard(false), credentialsLocked(false), needsAttention(false),
          inBackoff(false), serviceDownMs(-1), contactOkSinceStart(false),
          sensorKnown(false), sensorReadyMs(-1), startedAtMs(0) {}

    qint64 nowMs;
    qint64 lastSuccessMs;           // newest stored `current` record, -1 = none
    qint64 lastContactOkMs;         // end of the latest command that ended ok, any feed
    qint64 effCurrentMs;
    int staleFactor;
    bool disabledByGuard;
    bool credentialsLocked;
    bool needsAttention;
    QString attentionDetail;
    bool inBackoff;
    QString backoffDetail;
    qint64 serviceDownMs;           // length of the journal gap before this start, -1 = none
    bool contactOkSinceStart;
    bool sensorKnown;
    qint64 sensorReadyMs;           // activation + warm-up, -1 = unknown
    qint64 startedAtMs;             // when monitoring (this run) began
};

struct StalenessResult
{
    StalenessResult() : stale(false) {}

    bool stale;
    QString cause;                  // StaleCause
    QString detail;
};

StalenessResult computeStaleness(const StalenessInput &in);

} // namespace lm

#endif
