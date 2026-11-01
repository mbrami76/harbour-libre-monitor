// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROCESSMODE_H
#define LM_PROCESSMODE_H

// The three modes of the one application binary (
// REQ-ARC-07, REQ-ADP-15): the user interface, the headless service, and an adapter process.
// The binary parses its mode FIRST and does nothing else before it knows it: in adapter mode
// nothing may open the store, read the configuration or touch the secret store, so not even a
// Qt GUI object may be created before the decision. Parsing is a pure function of the
// argument list so it is tested without starting anything.
//
//   (no recognised mode argument)   user interface; the arguments are left to Qt and Silica
//   --service                       the headless service (started by D-Bus activation)
//   --adapter <id> [adapter options] an adapter process started by the service
//   --provision [options]           set up or remove a connection from a terminal (//                                   provisionmode.h); exists in every build on purpose
//
// A build without the flag `devsource` contains no development option (REQ-SIM-06): the only
// adapter option, `--base-url`, is compiled out and then refused with the others.

#include <QString>
#include <QStringList>

namespace lm {

struct ProcessMode
{
    enum Kind { Ui, Service, Adapter, Provision, Invalid };

    ProcessMode() : kind(Ui) {}

    Kind kind;
    QString adapterId;              // Adapter
    QStringList adapterOptions;     // Adapter: everything after the identifier
    QStringList provisionOptions;   // Provision: everything after --provision, already checked
    QString error;                  // Invalid: a fixed text, never the offending value
};

// `arguments` as QCoreApplication::arguments() or argv: the first entry is the program.
ProcessMode parseProcessMode(const QStringList &arguments);

} // namespace lm

#endif
