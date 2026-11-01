// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// lm-cli: command-line program of milestone M1 (REQ-ARC-04). Host builds only; it is
// installed in no package. Usage: lm-cli --help.

#include "options.h"
#include "runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    args.removeFirst();

    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        std::fputs(lmcli::usageText().toUtf8().constData(), stdout);
        return lmcli::ExitOk;
    }
    lmcli::Options options;
    QString error;
    if (!lmcli::parseArguments(args, &options, &error)) {
        std::fprintf(stderr, "lm-cli: %s\n\n%s", error.toUtf8().constData(),
                     lmcli::usageText().toUtf8().constData());
        return lmcli::ExitUsage;
    }

    QString adapter = options.adapterPath;
    if (adapter.isEmpty()) {
        const lm::AdapterEntry *entry = lm::AdapterRegistry::instance().find(QStringLiteral("libre"));
        adapter = QDir(QCoreApplication::applicationDirPath()).filePath(entry->hostExecutable);
    }

    lmcli::CliRunner runner(options, adapter);
    int exitCode = lmcli::ExitFailure;
    QObject::connect(&runner, &lmcli::CliRunner::finished, [&](int code) {
        exitCode = code;
        QCoreApplication::quit();
    });
    QTimer::singleShot(0, &runner, SLOT(run()));
    app.exec();
    return exitCode;
}
