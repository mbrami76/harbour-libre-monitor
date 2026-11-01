// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_GLUCOSEFORMAT_H
#define LM_UI_GLUCOSEFORMAT_H

// Glucose numbers as text for the screens (REQ-DAT-12).
//
// Why one helper: the conversion between mg/dL and mmol/L and its rounding are defined once in
// core (units/glucoseunits.h) and always start from the STORED value; this only adds the
// locale-dependent writing of the result (an Italian phone shows "6,2", not "6.2"). Keeping it
// out of QML means a test can check three locales without a screen.

#include <QLocale>
#include <QString>

namespace lm {
namespace ui {

// The stored value converted to `targetUnit` ("mg/dL" or "mmol/L"), rounded to that unit's
// native resolution (whole numbers, or one decimal) and written in `locale`. Empty when the
// unit is unknown or the value is not a finite number: the caller then shows no value rather
// than a wrong one.
QString formatGlucose(double storedValue, const QString &storedUnit, const QString &targetUnit,
                      const QLocale &locale);

} // namespace ui
} // namespace lm

#endif
