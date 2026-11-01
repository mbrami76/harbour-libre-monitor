// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libreunit.h"

#include "units/glucoseunits.h"

#include <QtGlobal>

#include <cmath>

namespace lm {

namespace {

const double kEqualEpsilon = 1e-9;

UnitDetermination make(const char *unit, const char *basis)
{
    UnitDetermination d;
    d.unit = QString::fromLatin1(unit);
    d.basis = QString::fromLatin1(basis);
    return d;
}

UnitDetermination none()
{
    UnitDetermination d;
    d.basis = QStringLiteral("none");
    return d;
}

} // namespace

UnitDetermination determineGlucoseUnit(const QJsonValue &unitCode, const QJsonValue &value,
                                       const QJsonValue &valueInMgPerDl)
{
    if (!value.isDouble())
        return none();
    const double v = value.toDouble();

    if (valueInMgPerDl.isDouble()) {
        const double mg = valueInMgPerDl.toDouble();
        const char *relation = 0;
        if (qAbs(v - mg) < kEqualEpsilon)
            relation = "mg/dL";
        else if (qAbs(v * kMgDlPerMmolL - mg) <= kLibreRelationToleranceMgDl)
            relation = "mmol/L";
        else
            return none();   // the two values fit neither unit: never guess

        // Step 1: only the verified code 1 may be reported as basis "code".
        const bool codeAgrees = unitCode.isDouble() && unitCode.toDouble() == 1.0
                                && qstrcmp(relation, "mg/dL") == 0;
        return make(relation, codeAgrees ? "code" : "relation");
    }

    // Step 3: ValueInMgPerDl missing, magnitude within the sensor range.
    if (v >= kLibreSensorMinMgDl)
        return make("mg/dL", "magnitude");
    if (v <= kLibreSensorMaxMmolL)
        return make("mmol/L", "magnitude");
    return none();
}

} // namespace lm
