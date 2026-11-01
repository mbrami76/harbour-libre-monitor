// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STARTCAUSE_H
#define LM_STARTCAUSE_H

// Why did the service start, and since when was it not running? (REQ-SCH-10,
//) Nothing writes a record when the service
// crashes or the phone loses power, so the only evidence is the newest lifecycle entry in the
// journal compared with the clocks now. This is a pure function of that evidence, so every
// shape of journal can be tested row by row.

#include "store/journal.h"
#include "time/utctime.h"

#include <QString>

namespace lm {

// The newest heartbeat, start or stop entry of the journal, or "none".
struct LifecycleMarker
{
    LifecycleMarker() : present(false), bootTimeMs(-1), monotonicMs(-1) {}
    static LifecycleMarker fromEntry(const JournalEntry &entry);

    bool present;
    QString kind;               // JournalKind::heartbeat(), start() or stop()
    UtcTime wallTime;
    qint64 bootTimeMs;          // CLOCK_BOOTTIME when it was written
    qint64 monotonicMs;
};

struct StartCauseInfo
{
    StartCauseInfo() : clockJumpS(0), hasClockJump(false) {}

    QString cause;              // StartCause::*
    QString previous;           // "stop", "heartbeat", "start" or "none": extra key `previous`
    bool hasClockJump;          // extra key `clock_jump_s`
    qint64 clockJumpS;
    // The start of the journal gap before this start: the wall time of the newest lifecycle
    // entry; invalid at the first start.
    UtcTime downSince;
};

// The first matching row of the table in section 4:
//   first-start         the journal holds no start record
//   after-restart       CLOCK_BOOTTIME now is smaller than the entry's: the counter was reset
//   after-stop          the newest entry is a stop record
//   after-missing-stop  otherwise: a heartbeat or a start of this same boot, so the service
//                       ended without writing a stop record (a crash or a kill)
// An orderly phone shutdown ends with a stop record AND a restart: the cause says
// after-restart and `previous` says "stop". The wall clock is not guessed from the counters
// except for `clock_jump_s`, which is set when, within one boot, the wall time moved by
// more than `toleranceS` seconds differently from CLOCK_BOOTTIME.
//
// Known limit: a restart is recognised by the counter being smaller. A phone that
// restarted and then ran longer than the previous boot did looks like the same boot; the
// journal does not store a boot identifier.
StartCauseInfo determineStartCause(bool hasStartRecord, const LifecycleMarker &last,
                                   const UtcTime &wallNow, qint64 bootNowMs, int toleranceS);

} // namespace lm

#endif
