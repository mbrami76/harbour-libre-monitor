// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/subjectlabel.h"

namespace lm {

bool isShortPatientLabel(const QString &label)
{
    const QString t = label.trimmed();
    return !t.isEmpty() && t.size() <= kPatientLabelMaxChars;
}

QString patientLabel(const QString &configuredLabel, int position)
{
    if (isShortPatientLabel(configuredLabel))
        return configuredLabel.trimmed();
    if (position < 0)
        return QStringLiteral("?");
    return QStringLiteral("P%1").arg(position + 1);
}

} // namespace lm
