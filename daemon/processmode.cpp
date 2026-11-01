// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "processmode.h"

#include "provisionmode.h"
#include "session/adapterregistry.h"

namespace lm {

ProcessMode parseProcessMode(const QStringList &arguments)
{
    ProcessMode mode;
    // The mode word is always the first argument after the program name, as the desktop
    // file's ExecDBus line and the service's own QProcess call write it (S2, S3). Looking
    // anywhere else would let a stray argument change the mode.
    if (arguments.size() < 2)
        return mode;

    const QString first = arguments.at(1);
    if (first == QLatin1String("--service")) {
        if (arguments.size() != 2) {
            mode.kind = ProcessMode::Invalid;
            mode.error = QStringLiteral("the service mode takes no further argument");
        } else {
            mode.kind = ProcessMode::Service;
        }
        return mode;
    }

    if (first == QLatin1String("--adapter")) {
        if (arguments.size() < 3) {
            mode.kind = ProcessMode::Invalid;
            mode.error = QStringLiteral("--adapter needs an adapter identifier");
            return mode;
        }
        // Only built-in adapters can run (REQ-ADP-05, REQ-ADP-16): the identifier must be
        // in the registry, so a crafted argument cannot name anything else.
        if (!AdapterRegistry::instance().find(arguments.at(2))) {
            mode.kind = ProcessMode::Invalid;
            mode.error = QStringLiteral("unknown adapter");
            return mode;
        }
        mode.kind = ProcessMode::Adapter;
        mode.adapterId = arguments.at(2);
        mode.adapterOptions = arguments.mid(3);
        return mode;
    }

    if (first == QLatin1String("--provision")) {
        // The options are checked here so a mistake is reported before any application object
        // exists; the error is a fixed text and never echoes an argument, which may be a
        // source reference.
        const ProvisionOptions parsed = parseProvisionOptions(arguments.mid(2));
        if (!parsed.error.isEmpty()) {
            mode.kind = ProcessMode::Invalid;
            mode.error = parsed.error;
            return mode;
        }
        mode.kind = ProcessMode::Provision;
        mode.provisionOptions = arguments.mid(2);
        return mode;
    }

    return mode;    // the user interface: the rest belongs to Qt and Silica
}

} // namespace lm
