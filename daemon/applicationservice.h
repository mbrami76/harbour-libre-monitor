// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_APPLICATIONSERVICE_H
#define LM_APPLICATIONSERVICE_H

// Service mode of the packaged application (`harbour-libre-monitor --service`, REQ-ARC-07): the
// real ports for the device, the application's own data and configuration directories, and the
// adapter command "this binary with --adapter <id>" (REQ-ADP-15). It is the only place
// that knows which ports the application uses, so the main function stays a mode switch.
//
// Requires an existing QCoreApplication: the application's main creates it the same way for
// every mode that needs one, so that the platform tooling that starts the process (the invoker
// and its booster, S2, S3) sees what it expects.
//
// In a host build the same function uses the stand-in ports of hostports.h, so the one binary
// can be tried on the PC; it then has no credentials and idles. The development executable
// `lm-service` is the usual way to run on the PC.

namespace lm {

int runApplicationService();

} // namespace lm

#endif
