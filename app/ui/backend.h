// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_BACKEND_H
#define LM_UI_BACKEND_H

// What the screens get their facts from. One abstract class with
// two implementations: the real one (the monitoring service over D-Bus plus the store, later)
// and a scripted one for development builds (fakebackend.h), so the whole interface can run
// on the emulator with no service, no credentials and no source.
//
// Pattern: a "snapshot" interface. The backend hands out plain value objects describing the
// current situation and emits changed() when something moved; it never hands out live
// objects, so the models are simple and a test can build any situation by filling a struct.
// Nothing here decides a state, a band or a colour (REQ-UI-07): the models do that from these
// facts.

#include "plotreduce.h"
#include "sensorview.h"
#include "subjectstate.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

namespace lm {
namespace ui {

struct SubjectSnapshot
{
    SubjectSnapshot() : hasReading(false), measuredAtMs(0), value(0), hasTrend(false),
                        trendCode(0), firstDataMs(0), reportProposed(false) {}

    QString id;                 // the Subject's opaque UUID
    QString name;               // personal data: shown on screen only
    QString label;              // up to 3 characters for the cover, may be empty
    QString connection;         // for the buttons
    // The service's report for the Subject (Status()): state, cause, stale, staleCause,
    // staleDetail, staleSinceAgeS. `ageS` and `band` are filled by the model.
    SubjectInput report;
    // The newest `current` reading in the store, in its stored unit.
    bool hasReading;
    qint64 measuredAtMs;
    double value;
    QString unit;               // "mg/dL" or "mmol/L"
    // The source's trend code of THAT reading, in memory only.
    bool hasTrend;
    int trendCode;
    // For the plot, in mg/dL, sorted by time.
    QVector<PlotPoint> current;
    QVector<PlotPoint> history;
    QVector<QPair<qint64, qint64> > serviceGaps;
    qint64 firstDataMs;         // the oldest stored reading, 0 = none
    // The service proposes a diagnostic report for this Subject's connection (the second consecutive
    // guard kill, REQ-GRD-15); stays until the user writes or declines it.
    bool reportProposed;
    // The sensor the patient wears, from the store read-only (sensorview.h, REQ-SEN-01).
    SensorFacts sensor;
};

// ServiceOlder: the service answers but is another build than this
// window (its Status() names another version or none, or it does not know a method this window calls).
// It still monitors with its own, older behaviour; the strip asks the user to stop and start it, and the
// window never restarts it by itself.
// ServiceNewer: the service's version is NEWER than this window's (the window
// was left open across an upgrade, after the service replaced itself): the strip asks the user to
// close and reopen the application; there is nothing to restart.
enum class ServiceStatus { Connecting, Running, Starting, Stopped, AwaitingApproval, CallFailed, ServiceOlder,
                           ServiceNewer };

// One change of a patient's low or high limit, as the journal keeps it (journal kind `threshold`,
//): when, which limit, the new and the previous value in whole
// mg/dL. There is no comment: the note on a threshold change was removed, and the one an entry
// written by an older version may still hold is not carried.
struct ThresholdHistoryEntry
{
    ThresholdHistoryEntry() : timeMs(0), value(0), previous(0) {}
    qint64 timeMs;
    QString boundary;           // "L" or "H" (internal identifiers, never shown)
    int value, previous;
};

class Backend : public QObject
{
    Q_OBJECT
public:
    explicit Backend(QObject *parent = 0) : QObject(parent) {}

    virtual QList<SubjectSnapshot> subjects() const = 0;
    virtual ServiceStatus serviceStatus() const = 0;
    virtual qint64 lastHeartbeatMs() const { return 0; }
    // The clock. A method so a test or a scripted backend can supply its own time.
    virtual qint64 nowMs() const { return QDateTime::currentMSecsSinceEpoch(); }

    // The buttons. Each is a request; the outcome arrives as changed().
    virtual void retry(const QString &subjectId) = 0;
    virtual void reenable(const QString &subjectId) = 0;
    // The user wrote or declined the proposed report (REQ-GRD-15). The default does nothing: a
    // source without the concept (or a service without the method) is not an error.
    virtual void reportHandled(const QString &/*subjectId*/) {}
    virtual void startMonitoring() = 0;
    // The user asked to stop the monitoring service. Offered ONLY while the service is an older build
    // (ServiceOlder): the strip tells the user to stop and start it, and this is the
    // "Stop" half; the strip then shows Stopped with its Start button. The window never calls it by itself.
    // The default does nothing (the scripted source has no older service).
    virtual void stopMonitoring() {}
    virtual void askAgain() = 0;
    // The history of a patient's threshold edits, read from the journal READ-ONLY ( no method of
    // the service). The answer is thresholdHistoryLoaded(), oldest first; `ok` false when the journal
    // could not be read. The default answers nothing (a source without a journal).
    virtual void requestThresholdHistory(const QString &/*subjectId*/) {}

signals:
    void changed();
    void thresholdHistoryLoaded(const QString &subjectId, bool ok, const QList<lm::ui::ThresholdHistoryEntry> &entries);
};

} // namespace ui
} // namespace lm

#endif
