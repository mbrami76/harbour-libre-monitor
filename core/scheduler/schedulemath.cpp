// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "scheduler/schedulemath.h"

namespace lm {

qint64 backoffDelayMs(int failures, int retryAfterS, const SchedulerSettings &s, double random01)
{
    double delay = double(s.backoffBaseMs);
    for (int i = 0; i < failures && delay < double(s.backoffCapMs); ++i)
        delay *= s.backoffFactor;
    if (delay > double(s.backoffCapMs))
        delay = double(s.backoffCapMs);
    // Jitter spreads the retries of several connections so they do not hit the source in step.
    const double jitter = 1.0 + (random01 * 2.0 - 1.0) * s.backoffJitter;
    qint64 ms = qint64(delay * jitter);
    if (ms < s.minFetchGapMs)
        ms = s.minFetchGapMs;
    if (retryAfterS > 0) {
        qint64 hint = qint64(retryAfterS) * 1000;
        if (hint > s.rateHoldCapMs)
            hint = s.rateHoldCapMs;
        if (ms < hint)
            ms = hint;
    }
    return ms;
}

qint64 effectiveIntervalMs(qint64 nominalMs, qint64 floorMs, bool current, qint64 nowMs,
                           qint64 lastReadingMs, const SchedulerSettings &s)
{
    qint64 eff = nominalMs > floorMs ? nominalMs : floorMs;
    if (current && lastReadingMs >= 0 && s.quietAfterMs > 0) {
        const qint64 silent = nowMs - lastReadingMs;
        if (silent > s.quietAfterMs) {
            // One doubling for each further quiet period, up to the cap.
            const int steps = int((silent - s.quietAfterMs) / s.quietAfterMs) + 1;
            qint64 stretched = eff;
            for (int i = 0; i < steps && stretched < s.quietMaxIntervalMs; ++i)
                stretched *= 2;
            if (stretched > s.quietMaxIntervalMs)
                stretched = s.quietMaxIntervalMs;
            if (stretched > eff)
                eff = stretched;
        }
    }
    return eff;
}

NextRun nextCurrentRun(qint64 nowMs, qint64 effMs, bool newReading, qint64 readingMs,
                       int recheckN, const SchedulerSettings &s)
{
    NextRun r;
    if (newReading) {
        // Aim just after the next reading should have been published (REQ-SCH-02).
        r.atMs = readingMs + effMs + s.publishLagMs;
        r.recheckN = 0;
    } else if (recheckN < s.recheckMax) {
        r.atMs = nowMs + s.recheckDelayMs;
        r.recheckN = recheckN + 1;
    } else {
        // Late mode: the sensor timestamp no longer predicts anything; keep the rhythm.
        r.atMs = nowMs + effMs;
        r.recheckN = recheckN;
    }
    // Lower bound: never a tight loop when a reading is late. Upper bound: a reading stamped
    // in the future (accepted up to the clock-skew tolerance) must not push the next poll
    // minutes away, or readings of a feed that keeps no history would be lost.
    if (r.atMs < nowMs + s.minFetchGapMs)
        r.atMs = nowMs + s.minFetchGapMs;
    if (r.atMs > nowMs + effMs + s.publishLagMs)
        r.atMs = nowMs + effMs + s.publishLagMs;
    return r;
}

StalenessResult computeStaleness(const StalenessInput &in)
{
    StalenessResult r;
    const qint64 threshold = qint64(in.staleFactor) * in.effCurrentMs;
    const qint64 reference = in.lastSuccessMs >= 0 ? in.lastSuccessMs : in.startedAtMs;
    r.stale = in.nowMs - reference > threshold;
    if (!r.stale)
        return r;

    // First match wins; the order is the priority table
    if (in.disabledByGuard) {
        r.cause = StaleCause::disabledByGuard();
    } else if (in.credentialsLocked) {
        r.cause = StaleCause::credentialsLocked();
    } else if (in.needsAttention) {
        r.cause = StaleCause::needsAttention();
        r.detail = in.attentionDetail;
    } else if (in.serviceDownMs > threshold && !in.contactOkSinceStart) {
        r.cause = StaleCause::serviceNotRunning();
    } else if (in.inBackoff
               || in.lastContactOkMs < 0 || in.nowMs - in.lastContactOkMs > threshold) {
        r.cause = StaleCause::unreachable();
        r.detail = in.backoffDetail;
    } else if (in.sensorKnown && in.sensorReadyMs >= 0 && in.nowMs < in.sensorReadyMs) {
        r.cause = StaleCause::warmUp();
    } else {
        r.cause = StaleCause::noNewReadings();
    }
    return r;
}

} // namespace lm
