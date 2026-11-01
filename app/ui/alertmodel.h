// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ALERTMODEL_H
#define LM_UI_ALERTMODEL_H

// The alerts as the QML sees them:
// the list of every alert of every patient in the arbiter's order for the alerts page, the alerts of
// one patient for its page, silent mode of one patient, and the user's actions (Acknowledge, Snooze,
// silent mode and its end). The pages only present these answers (REQ-UI-07).
//
// Pattern: a read-only list model rebuilt from the two backends' snapshots and the clock whenever one
// of them changes or the display tick fires (the minutes since an alert began grow with the clock).
// The actions are requests to the alerts backend; their answers come back as a text key the pages show
// once (`resultKey`), and the new picture as changed().
//
// Landing on the overview: a tap on a notification's banner opens the application and does not
// acknowledge, so a caregiver who taps a banner must find the alert at once. When the application comes
// to the front and an unacknowledged alert is there that was not there when it went to the background,
// the model asks the window to go back to the overview (landOnOverview()), whose top shows the alerts
// with their Acknowledge buttons.

#include "alertsbackend.h"
#include "alertview.h"
#include "backend.h"

#include <QAbstractListModel>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

namespace lm {
namespace ui {

class AlertModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY changed)
    Q_PROPERTY(int unacknowledgedCount READ unacknowledgedCount NOTIFY changed)
    // False while the service's alerts are not known (no answer yet, the service is not running, an
    // answer this version cannot read): the pages then say so instead of "no alerts".
    Q_PROPERTY(bool known READ known NOTIFY changed)
    // True when the alerts are not known because the service is an older build without Alerts() (the
    // error "service-older"); the alerts page then says so and what to do.
    Q_PROPERTY(bool serviceOlder READ serviceOlder NOTIFY changed)
    // The text key of the last action's answer when there is something to say ("" otherwise).
    Q_PROPERTY(QString resultKey READ resultKey NOTIFY resultChanged)

public:
    enum Role {
        SubjectIdRole = Qt::UserRole + 1, SlotRole, StateRole, AcknowledgedRole, LevelSinceMsRole,
        SnoozeUntilMsRole, NameRole, ShortLabelRole, LevelKeyRole, BandKeyRole, CauseKeyRole,
        MinutesSinceRole, CanAcknowledgeRole, CanSnoozeRole, IsPauseRole, SnoozedRole
    };

    explicit AlertModel(QObject *parent = 0);

    void setBackends(Backend *subjects, AlertsBackend *alerts);

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;
    QHash<int, QByteArray> roleNames() const;

    int unacknowledgedCount() const;
    bool known() const { return m_facts.known; }
    bool serviceOlder() const { return !m_facts.known && m_facts.error == QLatin1String("service-older"); }
    QString resultKey() const { return m_resultKey; }

    // Recompute from the backends now (the display tick calls this).
    Q_INVOKABLE void refresh();
    // One row as a map with the role names (QML cannot read a row of a list model by index otherwise).
    Q_INVOKABLE QVariantMap get(int row) const;
    // The alerts of one patient, in the same order, as maps with the role names.
    Q_INVOKABLE QVariantList forSubject(const QString &subjectId) const;
    // Silent mode of one patient: {silent, noLimit, untilMs, untilToday}.
    Q_INVOKABLE QVariantMap silentOf(const QString &subjectId) const;
    // The snooze durations in minutes, for the buttons.
    Q_INVOKABLE QVariantList snoozeChoices() const;

    Q_INVOKABLE void acknowledge(const QString &subjectId, const QString &slot);
    Q_INVOKABLE void snooze(const QString &subjectId, const QString &slot, int minutes);
    // Silent mode from a page's choice ("hour", "four-hours", "tomorrow", "no-limit"). Returns "sent",
    // or "confirm" for "no-limit": nothing is sent until the page has shown the confirmation
    // and calls confirmSilentWithoutLimit(); "invalid" for an unknown choice.
    Q_INVOKABLE QString requestSilent(const QString &subjectId, const QString &choice);
    Q_INVOKABLE void confirmSilentWithoutLimit(const QString &subjectId);
    Q_INVOKABLE void endSilent(const QString &subjectId);
    Q_INVOKABLE void clearResult();

    // The window's state (QGuiApplication::applicationStateChanged): see "Landing on the overview".
    void setApplicationActive(bool active);

signals:
    void changed();
    void resultChanged();
    void landOnOverview();

private slots:
    void onAnswered(const QString &action, const QString &subject, const QString &result);

private:
    QSet<QString> unacknowledgedKeys() const;
    void checkLanding();

    Backend *m_subjects;
    AlertsBackend *m_alerts;
    AlertsFacts m_facts;
    QList<AlertRow> m_rows;
    QString m_resultKey;
    bool m_active;
    bool m_landingCheckPending;
    QSet<QString> m_keysWhenLeft;
};

} // namespace ui
} // namespace lm

#endif
