// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PURGECONTROLLER_H
#define LM_UI_PURGECONTROLLER_H

// The controller of the purge page (REQ-STO-17): one Subject at a time, an age of 3, 6, 12 or 24 months, the service's estimate, the
// confirmation and the typed-label gate, the progress and the result.
//
// Pattern: a QObject the page binds to, over the MaintenanceBackend port, like StorageController. The
// estimate is the SERVICE's dry run (StartMaintenance "purge-estimate"): it needs the configuration's
// references and connections and the journal, so the window never computes its own. The order the page
// follows and this class enforces:
//   estimate known, something to remove, space "enough", the Subject has a label
//     -> the confirmation (naming the Subject by its label and the age)
//     -> the typed label, compared exactly with the label (purgeview.h) -> purge(typed)
// purge() sends nothing unless every condition holds; the service checks the label again.
// After a failed journal part the same purge can be repeated with the text already typed.
// Only answers of the kinds "purge-estimate" and "purge" for this page's Subject are read: a compaction's
// result is never shown here, and the storage page shows none of these.
// Nothing here polls; nothing is logged; the label and the typed text are never written anywhere.

#include "maintenancebackend.h"
#include "purgeview.h"

#include <QObject>
#include <QVariantList>

namespace lm {
namespace ui {

class PurgeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString subjectId READ subjectId NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // The label the service compares with (the configuration's label, trimmed); "" without one.
    Q_PROPERTY(QString label READ label NOTIFY changed)
    Q_PROPERTY(bool hasLabel READ hasLabel NOTIFY changed)
    Q_PROPERTY(int ageMonths READ ageMonths WRITE setAgeMonths NOTIFY changed)
    Q_PROPERTY(QVariantList ageChoices READ ageChoicesList CONSTANT)
    // "idle", "estimating", "estimated", "starting", "running", "finished"
    Q_PROPERTY(QString stage READ stage NOTIFY changed)
    Q_PROPERTY(bool estimateKnown READ estimateKnown NOTIFY changed)
    Q_PROPERTY(double storeRows READ storeRows NOTIFY changed)
    Q_PROPERTY(double journalEntries READ journalEntries NOTIFY changed)
    Q_PROPERTY(double estimateBytes READ estimateBytes NOTIFY changed)
    Q_PROPERTY(double neededBytes READ neededBytes NOTIFY changed)
    Q_PROPERTY(double freeBytes READ freeBytes NOTIFY changed)
    Q_PROPERTY(QString space READ space NOTIFY changed)
    Q_PROPERTY(double missingBytes READ missingBytes NOTIFY changed)
    // True when the confirmation may be opened (and the purge sent once the typed label matches).
    Q_PROPERTY(bool canContinue READ canContinue NOTIFY changed)
    Q_PROPERTY(QString phaseKey READ phaseKey NOTIFY changed)
    Q_PROPERTY(double fraction READ fraction NOTIFY changed)
    Q_PROPERTY(int step READ step NOTIFY changed)
    Q_PROPERTY(int steps READ steps NOTIFY changed)
    // The last answer: a text identifier, whether it was a success, and the result's counts and bytes.
    Q_PROPERTY(QString resultKey READ resultKey NOTIFY changed)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY changed)
    Q_PROPERTY(double removedStore READ removedStore NOTIFY changed)
    Q_PROPERTY(double removedJournal READ removedJournal NOTIFY changed)
    Q_PROPERTY(double bytesFreed READ bytesFreed NOTIFY changed)
    Q_PROPERTY(double resultMissingBytes READ resultMissingBytes NOTIFY changed)
    // After a failed journal part: the same purge can be sent again.
    Q_PROPERTY(bool canRepeat READ canRepeat NOTIFY changed)

public:
    explicit PurgeController(QObject *parent = 0);

    void setBackend(MaintenanceBackend *backend);

    // The Subject of the page: its id, its name (shown) and the label as the configuration holds it.
    // Asks for the estimate at once.
    Q_INVOKABLE void open(const QString &subjectId, const QString &name, const QString &configuredLabel);
    Q_INVOKABLE void refreshEstimate();
    Q_INVOKABLE bool typedMatches(const QString &typed) const { return typedLabelMatches(typed, m_label); }
    // The label the service would compare with for a configured label (the list of patients shows it).
    Q_INVOKABLE QString labelOf(const QString &configuredLabel) const { return purgeLabelOf(configuredLabel); }
    // The final button: sends the purge with the typed text unchanged. False (nothing sent) unless
    // canContinue() and the text matches the label.
    Q_INVOKABLE bool purge(const QString &typed);
    Q_INVOKABLE bool repeat();
    Q_INVOKABLE void cancel();

    QString subjectId() const { return m_subject; }
    QString name() const { return m_name; }
    QString label() const { return m_label; }
    bool hasLabel() const { return !m_label.isEmpty(); }
    int ageMonths() const { return m_age; }
    void setAgeMonths(int months);
    QVariantList ageChoicesList() const;
    QString stage() const { return m_stage; }
    bool estimateKnown() const { return m_estimate.known; }
    double storeRows() const { return double(m_estimate.storeRows); }
    double journalEntries() const { return double(m_estimate.journalEntries); }
    double estimateBytes() const { return double(m_estimate.bytesEstimated); }
    double neededBytes() const { return double(m_estimate.neededBytes); }
    double freeBytes() const { return double(m_estimate.freeBytes); }
    QString space() const { return m_estimate.known ? m_estimate.space : QString(); }
    double missingBytes() const { return double(m_estimate.missingBytes); }
    bool canContinue() const;
    QString phaseKey() const { return m_phaseKey; }
    double fraction() const { return m_fraction; }
    int step() const { return m_step; }
    int steps() const { return m_steps; }
    QString resultKey() const { return m_resultKey; }
    bool resultOk() const { return m_resultOk; }
    double removedStore() const { return double(m_result.removedStore); }
    double removedJournal() const { return double(m_result.removedJournal); }
    double bytesFreed() const { return double(m_result.bytesFreed); }
    double resultMissingBytes() const { return double(m_result.missingBytes); }
    bool canRepeat() const;

signals:
    void changed();
    // A purge ended (whatever the outcome): the window reloads what it shows of this Subject.
    void purgeFinished(const QString &subjectId);

private slots:
    void onAnswered(bool ok, const QString &kind, const QString &result);
    void onProgress(const QString &kind, const QString &phase, int step, int steps, qlonglong done,
                    qlonglong total, double fraction);
    void onFinished(const QString &kind, bool ok, const QString &errorClass, const QString &message,
                    const QString &cancelReason, const QString &detailsJson);

private:
    bool busy() const;
    void send(const QString &kind, const QString &parameters);

    MaintenanceBackend *m_backend;
    QString m_subject, m_name, m_label;
    int m_age;
    QString m_stage;
    QString m_pendingKind;      // the kind sent and not answered yet
    PurgeFacts m_estimate, m_result;
    QString m_lastTyped;        // for the repeat only, in memory
    QString m_phaseKey;
    double m_fraction;
    int m_step, m_steps;
    QString m_resultKey;
    bool m_resultOk;
};

} // namespace ui
} // namespace lm

#endif
