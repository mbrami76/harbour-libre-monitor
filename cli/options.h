// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LMCLI_OPTIONS_H
#define LMCLI_OPTIONS_H

#include "session/adapterlink.h"

#include <QString>
#include <QStringList>

namespace lmcli {

// Exit codes (module; 21 is a proposal).
enum ExitCode {
    ExitOk = 0,
    ExitFailure = 1,            // the adapter could not be started, or an unexpected end
    ExitUsage = 2,
    ExitAuthFailed = 10,
    ExitTermsMustBeAccepted = 11,
    ExitRateLimited = 12,
    ExitTransient = 13,
    ExitProtocolChanged = 14,
    ExitInvalidCommand = 15,
    ExitAdmissionRejected = 20,
    ExitContractViolation = 21  // a hard violation of contract section 7
};

int exitCodeFor(lm::ErrorClass errorClass);

struct Options
{
    Options() : json(false), verbose(false), showRef(false), subjectIndex(-1) {}

    QString command;            // discover, current, range
    QString adapterPath;        // empty = libre-adapter-host next to the program
    QString credentialsFile;    // empty = hidden prompt on a terminal
    QString sessionFile;        // optional
    QString baseUrl;            // devsource builds only
    bool json;
    bool verbose;
    // discover --json only: also print each Subject's source reference. The reference
    // identifies a person, so it is off by default; it exists because the provisioning mode of
    // the app needs the value and nothing else shows it.
    bool showRef;
    int subjectIndex;           // -1 = every Subject
    lm::UtcTime from;           // range only
    lm::UtcTime to;
};

// Parses the arguments after the program name. Returns true on success; on failure
// `error` says what is wrong. The text never repeats the value of an argument (a
// mistyped secret must not be echoed) and credentials are not options at all
// (REQ-ADP-11).
bool parseArguments(const QStringList &args, Options *out, QString *error);

QString usageText();

} // namespace lmcli

#endif
