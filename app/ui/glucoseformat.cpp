// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "glucoseformat.h"

#include "units/glucoseunits.h"

namespace lm {
namespace ui {

QString formatGlucose(double storedValue, const QString &storedUnit, const QString &targetUnit,
                      const QLocale &locale)
{
    double shown = 0;
    if (!convertGlucoseForDisplay(storedValue, storedUnit, targetUnit, &shown))
        return QString();
    // Native resolution of the target unit (REQ-DAT-12): mg/dL whole numbers, mmol/L one decimal.
    const int decimals = targetUnit == QLatin1String("mmol/L") ? 1 : 0;
    // The locale only decides the separators; the rounding was already done on the decimal
    // representation by the core, so no second rounding can disagree with it.
    QLocale plain(locale);
    plain.setNumberOptions(QLocale::OmitGroupSeparator);
    return plain.toString(shown, 'f', decimals);
}

} // namespace ui
} // namespace lm
