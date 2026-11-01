// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_GUARD_PORTS_H
#define LM_GUARD_PORTS_H

// Small interfaces through which the guard and the scheduler reach the outside
// world: time, randomness and the ingest log. Pattern: ports and adapters. The decision
// logic of this module never calls a system clock, a random generator or the store
// directly, so every rule can be tested on the PC with simulated days;
//). Real implementations come from the service
// the tests use a manual clock, scripted random numbers and a recording sink.

#include "time/utctime.h"

#include <QString>

namespace lm {

// Time source. The logic reads the time only here (REQ-ARC-08: in the service it
// is driven by keepalive wake-ups, so it must never assume a running Qt timer).
class Clock
{
public:
    virtual ~Clock() {}
    virtual UtcTime now() const = 0;
};

// Source of jitter. Returns a number in [0, 1). Injected so that backoff delays are
// deterministic in tests (REQ-SCH-04).
class RandomSource
{
public:
    virtual ~RandomSource() {}
    virtual double next() = 0;
};

// One entry for the ingest log. The event names are an open set
// owned by this module: session-start, session-end, violation, pause, drop, circuit-open,
// circuit-close, disabled, acknowledgement, settings-update-rejected, entries-lost.
// `detail` never carries a credential, a token, a name or a glucose value (REQ-GRD-11).
// The connection id is an opaque identifier, not a name.
struct IngestLogEntry
{
    UtcTime time;
    QString event;
    QString adapter;
    QString adapterVersion;
    QString sessionId;
    QString connection;
    QString detail;
};

// Where ingest-log entries go. Returns false when the entry could not be written now (the
// store may be busy with maintenance, REQ-STO-15); the IngestLog then keeps it queued.
class IngestLogSink
{
public:
    virtual ~IngestLogSink() {}
    virtual bool append(const IngestLogEntry &entry) = 0;
};

} // namespace lm

#endif
