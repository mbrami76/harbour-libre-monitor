// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "storageview.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace lm {
namespace ui {

QList<int> windowChoices() { return QList<int>() << 0 << 1 << 3 << 6 << 12; }

QString compactionParameters(int windowMonths)
{
    QJsonObject o;
    if (windowMonths <= 0)
        o.insert(QStringLiteral("window_days"), 14);          // REQ-STO-13: never below 14 days
    else
        o.insert(QStringLiteral("window_months"), windowMonths);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString startAnswerKey(const QString &result)
{
    if (result.isEmpty())
        return QString();
    // The words StartMaintenance can answer with ("unknown-kind" would mean a service that does
    // not know compaction), and "no-answer" when the bus did not answer.
    static const char *known[] = { "service-not-running", "maintenance-running", "unknown-kind", "parameters-not-json",
                                   "parameter-window", "no-answer" };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (result == QLatin1String(known[i]))
            return QStringLiteral("ui-maint-start-") + result;
    return QStringLiteral("ui-maint-start-other");
}

QString finishedKey(const QString &kind, bool ok, const QString &errorClass, const QString &cancelReason)
{
    if (ok)
        return kind == QLatin1String("compaction") ? QStringLiteral("ui-maint-done-compaction")
                                                   : QStringLiteral("ui-maint-done-other");
    if (cancelReason == QLatin1String("queue-high-water"))
        return QStringLiteral("ui-maint-queue-stop");
    if (cancelReason == QLatin1String("shutdown"))
        return QStringLiteral("ui-maint-shutdown");
    if (!cancelReason.isEmpty() || errorClass == QLatin1String("cancelled"))
        return QStringLiteral("ui-maint-cancelled");
    // core/store/compactor.h CompactionError. In every case the store is as it was before.
    static const char *known[] = { "io", "window-too-short", "insufficient-space", "free-space-unknown", "aborted" };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (errorClass == QLatin1String(known[i]))
            return QStringLiteral("ui-maint-fail-") + errorClass;
    return QStringLiteral("ui-maint-fail-other");
}

QString phaseKey(const QString &phase)
{
    // "purging" is the purge's; the other three are shared with compaction.
    static const char *known[] = { "checking-space", "compacting", "purging", "finalizing", "done" };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (phase == QLatin1String(known[i]))
            return QStringLiteral("ui-maint-phase-") + phase;
    return QStringLiteral("ui-maint-phase-other");
}

qint64 detailBytes(const QString &detailsJson, const char *key)
{
    const QJsonValue v = QJsonDocument::fromJson(detailsJson.toUtf8()).object().value(QLatin1String(key));
    if (!v.isDouble())
        return -1;
    const double d = v.toDouble();
    return d >= 0 ? qint64(d) : -1;
}

} // namespace ui
} // namespace lm
