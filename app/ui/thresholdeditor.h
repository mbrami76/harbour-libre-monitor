// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_THRESHOLDEDITOR_H
#define LM_UI_THRESHOLDEDITOR_H

// The thresholds page of one patient as the QML sees it (REQ-THR-01 to 04): the low and high limits in force in the
// user's unit, the standard values, the very low and very high limits (read-only, the same for every
// patient), the edit with its CHALLENGE, every refusal in words with numbers, the history of the edits,
// and the choices of the three alert times.
//
// The flow of an edit, which the service enforces (SetThresholds; the page never decides "looser"):
//   1. save(low, high): the texts are read in the user's unit (an mmol/L edit is rounded once to
//      whole mg/dL) and sent with nothing typed again;
//   2. the service answers `needs-confirmation` when a limit that moves becomes looser than the standard
//      value: the page then shows the CHALLENGE, a separate screen that warns and asks for that limit
//      to be typed again, and does NOT show the value just asked for (no property of this object
//      carries it, which a test checks);
//   3. confirmChallenge(typed): the values typed again are sent once and forgotten at once; the service
//      answers `accepted` or `confirmation-mismatch` (which limit, never with which number).
// Every other answer is explained in words with the numbers the service gave, in the user's unit.
//
// Pattern: a controller over the two ports (the alerts backend for the facts and SetThresholds, the
// data backend for the journal's history), like AlertModel. One object serves the one thresholds page
// that can be open; open() starts it afresh for a patient. Nothing here polls: the history is read when
// the page opens and after an accepted edit.

#include "alertsbackend.h"
#include "backend.h"
#include "thresholdview.h"

#include <QLocale>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace lm {
namespace ui {

class ThresholdEditor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString subjectId READ subjectId NOTIFY changed)
    Q_PROPERTY(QString unit READ unit NOTIFY changed)
    // The limits in force are known (the service answered and follows the patient).
    Q_PROPERTY(bool known READ known NOTIFY changed)
    Q_PROPERTY(QString lowText READ lowText NOTIFY changed)             // the number alone, for the field
    Q_PROPERTY(QString highText READ highText NOTIFY changed)
    Q_PROPERTY(QString standardLowText READ standardLowText NOTIFY changed)   // with the unit
    Q_PROPERTY(QString standardHighText READ standardHighText NOTIFY changed)
    Q_PROPERTY(QString veryLowText READ veryLowText NOTIFY changed)
    Q_PROPERTY(QString veryHighText READ veryHighText NOTIFY changed)
    // "idle", "sending" (an edit is on its way), "challenge" (the page shows the challenge screen).
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(bool challengeLow READ challengeLow NOTIFY changed)
    Q_PROPERTY(bool challengeHigh READ challengeHigh NOTIFY changed)
    // The last message, {success, parts: [{key, args}]}; empty parts = nothing to say.
    Q_PROPERTY(QVariantMap message READ message NOTIFY changed)
    // The history, newest first (thresholdview.h); `historyState` is "loading", "ready" or "unreadable".
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(QString historyState READ historyState NOTIFY changed)
    // The three alert times are known (Alerts() gave the values and the ranges).
    Q_PROPERTY(bool timesKnown READ timesKnown NOTIFY changed)

public:
    explicit ThresholdEditor(QObject *parent = 0);

    void setBackends(Backend *data, AlertsBackend *alerts);
    void setDisplayUnit(const QString &unit);
    void setLocale(const QLocale &locale);

    QString subjectId() const { return m_subject; }
    QString unit() const { return m_unit; }
    bool known() const;
    QString lowText() const;
    QString highText() const;
    QString standardLowText() const;
    QString standardHighText() const;
    QString veryLowText() const;
    QString veryHighText() const;
    QString state() const { return m_state; }
    bool challengeLow() const { return m_need.low; }
    bool challengeHigh() const { return m_need.high; }
    QVariantMap message() const { return m_message.toMap(); }
    QVariantList history() const { return thresholdHistoryRows(m_history, m_unit, m_locale); }
    QString historyState() const { return m_historyState; }
    bool timesKnown() const;

    // Starts the page for a patient: no edit, no message, the history asked for.
    Q_INVOKABLE void open(const QString &subjectId);
    // No note: the threshold change carries no comment.
    Q_INVOKABLE void save(const QString &lowText, const QString &highText);
    // The texts typed again on the challenge screen; a limit not asked for is ignored.
    Q_INVOKABLE void confirmChallenge(const QString &typedLowText, const QString &typedHighText);
    // The user left the challenge: nothing is sent, the page says why nothing changed.
    Q_INVOKABLE void cancelChallenge();
    Q_INVOKABLE void clearMessage();

    // The choices of one alert time ("low", "high", "missing"): [{seconds, isDefault}], and the place of
    // the value in force among them (-1 when not known).
    Q_INVOKABLE QVariantList timeChoices(const QString &kind) const;
    Q_INVOKABLE int timeIndex(const QString &kind) const;

signals:
    void changed();

private slots:
    void onAnswered(const QString &subject, const lm::ui::ThresholdAnswer &answer);
    void onHistory(const QString &subject, bool ok, const QList<lm::ui::ThresholdHistoryEntry> &entries);

private:
    // The facts of the open patient from the newest Alerts() answer; false when not known.
    bool patient(PatientAlertFacts *out) const;
    void finishEdit();
    bool timeFacts(const QString &kind, AlertTimeRange *range, int *current) const;

    Backend *m_data;
    AlertsBackend *m_alerts;
    QString m_unit;
    QLocale m_locale;
    QString m_subject;
    QString m_state;
    ChallengeNeed m_need;
    ThresholdMessage m_message;
    // The edit on its way, between the first answer and the challenge: whole mg/dL. The
    // values TYPED AGAIN are never kept: confirmChallenge() sends them and forgets them.
    int m_pendingL, m_pendingH;
    QList<ThresholdHistoryEntry> m_history;
    QString m_historyState;
};

} // namespace ui
} // namespace lm

#endif
