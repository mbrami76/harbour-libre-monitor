// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "purgeview.h"
#include "storageview.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace lm {
namespace ui {

namespace {
QString compact(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// The sum of the numbers of an object such as {"record": 3000, "mark": 0, ...}; keys this version does not
// know are counted too (they are rows the service would remove).
qint64 sumOf(const QJsonValue &v)
{
    qint64 n = 0;
    const QJsonObject o = v.toObject();
    for (QJsonObject::const_iterator it = o.constBegin(); it != o.constEnd(); ++it)
        if (it.value().isDouble() && it.value().toDouble() > 0)
            n += qint64(it.value().toDouble());
    return n;
}

qint64 number(const QJsonObject &o, const char *key, qint64 absent)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? qint64(v.toDouble()) : absent;
}
}

QList<int> purgeAgeChoices() { return QList<int>() << 3 << 6 << 12 << 24; }

QString purgeLabelOf(const QString &configuredLabel) { return configuredLabel.trimmed(); }

bool typedLabelMatches(const QString &typed, const QString &label)
{
    return !label.isEmpty() && typed == label;
}

QString purgeEstimateParameters(const QString &subject, int ageMonths)
{
    QJsonObject o;
    o.insert(QStringLiteral("subject"), subject);
    o.insert(QStringLiteral("age_months"), ageMonths);
    return compact(o);
}

QString purgeParameters(const QString &subject, int ageMonths, const QString &typedLabel)
{
    QJsonObject o;
    o.insert(QStringLiteral("subject"), subject);
    o.insert(QStringLiteral("age_months"), ageMonths);
    o.insert(QStringLiteral("typed_label"), typedLabel);         // unchanged: the service compares exactly
    return compact(o);
}

PurgeFacts purgeFactsFromDetails(const QString &detailsJson)
{
    PurgeFacts f;
    const QJsonObject o = QJsonDocument::fromJson(detailsJson.toUtf8()).object();
    f.subject = o.value(QStringLiteral("subject")).toString();
    if (f.subject.isEmpty())
        return f;
    f.known = true;
    f.ageMonths = int(number(o, "age_months", 0));
    f.storeRows = sumOf(o.value(QStringLiteral("store_rows")));
    f.journalEntries = sumOf(o.value(QStringLiteral("journal_entries")));
    f.bytesEstimated = qMax<qint64>(0, number(o, "total_bytes_estimated", 0));
    f.neededBytes = number(o, "needed_bytes", -1);
    f.freeBytes = number(o, "free_bytes", -1);
    f.space = o.value(QStringLiteral("space")).toString();
    f.missingBytes = qMax<qint64>(0, number(o, "missing_bytes", 0));
    f.storePartDone = o.value(QStringLiteral("store_part_done")).toBool(false);
    f.removedStore = sumOf(o.value(QStringLiteral("removed")));
    f.removedJournal = sumOf(o.value(QStringLiteral("journal_removed")));
    f.bytesFreed = qMax<qint64>(0, number(o, "bytes_freed", 0)) + qMax<qint64>(0, number(o, "journal_bytes_freed", 0));
    return f;
}

QString purgeStartKey(const QString &result)
{
    if (result.isEmpty())
        return QString();
    static const char *own[] = { "label-mismatch", "no-label", "unknown-subject", "age-not-allowed", "parameter-subject" };
    for (unsigned i = 0; i < sizeof(own) / sizeof(own[0]); ++i)
        if (result == QLatin1String(own[i]))
            return QStringLiteral("ui-purge-start-") + result;
    return startAnswerKey(result);
}

QString purgeFinishedKey(bool ok, const QString &errorClass, const QString &cancelReason, bool storePartDone)
{
    if (ok)
        return QStringLiteral("ui-purge-done");
    // the store part is done and the journal part failed; the same purge can be repeated.
    if (storePartDone && errorClass == QLatin1String("io"))
        return QStringLiteral("ui-purge-repeat");
    if (errorClass == QLatin1String("no-subject"))
        return QStringLiteral("ui-purge-start-unknown-subject");
    if (errorClass == QLatin1String("age-not-allowed"))
        return QStringLiteral("ui-purge-start-age-not-allowed");
    return finishedKey(QStringLiteral("purge"), false, errorClass, cancelReason);
}

} // namespace ui
} // namespace lm
