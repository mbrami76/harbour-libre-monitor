// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBREERROR_H
#define LM_LIBREERROR_H

#include <QString>

namespace lm {

// A failed command, in the classes of REQ-ADP-10. InvalidCommand is a defect of the core
// (a payload the adapter cannot execute), not of the source.
// The detail never holds credentials,
// tokens, names or glucose values (contract 5.7): only fixed texts, key names and numbers.
struct LibreError
{
    enum Class { None, AuthFailed, TermsMustBeAccepted, RateLimited, Transient, ProtocolChanged,
                 InvalidCommand };

    LibreError() : cls(None), retryAfterS(-1) {}
    LibreError(Class c, const QString &d, int retry = -1) : cls(c), detail(d), retryAfterS(retry) {}

    bool isError() const { return cls != None; }

    // Wire name of the class (contract 5.7).
    QString className() const
    {
        switch (cls) {
        case AuthFailed: return QStringLiteral("auth-failed");
        case TermsMustBeAccepted: return QStringLiteral("terms-must-be-accepted");
        case RateLimited: return QStringLiteral("rate-limited");
        case Transient: return QStringLiteral("transient");
        case ProtocolChanged: return QStringLiteral("protocol-changed");
        case InvalidCommand: return QStringLiteral("invalid-command");
        case None: break;
        }
        return QString();
    }

    Class cls;
    QString detail;
    int retryAfterS;    // -1 = no hint
};

} // namespace lm

#endif
