// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "snapshotbuilder.h"

#include <QRegularExpression>
#include <QStringList>

namespace lm {
namespace real {

QList<ui::SubjectSnapshot> buildSnapshots(const QList<ConfigSubject> &subjects, const ServiceReport *report,
                                          const QHash<QString, SubjectData> &data,
                                          const QVector<QPair<qint64, qint64> > &serviceGaps, qint64 nowMs)
{
    QList<ui::SubjectSnapshot> out;
    for (int i = 0; i < subjects.size(); ++i) {
        const ConfigSubject &c = subjects.at(i);
        ui::SubjectSnapshot s;
        s.id = c.id;
        s.name = c.name;
        s.label = c.label;
        s.connection = c.connection;

        const SubjectReport *r = report ? report->subject(c.id) : 0;
        if (r) {
            s.report.state = r->state;
            s.report.cause = r->cause;
            s.report.stale = r->stale;
            s.report.staleCause = r->staleCause;
            s.report.staleDetail = r->staleDetail;
            s.report.staleSinceAgeS = r->staleSinceMs >= 0 ? (nowMs - r->staleSinceMs) / 1000 : -1;
            if (!r->connection.isEmpty())
                s.connection = r->connection;
            s.reportProposed = r->reportProposed;
        } else if (!report) {
            // Nobody to ask: the Subject is not being monitored as far as anyone knows.
            s.report.state = QStringLiteral("idle");
            s.report.stale = true;
            s.report.staleCause = QStringLiteral("service-not-running");
        } else {
            // The service answers but has not taken this Subject up yet (just added, or waiting for
            // a restart of the service): a Subject waiting for its first reading, not a fault.
            s.report.state = QStringLiteral("idle");
        }

        QHash<QString, SubjectData>::const_iterator it = data.constFind(c.id);
        if (it != data.constEnd()) {
            const SubjectData &d = it.value();
            s.hasReading = d.hasReading;
            s.measuredAtMs = d.measuredAtMs;
            s.value = d.value;
            s.unit = d.unit;
            s.current = d.current;
            s.history = d.history;
            s.firstDataMs = d.firstDataMs;
            s.sensor = d.sensor;
            // The arrow belongs to ONE reading and means nothing once that reading is stale: it is
            // attached only to the newest stored reading, and never while the Subject is stale
            // (the service already stops sending it after three minutes).
            if (r && r->hasTrend && !r->stale && d.hasReading && r->trendForMs == d.measuredAtMs) {
                s.hasTrend = true;
                s.trendCode = r->trendCode;
            }
        }
        s.serviceGaps = serviceGaps;
        out.append(s);
    }
    return out;
}

ui::ServiceStatus serviceStatusOf(const ServiceReport *report, bool answered, bool reachable, bool callFailed,
                                  VersionRelation relation)
{
    if (callFailed)
        return ui::ServiceStatus::CallFailed;
    if (!answered)
        return reachable ? ui::ServiceStatus::Connecting : ui::ServiceStatus::Connecting;
    if (!reachable || !report)
        return ui::ServiceStatus::Stopped;
    // Only a service that is alive can be "older" or "newer": one that is stopping is about to be gone, and
    // the next Start runs the installed binary.
    const bool alive = report->state == QLatin1String("running") || report->state == QLatin1String("starting")
                       || report->state == QLatin1String("running-awaiting-approval");
    if (alive && relation == VersionRelation::Older)
        return ui::ServiceStatus::ServiceOlder;
    if (alive && relation == VersionRelation::Newer)
        return ui::ServiceStatus::ServiceNewer;
    if (report->state == QLatin1String("running"))
        return ui::ServiceStatus::Running;
    if (report->state == QLatin1String("starting"))
        return ui::ServiceStatus::Starting;
    if (report->state == QLatin1String("running-awaiting-approval"))
        return ui::ServiceStatus::AwaitingApproval;
    return ui::ServiceStatus::Stopped;          // "stopping", "not-running"
}

int compareAppVersions(const QString &a, const QString &b)
{
    const QRegularExpression split(QStringLiteral("[-.]"));
    const QStringList pa = a.split(split), pb = b.split(split);
    QList<qint64> na, nb;
    for (int i = 0; i < pa.size(); ++i) {
        bool ok = false;
        na << pa.at(i).toLongLong(&ok);
        if (!ok || pa.at(i).isEmpty())
            return -2;
    }
    for (int i = 0; i < pb.size(); ++i) {
        bool ok = false;
        nb << pb.at(i).toLongLong(&ok);
        if (!ok || pb.at(i).isEmpty())
            return -2;
    }
    for (int i = 0; i < qMax(na.size(), nb.size()); ++i) {
        const qint64 x = i < na.size() ? na.at(i) : 0, y = i < nb.size() ? nb.at(i) : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

VersionRelation serviceVersionRelation(const ServiceReport *report, const QString &ownVersion)
{
    if (!report || ownVersion.isEmpty() || report->appVersion == ownVersion)
        return VersionRelation::Same;
    if (!report->appVersion.isEmpty() && compareAppVersions(report->appVersion, ownVersion) == 1)
        return VersionRelation::Newer;
    return VersionRelation::Older;
}

} // namespace real
} // namespace lm
