// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/diagnosticcollector.h"

#include "record/registry.h"
#include "session/adapterlink.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace lm {

namespace {

// The feeds of the built-in adapter (adapters/libre/libreadapter.cpp, its `hello`). The report copies
// a feed name only when it is on this list; any other text in a feed field is masked. These are
// protocol words, not personal data. A new adapter adds its feeds here.
QStringList knownFeeds()
{
    return QStringList() << QStringLiteral("current") << QStringLiteral("graph") << QStringLiteral("logbook");
}

// The metric and unit names the record registry knows (core/record/registry.h): the only other text
// values the report may copy, because they come from our own code, never from the source.
void registryWords(QStringList *metrics, QStringList *units)
{
    const MetricRegistry &r = MetricRegistry::instance();
    const QStringList names = r.names();
    for (int i = 0; i < names.size(); ++i) {
        metrics->append(names.at(i));
        const MetricDef *def = r.find(names.at(i));
        if (!def)
            continue;
        for (int u = 0; u < def->units.size(); ++u)
            if (!units->contains(def->units.at(u).unit))
                units->append(def->units.at(u).unit);
    }
}

QJsonObject stripValue(QJsonObject o)
{
    if (o.value(QStringLiteral("value")).isDouble())
        o.insert(QStringLiteral("value"), QStringLiteral("number"));
    return o;
}

} // namespace

Action DiagnosticCollector::onViolation(const Violation &violation)
{
    Run &run = m_running[violation.link.connectionId];
    run.link = violation.link;
    DiagnosticViolation v;
    v.kind = violationKindName(violation.kind);
    v.detail = violation.detail;
    run.violations.append(v);
    return m_inner->onViolation(violation);
}

void DiagnosticCollector::rejected(const LinkId &link, const QByteArray &rawLine,
                                   CommandOutcome::KilledBy killedBy, bool hasViolation, ViolationKind kind)
{
    Q_UNUSED(killedBy);
    Q_UNUSED(hasViolation);
    Q_UNUSED(kind);     // the violations of the command are already noted, with their details
    Run &run = m_running[link.connectionId];
    run.link = link;
    run.hasBatch = true;
    run.batch = rawLine;
}

void DiagnosticCollector::onCommandFinished(const CommandOutcome &outcome)
{
    const QString connection = outcome.link.connectionId;
    Run run = m_running.take(connection);
    if (outcome.killedBy == CommandOutcome::Guard) {
        Held h;
        h.run = run;
        h.run.link = outcome.link;
        h.outcome = outcome;
        m_held.insert(connection, h);
    } else if (outcome.result == CommandOutcome::Ok && outcome.killedBy == CommandOutcome::NotKilled) {
        m_held.remove(connection);      // the problem did not repeat: the held batch is discarded
    }
    m_inner->onCommandFinished(outcome);
}

QByteArray DiagnosticCollector::report(const QString &connection, const QString &applicationVersion) const
{
    QHash<QString, Held>::const_iterator it = m_held.constFind(connection);
    if (it == m_held.constEnd())
        return QByteArray();
    const Held &h = it.value();
    DiagnosticContext c;
    c.applicationVersion = applicationVersion;
    c.adapterId = h.run.link.adapterId;
    c.adapterVersion = h.run.link.adapterVersion;
    c.commandType = h.outcome.commandType;
    c.violations = h.run.violations;
    c.batches = h.outcome.batches;
    c.recordsReceived = h.outcome.recordsReceived;
    c.recordsValid = h.outcome.recordsValid;
    for (QMap<int, qint64>::const_iterator q = h.outcome.quarantinedByReason.constBegin();
         q != h.outcome.quarantinedByReason.constEnd(); ++q)
        c.quarantinedByReason.insert(quarantineReasonName(static_cast<Validation::Reason>(q.key())), q.value());
    c.softViolations = h.outcome.softViolations;
    c.hardViolations = h.outcome.hardViolations;
    c.declaredFeeds = knownFeeds();
    registryWords(&c.declaredMetrics, &c.declaredUnits);

    if (h.run.hasBatch)
        return withoutMeasurements(buildDiagnosticReport(h.run.batch, c));
    // The kill did not fall in the middle of a batch (a framing error, a deadline of the guard's
    // own): the context is the whole report and the batch line says there was none.
    QByteArray first = buildDiagnosticReport(QByteArray(), c);
    first = first.left(first.indexOf('\n') + 1);
    QJsonObject none;
    none.insert(QStringLiteral("line_type"), QStringLiteral("batch"));
    none.insert(QStringLiteral("status"), QStringLiteral("none"));
    return first + QJsonDocument(none).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray DiagnosticCollector::withoutMeasurements(const QByteArray &jsonLines)
{
    QByteArray out;
    const QList<QByteArray> lines = jsonLines.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).isEmpty())
            continue;
        QJsonObject o = QJsonDocument::fromJson(lines.at(i)).object();
        if (o.value(QStringLiteral("line_type")).toString() == QLatin1String("record")) {
            o = stripValue(o);
            if (o.value(QStringLiteral("payload")).isObject())
                o.insert(QStringLiteral("payload"), stripValue(o.value(QStringLiteral("payload")).toObject()));
        }
        out += QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    }
    return out;
}

} // namespace lm
