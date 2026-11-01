// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "fakeaccountbackend.h"

#ifdef LIBREMON_DEVSOURCE

namespace lm {
namespace ui {

FakeAccountBackend::FakeAccountBackend(QObject *parent)
    : AccountBackend(parent), m_failSave(false), m_saving(false)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(1200);
    connect(&m_timer, &QTimer::timeout, this, [this]() { if (m_saving) answerSave(); else answerTest(); });
}

void FakeAccountBackend::testLogin(const QString &username, const QString &)
{
    m_pendingUser = username.trimmed().toLower();
    m_saving = false;
    m_timer.start();
}

void FakeAccountBackend::answerTest()
{
    LoginTestResult r;
    if (m_pendingUser == QLatin1String("wrong")) {
        r.errorKey = QStringLiteral("auth-failed");
    } else if (m_pendingUser == QLatin1String("terms")) {
        r.errorKey = QStringLiteral("terms-must-be-accepted");
    } else if (m_pendingUser == QLatin1String("offline")) {
        r.errorKey = QStringLiteral("unreachable");
    } else if (m_pendingUser == QLatin1String("limit")) {
        r.errorKey = QStringLiteral("rate-limited");
    } else if (m_pendingUser == QLatin1String("empty")) {
        r.ok = true;                    // the login works but the account sees nobody
    } else {
        r.ok = true;
        DiscoveredSubject a; a.ref = QStringLiteral("fake-1"); a.displayName = QStringLiteral("Anna");
        a.unit = QStringLiteral("mmol/L"); a.unitBasis = QStringLiteral("code");
        DiscoveredSubject b; b.ref = QStringLiteral("fake-2"); b.displayName = QStringLiteral("Marco");
        b.unit = QStringLiteral("mg/dL"); b.unitBasis = QStringLiteral("relation");
        DiscoveredSubject c; c.ref = QStringLiteral("fake-3"); c.displayName = QStringLiteral("Luca");
        c.unit = QStringLiteral("mmol/L"); c.unitBasis = QStringLiteral("magnitude");
        r.subjects << a << b << c;
    }
    emit loginTested(r);
}

void FakeAccountBackend::save(const SaveRequest &request)
{
    m_failSave = request.password == QLatin1String("fail");
    m_saving = true;
    m_timer.start();
}

void FakeAccountBackend::answerSave()
{
    m_saving = false;
    emit saved(!m_failSave, m_failSave ? QStringLiteral("secret-store-failed") : QString());
}

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE
