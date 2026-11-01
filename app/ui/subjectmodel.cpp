// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "subjectmodel.h"

#include "coverrank.h"
#include "direction.h"
#include "glucoseformat.h"
#include "uiconstants.h"
#include "service/subjectlabel.h"
#include "units/glucoseunits.h"

#include <algorithm>
#include <cmath>

namespace lm {
namespace ui {

SubjectView viewOfSnapshot(const SubjectSnapshot &s, qint64 nowMs)
{
    SubjectInput in = s.report;
    in.hasReading = s.hasReading;
    in.ageS = s.hasReading ? (nowMs - s.measuredAtMs) / 1000 : -1;
    in.band = s.hasReading ? bandOf(s.value, s.unit) : Band::Unknown;
    return resolveSubject(in);
}

SubjectModel::SubjectModel(QObject *parent)
    : QAbstractListModel(parent), m_backend(0), m_alerts(0), m_unit(QStringLiteral("mmol/L")),
      m_coverManual(-1), m_coverManualAtMs(0)
{
}

void SubjectModel::setAlertsBackend(AlertsBackend *alerts)
{
    if (m_alerts)
        disconnect(m_alerts, 0, this, 0);
    m_alerts = alerts;
    if (m_alerts)
        connect(m_alerts, &AlertsBackend::changed, this, &SubjectModel::refresh);
    refresh();
}

void SubjectModel::setBackend(Backend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (m_backend)
        connect(m_backend, &Backend::changed, this, &SubjectModel::refresh);
    refresh();
}

void SubjectModel::setDisplayUnit(const QString &unit)
{
    if ((unit != QLatin1String("mg/dL") && unit != QLatin1String("mmol/L")) || unit == m_unit)
        return;
    m_unit = unit;
    refresh();
}

void SubjectModel::setLocale(const QLocale &locale)
{
    m_locale = locale;
    refresh();
}

QHash<int, QByteArray> SubjectModel::roleNames() const
{
    QHash<int, QByteArray> r;
    r[SubjectIdRole] = "subjectId";        r[NameRole] = "name";
    r[LabelRole] = "label";                r[ConnectionRole] = "connection";
    r[ValueTextRole] = "valueText";        r[UnitTextRole] = "unitText";
    r[BandKeyRole] = "bandKey";          r[StateNameRole] = "stateName";
    r[StateKeyRole] = "stateKey";          r[FreshRole] = "fresh";
    r[DirectionSymbolRole] = "directionSymbol";  r[DirectionKeyRole] = "directionKey";
    r[DirectionIsArrowRole] = "directionIsArrow"; r[AgeSecondsRole] = "ageSeconds";
    r[MeasuredAtMsRole] = "measuredAtMs";  r[GuardRole] = "guard";
    r[ShowRetryRole] = "showRetry";        r[ShowReenableRole] = "showReenable";
    r[ShowEditAccountRole] = "showEditAccount"; r[ShowAskAgainRole] = "showAskAgain";
    r[ShowSetUpAccountRole] = "showSetUpAccount"; r[NeedsAttentionRole] = "needsAttention";
    r[CoverRankRole] = "coverRank";
    r[ReportProposedRole] = "reportProposed";
    r[ShortLabelRole] = "shortLabel";
    r[AlertsRole] = "alertList";     // not "alerts": the context property of the alert model has that name
    r[UnacknowledgedAlertsRole] = "unacknowledgedAlerts";
    r[SilentRole] = "silent";
    r[SilentNoLimitRole] = "silentNoLimit";
    r[SilentUntilMsRole] = "silentUntilMs";
    r[SilentUntilTodayRole] = "silentUntilToday";
    r[SensorRole] = "sensor";
    return r;
}

int SubjectModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

void SubjectModel::rebuild()
{
    m_rows.clear();
    if (!m_backend)
        return;
    const qint64 now = m_backend->nowMs();
    const QList<SubjectSnapshot> snaps = m_backend->subjects();
    const AlertsFacts alertFacts = m_alerts ? m_alerts->facts() : AlertsFacts();
    const QList<AlertRow> alertRows = buildAlertRows(alertFacts, snaps, now);
    for (int i = 0; i < snaps.size(); ++i) {
        Row row;
        row.s = snaps.at(i);
        row.unacknowledged = 0;
        for (int k = 0; k < alertRows.size(); ++k)
            if (alertRows.at(k).fact.subject == row.s.id) {
                row.alerts.append(alertRows.at(k).toMap());
                if (alertRows.at(k).canAcknowledge)
                    ++row.unacknowledged;
            }
        row.silent = silentViewOf(alertFacts, row.s.id, now);
        row.sensor = sensorMap(row.s.sensor, now);
        row.ageS = row.s.hasReading ? (now - row.s.measuredAtMs) / 1000 : -1;
        row.view = viewOfSnapshot(row.s, now);
        // The number is always readable (a stale reading is shown dated and neutral, never
        // hidden:); whether it is "current" is the view's decision.
        if (row.s.hasReading) {
            row.valueText = formatGlucose(row.s.value, row.s.unit, m_unit, m_locale);
            row.unitText = m_unit;
        }
        // The direction belongs to the newest reading and is shown only while that reading is
        // fresh (docs 2.3); the model answers "no arrow, show nothing" with a Null direction
        // that the page shows as NULL only for a fresh reading.
        row.arrowKept = false;
        if (row.s.hasTrend) {
            m_lastTrend.insert(row.s.id, qMakePair(row.s.measuredAtMs, row.s.trendCode));
            row.direction = directionOfCode(true, row.s.trendCode);
        } else if (row.s.hasReading && m_lastTrend.contains(row.s.id)
                   && m_lastTrend.value(row.s.id).first == row.s.measuredAtMs) {
            // The service no longer reports the arrow (its limit passed) but this is the same reading: the
            // description may stay while the display grace lasts, the ARROW does not.
            row.direction = directionOfCode(true, m_lastTrend.value(row.s.id).second);
            row.arrowKept = true;
        } else {
            row.direction = directionOfCode(false, 0);
        }
        m_rows.append(row);
    }
}

void SubjectModel::refresh()
{
    beginResetModel();
    rebuild();
    endResetModel();
    // A manual cover choice lapses after the hold or when the list shrank.
    if (m_coverManual >= m_rows.size() ||
        (m_backend && m_coverManual >= 0 && m_backend->nowMs() - m_coverManualAtMs > kUiCoverManualHoldS * 1000LL))
        m_coverManual = -1;
    emit changed();
}

QVariant SubjectModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();
    const Row &r = m_rows.at(index.row());
    const bool fresh = isFresh(r.view.id);
    switch (role) {
    case SubjectIdRole: return r.s.id;
    case NameRole: return r.s.name;
    case LabelRole: return r.s.label;
    case ConnectionRole: return r.s.connection;
    case ValueTextRole: return r.valueText;
    case UnitTextRole: return r.unitText;
    case BandKeyRole: return fresh ? bandKey(r.view.band) : QString();     // no band for a stale reading, REQ-UI-05
    case StateNameRole: return stateIdName(r.view.id);
    case StateKeyRole: return stateTextKey(r.view.id, r.view.rateLimited);
    case FreshRole: return fresh;
    case DirectionSymbolRole: return fresh && !r.arrowKept ? r.direction.symbol() : QString();
    case DirectionKeyRole: return fresh ? r.direction.textKey() : QString();
    case DirectionIsArrowRole: return fresh && !r.arrowKept && r.direction.isArrow();
    case AgeSecondsRole: return r.ageS;
    case MeasuredAtMsRole: return r.s.measuredAtMs;
    case GuardRole: return int(r.view.guard);
    case ShowRetryRole: return r.view.showRetry;
    case ShowReenableRole: return r.view.showReenable;
    case ShowEditAccountRole: return r.view.showEditAccount;
    case ShowAskAgainRole: return r.view.showAskAgain;
    case ShowSetUpAccountRole: return r.view.showSetUpAccount;
    case NeedsAttentionRole: return !fresh || r.view.guard != GuardStrip::None;
    case CoverRankRole: return coverRank(r.view.id);
    case ReportProposedRole: return r.s.reportProposed;
    // The row is the patient's place in the configuration file (the backend keeps the file's order).
    case ShortLabelRole: return patientLabel(r.s.label, index.row());
    case AlertsRole: return r.alerts;
    case UnacknowledgedAlertsRole: return r.unacknowledged;
    case SilentRole: return r.silent.silent;
    case SilentNoLimitRole: return r.silent.noLimit;
    case SilentUntilMsRole: return double(r.silent.untilMs);
    case SilentUntilTodayRole: return r.silent.untilToday;
    case SensorRole: return r.sensor;
    }
    return QVariant();
}

int SubjectModel::attentionCount() const
{
    int n = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows.at(i);
        // "Needs attention" = not being monitored (docs 1.4); a new Subject is not a problem.
        if (!isFresh(r.view.id) && r.view.id != StateId::NoDataYet)
            ++n;
    }
    return n;
}

