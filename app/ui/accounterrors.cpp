// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "accounterrors.h"

namespace lm {
namespace ui {

namespace {
struct Entry { const char *key; const char *textId; };
const Entry kEntries[] = {
    { "auth-failed", "ui-acct-err-auth" },
    { "terms-must-be-accepted", "ui-acct-err-terms" },
    { "unreachable", "ui-acct-err-unreachable" },
    { "rate-limited", "ui-acct-err-rate" },
    { "protocol-changed", "ui-acct-err-protocol" },
    { "no-subjects", "ui-acct-err-nosubjects" },
    { "not-available", "ui-acct-err-notavailable" },
    { "secret-store-failed", "ui-acct-err-secret" },
    { "config-invalid", "ui-acct-err-config" },
    { "config-write-failed", "ui-acct-err-config" },
    { "unknown-connection", "ui-acct-err-unknown-connection" },
    { "credentials-missing", "ui-acct-err-credentials-missing" },
    { "secrets-locked", "ui-acct-err-secrets-locked" },
    { "stopped-by-guard", "ui-acct-err-stopped-by-guard" },
    { "service-stopping", "ui-acct-err-service-stopping" },
    { "service-not-running", "ui-acct-err-service-not-running" },
    { "internal", "ui-acct-err-internal" }
};
}

QStringList knownAccountErrorKeys()
{
    QStringList out;
    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i)
        out << QLatin1String(kEntries[i].key);
    return out;
}

QString normalizedAccountError(const QString &key)
{
    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i)
        if (key == QLatin1String(kEntries[i].key))
            return key;
    return QStringLiteral("internal");
}

QString accountErrorTextId(const QString &key)
{
    const QString k = normalizedAccountError(key);
    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i)
        if (k == QLatin1String(kEntries[i].key))
            return QLatin1String(kEntries[i].textId);
    return QStringLiteral("ui-acct-err-internal");
}

} // namespace ui
} // namespace lm
