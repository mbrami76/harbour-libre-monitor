// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ACCOUNTBACKEND_H
#define LM_UI_ACCOUNTBACKEND_H

// What the account page needs from the rest of the application:
// try a login WITHOUT storing anything, and, once the user decided, store the credentials and the
// configuration. One abstract class with two kinds of implementation: the real one (the
// monitoring service over D-Bus for the test, the secret store and the configuration file for
// the saving, later) and a scripted one for development builds (fakeaccountbackend.h).
//
// Why a "test" separate from "save": the user must be able to find out that the account works
// before anything is written, so a wrong password never leaves a half set-up account behind and
// never starts the service polling with credentials that will be refused (a repeated refused
// login is a risk for the account).
//
// The password only ever travels in these calls; no implementation may log it, keep it after the
// call, or put it in any message (REQ-SEC-01, REQ-ADP-11).

#include <QList>
#include <QObject>
#include <QString>

namespace lm {
namespace ui {

// One person the account can see, as the source reports it (REQ-SRC-04). The display name is
// personal data: shown on screen and written to the configuration file only.
struct DiscoveredSubject
{
    QString ref;            // the source's own identifier, opaque
    QString displayName;
    QString unit;           // "mg/dL" or "mmol/L"
    QString unitBasis;      // how the unit was determined: "code", "relation" or "magnitude" (REQ-SUB-05)
};

struct LoginTestResult
{
    LoginTestResult() : ok(false) {}

    bool ok;
    // A fixed key when !ok: "auth-failed", "terms-must-be-accepted", "rate-limited", "unreachable",
    // "protocol-changed", "no-subjects", "not-available", "internal". Never the source's text.
    QString errorKey;
    QList<DiscoveredSubject> subjects;
};

struct SaveSubject
{
    SaveSubject() : unitConfirmed(true) {}

    QString ref;
    QString name;           // as the user left it
    QString unit;           // as the user confirmed it
    QString unitBasis;      // "code", "relation", "magnitude", or "user" when the user changed it
    bool unitConfirmed;
};

struct SaveRequest
{
    QString connectionId;
    // The default label of the connection, the translated word "Account": written only
    // when the connection has no label yet, so a label the user changed in the settings survives.
    QString connectionLabel;
    QString username;
    QString password;
    QList<SaveSubject> subjects;
};

class AccountBackend : public QObject
{
    Q_OBJECT
public:
    explicit AccountBackend(QObject *parent = 0) : QObject(parent) {}

    // Asynchronous: the answer arrives as loginTested(). Stores nothing.
    virtual void testLogin(const QString &username, const QString &password) = 0;
    // Asynchronous: the people of an account that is ALREADY saved (its stored login, no new probe),
    // for adding or removing a person later; the answer arrives as loginTested(), like a test.
    // The default answers "not-available".
    virtual void discoverKnown(const QString &/*connectionId*/)
    {
        LoginTestResult r;
        r.errorKey = QStringLiteral("not-available");
        emit loginTested(r);
    }
    // Asynchronous: the answer arrives as saved(). Secret first, then the configuration, then the
    // service is told; a failure leaves nothing half done (the same rule as the Provisioner).
    virtual void save(const SaveRequest &request) = 0;

signals:
    void loginTested(const lm::ui::LoginTestResult &result);
    void saved(bool ok, const QString &errorKey);
};

} // namespace ui
} // namespace lm

#endif
