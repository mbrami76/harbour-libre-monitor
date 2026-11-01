// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "realaccountbackend.h"

#include "daemon/dbusservice.h"
#include "daemon/provisioning.h"
#include "daemon/serviceidentity.h"
#include "ui/accounterrors.h"
#include "ui/configstore.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace lm {
namespace real {

namespace {
const char *kProbeId = "probe";         // the secret under which a login is tried
const char *kConnectionId = "c1";       // the one account of the normal case
const char *kAdapterId = "libre";

// The classes the page knows are in one list (ui/accounterrors.h); anything else from the service becomes
// "internal", never the service's own words.
QString knownError(const QString &key) { return ui::normalizedAccountError(key); }
}

ui::LoginTestResult parseDiscoverAnswer(const QString &json, const QString &errorKey)
{
    ui::LoginTestResult r;
    if (!errorKey.isEmpty()) {
        r.errorKey = knownError(errorKey);
        return r;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        r.errorKey = QStringLiteral("internal");
        return r;
    }
    const QJsonObject o = doc.object();
    if (!o.value(QLatin1String("ok")).toBool(false)) {
        r.errorKey = knownError(o.value(QLatin1String("error")).toString());
        return r;
    }
    const QJsonArray subs = o.value(QLatin1String("subjects")).toArray();
    for (int i = 0; i < subs.size(); ++i) {
        const QJsonObject s = subs.at(i).toObject();
        ui::DiscoveredSubject d;
        d.ref = s.value(QLatin1String("subject_ref")).toString();
        d.displayName = s.value(QLatin1String("display_name")).toString();
        d.unit = s.value(QLatin1String("unit")).toString();
        d.unitBasis = s.value(QLatin1String("unit_basis")).toString();
        if (!d.ref.isEmpty())
            r.subjects.append(d);
    }
    r.ok = true;
    return r;
}

// ---- the worker ------------------------------------------------------------------------------

class AccountWorkerImpl : public QObject
{
    Q_OBJECT
public:
    AccountWorkerImpl(CredentialWriter *w, AccountServicePort *p, const QString &configPath)
        : m_writer(w), m_port(p), m_configPath(configPath) {}

public slots:
    void test(const QString &user, const QString &password)
    {
        ui::LoginTestResult result;
        QString err;
        // The probe secret holds the login for the length of the test only.
        if (!m_writer->store(QString::fromLatin1(kProbeId), user, password, &err)) {
            result.errorKey = QStringLiteral("secret-store-failed");
            emit loginTested(result);
            return;
        }
        QString errorKey;
        const QString json = m_port->discover(QString::fromLatin1(kProbeId), &errorKey);
        QString ignore;
        m_writer->remove(QString::fromLatin1(kProbeId), &ignore);       // always, whatever happened
        emit loginTested(parseDiscoverAnswer(json, errorKey));
    }

    // A saved account: the service uses its stored credentials (and the stored session while it
    // lives, nothing new is saved); this side stores and removes nothing.
    void discoverKnown(const QString &connection)
    {
        QString errorKey;
        const QString json = m_port->discover(connection, &errorKey);
        emit loginTested(parseDiscoverAnswer(json, errorKey));
    }

    void save(const lm::ui::SaveRequest &req)
    {
        QString err;
        ui::ConfigStore config;
        if (!config.load(m_configPath)) {
            emit saved(false, QStringLiteral("config-invalid"));
            return;
        }
        // 1. the secret, replacing an earlier one
        if (!m_writer->store(req.connectionId, req.username, req.password, &err)) {
            emit saved(false, QStringLiteral("secret-store-failed"));
            return;
        }
        // 2. the configuration: a Subject that was followed before keeps its UUID (REQ-SUB-02:
        //    created once, never reused), a new one gets a new UUID.
        ui::ConnectionEntry conn;
        const QList<ui::ConnectionEntry> existing = config.connections();
        for (int i = 0; i < existing.size(); ++i)
            if (existing.at(i).id == req.connectionId)
                conn = existing.at(i);
        const QList<ui::SubjectEntry> before = conn.subjects;
        conn.id = req.connectionId;
        conn.adapter = QString::fromLatin1(kAdapterId);
        // The name the guard and approval notices use: the translated word "Account" at the
        // first save; a label already there (perhaps changed by the user in the settings) is kept.
        if (conn.label.isEmpty())
            conn.label = req.connectionLabel.left(ui::kConnectionLabelMaxChars);
        conn.subjects.clear();
        for (int i = 0; i < req.subjects.size(); ++i) {
            ui::SubjectEntry s;
            for (int k = 0; k < before.size(); ++k)
                if (before.at(k).ref == req.subjects.at(i).ref)
                    s = before.at(k);
            if (s.id.isEmpty())
                s.id = ui::ConfigStore::newSubjectId();
            s.ref = req.subjects.at(i).ref;
            s.name = req.subjects.at(i).name;
            s.unit = req.subjects.at(i).unit;
            s.unitBasis = req.subjects.at(i).unitBasis;
            s.unitConfirmed = req.subjects.at(i).unitConfirmed;
            conn.subjects.append(s);
        }
        config.setConnection(conn);
        if (!config.save()) {
            // Take the secret back: a failed save leaves nothing half set up.
            m_writer->remove(req.connectionId, &err);
            emit saved(false, QStringLiteral("config-write-failed"));
            return;
        }
        // 3. tell the service; restart it once when it cannot take the change while running.
        m_port->credentialsChanged(req.connectionId);
        QString result;
        const bool answered = m_port->configChanged(&result);
        if (answered && result == QLatin1String("restart-needed")) {
            m_port->stop();
            m_port->waitStopped(8000);
            m_port->start();
        }
        emit saved(true, QString());
    }

signals:
    void loginTested(const lm::ui::LoginTestResult &result);
    void saved(bool ok, const QString &errorKey);

private:
    CredentialWriter *m_writer;
    AccountServicePort *m_port;
    QString m_configPath;
};

RealAccountBackend::RealAccountBackend(CredentialWriter *writer, AccountServicePort *port,
                                       const QString &configPath, QObject *parent)
    : ui::AccountBackend(parent), m_impl(new AccountWorkerImpl(writer, port, configPath))
{
    qRegisterMetaType<lm::ui::SaveRequest>();
    qRegisterMetaType<lm::ui::LoginTestResult>();
    m_impl->moveToThread(&m_thread);
    connect(m_impl, &AccountWorkerImpl::loginTested, this, &RealAccountBackend::loginTested);
    connect(m_impl, &AccountWorkerImpl::saved, this, &RealAccountBackend::saved);
    connect(&m_thread, &QThread::finished, m_impl, &QObject::deleteLater);
    m_thread.start();
}

RealAccountBackend::~RealAccountBackend()
{
    m_thread.quit();
    m_thread.wait(10000);
}

void RealAccountBackend::testLogin(const QString &username, const QString &password)
{
    QMetaObject::invokeMethod(m_impl, "test", Qt::QueuedConnection, Q_ARG(QString, username), Q_ARG(QString, password));
}

void RealAccountBackend::discoverKnown(const QString &connectionId)
{
    QMetaObject::invokeMethod(m_impl, "discoverKnown", Qt::QueuedConnection, Q_ARG(QString, connectionId));
}

void RealAccountBackend::save(const ui::SaveRequest &request)
{
    QMetaObject::invokeMethod(m_impl, "save", Qt::QueuedConnection, Q_ARG(lm::ui::SaveRequest, request));
}

// ---- the D-Bus port ----------------------------------------------------------------------------

namespace {

class DBusAccountServicePort : public AccountServicePort
{
public:
    // The client's blocking discover waits up to 70 s and reports a missing method (an older service)
    // as "not-available" and anything else that went wrong as "unreachable".
    QString discover(const QString &connection, QString *errorKey) { return client().discover(connection, errorKey); }
    bool credentialsChanged(const QString &c) { return client().credentialsChanged(c); }
    bool configChanged(QString *r) { return client().configChanged(r); }
    void stop() { client().stop(); }
    bool waitStopped(int ms)
    {
        const ServiceIdentity id = ServiceIdentity::forThisApplication();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            if (!QDBusConnection::sessionBus().interface()->isServiceRegistered(id.busName))
                return true;
            QThread::msleep(200);
        }
        return false;
    }
    QString start() { return client().start(); }

private:
    // A client per call: cheap, and it keeps the thread affinity trivial.
    ServiceClient &client() { static thread_local ServiceClient c; return c; }
};

} // namespace

AccountServicePort *makeDBusAccountServicePort() { return new DBusAccountServicePort; }

} // namespace real
} // namespace lm

#include "realaccountbackend.moc"
