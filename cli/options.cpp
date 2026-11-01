// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "options.h"

namespace lmcli {

int exitCodeFor(lm::ErrorClass errorClass)
{
    switch (errorClass) {
    case lm::ErrorClass::AuthFailed: return ExitAuthFailed;
    case lm::ErrorClass::TermsMustBeAccepted: return ExitTermsMustBeAccepted;
    case lm::ErrorClass::RateLimited: return ExitRateLimited;
    case lm::ErrorClass::Transient: return ExitTransient;
    case lm::ErrorClass::ProtocolChanged: return ExitProtocolChanged;
    case lm::ErrorClass::InvalidCommand: return ExitInvalidCommand;
    }
    return ExitFailure;
}

QString usageText()
{
    return QStringLiteral(
        "usage: lm-cli <command> [options]\n"
        "\n"
        "commands:\n"
        "  discover                list the Subjects the account can follow\n"
        "  current                 print the current value of each Subject\n"
        "  range --from T --to T   print the readings between two UTC times\n"
        "                          (YYYY-MM-DDTHH:MM:SSZ)\n"
        "\n"
        "options:\n"
        "  --credentials-file P    file with username= and password= lines, readable by\n"
        "                          the user only (without it: hidden prompt on a terminal)\n"
        "  --session-file P        keep the opaque session between runs (same mode rule)\n"
        "  --adapter P             adapter host executable (default: libre-adapter-host\n"
        "                          next to this program)\n"
        "  --subject N             only the Subject with this index (default: all)\n"
        "  --json                  machine-readable output on standard output\n"
        "  --show-ref              discover --json only: also print each Subject's source\n"
        "                          reference (it identifies a person: keep it private)\n"
        "  --verbose               adapter log and driver diagnostics on standard error\n"
#ifdef LIBREMON_DEVSOURCE
        "  --base-url URL          development only: talk to a simulated source\n"
#endif
        "  --help                  this text\n"
        "\n"
        "exit codes: 0 ok, 2 usage, 10 auth-failed, 11 terms-must-be-accepted,\n"
        "  12 rate-limited, 13 transient, 14 protocol-changed, 15 invalid-command,\n"
        "  20 admission rejected, 21 contract violation, 1 other failure\n");
}

namespace {

bool needsValue(const QString &name)
{
    return name == QLatin1String("--credentials-file") || name == QLatin1String("--session-file")
        || name == QLatin1String("--adapter") || name == QLatin1String("--subject")
        || name == QLatin1String("--from") || name == QLatin1String("--to")
#ifdef LIBREMON_DEVSOURCE
        || name == QLatin1String("--base-url")
#endif
        ;
}

bool isFlag(const QString &name)
{
    return name == QLatin1String("--json") || name == QLatin1String("--verbose")
        || name == QLatin1String("--show-ref");
}

} // namespace

bool parseArguments(const QStringList &args, Options *out, QString *error)
{
    Options o;
    int i = 0;
    if (args.isEmpty()) {
        *error = QStringLiteral("no command given");
        return false;
    }
    o.command = args.at(i++);
    if (o.command != QLatin1String("discover") && o.command != QLatin1String("current")
        && o.command != QLatin1String("range")) {
        *error = QStringLiteral("unknown command");
        return false;
    }

    QString fromText, toText;
    while (i < args.size()) {
        QString name = args.at(i++);
        QString value;
        bool hasValue = false;
        const int eq = name.indexOf(QLatin1Char('='));
        if (name.startsWith(QLatin1String("--")) && eq > 0) {
            value = name.mid(eq + 1);
            name = name.left(eq);
            hasValue = true;
        }
        if (isFlag(name)) {
            if (hasValue) {
                *error = name + QStringLiteral(" takes no value");
                return false;
            }
            if (name == QLatin1String("--json"))
                o.json = true;
            else if (name == QLatin1String("--show-ref"))
                o.showRef = true;
            else
                o.verbose = true;
            continue;
        }
        if (!needsValue(name)) {
            // Never echo more than the option name: the rest may be a mistyped secret.
            *error = (name.startsWith(QLatin1String("--")) ? QStringLiteral("unknown option ") + name
                                                            : QStringLiteral("unexpected argument"));
            return false;
        }
        if (!hasValue) {
            if (i >= args.size()) {
                *error = name + QStringLiteral(" needs a value");
                return false;
            }
            value = args.at(i++);
        }
        if (name == QLatin1String("--credentials-file")) o.credentialsFile = value;
        else if (name == QLatin1String("--session-file")) o.sessionFile = value;
        else if (name == QLatin1String("--adapter")) o.adapterPath = value;
        else if (name == QLatin1String("--from")) fromText = value;
        else if (name == QLatin1String("--to")) toText = value;
        else if (name == QLatin1String("--subject")) {
            bool ok = false;
            const int n = value.toInt(&ok);
            if (!ok || n < 0) {
                *error = QStringLiteral("--subject needs a non-negative index");
                return false;
            }
            o.subjectIndex = n;
        }
#ifdef LIBREMON_DEVSOURCE
        else if (name == QLatin1String("--base-url")) o.baseUrl = value;
#endif
    }

    if (o.showRef && (o.command != QLatin1String("discover") || !o.json)) {
        *error = QStringLiteral("--show-ref works only with discover --json");
        return false;
    }

    if (o.command == QLatin1String("range")) {
        o.from = lm::UtcTime::fromIso(fromText);
        o.to = lm::UtcTime::fromIso(toText);
        if (fromText.isEmpty() || toText.isEmpty()) {
            *error = QStringLiteral("range needs --from and --to");
            return false;
        }
        if (!o.from.isValid() || !o.to.isValid()) {
            *error = QStringLiteral("--from and --to must be UTC times like 2026-01-01T12:00:00Z");
            return false;
        }
        if (!(o.from < o.to)) {
            *error = QStringLiteral("--from must be before --to");
            return false;
        }
    } else if (!fromText.isEmpty() || !toText.isEmpty()) {
        *error = QStringLiteral("--from and --to belong to the range command");
        return false;
    }
    *out = o;
    return true;
}

} // namespace lmcli
