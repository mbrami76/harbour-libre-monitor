// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/band.h"

#include "units/glucoseunits.h"

#include <cmath>

namespace lm {

Band bandOfMgDl(double mgdl, const Thresholds &t)
{
    if (!std::isfinite(mgdl))
        return Band::Unknown;
    // Strict comparisons on the stored value: "below LL" is LL, so a reading equal to the
    // threshold belongs to the band above it. Integer thresholds convert to double
    // exactly, so there is no rounding in the comparison.
    if (mgdl < t.ll) return Band::LL;
    if (mgdl < t.l) return Band::L;
    if (mgdl > t.hh) return Band::HH;
    if (mgdl > t.h) return Band::H;
    return Band::N;
}

Band bandOf(double storedValue, const QString &unit, const Thresholds &t)
{
    if (!std::isfinite(storedValue))
        return Band::Unknown;
    if (unit == QLatin1String("mg/dL"))
        return bandOfMgDl(storedValue, t);
    if (unit == QLatin1String("mmol/L"))
        return bandOfMgDl(storedValue * kMgDlPerMmolL, t);   // exact, from the stored value
    return Band::Unknown;
}

} // namespace lm
