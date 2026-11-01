// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "provisionmode.h"

namespace lm {

const char *const kProvisionAdapterId = "libre";

namespace {

ProvisionOptions bad(const char *text)
{
    ProvisionOptions o;
    o.error = QString::fromLatin1(text);
    return o;
}

// Tells the service what changed. A service that cannot be reached is reported but is not a
// failure of the set-up: the files are written and the next start of the service reads them.
void notifyService(ServiceCaller *service, const QString &connectionId, const LineSink &out)
{
    QString result;
    if (!service->configChanged(&result))
        out(QStringLiteral("service not reached (configuration)"));
    else if (result.isEmpty())
        out(QStringLiteral("service told: configuration reloaded"));
    else if (result == QLatin1String("restart-needed"))
        out(QStringLiteral("service told: configuration changed, restart needed (stop, then start)"));
    else
        out(QStringLiteral("service told: configuration refused"));

    out(service->credentialsChanged(connectionId)
            ? QStringLiteral("service told: credentials changed")
            : QStringLiteral("service not reached (credentials)"));
}

} // namespace

ProvisionOptions parseProvisionOptions(const QStringList &arguments)
{
    ProvisionOptions o;
    bool haveConnection = false;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &a = arguments.at(i);
        if (a == QLatin1String("--remove")) {
            o.remove = true;
        } else if (a == QLatin1String("--connection") || a == QLatin1String("--subject-ref")) {
            if (i + 1 >= arguments.size())
                return bad("an option is missing its value");
            const QString &value = arguments.at(++i);
            if (a == QLatin1String("--connection")) {
                if (!isValidConnectionId(value))
                    return bad("the connection id may hold letters, digits, - and _ (1 to 64)");
                o.connectionId = value;
                haveConnection = true;
            } else {
                if (value.isEmpty())
                    return bad("the source reference must not be empty");
                o.subjectRef = value;
            }
        } else {
            return bad("unknown option");       // never the argument itself
        }
    }
    if (!haveConnection)
        return bad("--connection <id> is required");
    if (o.remove && !o.subjectRef.isEmpty())
        return bad("--remove takes no --subject-ref");
    return o;
}

int runProvisionMode(const ProvisionOptions &options, Provisioner *provisioner,
                     Prompter *prompter, ServiceCaller *service, const LineSink &out)
{
    if (!options.error.isEmpty()) {
        out(options.error);
        return 64;
    }

    if (options.remove) {
        const ProvisionResult r = provisioner->remove(options.connectionId);
        if (r.secretDone)
            out(QStringLiteral("secret removed"));
        if (r.connectionDone)
            out(QStringLiteral("connection removed"));
        else if (r.ok)
            out(QStringLiteral("connection was not in the configuration"));
        if (!r.ok) {
            out(QStringLiteral("failed: ") + r.errorKey);
            return 1;
        }
        notifyService(service, options.connectionId, out);
        return 0;
    }

    // The prompts come first: a wrong terminal is found out before anything is written.
    ProvisionRequest request;
    request.connectionId = options.connectionId;
    request.adapterId = QString::fromLatin1(kProvisionAdapterId);
    request.subjectRef = options.subjectRef;
    QString key;
    if (!prompter->hidden(QStringLiteral("account name: "), &request.username, &key)
        || !prompter->hidden(QStringLiteral("password: "), &request.password, &key)
        || (request.subjectRef.isEmpty()
            && !prompter->hidden(QStringLiteral("source reference: "), &request.subjectRef, &key))) {
        out(QStringLiteral("failed: ") + key);
        return 1;
    }
    if (request.username.isEmpty() || request.password.isEmpty() || request.subjectRef.isEmpty()) {
        out(QStringLiteral("failed: input-empty"));
        return 1;
    }

    const ProvisionResult r = provisioner->provision(request);
    if (r.secretDone)
        out(QStringLiteral("secret stored"));
    if (r.connectionDone)
        out(QStringLiteral("connection written"));
    if (!r.ok) {
        out(QStringLiteral("failed: ") + r.errorKey);
        return 1;
    }
    notifyService(service, options.connectionId, out);
    return 0;
}

} // namespace lm
