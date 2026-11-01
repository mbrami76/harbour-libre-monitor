// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/startcause.h"

namespace lm {

LifecycleMarker LifecycleMarker::fromEntry(const JournalEntry &entry)
{
    LifecycleMarker m;
    m.present = true;
    m.kind = entry.kind;
    m.wallTime = entry.wallTime;
    m.bootTimeMs = entry.bootTimeMs;
    m.monotonicMs = entry.monotonicMs;
    return m;
}

StartCauseInfo determineStartCause(bool hasStartRecord, const LifecycleMarker &last,
                                   const UtcTime &wallNow, qint64 bootNowMs, int toleranceS)
{
    StartCauseInfo info;
    info.previous = last.present ? last.kind : QStringLiteral("none");

    // A journal with heartbeats but no start record, or a start record without any entry,
    // cannot happen in a journal this service wrote; it is treated as a first start rather
    // than guessing a gap from entries of unknown origin.
    if (!hasStartRecord || !last.present) {
        info.cause = StartCause::firstStart();
        return info;
    }

    info.downSince = last.wallTime;

    if (bootNowMs < last.bootTimeMs) {
        info.cause = StartCause::afterRestart();
        return info;
    }

    info.cause = (last.kind == JournalKind::stop()) ? StartCause::afterStop()
                                                    : StartCause::afterMissingStop();

    // Same boot: the two clocks must have advanced together. CLOCK_BOOTTIME includes
    // suspend, so suspend alone never makes them differ; only a change of the wall clock
    // (the user, the network time) does.
    if (wallNow.isValid() && last.wallTime.isValid()) {
        const qint64 wallS = (wallNow.msecsSinceEpoch() - last.wallTime.msecsSinceEpoch()) / 1000;
        const qint64 bootS = (bootNowMs - last.bootTimeMs) / 1000;
        const qint64 jump = wallS - bootS;
        if (jump > toleranceS || jump < -toleranceS) {
            info.hasClockJump = true;
            info.clockJumpS = jump;
        }
    }
    return info;
}

} // namespace lm
