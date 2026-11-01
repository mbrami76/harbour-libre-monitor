// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "serviceidentity.h"

#include <QCoreApplication>

namespace lm {

const char *const kServiceInterface = "it.michelebrami.libre_monitor.Service";

ServiceIdentity ServiceIdentity::fromNames(const QString &organization, const QString &application)
{
    ServiceIdentity id;
    id.busName = organization + QLatin1Char('.') + application;
    QString path = id.busName;
    path.replace(QLatin1Char('.'), QLatin1Char('/')).replace(QLatin1Char('-'), QLatin1Char('_'));
    id.objectPath = QLatin1Char('/') + path;
    id.interfaceName = QString::fromLatin1(kServiceInterface);
    return id;
}

ServiceIdentity ServiceIdentity::forThisApplication()
{
    const QString org = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    if (!org.isEmpty() && !app.isEmpty() && org.contains(QLatin1Char('.')))
        return fromNames(org, app);
    // Service mode is a plain QCoreApplication: its name is the executable's, which is the package's
    // (`harbour-libre-monitor`, or `harbour-libre-monitor-dev` for a development package, REQ-SIM-09).
    // The application name of the desktop file is the package name without the `harbour-` prefix, so a
    // development package gets its own bus name and never the release package's.
    if (app.startsWith(QLatin1String("harbour-")) && app.size() > 8)
        return fromNames(QStringLiteral("it.michelebrami"), app.mid(8));
    return fromNames(QStringLiteral("it.michelebrami"), QStringLiteral("libre-monitor"));
}

} // namespace lm
