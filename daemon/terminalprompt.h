// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_TERMINALPROMPT_H
#define LM_TERMINALPROMPT_H

// Reading a secret from a person at a terminal, with nothing shown while they type (// REQ-ADP-11: a credential never travels as an argument or an environment variable, because
// both are visible to other processes and end up in shell history). Used by the provisioning
// mode (provisionmode.h); small and separate so a test can drive it through a pseudo-terminal.
//
// Assumption: the terminal honours the ECHO flag (every real one does; a pseudo-terminal of the
// test does too). When there is no terminal on the input the call fails instead of reading a
// secret from a pipe or a file, because a pipe could be a script or a log that keeps it.

#include <QString>

namespace lm {

// Writes `label` to `outFd`, reads one line from `inFd` with echo switched off, and puts the
// line (without the newline) in `line`. Echo is switched on again before returning, and also
// when the process is interrupted (Ctrl-C), so a typed password does not leave the user's
// shell silent. On failure `errorKey` is "no-terminal" or "input-failed" and `line` is empty.
// The raw buffer that held the typed bytes is overwritten before return; the QString that
// carries the secret on cannot be wiped and lives as long as the caller keeps it.
bool readHiddenLine(int inFd, int outFd, const char *label, QString *line, QString *errorKey);

} // namespace lm

#endif
