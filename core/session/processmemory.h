// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROCESSMEMORY_H
#define LM_PROCESSMEMORY_H

// How much memory a child process has used, read the way a Linux parent can: from the
// "/proc/<pid>/status" file the kernel keeps for every process. The adapter process is
// untrusted and long-lived, so the core watches its size; this is the one place
// that knows the file format.
//
// Why VmHWM and not VmPeak or RLIMIT_AS: VmHWM is the "high water mark" of the RESIDENT size,
// the memory the process really holds in RAM. The address space (VmPeak) is huge for a TLS
// client on this phone (479 MB in the first real run, 264 MB in an earlier test) and says
// nothing about a leak, so a limit on it would kill a healthy adapter. The
// resident size of the same process was about 26 MB after 11 hours.
//
// Assumption (register): the file has a line "VmHWM:   <n> kB" on the phone's kernel.
// If a platform lacks it, the functions answer -1 and no limit is applied (the check is a
// protection, not a function the service depends on).

#include <QByteArray>
#include <QtGlobal>

namespace lm {

// The value of the "VmHWM:" line in the text of a status file, in bytes; -1 when the line is
// missing or malformed. Pure, so a test needs no process.
qint64 parseVmHwmBytes(const QByteArray &statusText);

// The peak resident size of the process in bytes, -1 when it cannot be read (the process has
// ended, no permission, no /proc).
qint64 readPeakResidentBytes(qint64 pid);

} // namespace lm

#endif
