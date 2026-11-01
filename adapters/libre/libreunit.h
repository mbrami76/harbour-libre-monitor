// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBREUNIT_H
#define LM_LIBREUNIT_H

#include <QJsonValue>
#include <QString>

namespace lm {

// Result of the unit determination (REQ-DAT-13, contract v0).
struct UnitDetermination
{
    QString unit;     // "mg/dL" or "mmol/L"; empty when undetermined
    QString basis;    // "code", "relation", "magnitude" or "none" (contract 5.4)

    bool ok() const { return !unit.isEmpty(); }
};

// Tolerance of the relation test, in mg/dL. PROVISIONAL: proposal of contract section 6
// step 2. A one-decimal mmol/L value is off by at most 0.9 mg/dL from its whole-number
// mg/dL partner, plus 0.5 for that partner's own rounding.
const double kLibreRelationToleranceMgDl = 1.5;

// Sensor range in mg/dL used by the magnitude step (REQ-DAT-13; 2.2 to 22.2 mmol/L).
const double kLibreSensorMinMgDl = 40.0;
const double kLibreSensorMaxMmolL = 22.3;   // "22.3 or less is mmol/L"

// Determines the unit of one reading from the source's three fields, in this order:
//  1. The unit code (GlucoseUnits): 1 is mg/dL (verified), 2 is mmol/L (PROVISIONAL, never
//     seen). A verified code is used (basis "code") only when it agrees with the
//     relation of step 2. A provisional or unknown code is never trusted alone.
//  2. The relation of Value and ValueInMgPerDl: equal means mg/dL; Value times 18.0156
//     within kLibreRelationToleranceMgDl of ValueInMgPerDl means mmol/L (basis "relation").
//     It decides when the code is unknown, provisional or disagrees. When both values are
//     present and fit neither relation the reading is undetermined.
//  3. Only if ValueInMgPerDl is missing: magnitude (basis "magnitude"). 40 or more is
//     mg/dL, 22.3 or less is mmol/L, in between undetermined.
// A missing Value, or a field that is not a JSON number, counts as missing. The values are
// never modified or converted; this function only names the unit.
UnitDetermination determineGlucoseUnit(const QJsonValue &unitCode, const QJsonValue &value,
                                       const QJsonValue &valueInMgPerDl);

} // namespace lm

#endif
