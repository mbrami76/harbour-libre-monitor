// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/wakepolicy.h"

#include <QtGlobal>

namespace lm {

int WakePolicy::choose(const QList<int> &supportedS, qint64 deltaMs) const
{
    if (supportedS.isEmpty())
        return 0;

    const bool idle = deltaMs < 0 || deltaMs >= qint64(m_s.idleThresholdS) * 1000;
    const int cap = idle ? m_s.idleMaxS : m_s.busyMaxS;
    const qint64 limitS = (deltaMs < 0) ? cap : qMin<qint64>(deltaMs / 1000, cap);

    int best = 0;
    int smallest = supportedS.first();
    for (int i = 0; i < supportedS.size(); ++i) {
        const int f = supportedS.at(i);
        if (f < smallest)
            smallest = f;
        if (f <= limitS && f > best)
            best = f;
    }
    return best > 0 ? best : smallest;
}

} // namespace lm
