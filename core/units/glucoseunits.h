// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_GLUCOSEUNITS_H
#define LM_GLUCOSEUNITS_H

#include <QString>

namespace lm {

// Display conversion between mg/dL and mmol/L (REQ-DAT-12). The single definition of the
// factor: molar mass of glucose 180.156 g/mol, so 18.0156 mg/dL per mmol/L.
const double kMgDlPerMmolL = 18.0156;

// Rounds to a number of decimals, half away from zero, on the decimal representation:
// the value is written as decimal text first, so 5.55 gives 5.6 although its binary
// double is 5.5499999.... Independent of the locale. Ties closer than 5e-7 of the last
// kept digit count as ties. Not finite input is returned unchanged.
double roundHalfAwayFromZero(double value, int decimals);

// Rounds to the native resolution of a registry unit (REQ-DAT-12). False if the unit is
// unknown to the registry or the value is not finite.
bool roundToNativeResolution(double value, const QString &unit, double *result);

// Converts a STORED value to the target unit and rounds to the target's native
// resolution. Same unit: the stored value is only rounded. False for a unit other than
// "mg/dL" or "mmol/L" or a value that is not finite. There is deliberately no function
// for already converted or rounded values: always start from the stored one.
bool convertGlucoseForDisplay(double storedValue, const QString &storedUnit,
                              const QString &targetUnit, double *result);

} // namespace lm

#endif
