// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "dbusservice.h"

#include "serviceidentity.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QVariantList>

namespace lm {

ServiceAdaptor::ServiceAdaptor(ServiceObject *parent, ServiceApi *api)
    : QDBusAbstractAdaptor(parent), m_object(parent), m_api(api)
{
    // Signals of the adaptor are emitted by the runtime directly; nothing to connect here.
    setAutoRelaySignals(false);
}

QString ServiceAdaptor::Start() { return m_api->apiStart(); }
QString ServiceAdaptor::Status() { return m_api->apiStatusJson(); }
void ServiceAdaptor::Retry(const QString &c) { m_api->apiRetry(c); }
bool ServiceAdaptor::Reenable(const QString &c) { return m_api->apiReenable(c); }
bool ServiceAdaptor::ReenableVersion(const QString &a, const QString &v) { return m_api->apiReenableVersion(a, v); }
bool ServiceAdaptor::ReportHandled(const QString &c) { return m_api->apiReportHandled(c); }
QString ServiceAdaptor::DiagnosticReport(const QString &c) { return m_api->apiDiagnosticReport(c); }
void ServiceAdaptor::CredentialsChanged(const QString &c) { m_api->apiCredentialsChanged(c); }
void ServiceAdaptor::IntervalsChanged(const QString &c) { m_api->apiIntervalsChanged(c); }
QString ServiceAdaptor::ConfigChanged() { return m_api->apiConfigChanged(); }
QString ServiceAdaptor::Alerts() { return m_api->apiAlerts(); }
QString ServiceAdaptor::AcknowledgeAlert(const QString &s, const QString &slot) { return m_api->apiAcknowledgeAlert(s, slot); }
QString ServiceAdaptor::SnoozeAlert(const QString &s, const QString &slot, int minutes) { return m_api->apiSnoozeAlert(s, slot, minutes); }
QString ServiceAdaptor::SetThresholds(const QString &s, int l, int h, int tl, int th)
{ return m_api->apiSetThresholds(s, l, h, tl, th); }
// A delayed reply: the slot returns at once and the answer is sent when the discovery is done,
// possibly a minute later. The call and the connection it came in on are copied into the
// callback, because the context only describes the call being delivered now.
#ifdef LIBREMON_DEVSOURCE
QString ServiceAdaptor::DevInject(const QString &json) { return m_api->apiDevInject(json); }
#endif
QString ServiceAdaptor::SetSilent(const QString &s, int minutes) { return m_api->apiSetSilent(s, minutes); }
QString ServiceAdaptor::EndSilent(const QString &s) { return m_api->apiEndSilent(s); }
QString ServiceAdaptor::Discover(const QString &connectionId)
{
    m_object->setDelayedReply(true);
    const QDBusMessage call = m_object->message();
    const QDBusConnection bus = m_object->connection();
    m_api->apiDiscover(connectionId, [call, bus](const QString &json) {
        QDBusConnection b = bus;
        b.send(call.createReply(json));
    });
    return QString();
}

QString ServiceAdaptor::StartMaintenance(const QString &k, const QString &p) { return m_api->apiStartMaintenance(k, p); }
void ServiceAdaptor::CancelMaintenance() { m_api->apiCancelMaintenance(); }
void ServiceAdaptor::Stop() { m_api->apiStop(); }

// ---- client ------------------------------------------------------------------------------

ServiceClient::ServiceClient(const QString &busName, const QString &objectPath, QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus())
{
    init(busName, objectPath);
}

ServiceClient::ServiceClient(const QString &busName, const QString &objectPath,
                             const QDBusConnection &bus, QObject *parent)
    : QObject(parent), m_bus(bus)
{
    init(busName, objectPath);
}

void ServiceClient::init(const QString &busName, const QString &objectPath)
{
    const ServiceIdentity id = ServiceIdentity::forThisApplication();
    m_busName = busName.isEmpty() ? id.busName : busName;
    m_path = objectPath.isEmpty() ? id.objectPath : objectPath;

    QDBusConnection bus = m_bus;
    const QString iface = QString::fromLatin1(kServiceInterface);
    bus.connect(m_busName, m_path, iface, QStringLiteral("DataChanged"), this, SIGNAL(dataChanged(QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("StateChanged"), this, SIGNAL(stateChanged(QString,QString,QString,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("StalenessChanged"), this, SIGNAL(stalenessChanged(QString,bool,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("Warning"), this, SIGNAL(warning(QString,QString,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("DiagnosticReportProposed"), this, SIGNAL(diagnosticReportProposed(QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("MaintenanceProgress"), this, SIGNAL(maintenanceProgress(QString,QString,int,int,qlonglong,qlonglong,double)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("MaintenanceFinished"), this, SIGNAL(maintenanceFinished(QString,bool,QString,QString,QString,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("GapDetected"), this, SIGNAL(gapDetected(QString,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("AlertChanged"), this, SIGNAL(alertChanged(QString,QString,QString)));
    bus.connect(m_busName, m_path, iface, QStringLiteral("SilentChanged"), this, SIGNAL(silentChanged(QString,bool,QString)));
}

bool ServiceClient::isValid() const
{
    return m_bus.isConnected();
}

QString ServiceClient::callString(const char *method, const QVariantList &args)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface),
                         m_bus);
    iface.setTimeout(5000);
    const QDBusMessage reply = iface.callWithArgumentList(QDBus::Block, QString::fromLatin1(method), args);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return QString();
    return reply.arguments().first().toString();
}

bool ServiceClient::callVoid(const char *method, const QVariantList &args)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface),
                         m_bus);
    iface.setTimeout(5000);
    return iface.callWithArgumentList(QDBus::Block, QString::fromLatin1(method), args).type()
        == QDBusMessage::ReplyMessage;
}

QString ServiceClient::start() { return callString("Start", QVariantList()); }
QString ServiceClient::statusJson() { return callString("Status", QVariantList()); }
void ServiceClient::retry(const QString &c) { callVoid("Retry", QVariantList() << c); }
bool ServiceClient::credentialsChanged(const QString &c) { return callVoid("CredentialsChanged", QVariantList() << c); }

bool ServiceClient::configChanged(QString *result)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface),
                         m_bus);
    iface.setTimeout(5000);
    const QDBusMessage r = iface.call(QDBus::Block, QStringLiteral("ConfigChanged"));
    if (r.type() != QDBusMessage::ReplyMessage)
        return false;
    if (result)
        *result = r.arguments().isEmpty() ? QString() : r.arguments().first().toString();
    return true;
}
void ServiceClient::intervalsChanged(const QString &c) { callVoid("IntervalsChanged", QVariantList() << c); }
void ServiceClient::cancelMaintenance() { callVoid("CancelMaintenance", QVariantList()); }
void ServiceClient::stop() { callVoid("Stop", QVariantList()); }

QString ServiceClient::startMaintenance(const QString &kind, const QString &parametersJson)
{
    return callString("StartMaintenance", QVariantList() << kind << parametersJson);
}

bool ServiceClient::reenable(const QString &c)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface), m_bus);
    iface.setTimeout(5000);
    const QDBusMessage r = iface.call(QDBus::Block, QStringLiteral("Reenable"), c);
    return r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty() && r.arguments().first().toBool();
}

bool ServiceClient::reenableVersion(const QString &a, const QString &v)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface), m_bus);
    iface.setTimeout(5000);
    const QDBusMessage r = iface.call(QDBus::Block, QStringLiteral("ReenableVersion"), a, v);
    return r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty() && r.arguments().first().toBool();
}

#ifdef LIBREMON_DEVSOURCE
QString ServiceClient::devInject(const QString &json) { return callString("DevInject", QVariantList() << json); }
#endif
QString ServiceClient::setSilent(const QString &s, int minutes) { return callString("SetSilent", QVariantList() << s << minutes); }
QString ServiceClient::endSilent(const QString &s) { return callString("EndSilent", QVariantList() << s); }
QString ServiceClient::alertsJson() { return callString("Alerts", QVariantList()); }
QString ServiceClient::acknowledgeAlert(const QString &s, const QString &slot)
{
    return callString("AcknowledgeAlert", QVariantList() << s << slot);
}
QString ServiceClient::snoozeAlert(const QString &s, const QString &slot, int minutes)
{
    return callString("SnoozeAlert", QVariantList() << s << slot << minutes);
}
QString ServiceClient::setThresholds(const QString &s, int l, int h, int typedL, int typedH)
{
    return callString("SetThresholds", QVariantList() << s << l << h << typedL << typedH);
}

QString ServiceClient::diagnosticReport(const QString &c)
{
    return callString("DiagnosticReport", QVariantList() << c);
}

bool ServiceClient::reportHandled(const QString &c)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface), m_bus);
    iface.setTimeout(5000);
    const QDBusMessage r = iface.call(QDBus::Block, QStringLiteral("ReportHandled"), c);
    return r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty() && r.arguments().first().toBool();
}

