// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/thresholds.h"

#include "units/glucoseunits.h"

#include <cmath>

namespace lm {

bool mmolEditToMgDl(double mmol, int *mgdl)
{
    if (!std::isfinite(mmol) || mmol <= 0.0 || mmol > 100.0)
        return false;       // 100 mmol/L is far outside any plausible reading; refuses typos
    *mgdl = int(roundHalfAwayFromZero(mmol * kMgDlPerMmolL, 0));
    return *mgdl > 0;
}

double mgDlToMmolForDisplay(int mgdl)
{
    return roundHalfAwayFromZero(double(mgdl) / kMgDlPerMmolL, 1);
}

} // namespace lm
