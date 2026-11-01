// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "servicelink.h"

#include "daemon/dbusservice.h"
#include "daemon/serviceidentity.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusServiceWatcher>

namespace lm {
namespace real {

ServiceLink::ServiceLink(QObject *parent)
    : QObject(parent), m_client(new ServiceClient(QString(), QString(), this)), m_watcher(0),
      m_bus(QDBusConnection::sessionBus())
{
    const ServiceIdentity id = ServiceIdentity::forThisApplication();
    m_busName = id.busName;
    m_objectPath = id.objectPath;
    wire(id.busName, m_bus);
}

ServiceLink::ServiceLink(const QString &busName, const QString &objectPath, const QDBusConnection &bus,
                         QObject *parent)
    : QObject(parent), m_client(new ServiceClient(busName, objectPath, bus, this)), m_watcher(0),
      m_busName(busName), m_objectPath(objectPath), m_bus(bus)
{
    wire(busName, bus);
}

void ServiceLink::wire(const QString &busName, const QDBusConnection &bus)
{
    connect(m_client, &ServiceClient::startAnswered, this, &ServiceLink::startAnswered);
    connect(m_client, &ServiceClient::statusAnswered, this,
            [this](bool ok, const QString &json) { emit statusAnswered(ok, json.toUtf8()); });
    connect(m_client, &ServiceClient::configChangedAnswered, this, &ServiceLink::configChangeAnswered);
    connect(m_client, &ServiceClient::discoverAnswered, this, &ServiceLink::discoverAnswered);
    connect(m_client, &ServiceClient::dataChanged, this, &ServiceLink::dataChanged);
    connect(m_client, &ServiceClient::stateChanged, this, &ServiceLink::stateChanged);
    connect(m_client, &ServiceClient::stalenessChanged, this, &ServiceLink::stalenessChanged);
    connect(m_client, &ServiceClient::warning, this, &ServiceLink::warning);
    connect(m_client, &ServiceClient::diagnosticReportProposed, this, &ServiceLink::diagnosticReportProposed);
    connect(m_client, &ServiceClient::gapDetected, this, &ServiceLink::gapDetected);
    connect(m_client, &ServiceClient::alertChanged, this, &ServiceLink::alertChanged);
    connect(m_client, &ServiceClient::silentChanged, this, &ServiceLink::silentChanged);
    connect(m_client, &ServiceClient::alertsAnswered, this,
            [this](bool ok, const QString &json) { emit alertsAnswered(ok, json.toUtf8()); });
    connect(m_client, &ServiceClient::acknowledgeAlertAnswered, this, &ServiceLink::acknowledgeAnswered);
    connect(m_client, &ServiceClient::snoozeAlertAnswered, this, &ServiceLink::snoozeAnswered);
    connect(m_client, &ServiceClient::setSilentAnswered, this,
            [this](bool ok, const QString &subject, const QString &json) { emit setSilentAnswered(ok, subject, json.toUtf8()); });
    connect(m_client, &ServiceClient::endSilentAnswered, this, &ServiceLink::endSilentAnswered);
    connect(m_client, &ServiceClient::setThresholdsAnswered, this,
            [this](bool ok, const QString &subject, const QString &json) { emit setThresholdsAnswered(ok, subject, json.toUtf8()); });

    connect(m_client, &ServiceClient::startMaintenanceAnswered, this, &ServiceLink::startMaintenanceAnswered);
    connect(m_client, &ServiceClient::maintenanceProgress, this, &ServiceLink::maintenanceProgress);
    connect(m_client, &ServiceClient::maintenanceFinished, this, &ServiceLink::maintenanceFinished);
    connect(m_client, &ServiceClient::diagnosticReportAnswered, this, &ServiceLink::diagnosticReportAnswered);
    // Direct connection on purpose (the default, same thread): the reason must reach the receivers before
    // the answer signal that the client emits right after it.
    connect(m_client, &ServiceClient::callFailed, this, &ServiceLink::callFailed);

    m_watcher = new QDBusServiceWatcher(busName, bus,
                                        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this]() { emit reachableChanged(true); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() { emit reachableChanged(false); });
}

void ServiceLink::start() { m_client->startAsync(); }
void ServiceLink::refreshStatus() { m_client->statusJsonAsync(); }
void ServiceLink::retry(const QString &c) { m_client->retryAsync(c); }
void ServiceLink::reenable(const QString &c) { m_client->reenableAsync(c); }
void ServiceLink::reenableVersion(const QString &a, const QString &v) { m_client->reenableVersionAsync(a, v); }
void ServiceLink::credentialsChanged(const QString &c) { m_client->credentialsChangedAsync(c); }
void ServiceLink::configChanged() { m_client->configChangedAsync(); }
void ServiceLink::stop() { m_client->stopAsync(); }
void ServiceLink::discover(const QString &c) { m_client->discoverAsync(c); }
void ServiceLink::alerts() { m_client->alertsAsync(); }
void ServiceLink::acknowledgeAlert(const QString &s, const QString &slot) { m_client->acknowledgeAlertAsync(s, slot); }
void ServiceLink::snoozeAlert(const QString &s, const QString &slot, int m) { m_client->snoozeAlertAsync(s, slot, m); }
void ServiceLink::setSilent(const QString &s, int minutes) { m_client->setSilentAsync(s, minutes); }
void ServiceLink::endSilent(const QString &s) { m_client->endSilentAsync(s); }
// No comment: SetThresholds takes the numbers and the values typed again only.
void ServiceLink::setThresholds(const QString &s, int l, int h, int typedL, int typedH)
{
    m_client->setThresholdsAsync(s, l, h, typedL, typedH);
}

void ServiceLink::startMaintenance(const QString &kind, const QString &parametersJson)
{
    m_client->startMaintenanceAsync(kind, parametersJson);
}
void ServiceLink::cancelMaintenance() { m_client->cancelMaintenanceAsync(); }
void ServiceLink::diagnosticReport(const QString &connection) { m_client->diagnosticReportAsync(connection); }

void ServiceLink::reportHandled(const QString &connection)
{
    // Fire and forget: the service adds the method later. An older service answers "unknown method"
    // and nothing is shown for it. The call is made with the service already known to be there (the
    // proposal came from it), so it never activates a stopped service.
    if (!m_bus.interface() || !m_bus.interface()->isServiceRegistered(m_busName))
        return;
    QDBusInterface iface(m_busName, m_objectPath, QString::fromLatin1(kServiceInterface), m_bus);
    iface.asyncCall(QStringLiteral("ReportHandled"), connection);
}

} // namespace real
} // namespace lm