int SubjectModel::coverIndex() const
{
    if (m_coverManual >= 0 && m_coverManual < m_rows.size())
        return m_coverManual;
    QList<CoverCandidate> c;
    for (int i = 0; i < m_rows.size(); ++i) {
        CoverCandidate cc;
        cc.id = m_rows.at(i).view.id;
        cc.staleSinceAgeS = m_rows.at(i).s.report.staleSinceAgeS;
        c.append(cc);
    }
    return pickCoverSubject(c);
}

QVariantList SubjectModel::coverRows(int maxRows) const
{
    QList<CoverCandidate> c;
    const Thresholds t;
    // The limits in mg/dL: the very numbers the bands use.
    const double limits[4] = { double(t.ll), double(t.l), double(t.h), double(t.hh) };
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows.at(i);
        CoverCandidate cc;
        cc.id = r.view.id;
        cc.staleSinceAgeS = r.s.report.staleSinceAgeS;
        if (isFresh(r.view.id) && r.s.hasReading) {
            double mgdl = r.s.value;
            if (r.s.unit == QLatin1String("mmol/L"))
                mgdl = r.s.value * kMgDlPerMmolL;     // exact, from the stored value
            double d = 1e9;
            for (int k = 0; k < 4; ++k)
                d = std::min(d, std::fabs(mgdl - limits[k]));
            cc.limitDistanceMgdl = d;
            cc.valueMgdl = mgdl;
        }
        c.append(cc);
    }
    QVariantList out;
    const QList<int> order = orderForCover(c, maxRows);
    for (int i = 0; i < order.size(); ++i)
        out.append(get(order.at(i)));
    return out;
}

void SubjectModel::stepCover(int delta)
{
    if (m_rows.isEmpty())
        return;
    const int cur = coverIndex() < 0 ? 0 : coverIndex();
    const int n = m_rows.size();
    m_coverManual = ((cur + delta) % n + n) % n;
    m_coverManualAtMs = m_backend ? m_backend->nowMs() : 0;
    emit changed();
}

int SubjectModel::indexOfSubject(const QString &subjectId) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).s.id == subjectId)
            return i;
    return -1;
}

QVariantMap SubjectModel::get(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_rows.size())
        return m;
    const QHash<int, QByteArray> names = roleNames();
    for (QHash<int, QByteArray>::const_iterator it = names.begin(); it != names.end(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return m;
}

void SubjectModel::retry(const QString &subjectId)
{
    if (m_backend)
        m_backend->retry(subjectId);
}

void SubjectModel::declineReport(const QString &subjectId)
{
    if (m_backend)
        m_backend->reportHandled(subjectId);
}

void SubjectModel::reenable(const QString &subjectId)
{
    if (m_backend)
        m_backend->reenable(subjectId);
}

} // namespace ui
} // namespace lm
