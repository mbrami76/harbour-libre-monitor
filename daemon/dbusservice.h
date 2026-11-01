// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DBUSSERVICE_H
#define LM_DBUSSERVICE_H

// The D-Bus interface of the service (REQ-ARC-02,
// REQ-ARC-07). Names and signatures are fixed HERE and documented;
// this is the contract the user interface (the UI) and the optional add-on (a ping) rely on.
//
// Pattern: a Qt D-Bus adaptor over a plain C++ interface (ServiceApi). The adaptor only
// translates between D-Bus and ServiceApi; the logic lives in the runtime, which tests can
// call without a bus. A call to ANY method, and to org.freedesktop.DBus.Peer.Ping, activates
// The service when it is not running; a call to a running service does nothing harmful.
//
// Payload choices (stated once, here): simple values are plain D-Bus types; structured
// results travel as ONE JSON text (`s`) so that adding a field never changes a signature
// (the contract is unstable until the first release, REQ-ADP-14). Subjects are named by their
// opaque UUID in braces, connections by their opaque identifier: never a name, a credential
// or a token (REQ-SEC-01).
//
// Methods                                               returns
//   Start()                                             s  state name of the service
//   Status()                                            s  JSON,
//   Retry(s connection)                                 -  scheduler.userRetry (REQ-GRD-14)
//   Reenable(s connection)                              b  guard.reenable (REQ-GRD-08)
//   ReenableVersion(s adapter, s version)               b  guard.reenableVersion
//   ReportHandled(s connection)                         b  the report was written or declined: ends the
//                                                          proposal (REQ-GRD-15); true when one was open
//   DiagnosticReport(s connection)                      s  the anonymised diagnostic report of the connection's
//                                                          last guard kill as JSON Lines (REQ-GRD-15), or
//                                                          {"format":1,"error":...}: unknown-connection,
//                                                          not-proposed, no-report, service-not-running. No
//                                                          glucose value, name, reference or credential in it;
//                                                          it does NOT end the proposal (ReportHandled does)
//   CredentialsChanged(s connection)                    -  after the UI changed them
//   IntervalsChanged(s connection)                      -  the UI changed the intervals
//   ConfigChanged()                                     s  "" when the new file is applied, else
//                                                          "restart-needed" or "config-invalid"
//   Discover(s connection)                              s  ONE JSON text, answered LATER (a delayed
//                                                          reply, up to about a minute): the Subjects
//                                                          the account may see, or an error class
//                                                          (daemon/discovery.h). Carries
//                                                          names; never a credential or a session.
//   Alerts()                                            s  JSON: the Active, Acknowledged and Snoozed alert
//                                                          slots and the thresholds in force
//   AcknowledgeAlert(s subject, s slot)                 s  one word: done, not-active, already-acknowledged,
//                                                          unknown-subject, bad-slot, service-not-running
//   SnoozeAlert(s subject, s slot, i minutes)           s  as above, plus not-snoozable (LL, HH) and bad-duration
//                                                          (not 15, 30, 60 or 120)
//   SetThresholds(s subject, i l, i h, i typedL, i typedH)
//                                                       s  JSON {"result": word, ...}: accepted, no-change,
//                                                          invalid-value, inconsistent, needs-confirmation,
//                                                          confirmation-mismatch, unknown-subject, not-saved,
//                                                          service-not-running (details with it).
//                                                          No comment: a caller of the old
//                                                          six-argument shape gets UnknownMethod
//   SetSilent(s subject, i minutes)                     s  JSON {"result": word, "silent_until": iso}: the silent
//                                                          mode of a Subject: minutes > 0, 0 without limit,
//                                                          -1 until tomorrow. done, unknown-subject, bad-duration,
//                                                          unsupported, service-not-running
//   EndSilent(s subject)                                s  done, not-silent, unknown-subject, unsupported, service-not-running
//   DevInject(s json)                                   s  DEVELOPMENT BUILDS ONLY (flag devsource, absent from a
//                                                          release package): feeds synthetic effects to the
//                                                          notifier (daemon/devfeed.h); JSON {"ok":..}
//   StartMaintenance(s kind, s parametersJson)          s  "" when started, else an error key
//   CancelMaintenance()                                 -
//   Stop()                                              -  an orderly stop (reason `requested`)
//
// Signals
//   DataChanged(s subject)
//   StateChanged(s subject, s state, s cause, s connection)   the connection was added
//   StalenessChanged(s subject, b stale, s staleCause)
//   Warning(s kind, s connection, s detail)             also reading-queue-high-water and
//                                                       reading-queue-full (detail "count=N")
//   DiagnosticReportProposed(s connection)
//   MaintenanceProgress(s kind, s phase, i step, i steps, x done, x total, d fraction)
//   MaintenanceFinished(s kind, b ok, s errorClass, s message, s cancelReason, s detailsJson)
//   GapDetected(s fromIso, s toIso)
//   SilentChanged(s subject, b silent, s untilIso)      a Subject's silent mode started, was ended or ran out
//   AlertChanged(s subject, s slot, s state)            an alert slot was raised, escalated, acknowledged,
//                                                       snoozed, lowered or cleared; state is its name after
//                                                       the change; never a value

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusContext>
#include <QObject>
#include <QString>
#include <QDBusMessage>
#include <QVariantList>

