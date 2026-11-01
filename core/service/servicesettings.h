// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICESETTINGS_H
#define LM_SERVICESETTINGS_H

// Named parameters of the service lifecycle. Every value is PROVISIONAL; they are parameters, not constants
// in the logic, so a measurement on the phone changes a number and not the code.

#include <QList>
#include <QtGlobal>

namespace lm {

// The rule that picks the wake-up frequency.
struct WakeSettings
{
    WakeSettings() : busyMaxS(60), idleMaxS(300), idleThresholdS(600) {}

    // Longest frequency used while something is due soon: the nominal interval of the
    // `current` feed (REQ-SCH-02), so that no reading is awaited longer than that.
    int busyMaxS;
    // Longest frequency used when nothing is due for a long time (a quiet sensor, a hold).
    int idleMaxS;
    // "Nothing due for a long time" means the next wake-up is at least this far away.
    int idleThresholdS;
};

struct LifecycleSettings
{
    LifecycleSettings()
        : heartbeatIntervalS(60), gapLimitS(600), clockJumpToleranceS(60),
          approvalReraiseS(900), heartbeatSlackS(5), noticeRefreshS(1800), stopLinksMaxS(4), stopFlushMaxS(5), stopTotalMaxS(10),
          readingQueueHighWater(16000) {}

    // REQ-SCH-09: one heartbeat per nominal sensor interval.
    int heartbeatIntervalS;
    // A heartbeat is due when the interval minus this slack has passed. A wake-up that comes
    // a little early (the night run of 2026-10-15 saw spacings of 59.9 s) would otherwise
    // skip a beat and the next one would come 30 s later: 90 s instead of 60 s.
    int heartbeatSlackS;
    // Section 6: a journal gap longer than this raises gapDetected (REQ-ALR-07).
    int gapLimitS;
    // Section 4: a difference between the change of the wall time and of CLOCK_BOOTTIME
    // larger than this, within one boot, is recorded as `clock_jump_s`.
    int clockJumpToleranceS;
    // Section 7: the approval dialog is raised again this often while any connection waits
    // The system's own dialog disappears by itself after 2 to 3 minutes if
    // nobody acts, so an hourly re-raise left a user who looked away without a prompt for most
    // of an hour; 15 minutes is PROVISIONAL. A tap on the notice raises it at once.
    int approvalReraiseS;
    // Section 5.3: a heartbeat alone refreshes the resident notice at most this often. The
    // phone test of 2026-10-15 showed that every republication of the notice raises a banner,
    // so one per minute was unbearable; a state change is still shown at once. The time in the
    // tray is therefore up to this old while the service runs.
    int noticeRefreshS;
    // Section 8: how long the stop sequence waits for the adapters (the scheduler already
    // gives each its 2 s), for a busy store, and in all.
    int stopLinksMaxS;
    int stopFlushMaxS;
    int stopTotalMaxS;
    // REQ-STO-15: readings held in memory at which the maintenance run is stopped.
    // About 80 % of the scheduler's `readingQueueMax` (20000). The scheduler is to raise
    // this itself; until it does, the lifecycle watches
    // queuedReadings() at every wake-up.
    int readingQueueHighWater;

    WakeSettings wake;
};

} // namespace lm

#endif
