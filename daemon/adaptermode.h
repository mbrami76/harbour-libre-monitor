// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ADAPTERMODE_H
#define LM_ADAPTERMODE_H

// Adapter mode of the application binary (REQ-ADP-15):
// the service starts the binary itself with `--adapter <id>`, and this runs the adapter's loop
// on standard input and output. Pattern: a registry-selected entry point. The built-in adapters
// are listed here by identifier; the identifier has already been checked against the adapter
// registry by parseProcessMode.
//
// What adapter mode must NOT do (REQ-ADP-15, REQ-SEC-06): open the store or the journal, read
// the configuration file, access Sailfish Secrets, or create any window. Credentials arrive
// only inside the `authenticate` command, over standard input. The function is kept free of
// any include that could do those things, and tst_daemon checks the mode parser that
// selects it.

#include <QStringList>

namespace lm {

// Blocks until the adapter's loop ends; returns its exit code. Requires a QCoreApplication.
int runAdapterMode(const QString &adapterId, const QStringList &options);

} // namespace lm

#endif
