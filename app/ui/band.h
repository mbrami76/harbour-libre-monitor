// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_BAND_H
#define LM_UI_BAND_H

// The band function used to live here (first version). It moved to the core
// (core/alerts/band.h, thresholds in core/alerts/thresholds.h) when the alert engine took over the
// thresholds, so that the alert engine and the screens use ONE rule and can never disagree about
// which band a reading is in. The names stay available in lm::ui so the UI's code is unchanged;
// the rule itself is now's: whole mg/dL thresholds, strict comparison on the stored value.

#include "alerts/band.h"
#include "subjectstate.h"

namespace lm {
namespace ui {

using lm::Thresholds;
using lm::bandOf;

} // namespace ui
} // namespace lm

#endif
