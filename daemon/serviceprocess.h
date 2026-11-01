// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICEPROCESS_H
#define LM_SERVICEPROCESS_H

// The body of service mode: connects the Unix signals to the runtime, starts it and runs the
// Qt event loop until the stop sequence has finished. Used by the application binary
// (`--service`, phone ports) and by the host service executable (PC ports), so the process
// behaviour is one piece of code. The caller has created the QCoreApplication (a
// QGuiApplication on the phone, because the platform's activation tooling expects one) and
// owns the platform ports.

#include "daemon/serviceruntime.h"

namespace lm {

// Returns the process exit code: 0 after an orderly stop or when this instance stepped aside
// because another one runs, ServiceExitOpenFailed when the journal, the store or the
// configuration did not open.
int runServiceProcess(const RuntimeOptions &options, const ServicePlatform &platform);

} // namespace lm

#endif
