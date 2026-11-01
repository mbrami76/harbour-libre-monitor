// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_BAND_H
#define LM_BAND_H

// Which of the five bands (LL, L, N, H, HH) a stored reading is in (REQ-UI-03, REQ-UI-07,
// requirements 9.2 and 9.3). The core says which band; the UI only decides how to show it.
//
// History: first written in app/ui/band.* so that the interface could run;
// moved here so that the alert engine and the screens use the same function and can
// never disagree. The rule changed: the thresholds are whole mg/dL and the comparison is
// strict ("below" and "above"), so 54 mg/dL is L (not below 54), 70 is N and 180 is N.
//
// The comparison starts from the STORED value, converted exactly if the Subject's unit is
// mmol/L, never from a rounded display value (REQ-DAT-12).

#include "alerts/thresholds.h"

#include <QString>

namespace lm {

enum class Band { Unknown, LL, L, N, H, HH };

// `unit` is the stored unit ("mg/dL" or "mmol/L"). Any other unit, or a value that is not a
// finite number, gives Band::Unknown: never a guess.
Band bandOf(double storedValue, const QString &unit, const Thresholds &thresholds = Thresholds());

// The same decision for a value already in mg/dL. Shared with the engine so it has one rule.
Band bandOfMgDl(double mgdl, const Thresholds &thresholds);

} // namespace lm

#endif
