// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_REALACCOUNTBACKEND_H
#define LM_REAL_REALACCOUNTBACKEND_H

// The account page's real backend: TEST a login through
// the monitoring service without keeping anything, and SAVE the account (secret, configuration,
// then tell the service).
//
// How the test works without storing the account first (the password must never travel in a D-Bus
// message, and the service has no password of its own): the credentials go into the secret store
// under a PROBE identifier ("probe"), the service is asked to `Discover("probe")` with them, and
// the probe secret is removed again whatever the outcome. The service must treat an identifier that
// is not in the configuration but has a secret as a one-time probe of the built-in adapter. That
// behaviour is a PROPOSAL: until it exists, the call fails
// with "unknown method" and the page says "not available".
//
// How saving works: the secret goes to the secret store through the SAME port the provisioning
// mode uses (CredentialWriter, one implementation of "store a login", REQ-SEC-01, REQ-SEC-05);
// the configuration file is written atomically with the people, names and confirmed units
// (ConfigStore,); when the file cannot be written the secret is taken back, so a
// failed save leaves nothing half done. Then the service is told (CredentialsChanged,
// ConfigChanged); when ConfigChanged answers "restart-needed" the service is stopped and started
// once, harmless at a first setup because nothing was monitored yet.
//
// Pattern: ports and adapters again. The blocking work (the secret store may wait for the user's
// approval, a discovery is a network round trip) runs in a worker thread; the three ports are
// plain interfaces so the host tests run the whole flow with fakes. No credential is ever logged
// or returned.

#include "ui/accountbackend.h"

#include <QMetaType>
#include <QThread>

Q_DECLARE_METATYPE(lm::ui::SaveRequest)
Q_DECLARE_METATYPE(lm::ui::LoginTestResult)

namespace lm {

class CredentialWriter;

namespace real {

// What the account flow needs from the monitoring service: blocking calls, made in the worker thread.
class AccountServicePort
{
public:
    virtual ~AccountServicePort() {}
    // The service's answer to Discover(connection) as one JSON text; empty with `errorKey` set when
    // the call did not work ("not-available", "unreachable").
    virtual QString discover(const QString &connection, QString *errorKey) = 0;
    virtual bool credentialsChanged(const QString &connection) = 0;
    virtual bool configChanged(QString *result) = 0;    // "", "restart-needed", "config-invalid"
    virtual void stop() = 0;
    // Waits until the service has gone, at most `ms`; false if it is still there.
    virtual bool waitStopped(int ms) = 0;
    virtual QString start() = 0;
};

class AccountWorkerImpl;

class RealAccountBackend : public ui::AccountBackend
{
    Q_OBJECT
public:
    // The writer and the port are not owned and must outlive this object; they are used from the
    // worker thread only.
    RealAccountBackend(CredentialWriter *writer, AccountServicePort *port, const QString &configPath,
                       QObject *parent = 0);
    ~RealAccountBackend();

    void testLogin(const QString &username, const QString &password);
    void discoverKnown(const QString &connectionId);
    void save(const ui::SaveRequest &request);

private:
    QThread m_thread;
    AccountWorkerImpl *m_impl;
};

// The real port, over D-Bus (blocking; for the worker thread).
AccountServicePort *makeDBusAccountServicePort();

// Exposed for the tests: reads the service's Discover answer into a result.
ui::LoginTestResult parseDiscoverAnswer(const QString &json, const QString &errorKey);

} // namespace real
} // namespace lm

#endif
