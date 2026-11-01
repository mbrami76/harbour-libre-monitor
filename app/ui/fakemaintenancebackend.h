// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_FAKEMAINTENANCEBACKEND_H
#define LM_UI_FAKEMAINTENANCEBACKEND_H

// The scripted MaintenanceBackend (maintenancebackend.h) for development builds (the emulator without a
// service) and the host tests. Invented sizes; a compaction plays the service's phases (checking-space,
// compacting, finalizing) and ends well, or is refused as the service refuses when the scripted space
// check says "short" or "unknown" (REQ-STO-16); the
// diagnostic report is a synthetic text in the service's shape, with every measurement already "number"
// or the error word a test chooses. Nothing is written anywhere by this class.

#include "maintenancebackend.h"

#include <QStringList>

namespace lm {
namespace ui {

class FakeMaintenanceBackend : public MaintenanceBackend
{
    Q_OBJECT
public:
    explicit FakeMaintenanceBackend(QObject *parent = 0);

    // Tests: no timers; the test plays the service with the emit* functions below.
    void setAutoPlay(bool on) { m_autoPlay = on; }
    void setFacts(const StorageFacts &facts) { m_facts = facts; }
    void setStartAnswer(const QString &word) { m_startAnswer = word; }
    // The answer of DiagnosticReport: a synthetic report (empty word) or {"error": word}.
    void setReportError(const QString &word) { m_reportError = word; }
    void setRunning(bool running, const QString &kind);
    // The scripted space check (development builds: LM_DEV_STORAGE; tests): "" computes it from the invented
    // sizes, "short" or "unknown" forces that answer, "low" also marks the weekly check as low.
    void setSpaceScript(const QString &script);
    // The scripted purge (development builds: LM_DEV_PURGE; tests): "" an estimate with rows and enough
    // space and a purge that ends well; "short" or "unknown" the space check; "nothing" an estimate with
    // nothing to remove; "repeat" a purge whose journal part fails once (io, store_part_done), then ends
    // well when repeated; "slow" a purge whose phases last 4 s each (for a screenshot of the progress).
    void setPurgeScript(const QString &script) { m_purgeScript = script; }

    void requestStorageFacts(int windowMonths);
    void startMaintenance(const QString &kind, const QString &parametersJson);
    void cancelMaintenance();
    bool maintenanceRunning() const { return m_running; }
    QString maintenanceKind() const { return m_kind; }
    void requestDiagnosticReport(const QString &connection);
    void reportHandled(const QString &connection);

    // What was asked, for the tests.
    QStringList calls;              // "start:<kind>:<json>", "cancel", "report:<c>", "handled:<c>", "facts:<m>"

    static QString syntheticReport();

private:
    void playStep();
    void playPurge();
    QString purgeDetails(bool result, bool storePartDone) const;

    bool m_autoPlay;
    StorageFacts m_facts;
    QString m_startAnswer, m_reportError, m_spaceScript, m_purgeScript;
    bool m_purgeJournalFailed = false;
    bool m_running;
    QString m_kind, m_parameters;
    int m_stepDone;
};

} // namespace ui
} // namespace lm

#endif
