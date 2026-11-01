// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBREADAPTER_H
#define LM_LIBREADAPTER_H

#include "httptransport.h"
#include "libreclient.h"
#include "time/utctime.h"

#include <QIODevice>

#include <functional>

namespace lm {

// Version reported in `hello`.
inline QString libreAdapterVersion() { return QStringLiteral("0.1.0"); }

// Everything the loop needs from outside, so tests need neither sockets nor a process.
struct LibreAdapterEnv
{
    LibreAdapterEnv() : transport(0) {}

    HttpTransport *transport;           // not owned; null = a QtHttpTransport of the loop
    LibreConfig config;
    std::function<UtcTime()> clock;     // empty = UtcTime::now
};

// Exit codes of the loop.
enum LibreAdapterExit {
    LibreExitOk = 0,            // `shutdown` or end of input
    LibreExitRejected = 2,      // the core answered `reject`
    LibreExitBadInput = 3       // an oversize line: the stream cannot be trusted any more
};

// The adapter loop of contract v0 over two devices: `hello`, `accept` or
// `reject`, `configure`, one command at a time, `shutdown`. `out` receives protocol
// lines only. Blocks until `shutdown`, end of input or a fatal input fault. The adapter
// does not open the store or the configuration and keeps no credentials after
// `authenticate` returns (REQ-ADP-03, REQ-ADP-15).
int runLibreAdapter(QIODevice &in, QIODevice &out);
int runLibreAdapter(QIODevice &in, QIODevice &out, const LibreAdapterEnv &env);

} // namespace lm

#endif
