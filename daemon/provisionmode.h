// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROVISIONMODE_H
#define LM_PROVISIONMODE_H

// The provisioning mode of the one binary:
//
//   harbour-libre-monitor --provision --connection c1 [--subject-ref <ref>]
//   harbour-libre-monitor --provision --remove --connection c1
//
// It exists so that the first run with real credentials on the author's own phone can happen
// before the user interface exists. It asks for the account name and the password
// at a hidden prompt (never an argument, never an environment variable, REQ-ADP-11), stores
// them and the connection through the Provisioner (provisioning.h, the code the interface will
// call too), and then tells the running service: ConfigChanged (the file) and
// CredentialsChanged (the secret) over D-Bus. Removing deletes both.
//
// Output is a few fixed lines and nothing else: no account name, password, token, source
// reference or Subject identifier is ever printed, logged or put into an error message
// (REQ-SEC-01). The source reference may be given as an argument, which shows it in the shell history and the process list of the author's own phone; if
// it is left out the mode asks for it at the hidden prompt instead.
//
// Pattern: ports and adapters again. The mode itself (runProvisionMode) only talks to a
// Prompter, a ServiceCaller and a line sink, so the host tests run the whole flow with fakes
// and then look at every line it produced.
//
// Exit codes: 0 done; 1 something failed (a fixed key is printed); 64 wrong arguments.

#include "provisioning.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace lm {

// The adapter a connection set up by this mode uses. The registry holds only this one today.
extern const char *const kProvisionAdapterId;       // "libre"

struct ProvisionOptions
{
    ProvisionOptions() : remove(false) {}

    bool remove;
    QString connectionId;
    QString subjectRef;         // empty: ask at the hidden prompt
    QString error;              // a fixed text, never the offending argument
};

// `arguments` are the ones AFTER `--provision`.
ProvisionOptions parseProvisionOptions(const QStringList &arguments);

// Hidden input from a person.
class Prompter
{
public:
    virtual ~Prompter() {}
    virtual bool hidden(const QString &label, QString *value, QString *errorKey) = 0;
};

// The two notifications to the running service. Each returns false when the service could not
// be reached; a call also starts a service that is not running (D-Bus activation).
class ServiceCaller
{
public:
    virtual ~ServiceCaller() {}
    // `result` is "" (applied), "restart-needed" or "config-invalid".
    virtual bool configChanged(QString *result) = 0;
    virtual bool credentialsChanged(const QString &connectionId) = 0;
};

typedef std::function<void(const QString &)> LineSink;

// Runs the mode with the given ports; returns the exit code.
int runProvisionMode(const ProvisionOptions &options, Provisioner *provisioner,
                     Prompter *prompter, ServiceCaller *service, const LineSink &out);

} // namespace lm

#endif
