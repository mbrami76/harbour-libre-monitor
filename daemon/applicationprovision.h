// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_APPLICATIONPROVISION_H
#define LM_APPLICATIONPROVISION_H

#include <QStringList>

namespace lm {

// Entry point of the provisioning mode (provisionmode.h) for the packaged application: wires
// the real ports (the terminal, Sailfish Secrets, the D-Bus client) and the real
// configuration file, then runs the mode. `options` are the arguments after `--provision`.
// Needs a QCoreApplication. On a build without the phone libraries (the host build) it prints
// that provisioning is not available and returns 1: there is no real secret store there, and
// a plain file with credentials is not allowed (REQ-SEC-01).
int runApplicationProvisioning(const QStringList &options);

} // namespace lm

#endif
