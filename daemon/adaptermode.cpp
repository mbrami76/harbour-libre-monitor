// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "adaptermode.h"

#include "libremain.h"

namespace lm {

int runAdapterMode(const QString &adapterId, const QStringList &options)
{
    if (adapterId == QLatin1String("libre"))
        return runLibreAdapterOnStdio(options);
    return 64;      // not reachable through parseProcessMode; refused rather than guessed
}

} // namespace lm
