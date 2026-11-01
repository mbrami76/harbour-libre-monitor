// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ACCOUNTERRORS_H
#define LM_UI_ACCOUNTERRORS_H

// The fixed keys the account page can show, and the text identifier of each. One list in one place, so a test can check that EVERY key has a text in the English file, in
// the Italian file and in the page's word table (StateText.js), and so an unknown key from a newer
// service can never show anything but the "internal" text.
//
// Where the keys come from:
//  - the answer of the service's Discover: auth-failed, terms-must-be-accepted,
//    rate-limited, unreachable, protocol-changed, unknown-connection, credentials-missing,
//    secrets-locked, stopped-by-guard, service-stopping, service-not-running, internal;
//  - the asynchronous client: not-available (a service without the method, or none that can start);
//  - this application: no-subjects (the login works, nobody is shared), secret-store-failed,
//    config-invalid, config-write-failed (the saving).
// A key of the service is NEVER shown as the service wrote it, only mapped to our own words.

#include <QString>
#include <QStringList>

namespace lm {
namespace ui {

// Every key with its own text.
QStringList knownAccountErrorKeys();

// The key as the page should treat it: itself when known, "internal" otherwise.
QString normalizedAccountError(const QString &key);

// The text identifier ("ui-acct-err-...") of a key; an unknown key gives the one of "internal".
QString accountErrorTextId(const QString &key);

} // namespace ui
} // namespace lm

#endif
