// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_FAKEACCOUNTBACKEND_H
#define LM_UI_FAKEACCOUNTBACKEND_H

// A scripted account backend for DEVELOPMENT builds only (REQ-SIM-06), like FakeBackend:
// the account page can be tried on the emulator with no service, no source and no credentials.
// Compiled only with the flag `devsource`; there is no runtime switch.
//
// The script is chosen by the account name typed in the page (so every answer of the real source
// can be provoked): "wrong" is a refused login, "terms" the updated-terms case, "offline" an
// unreachable source, "limit" a rate limit, "empty" an account that sees nobody; any other name
// succeeds and finds three invented people (two in mmol/L, one in mg/dL). The answer comes after
// about a second, as a real one would. Saving stores NOTHING anywhere (the real configuration
// file and secrets of an installed app must never receive invented people) and succeeds, except
// for the password "fail", which makes the save fail.

#ifdef LIBREMON_DEVSOURCE

#include "accountbackend.h"

#include <QTimer>

namespace lm {
namespace ui {

class FakeAccountBackend : public AccountBackend
{
    Q_OBJECT
public:
    explicit FakeAccountBackend(QObject *parent = 0);

    void testLogin(const QString &username, const QString &password);
    void save(const SaveRequest &request);

private slots:
    void answerTest();
    void answerSave();

private:
    QTimer m_timer;
    QString m_pendingUser;
    bool m_failSave;
    bool m_saving;
};

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE

#endif
