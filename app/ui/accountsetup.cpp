// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "accountsetup.h"

#include "accounterrors.h"

namespace lm {
namespace ui {

namespace {
// One account is the normal case: its connection has a fixed identifier. A
// second account would need its own (the page offers it only under "advanced", later).
const char *kConnectionId = "c1";
}

AccountSetup::AccountSetup(QObject *parent)
    : QObject(parent), m_backend(0), m_state(QStringLiteral("editing"))
{
}

void AccountSetup::setBackend(AccountBackend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (m_backend) {
        connect(m_backend, &AccountBackend::loginTested, this, &AccountSetup::onLoginTested);
        connect(m_backend, &AccountBackend::saved, this, &AccountSetup::onSaved);
    }
}

void AccountSetup::setState(const QString &state, const QString &errorKey)
{
    m_state = state;
    m_errorKey = errorKey;
    emit changed();
}

void AccountSetup::forgetTest()
{
    m_testedUser.clear();
    m_testedPassword.clear();
    m_people.clear();
}

QVariantList AccountSetup::subjects() const
{
    QVariantList out;
    for (int i = 0; i < m_people.size(); ++i) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), m_people.at(i).name);
        m.insert(QStringLiteral("follow"), m_people.at(i).follow);
        m.insert(QStringLiteral("unit"), m_people.at(i).unit);
        m.insert(QStringLiteral("unitBasis"), m_people.at(i).unitBasis);
        out.append(m);
    }
    return out;
}

bool AccountSetup::canTest() const
{
    return m_backend && !busy() && !m_user.trimmed().isEmpty() && !m_password.isEmpty();
}

bool AccountSetup::canSave() const
{
    if (!m_backend || m_state != QLatin1String("test-ok"))
        return false;
    // The test must be of exactly what is in the fields now.
    if (m_user != m_testedUser || m_password != m_testedPassword)
        return false;
    for (int i = 0; i < m_people.size(); ++i)
        if (m_people.at(i).follow)
            return true;
    return false;
}

void AccountSetup::fieldsChanged(const QString &username, const QString &password)
{
    if (username == m_user && password == m_password)
        return;
    m_user = username;
    m_password = password;
    if (m_state == QLatin1String("testing") || m_state == QLatin1String("saving")) {
        emit changed();
        return;                 // an answer is on its way; the page disables the fields meanwhile
    }
    if (m_state != QLatin1String("editing")) {
        forgetTest();           // a test of other credentials proves nothing
        setState(QStringLiteral("editing"));
    } else {
        emit changed();
    }
}

void AccountSetup::test()
{
    if (!canTest())
        return;
    forgetTest();
    m_testedUser = m_user;
    m_testedPassword = m_password;
    setState(QStringLiteral("testing"));
    m_backend->testLogin(m_user, m_password);
}

void AccountSetup::onLoginTested(const LoginTestResult &r)
{
    if (m_state != QLatin1String("testing"))
        return;                 // a late answer after a reset
    if (!r.ok) {
        forgetTest();
        // Only keys the page has words for; an unknown key of a newer service shows as "internal".
        setState(QStringLiteral("test-failed"), normalizedAccountError(r.errorKey));
        return;
    }
    if (r.subjects.isEmpty()) {
        forgetTest();
        setState(QStringLiteral("test-failed"), QStringLiteral("no-subjects"));
        return;
    }
    m_people.clear();
    for (int i = 0; i < r.subjects.size(); ++i) {
        Person p;
        p.ref = r.subjects.at(i).ref;
        p.name = r.subjects.at(i).displayName;
        p.unit = r.subjects.at(i).unit;
        p.unitBasis = r.subjects.at(i).unitBasis;
        m_people.append(p);
    }
    setState(QStringLiteral("test-ok"));
}

void AccountSetup::setFollow(int i, bool follow)
{
    if (i < 0 || i >= m_people.size() || m_people.at(i).follow == follow)
        return;
    m_people[i].follow = follow;
    emit changed();
}

void AccountSetup::setName(int i, const QString &name)
{
    if (i < 0 || i >= m_people.size() || m_people.at(i).name == name)
        return;
    m_people[i].name = name;
    emit changed();
}

void AccountSetup::setUnit(int i, const QString &unit)
{
    if (i < 0 || i >= m_people.size() || m_people.at(i).unit == unit)
        return;
    if (unit != QLatin1String("mg/dL") && unit != QLatin1String("mmol/L"))
        return;
    m_people[i].unit = unit;
    m_people[i].unitBasis = QStringLiteral("user");     // the user corrected it
    emit changed();
}

void AccountSetup::save()
{
    if (!canSave())
        return;
    SaveRequest req;
    req.connectionId = QString::fromLatin1(kConnectionId);
    req.connectionLabel = m_defaultLabel;
    req.username = m_testedUser;
    req.password = m_testedPassword;
    for (int i = 0; i < m_people.size(); ++i) {
        if (!m_people.at(i).follow)
            continue;
        SaveSubject s;
        s.ref = m_people.at(i).ref;
        s.name = m_people.at(i).name.trimmed();
        s.unit = m_people.at(i).unit;
        s.unitBasis = m_people.at(i).unitBasis;
        s.unitConfirmed = true;         // the user saw it and chose to save (REQ-SUB-05)
        req.subjects.append(s);
    }
    setState(QStringLiteral("saving"));
    m_backend->save(req);
}

void AccountSetup::onSaved(bool ok, const QString &errorKey)
{
    if (m_state != QLatin1String("saving"))
        return;
    // The password is not needed any more, whatever the outcome.
    m_password.clear();
    m_testedPassword.clear();
    if (ok) {
        forgetTest();
        setState(QStringLiteral("saved"));
    } else {
        // Keep the people so the user can try again, but the password must be typed again.
        setState(QStringLiteral("save-failed"), normalizedAccountError(errorKey));
    }
}

void AccountSetup::reset()
{
    forgetTest();
    m_user.clear();
    m_password.clear();
    setState(QStringLiteral("editing"));
}

} // namespace ui
} // namespace lm
