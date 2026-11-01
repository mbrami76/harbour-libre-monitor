// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "units/glucoseunits.h"

#include "record/registry.h"

#include <cmath>

namespace lm {

namespace {

const QString &mgdl() { static const QString s = QStringLiteral("mg/dL"); return s; }
const QString &mmoll() { static const QString s = QStringLiteral("mmol/L"); return s; }

double pow10(int n)
{
    double p = 1.0;
    for (int i = 0; i < n; ++i)
        p *= 10.0;
    return p;
}

} // namespace

double roundHalfAwayFromZero(double value, int decimals)
{
    if (!std::isfinite(value) || decimals < 0 || decimals > 6)
        return value;
    const double scale = pow10(decimals);
    const double scaled = std::fabs(value) * scale;
    if (scaled > 1e15)
        return value;
    // QString::number does not use the locale. Six fractional digits absorb the binary
    // noise of the multiplication; the first dropped digit then decides.
    const QString text = QString::number(scaled, 'f', 6);
    const int dot = text.indexOf(QLatin1Char('.'));
    qint64 whole = text.left(dot).toLongLong();
    if (text.at(dot + 1) >= QLatin1Char('5'))
        ++whole;
    const double magnitude = double(whole) / scale;
    return value < 0 ? -magnitude : magnitude;
}

bool roundToNativeResolution(double value, const QString &unit, double *result)
{
    const int decimals = MetricRegistry::instance().unitDecimals(unit);
    if (decimals < 0 || !std::isfinite(value))
        return false;
    *result = roundHalfAwayFromZero(value, decimals);
    return true;
}

bool convertGlucoseForDisplay(double storedValue, const QString &storedUnit,
                              const QString &targetUnit, double *result)
{
    if (!std::isfinite(storedValue))
        return false;
    if ((storedUnit != mgdl() && storedUnit != mmoll())
        || (targetUnit != mgdl() && targetUnit != mmoll()))
        return false;
    double v = storedValue;
    if (storedUnit != targetUnit)
        v = (storedUnit == mmoll()) ? storedValue * kMgDlPerMmolL : storedValue / kMgDlPerMmolL;
    return roundToNativeResolution(v, targetUnit, result);
}

} // namespace lm
