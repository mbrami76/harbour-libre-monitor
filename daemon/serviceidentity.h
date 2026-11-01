// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICEIDENTITY_H
#define LM_SERVICEIDENTITY_H

// The names under which the service is found on the session bus (REQ-ARC-07): the bus
// name is `<OrganizationName>.<ApplicationName>`, here `it.michelebrami.libre-monitor`. A D-Bus
// object path and an interface name may not contain a hyphen, so they are derived with an
// underscore (for the path). The development package has an identity of its own (REQ-SIM-09), so the names
// come from the application's own organisation and application name when Qt has them (under
// Sailjail they come from the desktop file), and otherwise.

#include <QString>

namespace lm {

// The D-Bus interface of the service. One fixed name, whatever the bus name is: the
// interface is the contract with the UI and the add-on (is the
// reference; the strings below are what it documents), the bus name only says which
// installation answers (a development package has its own, REQ-SIM-09).
extern const char *const kServiceInterface;     // "it.michelebrami.libre_monitor.Service"

struct ServiceIdentity
{
    QString busName;        // it.michelebrami.libre-monitor
    QString objectPath;     // /it/michelebrami/libre_monitor
    QString interfaceName;  // always kServiceInterface

    static ServiceIdentity fromNames(const QString &organization, const QString &application);
    // From QCoreApplication when both names are set, else the names.
    static ServiceIdentity forThisApplication();
};

} // namespace lm

#endif
