// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Host-only development executable (module adapter-libre, scope). Standard output
// carries protocol lines only; diagnostics go to standard error through Qt's default
// message handler. Usage:
//   libre-adapter-host [--base-url http://127.0.0.1:PORT]   (--base-url: devsource builds only)
//
// The loop itself is lm::runLibreAdapterOnStdio, shared with the adapter mode of the
// application binary.

#include "libremain.h"

#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList options = app.arguments();
    options.removeFirst();
    return lm::runLibreAdapterOnStdio(options);
}
