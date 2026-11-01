// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBREMAIN_H
#define LM_LIBREMAIN_H

// The Libre adapter as a process: opens standard input and output and runs the adapter loop
// on them. It is one function so that the two ways of starting the
// adapter share it: the development host executable (adapters/libre/host) and the adapter
// MODE of the application's own binary (`--adapter libre`, REQ-ADP-15), which is how the
// packaged application starts it because Harbour allows no second executable.
//
// Adapter mode opens no store, reads no configuration file and does no secret access
// (REQ-ADP-15, REQ-SEC-06): this function touches nothing but the two standard streams and,
// for a development build, the network address it was told.

#include <QStringList>

namespace lm {

// `options` are the arguments after the adapter identifier. The only option is
// `--base-url URL`, and only in builds with the flag `devsource` (REQ-SIM-06): a release
// build has neither the option nor the code, and any option is then refused.
// Returns the exit code of the loop (LibreAdapterExit) or 64 for a refused option and 1 when
// the standard streams cannot be opened. Requires a QCoreApplication.
int runLibreAdapterOnStdio(const QStringList &options);

} // namespace lm

#endif