#include <functional>

namespace lm {

// What the adaptor calls; the runtime implements it.
class ServiceApi
{
public:
    virtual ~ServiceApi() {}
    virtual QString apiStart() = 0;
    virtual QString apiStatusJson() = 0;
    virtual void apiRetry(const QString &connection) = 0;
    virtual bool apiReenable(const QString &connection) = 0;
    virtual bool apiReenableVersion(const QString &adapter, const QString &version) = 0;
    virtual bool apiReportHandled(const QString &connection) = 0;
    virtual QString apiDiagnosticReport(const QString &connection) = 0;
    virtual void apiCredentialsChanged(const QString &connection) = 0;
    virtual void apiIntervalsChanged(const QString &connection) = 0;
    virtual QString apiConfigChanged() = 0;
    virtual QString apiAlerts() = 0;
    virtual QString apiAcknowledgeAlert(const QString &subject, const QString &slot) = 0;
    virtual QString apiSnoozeAlert(const QString &subject, const QString &slot, int minutes) = 0;
#ifdef LIBREMON_DEVSOURCE
    virtual QString apiDevInject(const QString &json) = 0;
#endif
    virtual QString apiSetSilent(const QString &subject, int minutes) = 0;
    virtual QString apiEndSilent(const QString &subject) = 0;
    virtual QString apiSetThresholds(const QString &subject, int l, int h, int typedL, int typedH) = 0;
    // Asynchronous: `done` is called once, later, with the JSON answer (daemon/discovery.h).
    virtual void apiDiscover(const QString &connection,
                             const std::function<void(const QString &)> &done) = 0;
    virtual QString apiStartMaintenance(const QString &kind, const QString &parametersJson) = 0;
    virtual void apiCancelMaintenance() = 0;
    virtual void apiStop() = 0;
};

// The object that is exported on the bus and that the adaptor hangs from. It is a QDBusContext
// because `Discover` replies LATER (a delayed reply): the slot returns at once and the answer
// is sent when the discovery is done. QtDBus describes the call being delivered to the exported
// OBJECT, not to its adaptor, so the context has to be here (found by a crash in a test: the
// context of an adaptor is empty). The runtime is one; a test's fake service is another.
class ServiceObject : public QObject, public QDBusContext
{
    Q_OBJECT
public:
    explicit ServiceObject(QObject *parent = 0) : QObject(parent) {}
};

class ServiceAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "it.michelebrami.libre_monitor.Service")