namespace {

// How long an asynchronous call may take. The first call to a stopped service activates it,
// which takes seconds; a discovery is a network round trip bounded by the service at about a
// minute (daemon/discovery.h).
const int kAsyncTimeoutMs = 25000;
const int kDiscoverTimeoutMs = 70000;

// "not-available" when the service has no such method, interface or object (an older service,
// or none that can be started); "unreachable" for anything else that went wrong.
QString discoverErrorKey(const QDBusMessage &reply)
{
    const QString err = reply.errorName();
    const bool unknown = err.contains(QLatin1String("UnknownMethod")) || err.contains(QLatin1String("UnknownInterface"))
                      || err.contains(QLatin1String("UnknownObject")) || err.contains(QLatin1String("ServiceUnknown"));
    return unknown ? QStringLiteral("not-available") : QStringLiteral("unreachable");
}

bool isReply(const QDBusMessage &m) { return m.type() == QDBusMessage::ReplyMessage; }

QString firstString(const QDBusMessage &m)
{
    return isReply(m) && !m.arguments().isEmpty() ? m.arguments().first().toString() : QString();
}

bool firstBool(const QDBusMessage &m)
{
    return isReply(m) && !m.arguments().isEmpty() && m.arguments().first().toBool();
}

} // namespace

QString ServiceClient::discover(const QString &connection, QString *errorKey)
{
    QDBusInterface iface(m_busName, m_path, QString::fromLatin1(kServiceInterface), m_bus);
    iface.setTimeout(kDiscoverTimeoutMs);
    const QDBusMessage r = iface.call(QDBus::Block, QStringLiteral("Discover"), connection);
    if (isReply(r) && !r.arguments().isEmpty()) {
        if (errorKey)
            errorKey->clear();
        return r.arguments().first().toString();
    }
    if (errorKey)
        *errorKey = discoverErrorKey(r);
    return QString();
}

