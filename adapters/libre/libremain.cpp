// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libremain.h"

#include "libreadapter.h"

#include <QFile>

#include <cstdio>

namespace lm {

int runLibreAdapterOnStdio(const QStringList &options)
{
    LibreAdapterEnv env;
    for (int i = 0; i < options.size(); ++i) {
#ifdef LIBREMON_DEVSOURCE
        if (options.at(i) == QLatin1String("--base-url") && i + 1 < options.size()) {
            env.config.baseUrlOverride = options.at(++i);
            continue;
        }
#endif
        // Standard output carries protocol lines only, so the complaint goes to standard
        // error and never names the offending value.
        std::fprintf(stderr, "libre adapter: unknown argument\n");
        return 64;
    }

    QFile in;
    QFile out;
    if (!in.open(stdin, QIODevice::ReadOnly) || !out.open(stdout, QIODevice::WriteOnly)) {
        std::fprintf(stderr, "libre adapter: cannot open the standard streams\n");
        return 1;
    }
    return runLibreAdapter(in, out, env);
}

} // namespace lm