public:
    // `parent` is the object the adaptor is attached to (the runtime); `api` is not owned.
    ServiceAdaptor(ServiceObject *parent, ServiceApi *api);

public slots:
    QString Start();
    QString Status();
    void Retry(const QString &connection);
    bool Reenable(const QString &connection);
    bool ReenableVersion(const QString &adapter, const QString &version);
    bool ReportHandled(const QString &connection);
    QString DiagnosticReport(const QString &connection);
    void CredentialsChanged(const QString &connection);
    void IntervalsChanged(const QString &connection);
    QString ConfigChanged();
    QString Alerts();
    QString AcknowledgeAlert(const QString &subject, const QString &slot);
    QString SnoozeAlert(const QString &subject, const QString &slot, int minutes);
    QString SetThresholds(const QString &subject, int l, int h, int typedL, int typedH);
#ifdef LIBREMON_DEVSOURCE
    QString DevInject(const QString &json);
#endif
    QString SetSilent(const QString &subject, int minutes);
    QString EndSilent(const QString &subject);
    QString Discover(const QString &connection);
    QString StartMaintenance(const QString &kind, const QString &parametersJson);
    void CancelMaintenance();
    void Stop();

signals:
    void DataChanged(const QString &subject);
    void StateChanged(const QString &subject, const QString &state, const QString &cause,
                      const QString &connection);
    void StalenessChanged(const QString &subject, bool stale, const QString &staleCause);
    void Warning(const QString &kind, const QString &connection, const QString &detail);
    void DiagnosticReportProposed(const QString &connection);
    void MaintenanceProgress(const QString &kind, const QString &phase, int step, int steps,
                             qlonglong done, qlonglong total, double fraction);
    void MaintenanceFinished(const QString &kind, bool ok, const QString &errorClass,
                             const QString &message, const QString &cancelReason,
                             const QString &detailsJson);
    void GapDetected(const QString &fromIso, const QString &toIso);
    void AlertChanged(const QString &subject, const QString &slot, const QString &state);
    void SilentChanged(const QString &subject, bool silent, const QString &untilIso);

private:
    ServiceObject *m_object;
    ServiceApi *m_api;
};

// The client the UI uses: typed calls and one Qt signal per D-Bus signal.
//
// Every method comes in two forms. The plain form is blocking with a short
// timeout, for tools and tests. The asynchronous form (`...Async`) returns AT ONCE and delivers
// the answer by a signal named `...Answered`, so a window never waits for the bus: the first call
// on a stopped service activates it, which takes seconds. Each answer signal starts with `ok`:
// false when the bus or the service did not answer (no service, a timeout, an unknown method);
// the other values are then empty. Answers repeat the arguments that tell concurrent calls apart.
class ServiceClient : public QObject
{
    Q_OBJECT
public:
    // Empty names mean "this application's identity". Uses the session bus of the process.
    explicit ServiceClient(const QString &busName = QString(), const QString &objectPath = QString(),
                           QObject *parent = 0);
    // The same on a connection of the caller's own (QDBusConnection::connectToBus). A test needs
    // one: a client that shares the service's connection in one process is answered by a local
    // shortcut that cannot carry a delayed reply (`Discover`), which two real processes never meet.
    ServiceClient(const QString &busName, const QString &objectPath, const QDBusConnection &bus,
                  QObject *parent = 0);

