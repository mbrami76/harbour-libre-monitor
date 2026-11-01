// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "applicationprovision.h"

#include "daemon/dbusservice.h"
#include "daemon/provisionmode.h"
#include "daemon/terminalprompt.h"

#ifdef LM_PHONE_PORTS
#include "daemon/phoneports.h"
#endif

#include <QCoreApplication>
#include <QStandardPaths>

#include <cstdio>
#include <unistd.h>

namespace lm {

namespace {

// The terminal: prompts go to standard error and the typed text is read from standard input
// with echo off. Standard output carries only the result lines.
class TerminalPrompter : public Prompter
{
public:
    bool hidden(const QString &label, QString *value, QString *errorKey)
    {
        return readHiddenLine(STDIN_FILENO, STDERR_FILENO, label.toUtf8().constData(), value, errorKey);
    }
};

class DBusServiceCaller : public ServiceCaller
{
public:
    bool configChanged(QString *result) { return m_client.configChanged(result); }
    bool credentialsChanged(const QString &id) { return m_client.credentialsChanged(id); }

private:
    ServiceClient m_client;     // this application's identity
};

void printLine(const QString &line)
{
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

} // namespace

int runApplicationProvisioning(const QStringList &arguments)
{
    // The same names the service sees under Sailjail (the desktop file's OrganizationName and
    // ApplicationName), set by hand so the directories and the bus name are the service's even
    // when this process was not started by the launcher.
    QCoreApplication::setOrganizationName(QStringLiteral("it.michelebrami"));
    QCoreApplication::setApplicationName(QStringLiteral("libre-monitor"));

    const ProvisionOptions options = parseProvisionOptions(arguments);

#ifdef LM_PHONE_PORTS
    // The file the service reads (daemon/applicationservice.cpp uses the same location).
    const QString configPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/config.json");
    SailfishCredentialWriter writer;
    Provisioner provisioner(&writer, configPath);
    TerminalPrompter prompter;
    DBusServiceCaller service;
    return runProvisionMode(options, &provisioner, &prompter, &service, printLine);
#else
    Q_UNUSED(options);
    printLine(QStringLiteral("failed: not-available-on-this-build"));
    return 1;
#endif
}

} // namespace lm
