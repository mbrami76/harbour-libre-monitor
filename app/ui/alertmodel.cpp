// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alertmodel.h"

#include <QDateTime>

namespace lm {
namespace ui {

AlertModel::AlertModel(QObject *parent)
    : QAbstractListModel(parent), m_subjects(0), m_alerts(0), m_active(true), m_landingCheckPending(false)
{
}

void AlertModel::setBackends(Backend *subjects, AlertsBackend *alerts)
{
    if (m_subjects)
        disconnect(m_subjects, 0, this, 0);
    if (m_alerts)
        disconnect(m_alerts, 0, this, 0);
    m_subjects = subjects;
    m_alerts = alerts;
    if (m_subjects)
        connect(m_subjects, &Backend::changed, this, &AlertModel::refresh);
    if (m_alerts) {
        connect(m_alerts, &AlertsBackend::changed, this, [this]() {
            refresh();
            // The first new picture after the window came to the front (the backend asks the service
            // then): look again for an alert that arrived while the window was away.
            if (m_landingCheckPending) {
                m_landingCheckPending = false;
                checkLanding();
            }
        });
        connect(m_alerts, &AlertsBackend::answered, this, &AlertModel::onAnswered);
    }
    refresh();
}

QHash<int, QByteArray> AlertModel::roleNames() const
{
    QHash<int, QByteArray> r;
    r[SubjectIdRole] = "subjectId";        r[SlotRole] = "slot";
    r[StateRole] = "state";                r[AcknowledgedRole] = "acknowledged";
    r[LevelSinceMsRole] = "levelSinceMs";  r[SnoozeUntilMsRole] = "snoozeUntilMs";
    r[NameRole] = "name";                  r[ShortLabelRole] = "shortLabel";
    r[LevelKeyRole] = "levelKey";          r[BandKeyRole] = "bandKey";
    r[CauseKeyRole] = "causeKey";          r[MinutesSinceRole] = "minutesSince";
    r[CanAcknowledgeRole] = "canAcknowledge"; r[CanSnoozeRole] = "canSnooze";
    r[IsPauseRole] = "isPause";            r[SnoozedRole] = "snoozed";
    return r;
}

int AlertModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant AlertModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();
    const QHash<int, QByteArray> names = roleNames();
    if (!names.contains(role))
        return QVariant();
    return m_rows.at(index.row()).toMap().value(QString::fromLatin1(names.value(role)));
}

int AlertModel::unacknowledgedCount() const
{
    int n = 0;
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).canAcknowledge)
            ++n;
    return n;
}

void AlertModel::refresh()
{
    beginResetModel();
    m_facts = m_alerts ? m_alerts->facts() : AlertsFacts();
    const QList<SubjectSnapshot> subs = m_subjects ? m_subjects->subjects() : QList<SubjectSnapshot>();
    const qint64 now = m_subjects ? m_subjects->nowMs() : QDateTime::currentMSecsSinceEpoch();
    m_rows = buildAlertRows(m_facts, subs, now);
    endResetModel();
    emit changed();
}

QVariantMap AlertModel::get(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).toMap() : QVariantMap();
}

QVariantList AlertModel::forSubject(const QString &subjectId) const
{
    QVariantList out;
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).fact.subject == subjectId)
            out.append(m_rows.at(i).toMap());
    return out;
}

QVariantMap AlertModel::silentOf(const QString &subjectId) const
{
    const qint64 now = m_subjects ? m_subjects->nowMs() : QDateTime::currentMSecsSinceEpoch();
    const SilentView v = silentViewOf(m_facts, subjectId, now);
    QVariantMap m;
    m.insert(QStringLiteral("silent"), v.silent);
    m.insert(QStringLiteral("noLimit"), v.noLimit);
    m.insert(QStringLiteral("untilMs"), double(v.untilMs));
    m.insert(QStringLiteral("untilToday"), v.untilToday);
    return m;
}

QVariantList AlertModel::snoozeChoices() const
{
    QVariantList out;
    const QList<int> c = snoozeChoicesMinutes();
    for (int i = 0; i < c.size(); ++i)
        out.append(c.at(i));
    return out;
}

void AlertModel::acknowledge(const QString &subjectId, const QString &slot)
{
    clearResult();
    if (m_alerts)
        m_alerts->acknowledge(subjectId, slot);
}

void AlertModel::snooze(const QString &subjectId, const QString &slot, int minutes)
{
    clearResult();
    if (m_alerts)
        m_alerts->snooze(subjectId, slot, minutes);
}

QString AlertModel::requestSilent(const QString &subjectId, const QString &choice)
{
    const int minutes = silentMinutesForChoice(choice);
    if (minutes < -1)
        return QStringLiteral("invalid");
    if (silentChoiceNeedsConfirmation(choice))
        return QStringLiteral("confirm");       // never without the page's confirmation
    clearResult();
    if (m_alerts)
        m_alerts->setSilent(subjectId, minutes);
    return QStringLiteral("sent");
}

void AlertModel::confirmSilentWithoutLimit(const QString &subjectId)
{
    clearResult();
    if (m_alerts)
        m_alerts->setSilent(subjectId, silentMinutesForChoice(QStringLiteral("no-limit")));
}

void AlertModel::endSilent(const QString &subjectId)
{
    clearResult();
    if (m_alerts)
        m_alerts->endSilent(subjectId);
}

void AlertModel::clearResult()
{
    if (m_resultKey.isEmpty())
        return;
    m_resultKey.clear();
    emit resultChanged();
}

void AlertModel::onAnswered(const QString &, const QString &, const QString &result)
{
    const QString key = alertResultKey(result);
    if (key == m_resultKey)
        return;
    m_resultKey = key;
    emit resultChanged();
}

QSet<QString> AlertModel::unacknowledgedKeys() const
{
    QSet<QString> keys;
    for (int i = 0; i < m_rows.size(); ++i) {
        const AlertRow &r = m_rows.at(i);
        if (r.canAcknowledge)
            keys.insert(r.fact.subject + QLatin1Char('|') + r.fact.slot + QLatin1Char('|')
                        + r.fact.episode + QLatin1Char('|') + r.fact.level);
    }
    return keys;
}

void AlertModel::setApplicationActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (!active) {
        m_keysWhenLeft = unacknowledgedKeys();
        m_landingCheckPending = false;
        return;
    }
    // What the model knows already (the service's signals reach the process in the background too) ...
    checkLanding();
    // ... and what the backend's fresh question brings.
    m_landingCheckPending = true;
}

void AlertModel::checkLanding()
{
    const QSet<QString> now = unacknowledgedKeys();
    QSet<QString> fresh = now;
    fresh.subtract(m_keysWhenLeft);
    m_keysWhenLeft = now;           // land once per new alert, not at every later check
    if (!fresh.isEmpty())
        emit landOnOverview();
}

} // namespace ui
} // namespace lm
