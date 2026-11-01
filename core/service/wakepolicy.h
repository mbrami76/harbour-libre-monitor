// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_WAKEPOLICY_H
#define LM_WAKEPOLICY_H

// Which keepalive frequency to arm after a tick (
// REQ-ARC-08). The platform offers a fixed list of wake-up frequencies, not arbitrary
// delays, so the service arms the LARGEST one that does not pass the instant the scheduler
// wants to run next: a wake-up that is a little early costs one cheap tick, one that is late
// misses a reading. How long a frequency may be is a parameter (WakeSettings), so the
// battery cost measured later changes numbers, not logic.

#include "service/servicesettings.h"

#include <QList>

namespace lm {

class WakePolicy
{
public:
    explicit WakePolicy(const WakeSettings &settings = WakeSettings()) : m_s(settings) {}

    // `supportedS`: the platform's frequencies in seconds. `deltaMs`: time until the
    // scheduler's next due instant; negative when nothing is due. Returns the frequency in
    // seconds, or 0 when the platform offers none.
    //
    // The cap is `busyMaxS` normally and `idleMaxS` when nothing is due for at least
    // `idleThresholdS`. When even the smallest frequency is longer than the wanted delay the
    // smallest is used: the platform cannot wake earlier.
    int choose(const QList<int> &supportedS, qint64 deltaMs) const;

private:
    WakeSettings m_s;
};

} // namespace lm

#endif