// ---- asynchronous forms ---------------------------------------------------------------------

void ServiceClient::callAsync(const QString &interfaceName, const char *method, const QVariantList &args,
                              int timeoutMs, const ReplyHandler &handler)
{
    QDBusMessage call = QDBusMessage::createMethodCall(m_busName, m_path, interfaceName,
                                                       QString::fromLatin1(method));
    call.setArguments(args);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
        m_bus.asyncCall(call, timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, handler, method]() {
        const QDBusMessage reply = watcher->reply();
        watcher->deleteLater();
        // the reason of a failure, for a window that must tell an older service from none.
        if (reply.type() == QDBusMessage::ErrorMessage)
            emit callFailed(QString::fromLatin1(method), reply.errorName());
        handler(reply);
    });
}

bool ServiceClient::isOlderServiceError(const QString &errorName)
{
    return errorName == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")
        || errorName == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface")
        || errorName == QLatin1String("org.freedesktop.DBus.Error.UnknownObject");
}

// The calls to the service's own interface, with the default timeout.
void ServiceClient::callAsync(const char *method, const QVariantList &args, const ReplyHandler &handler)
{
    callAsync(QString::fromLatin1(kServiceInterface), method, args, kAsyncTimeoutMs, handler);
}

void ServiceClient::startAsync()
{
    callAsync("Start", QVariantList(), [this](const QDBusMessage &r) { emit startAnswered(isReply(r), firstString(r)); });
}

void ServiceClient::statusJsonAsync()
{
    callAsync("Status", QVariantList(), [this](const QDBusMessage &r) { emit statusAnswered(isReply(r), firstString(r)); });
}

void ServiceClient::retryAsync(const QString &c)
{
    callAsync("Retry", QVariantList() << c, [this, c](const QDBusMessage &r) { emit retryAnswered(isReply(r), c); });
}

void ServiceClient::reenableAsync(const QString &c)
{
    callAsync("Reenable", QVariantList() << c,
             [this, c](const QDBusMessage &r) { emit reenableAnswered(isReply(r), c, firstBool(r)); });
}

void ServiceClient::reenableVersionAsync(const QString &a, const QString &v)
{
    callAsync("ReenableVersion", QVariantList() << a << v,
             [this, a, v](const QDBusMessage &r) { emit reenableVersionAnswered(isReply(r), a, v, firstBool(r)); });
}

void ServiceClient::reportHandledAsync(const QString &c)
{
    callAsync("ReportHandled", QVariantList() << c,
             [this, c](const QDBusMessage &r) { emit reportHandledAnswered(isReply(r), c, firstBool(r)); });
}

