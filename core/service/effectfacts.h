// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_EFFECTFACTS_H
#define LM_EFFECTFACTS_H

// THE FACTS THE TEXTS NEED ABOUT AN ALERT EFFECT.
//
// The texts of the notifications say since when a level has lasted, the limit that was crossed,
// whether an alert that ended had ever been acknowledged, and whether the Subject is in silent
// mode. the engine is to carry these on its effects (the
// fields `levelSince`, `episodeSince`, `limit`, `neverAcknowledged`, `clearedAt`, `silent`);
// The service did not wait for it. So the notifier reads them through ONE small interface,
// EffectFactsReader: the real reader (EngineEffectFacts, below) copies them from the effect, and a
// test passes a fake with every field. The engine's round is merged; when its fields change, only the
// real reader changes.
//
// No field is a glucose value, a name or a comment (an effect never carries one); the value for
// the optional privacy setting is read from the store at posting time, not from the effect.

#include "alerts/alertengine.h"
#include "time/utctime.h"

#include <QString>

namespace lm {

struct EffectFacts
{
    EffectFacts() : limit(0), neverAcknowledged(false), acknowledgementKnown(false), silent(false) {}

    UtcTime levelSince;         // the first reading beyond the limit of the CURRENT level in this stretch
    UtcTime episodeSince;       // the same for the first level of the slot's episode
    int limit;                  // the boundary in force, whole mg/dL; 0 = none (no data)
    bool neverAcknowledged;     // Withdraw: the alert cleared and nobody acknowledged it in the episode
    bool acknowledgementKnown;  // the reader knows the line above (the effect does); else the notifier counts itself
    UtcTime clearedAt;          // Withdraw: when it cleared
    bool silent;                // the Subject is in silent mode at this moment (never for no data)
    UtcTime lastReadingAt;      // no data: the newest reading's measurement time
    QString cause;              // no data: the staleness cause key of REQ-SCH-07
};

class EffectFactsReader
{
public:
    virtual ~EffectFactsReader() {}
    virtual EffectFacts factsOf(const AlertEffect &effect) const = 0;
};

// The real reader: the facts are on the engine's effects.
class EngineEffectFacts : public EffectFactsReader
{
public:
    EffectFacts factsOf(const AlertEffect &e) const
    {
        EffectFacts f;
        f.levelSince = e.levelSince;
        f.episodeSince = e.episodeSince;
        f.limit = e.hasLimit ? e.limit : 0;
        f.neverAcknowledged = e.neverAcknowledged;
        f.acknowledgementKnown = true;                  // the engine knows, also for a Withdraw
        f.clearedAt = e.clearedAt;
        f.silent = e.silent;
        f.lastReadingAt = e.newestReadingAt;
        f.cause = e.cause;
        return f;
    }
};

} // namespace lm

#endif
