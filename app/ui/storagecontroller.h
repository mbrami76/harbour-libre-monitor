// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_STORAGECONTROLLER_H
#define LM_UI_STORAGECONTROLLER_H

// The controller of the storage and maintenance page (REQ-STO-02, 05, 06, 07, 13,
// 15, 16) and of the two overview lines that lead to it
// (maintenance running, little space left).
//
// Pattern: a QObject the page binds to, over the MaintenanceBackend port. There is no export
// and no import: the page shows the facts and the estimate for the chosen full-resolution window, and
// compaction, which deletes permanently, is started after ONE confirmation on the page (REQ-STO-05). The
// page's dialog shows the estimate and the plain words of REQ-STO-05 and calls compact() only when the
// user accepts it; compact() is the "confirmed" request.
//
// compact() is refused (nothing sent) while a run is going on, before the estimate is known, when it
// would remove nothing, and when the core's space check already says the rebuild would not fit or the free
// space cannot be read: the service would refuse those too (REQ-STO-16, no override), and the page says
// why instead of offering a run that cannot happen. The service checks again before deleting anything.
// Nothing here polls: the facts are read when the page opens, when the window is changed and after a run
// ended; the progress comes from the service's signals.

#include "maintenancebackend.h"
#include "storageview.h"

#include <QObject>
#include <QVariantList>

namespace lm {
namespace ui {

class StorageController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool factsOk READ factsOk NOTIFY factsChanged)
    Q_PROPERTY(bool factsLoaded READ factsLoaded NOTIFY factsChanged)
    Q_PROPERTY(double storeBytes READ storeBytes NOTIFY factsChanged)
    Q_PROPERTY(double journalBytes READ journalBytes NOTIFY factsChanged)
    Q_PROPERTY(double freeBytes READ freeBytes NOTIFY factsChanged)
    Q_PROPERTY(bool checkKnown READ checkKnown NOTIFY factsChanged)
    Q_PROPERTY(double checkTimeMs READ checkTimeMs NOTIFY factsChanged)
    Q_PROPERTY(bool low READ low NOTIFY factsChanged)
    Q_PROPERTY(bool estimateKnown READ estimateKnown NOTIFY factsChanged)
    Q_PROPERTY(double estimateBytes READ estimateBytes NOTIFY factsChanged)
    Q_PROPERTY(double estimateRecords READ estimateRecords NOTIFY factsChanged)
    Q_PROPERTY(double rebuildNeedBytes READ rebuildNeedBytes NOTIFY factsChanged)
    // The core's space check for the estimate (REQ-STO-16): "enough", "short", "unknown", "" before an estimate.
    Q_PROPERTY(QString space READ space NOTIFY factsChanged)
    Q_PROPERTY(double missingBytes READ missingBytes NOTIFY factsChanged)
    // True when Compact can be offered: an estimate that removes something and a rebuild that fits, and no run.
    Q_PROPERTY(bool canCompact READ canCompact NOTIFY stateChanged)
    Q_PROPERTY(int windowMonths READ windowMonths WRITE setWindowMonths NOTIFY factsChanged)
    Q_PROPERTY(QVariantList windowChoices READ windowChoicesList CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(QString runningKind READ runningKind NOTIFY stateChanged)
    Q_PROPERTY(QString phaseKey READ phaseKey NOTIFY stateChanged)
    Q_PROPERTY(double fraction READ fraction NOTIFY stateChanged)
    Q_PROPERTY(int step READ step NOTIFY stateChanged)
    Q_PROPERTY(int steps READ steps NOTIFY stateChanged)
    // The last answer in words: a text identifier, an argument for it (bytes, as a number in text) and
    // whether it was a success. Empty when there is nothing to say.
    Q_PROPERTY(QString resultKey READ resultKey NOTIFY stateChanged)
    Q_PROPERTY(QString resultArg READ resultArg NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)
    // Readings were lost while the store was busy (the warning reading-queue-full): how many.
    Q_PROPERTY(int lostReadings READ lostReadings NOTIFY stateChanged)

public:
    explicit StorageController(QObject *parent = 0);

    void setBackend(MaintenanceBackend *backend);

    bool factsOk() const { return m_facts.ok; }
    bool factsLoaded() const { return m_factsLoaded; }
    double storeBytes() const { return double(m_facts.storeBytes); }
    double journalBytes() const { return double(m_facts.journalBytes); }
    double freeBytes() const { return double(m_facts.freeBytes); }
    bool checkKnown() const { return m_facts.checkKnown; }
    double checkTimeMs() const { return double(m_facts.checkTimeMs); }
    bool low() const { return m_facts.ok && m_facts.checkKnown && m_facts.checkLow; }
    bool estimateKnown() const { return m_facts.ok && m_facts.estimateKnown; }
    double estimateBytes() const { return double(m_facts.estimateBytes); }
    double estimateRecords() const { return double(m_facts.estimateRecords); }
    double rebuildNeedBytes() const { return double(m_facts.rebuildNeedBytes); }
    QString space() const { return estimateKnown() ? m_facts.space : QString(); }
    double missingBytes() const { return double(m_facts.missingBytes); }
    bool canCompact() const;
    int windowMonths() const { return m_windowMonths; }
    void setWindowMonths(int months);
    QVariantList windowChoicesList() const;
    bool running() const;
    QString runningKind() const;
    QString phaseKey() const { return m_phaseKey; }
    double fraction() const { return m_fraction; }
    int step() const { return m_step; }
    int steps() const { return m_steps; }
    QString resultKey() const { return m_resultKey; }
    QString resultArg() const { return m_resultArg; }
    bool resultOk() const { return m_resultOk; }
    int lostReadings() const { return m_lost; }

    // Reads the facts again (the page opened, the application came to the front).
    Q_INVOKABLE void refresh();
    // The user accepted the confirmation of REQ-STO-05: starts the compaction for the chosen window.
    // Refused (false, nothing sent) unless canCompact().
    Q_INVOKABLE bool compact();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void dismissResult();

signals:
    void factsChanged();
    void stateChanged();

private slots:
    void onFacts(const lm::ui::StorageFacts &facts);
    void onAnswered(bool ok, const QString &kind, const QString &result);
    void onProgress(const QString &kind, const QString &phase, int step, int steps, qlonglong done,
                    qlonglong total, double fraction);
    void onFinished(const QString &kind, bool ok, const QString &errorClass, const QString &message,
                    const QString &cancelReason, const QString &detailsJson);
    void onWarning(const QString &kind, const QString &detail);

private:
    void setResult(const QString &key, const QString &arg, bool ok);

    MaintenanceBackend *m_backend;
    StorageFacts m_facts;
    bool m_factsLoaded;
    int m_windowMonths;
    bool m_starting;            // a start was sent and not answered yet
    QString m_phaseKey;
    double m_fraction;
    int m_step, m_steps;
    QString m_resultKey, m_resultArg;
    bool m_resultOk;
    int m_lost;
};

} // namespace ui
} // namespace lm

#endif
