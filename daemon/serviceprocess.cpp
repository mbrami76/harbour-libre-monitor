// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "serviceprocess.h"

#include "daemon/unixsignals.h"

#include <QCoreApplication>

namespace lm {

int runServiceProcess(const RuntimeOptions &options, const ServicePlatform &platform)
{
    // The forwarder first: a SIGTERM that arrives while the pipeline starts is kept in the
    // pipe and acted on as soon as the loop runs.
    UnixSignalForwarder forwarder;
    ServiceRuntime runtime(options, platform);
    QObject::connect(&forwarder, SIGNAL(signalReceived(int)), &runtime, SLOT(onUnixSignal(int)));

    const int early = runtime.begin();
    if (early >= 0)
        return early;
    return QCoreApplication::exec();
}

} // namespace lm