    bool isValid() const;
    QString start();
    QString statusJson();
    void retry(const QString &connection);
    bool reenable(const QString &connection);
    bool reenableVersion(const QString &adapter, const QString &version);
    // The UI says the diagnostic report for the connection was written or declined. True
    // when the service had a proposal open and ended it; false when none was open or the bus or
    // The service did not answer (an older service has no such method).
    bool reportHandled(const QString &connection);
    // The diagnostic report of the connection: the JSON Lines text, or the JSON error object
    // the method documents above; empty when the bus or the service did not answer (an older
    // service has no such method).
    QString diagnosticReport(const QString &connection);
    // True when the service answered (the call may start it); false when the bus or the
    // service did not. Nothing about the connection travels back: the caller only learns
    // whether the notification arrived.
    bool credentialsChanged(const QString &connection);
    void intervalsChanged(const QString &connection);
    // As above; `result` gets the service's answer (see ConfigChanged above).
    bool configChanged(QString *result);
    QString startMaintenance(const QString &kind, const QString &parametersJson);
    void cancelMaintenance();
    void stop();
    // Activates a stopped service without any effect on a running one (the add-on's ping).
    bool ping();
    // The alerts. Each returns the service's answer as the text the
    // method documents above; empty when the bus or the service did not answer (a service
    // without the method is an older one).
    QString alertsJson();
    QString acknowledgeAlert(const QString &subject, const QString &slot);
    QString snoozeAlert(const QString &subject, const QString &slot, int minutes);
    // `typedL` and `typedH` are the values typed AGAIN for the challenge of a looser edit, whole
    // mg/dL, -1 where nothing was typed; any value of 0 or more counts as the confirmation.
    QString setThresholds(const QString &subject, int l, int h, int typedL, int typedH);
    // The silent mode of a Subject: `minutes` > 0, 0 without limit, -1 until tomorrow.
#ifdef LIBREMON_DEVSOURCE
    QString devInject(const QString &json);
#endif
    QString setSilent(const QString &subject, int minutes);
    QString endSilent(const QString &subject);
    // Blocks for up to about a minute: use discoverAsync from a window. Returns the JSON answer;
    // empty with `errorKey` set when the service did not answer: "not-available" (a service
    // without the method, an older one) or "unreachable".
    QString discover(const QString &connection, QString *errorKey);

    // ---- asynchronous forms -------------------------------------------------------------
    void startAsync();                                          // startAnswered(ok, state)
    void statusJsonAsync();                                     // statusAnswered(ok, json)
    void retryAsync(const QString &connection);                 // retryAnswered(ok, connection)
    void reenableAsync(const QString &connection);              // reenableAnswered(ok, connection, reenabled)
    void reenableVersionAsync(const QString &adapter, const QString &version);
                                                                // reenableVersionAnswered(ok, adapter, version, reenabled)
    void reportHandledAsync(const QString &connection);         // reportHandledAnswered(ok, connection, handled)
    void diagnosticReportAsync(const QString &connection);      // diagnosticReportAnswered(ok, connection, text)
    void credentialsChangedAsync(const QString &connection);    // credentialsChangedAnswered(ok, connection)
    void intervalsChangedAsync(const QString &connection);      // intervalsChangedAnswered(ok, connection)
    void configChangedAsync();                                  // configChangedAnswered(ok, result)
    void startMaintenanceAsync(const QString &kind, const QString &parametersJson);
                                                                // startMaintenanceAnswered(ok, kind, result)
    void cancelMaintenanceAsync();                              // cancelMaintenanceAnswered(ok)
    void stopAsync();                                           // stopAnswered(ok)
    void pingAsync();                                           // pingAnswered(ok)
    void discoverAsync(const QString &connection);              // discoverAnswered(connection, json, errorKey)
    void alertsAsync();                                         // alertsAnswered(ok, json)
    void acknowledgeAlertAsync(const QString &subject, const QString &slot);
                                                                // acknowledgeAlertAnswered(ok, subject, slot, result)
    void snoozeAlertAsync(const QString &subject, const QString &slot, int minutes);
                                                                // snoozeAlertAnswered(ok, subject, slot, result)
    void setSilentAsync(const QString &subject, int minutes);   // setSilentAnswered(ok, subject, json)
    void endSilentAsync(const QString &subject);                // endSilentAnswered(ok, subject, result)
    void setThresholdsAsync(const QString &subject, int l, int h, int typedL, int typedH);
                                                                // setThresholdsAnswered(ok, subject, json)

signals:
    void dataChanged(const QString &subject);
    void stateChanged(const QString &subject, const QString &state, const QString &cause,
                      const QString &connection);
    void stalenessChanged(const QString &subject, bool stale, const QString &staleCause);
    void warning(const QString &kind, const QString &connection, const QString &detail);
    void diagnosticReportProposed(const QString &connection);
    void maintenanceProgress(const QString &kind, const QString &phase, int step, int steps,
                             qlonglong done, qlonglong total, double fraction);
    void maintenanceFinished(const QString &kind, bool ok, const QString &errorClass,
                             const QString &message, const QString &cancelReason,
                             const QString &detailsJson);
    void gapDetected(const QString &fromIso, const QString &toIso);
    void alertChanged(const QString &subject, const QString &slot, const QString &state);
    void silentChanged(const QString &subject, bool silent, const QString &untilIso);

