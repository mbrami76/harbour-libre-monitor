// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_SUBJECTMODEL_H
#define LM_UI_SUBJECTMODEL_H

// The list of Subjects as the QML sees it: one row per Subject with everything a tile, the
// Subject page and the cover need, already decided. The QML only
// presents these answers (REQ-UI-07).
//
// Pattern: a read-only list model that recomputes every row from the backend's snapshots and
// the clock whenever the backend changes or the display tick fires. Ages and the fresh/stale
// decision depend on "now", so a tick must refresh() even when no data arrived: a value must
// stop looking current while nothing happens (REQ-UI-09).

#include "alertsbackend.h"
#include "alertview.h"
#include "backend.h"
#include "band.h"
#include "direction.h"
#include "subjectstate.h"

#include <QAbstractListModel>
#include <QHash>
#include <QLocale>
#include <QPair>
#include <QVariantMap>

namespace lm {
namespace ui {

// The decision for one Subject at time `nowMs` (age, band, fresh or not, buttons): the one place
// that turns a snapshot into a SubjectView, used by the list and by the plot.
SubjectView viewOfSnapshot(const SubjectSnapshot &snapshot, qint64 nowMs);

class SubjectModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY changed)
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY changed)
    Q_PROPERTY(int coverIndex READ coverIndex NOTIFY changed)
    Q_PROPERTY(QString displayUnit READ displayUnit WRITE setDisplayUnit NOTIFY changed)

public:
    enum Role {
        SubjectIdRole = Qt::UserRole + 1, NameRole, LabelRole, ConnectionRole,
        ValueTextRole, UnitTextRole, BandKeyRole, StateNameRole, StateKeyRole, FreshRole,
        DirectionSymbolRole, DirectionKeyRole, DirectionIsArrowRole, AgeSecondsRole,
        MeasuredAtMsRole, GuardRole, ShowRetryRole, ShowReenableRole, ShowEditAccountRole,
        ShowAskAgainRole, ShowSetUpAccountRole, NeedsAttentionRole, CoverRankRole, ReportProposedRole,
        // The short name of a patient where a stranger may read it (the cover with names off, a chip, a
        // page without a name): lm::patientLabel, the function the notifications use.
        ShortLabelRole,
        // Alerts and silent mode: the patient's alerts in the
        // arbiter's order as maps (alertview.h AlertRow), how many are not acknowledged, and silent mode
        // for the struck-through speaker on the tile, the patient page and the cover (REQ-UI-15).
        AlertsRole, UnacknowledgedAlertsRole, SilentRole, SilentNoLimitRole, SilentUntilMsRole,
        SilentUntilTodayRole,
        // The sensor block of the patient page (sensorview.h, REQ-SEN-01 to 03).
        SensorRole
    };

    explicit SubjectModel(QObject *parent = 0);

    void setBackend(Backend *backend);
    // The service's alerts and silent mode; optional (without it a row has no alerts and is not silent).
    void setAlertsBackend(AlertsBackend *alerts);
    // Display unit "mg/dL" or "mmol/L"; the value texts follow it. Empty = mmol/L until the
    // first-run choice is made.
    QString displayUnit() const { return m_unit; }
    void setDisplayUnit(const QString &unit);
    void setLocale(const QLocale &locale);

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;
    QHash<int, QByteArray> roleNames() const;

    int attentionCount() const;
    // The Subject the cover shows: the most urgent one, or the one the user stepped to by
    // hand while the hold of kUiCoverManualHoldS has not run out.
    int coverIndex() const;

    // Recompute from the backend now (the display tick calls this).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void stepCover(int delta);
    // The rows the cover shows (at most maxRows), the ones nearer the limits first, as maps like get().
    Q_INVOKABLE QVariantList coverRows(int maxRows) const;
    Q_INVOKABLE int indexOfSubject(const QString &subjectId) const;
    // The row as a map, for the Subject page header (QML cannot read a row by index otherwise).
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void retry(const QString &subjectId);
    Q_INVOKABLE void reenable(const QString &subjectId);
    // The user declined (or, later, wrote) the proposed diagnostic report (REQ-GRD-15).
    Q_INVOKABLE void declineReport(const QString &subjectId);

signals:
    void changed();

private:
    struct Row
    {
        SubjectSnapshot s;
        SubjectView view;
        QString valueText, unitText;
        Direction direction;
        bool arrowKept;         // the word is kept from the same reading, the arrow is not
        qint64 ageS;
        QVariantList alerts;
        int unacknowledged;
        SilentView silent;
        QVariantMap sensor;
    };

    void rebuild();

    Backend *m_backend;
    AlertsBackend *m_alerts;
    QString m_unit;
    QLocale m_locale;
    QList<Row> m_rows;
    // The trend code last seen for a Subject with the time of its reading: the service stops sending the
    // arrow after its limit, the word may stay a little longer for the SAME reading.
    QHash<QString, QPair<qint64, int> > m_lastTrend;
    int m_coverManual;          // -1 = automatic
    qint64 m_coverManualAtMs;
};

} // namespace ui
} // namespace lm

#endif