void ServiceClient::diagnosticReportAsync(const QString &c)
{
    callAsync("DiagnosticReport", QVariantList() << c,
             [this, c](const QDBusMessage &r) { emit diagnosticReportAnswered(isReply(r), c, firstString(r)); });
}

void ServiceClient::setSilentAsync(const QString &s, int minutes)
{
    callAsync("SetSilent", QVariantList() << s << minutes,
             [this, s](const QDBusMessage &r) { emit setSilentAnswered(isReply(r), s, firstString(r)); });
}

void ServiceClient::endSilentAsync(const QString &s)
{
    callAsync("EndSilent", QVariantList() << s,
             [this, s](const QDBusMessage &r) { emit endSilentAnswered(isReply(r), s, firstString(r)); });
}

void ServiceClient::alertsAsync()
{
    callAsync("Alerts", QVariantList(), [this](const QDBusMessage &r) { emit alertsAnswered(isReply(r), firstString(r)); });
}

void ServiceClient::acknowledgeAlertAsync(const QString &s, const QString &slot)
{
    callAsync("AcknowledgeAlert", QVariantList() << s << slot,
             [this, s, slot](const QDBusMessage &r) { emit acknowledgeAlertAnswered(isReply(r), s, slot, firstString(r)); });
}

void ServiceClient::snoozeAlertAsync(const QString &s, const QString &slot, int minutes)
{
    callAsync("SnoozeAlert", QVariantList() << s << slot << minutes,
             [this, s, slot](const QDBusMessage &r) { emit snoozeAlertAnswered(isReply(r), s, slot, firstString(r)); });
}

void ServiceClient::setThresholdsAsync(const QString &s, int l, int h, int typedL, int typedH)
{
    callAsync("SetThresholds", QVariantList() << s << l << h << typedL << typedH,
             [this, s](const QDBusMessage &r) { emit setThresholdsAnswered(isReply(r), s, firstString(r)); });
}

void ServiceClient::credentialsChangedAsync(const QString &c)
{
    callAsync("CredentialsChanged", QVariantList() << c,
             [this, c](const QDBusMessage &r) { emit credentialsChangedAnswered(isReply(r), c); });
}

void ServiceClient::intervalsChangedAsync(const QString &c)
{
    callAsync("IntervalsChanged", QVariantList() << c,
             [this, c](const QDBusMessage &r) { emit intervalsChangedAnswered(isReply(r), c); });
}

void ServiceClient::configChangedAsync()
{
    callAsync("ConfigChanged", QVariantList(), [this](const QDBusMessage &r) { emit configChangedAnswered(isReply(r), firstString(r)); });
}

void ServiceClient::startMaintenanceAsync(const QString &kind, const QString &parametersJson)
{
    callAsync("StartMaintenance", QVariantList() << kind << parametersJson,
             [this, kind](const QDBusMessage &r) { emit startMaintenanceAnswered(isReply(r), kind, firstString(r)); });
}

void ServiceClient::cancelMaintenanceAsync()
{
    callAsync("CancelMaintenance", QVariantList(), [this](const QDBusMessage &r) { emit cancelMaintenanceAnswered(isReply(r)); });
}

void ServiceClient::stopAsync()
{
    callAsync("Stop", QVariantList(), [this](const QDBusMessage &r) { emit stopAnswered(isReply(r)); });
}

void ServiceClient::pingAsync()
{
    callAsync(QStringLiteral("org.freedesktop.DBus.Peer"), "Ping", QVariantList(), kAsyncTimeoutMs,
              [this](const QDBusMessage &r) { emit pingAnswered(isReply(r)); });
}

void ServiceClient::discoverAsync(const QString &connection)
{
    callAsync(QString::fromLatin1(kServiceInterface), "Discover", QVariantList() << connection,
              kDiscoverTimeoutMs, [this, connection](const QDBusMessage &r) {
                  if (isReply(r) && !r.arguments().isEmpty())
                      emit discoverAnswered(connection, r.arguments().first().toString(), QString());
                  else
                      emit discoverAnswered(connection, QString(), discoverErrorKey(r));
              });
}

bool ServiceClient::ping()
{
    QDBusInterface peer(m_busName, m_path, QStringLiteral("org.freedesktop.DBus.Peer"),
                        m_bus);
    peer.setTimeout(5000);
    return peer.call(QDBus::Block, QStringLiteral("Ping")).type() == QDBusMessage::ReplyMessage;
}

} // namespace lm