    // ---- answers of the asynchronous forms ------------------------------------------------
    void startAnswered(bool ok, const QString &state);
    void statusAnswered(bool ok, const QString &json);
    void retryAnswered(bool ok, const QString &connection);
    void reenableAnswered(bool ok, const QString &connection, bool reenabled);
    void reenableVersionAnswered(bool ok, const QString &adapter, const QString &version, bool reenabled);
    void reportHandledAnswered(bool ok, const QString &connection, bool handled);
    void diagnosticReportAnswered(bool ok, const QString &connection, const QString &text);
    void credentialsChangedAnswered(bool ok, const QString &connection);
    void intervalsChangedAnswered(bool ok, const QString &connection);
    void configChangedAnswered(bool ok, const QString &result);     // "", "restart-needed", "config-invalid"
    void startMaintenanceAnswered(bool ok, const QString &kind, const QString &result);
    void cancelMaintenanceAnswered(bool ok);
    void stopAnswered(bool ok);
    void pingAnswered(bool ok);
    void alertsAnswered(bool ok, const QString &json);
    void acknowledgeAlertAnswered(bool ok, const QString &subject, const QString &slot, const QString &result);
    void snoozeAlertAnswered(bool ok, const QString &subject, const QString &slot, const QString &result);
    void setThresholdsAnswered(bool ok, const QString &subject, const QString &json);
    void setSilentAnswered(bool ok, const QString &subject, const QString &json);
    void endSilentAnswered(bool ok, const QString &subject, const QString &result);
    // `errorKey` is empty when the service answered (then `json` is its answer); else
    // "not-available" or "unreachable" as for discover().
    void discoverAnswered(const QString &connection, const QString &json, const QString &errorKey);
    // sent for EVERY asynchronous call that ended in a D-Bus error, just before its
    // `...Answered(false, ...)` signal, with the method's name and the D-Bus error name (for example
    // `org.freedesktop.DBus.Error.UnknownMethod`: the service runs but is an older version without
    // the method; `...ServiceUnknown` or `...NoReply`: no service could be reached). The answers keep
    // their shape; a window that needs the reason listens here. Only names travel: never an argument.
    void callFailed(const QString &method, const QString &errorName);

public:
    // True for the error names that mean "the service answered but does not know this method"
    // (UnknownMethod, UnknownInterface, UnknownObject): an older service, not a stopped one.
    static bool isOlderServiceError(const QString &errorName);

private:
    typedef std::function<void(const QDBusMessage &)> ReplyHandler;
    QString callString(const char *method, const QVariantList &args);
    bool callVoid(const char *method, const QVariantList &args);
    // Sends the call on the session bus without waiting; `handler` gets the reply (or the
    // error message) from the event loop.
    void callAsync(const QString &interfaceName, const char *method, const QVariantList &args,
                   int timeoutMs, const ReplyHandler &handler);
    void callAsync(const char *method, const QVariantList &args, const ReplyHandler &handler);

    void init(const QString &busName, const QString &objectPath);

    QDBusConnection m_bus;
    QString m_busName;
    QString m_path;
};

} // namespace lm

#endif
