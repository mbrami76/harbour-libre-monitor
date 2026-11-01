// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_SERVICELINK_H
#define LM_REAL_SERVICELINK_H

// The user interface's line to the monitoring service over D-Bus, without ever blocking the window
// (REQ-UI-11).
//
// Pattern: a thin facade over the ASYNCHRONOUS client (`ServiceClient::...Async`,
//). Every request returns at once and its answer arrives as a
// signal, so nothing waits in the window's thread and no thread of our own is needed. (The first
// version of this class used a worker thread around the blocking client, because the asynchronous
// one did not exist yet; this one is smaller. The account page's saving still uses a worker
// thread, because the secret store blocks and a discovery waits for a network round trip.) The
// first call to a stopped service starts it by D-Bus activation (REQ-UI-11), which can take
// seconds: the asynchronous timeout is 25 s.
//
// Also here: the service's own signals, passed on, and the appearance or loss of its bus name.
// `ReportHandled(connection)` is a method the service adds later;
// it is called raw, and a service that does not have it answers with an error that is ignored: for
// the user nothing changes.
//
// Never in a message here: a credential, a name or a reference. Subjects travel as UUIDs. The
// answer of Discover is the one exception, and it is not logged.

#include <QDBusConnection>
#include <QObject>
#include <QString>

class QDBusServiceWatcher;

namespace lm {

class ServiceClient;

namespace real {

class ServiceLink : public QObject
{
    Q_OBJECT
public:
    explicit ServiceLink(QObject *parent = 0);
    // The same line to another service name on a connection of the caller's own: a test points it at a
    // stand-in service on its private bus, or at the host service of an integration run ( a
    // client must not share the service's own connection in one process).
    ServiceLink(const QString &busName, const QString &objectPath, const QDBusConnection &bus,
                QObject *parent = 0);

    // Asynchronous requests; each answer is a signal below.
    void start();                                   // startAnswered
    void refreshStatus();                           // statusAnswered
    void retry(const QString &connection);
    void reenable(const QString &connection);
    void reenableVersion(const QString &adapter, const QString &version);
    void credentialsChanged(const QString &connection);
    void configChanged();                           // configChangeAnswered
    void stop();
    void discover(const QString &connection);       // discoverAnswered
    // The report proposal of the connection was written or declined (REQ-GRD-15). A service
    // without this method is not an error.
    void reportHandled(const QString &connection);
    // The alerts (the table at its top): every answer is a signal below.
    void alerts();                                                          // alertsAnswered
    void acknowledgeAlert(const QString &subject, const QString &slot);     // acknowledgeAnswered
    void snoozeAlert(const QString &subject, const QString &slot, int minutes); // snoozeAnswered
    void setSilent(const QString &subject, int minutes);                    // setSilentAnswered
    void endSilent(const QString &subject);                                 // endSilentAnswered
    // The low and high limits of one patient: setThresholdsAnswered.
    void setThresholds(const QString &subject, int l, int h, int typedL, int typedH);
    // Store maintenance and the diagnostic report: startMaintenanceAnswered, then maintenanceProgress / maintenanceFinished;
    // diagnosticReportAnswered.
    void startMaintenance(const QString &kind, const QString &parametersJson);
    void cancelMaintenance();
    void diagnosticReport(const QString &connection);

signals:
    void startAnswered(bool ok, const QString &state);
    void statusAnswered(bool ok, const QByteArray &json);
    void configChangeAnswered(bool ok, const QString &result);      // "", "restart-needed", "config-invalid"
    void discoverAnswered(const QString &connection, const QString &json, const QString &errorKey);
    // The bus name of the service gained or lost its holder.
    void reachableChanged(bool reachable);
    // The service's signals, passed on.
    void dataChanged(const QString &subject);
    void stateChanged(const QString &subject, const QString &state, const QString &cause,
                      const QString &connection);
    void stalenessChanged(const QString &subject, bool stale, const QString &cause);
    void warning(const QString &kind, const QString &connection, const QString &detail);
    void diagnosticReportProposed(const QString &connection);
    void gapDetected(const QString &fromIso, const QString &toIso);
    void alertChanged(const QString &subject, const QString &slot, const QString &state);
    void silentChanged(const QString &subject, bool silent, const QString &untilIso);
    // The answers of the alert calls; `ok` false when the bus or the service did not answer.
    void alertsAnswered(bool ok, const QByteArray &json);
    void acknowledgeAnswered(bool ok, const QString &subject, const QString &slot, const QString &result);
    void snoozeAnswered(bool ok, const QString &subject, const QString &slot, const QString &result);
    void setSilentAnswered(bool ok, const QString &subject, const QByteArray &json);
    void endSilentAnswered(bool ok, const QString &subject, const QString &result);
    void setThresholdsAnswered(bool ok, const QString &subject, const QByteArray &json);
    void startMaintenanceAnswered(bool ok, const QString &kind, const QString &result);
    void maintenanceProgress(const QString &kind, const QString &phase, int step, int steps,
                             qlonglong done, qlonglong total, double fraction);
    void maintenanceFinished(const QString &kind, bool ok, const QString &errorClass, const QString &message,
                             const QString &cancelReason, const QString &detailsJson);
    // The report's text is never logged: it is shown to the user and written only with consent.
    void diagnosticReportAnswered(bool ok, const QString &connection, const QString &text);
    // Why a call failed: passed on client, which sends it just BEFORE
    // the call's `...Answered(false,...)` signal, with the D-Bus method name ("Alerts") and the D-Bus error
    // name. A receiver that wants to tell "an older service without this method" from "no service" keeps
    // the reason from here and reads it when the answer arrives (ServiceClient::isOlderServiceError).
    // Only names travel here, never an argument of the call.
    void callFailed(const QString &method, const QString &errorName);

private:
    void wire(const QString &busName, const QDBusConnection &bus);

    ServiceClient *m_client;
    QDBusServiceWatcher *m_watcher;
    QString m_busName;
    QString m_objectPath;
    QDBusConnection m_bus;
};

} // namespace real
} // namespace lm

#endif
