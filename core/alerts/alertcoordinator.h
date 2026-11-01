// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTCOORDINATOR_H
#define LM_ALERTCOORDINATOR_H

// The adapter between the service and the alert engine.
//
// WHAT IT IS FOR. The engine is pure: it knows no store, no scheduler, no journal and no
// notification system. The service, on the other hand, knows nothing about alerts. This class sits
// between them and does the small amount of plumbing that neither side should: it listens to the
// service's events (new readings stored, staleness changed, a gap found), reads the newest
// `current` reading from the store, passes both and the clock to the engine, and carries out what
// the engine answers: journal lines are written (and retried when the journal is busy), and
// notifications go to a port.
//
// PATTERN. Observer plus ports and adapters, like the rest of the service: it is a
// ServiceEventSink (serviceevents.h), and everything it touches is behind a small interface
// (AlertReadingSource, AlertNotifier, Clock), so a test drives it with fakes and a manual clock and
// the phone, the PC host and the tests only differ in the ports.
//
// WHAT IT DOES NOT DO. It does not decide anything about alerts (the engine does), does not build
// notification texts (the notifier does, from the effect's Subject, slot, level and state), and
// has no timer: the service calls tick() at its wake-ups and asks nextWakeAt() when to wake
// (SchedulerHost's way). One thread owns it, the service's main thread.
//
// ASSUMPTIONS. The `DataChanged` event means new `current` readings were stored (ingest of other
// feeds also raises it; reading the newest `current` record again is harmless, the engine ignores a
// reading that is not newer). A journal write that fails is kept and retried at the next tick, the
// alerts themselves never wait for it (REQ-STO-15: alerts do not depend on the store being
// writable). The staleness limit of a Subject is the stale factor times its `current` interval
// given by the wiring.

#include "alerts/alertengine.h"
#include "alerts/silentbook.h"
#include "alerts/thresholdbook.h"
#include "guard/ports.h"
#include "service/serviceevents.h"
#include "store/journal.h"

#include <QHash>
#include <QList>
#include <QUuid>

namespace lm {

class Store;

// Where the newest stored `current` reading of a Subject comes from. The real one reads the store.
class AlertReadingSource
{
public:
    virtual ~AlertReadingSource() {}
    // False when the Subject has no usable `current` reading yet.
    virtual bool newestCurrent(const QUuid &subject, const QString &source, double *value,
                               QString *unit, UtcTime *measuredAt) = 0;
};

class StoreAlertReadings : public AlertReadingSource
{
public:
    explicit StoreAlertReadings(Store *store) : m_store(store) {}     // not owned
    bool newestCurrent(const QUuid &subject, const QString &source, double *value, QString *unit,
                       UtcTime *measuredAt);

private:
    Store *m_store;
};

// Where notifications go. The real one posts system notifications; the
// PC host writes lines. The effect carries the Subject's UUID, the slot, the level word and the
// state, never a value or a name; the text is the notifier's to build.
class AlertNotifier
{
public:
    virtual ~AlertNotifier() {}
    // Kinds Notify, Remind, Update, Withdraw and Interruption (not Journal).
    virtual void deliver(const AlertEffect &effect) = 0;
};

// What the coordinator needs to know about one Subject from the configuration.
struct AlertSubjectSettings
{
    AlertSubjectSettings() : currentIntervalMs(60 * 1000), staleFactor(3), persistenceLowMs(0),
                             persistenceHighMs(15 * kAlertMinuteMs), missingLimitMs(15 * kAlertMinuteMs) {}

    QString currentSource;          // the source tag of the `current` feed, e.g. "libre.current"
    qint64 currentIntervalMs;       // the connection's nominal `current` interval
    int staleFactor;                // kStaleFactor
    qint64 persistenceLowMs;
    qint64 persistenceHighMs;
    qint64 missingLimitMs;
};

class AlertCoordinator : public ServiceEventSink
{
public:
    // Nothing is owned; all must outlive the coordinator. `notifier` may be null (then only the
    // journal is written, as on a build without a notification port).
    AlertCoordinator(Journal *journal, AlertReadingSource *readings, AlertNotifier *notifier,
                     Clock *clock, const AlertSettings &settings = AlertSettings());

    // Registers a Subject before start().
    void addSubject(const QUuid &subject, const AlertSubjectSettings &settings);
    // Replaces the settings of a registered Subject, also after start: the configuration file carries the Subject's persistence and missing-data
    // times and the service re-reads it at ConfigChanged without a restart. The engine judges the
    // newest reading again only when a value really changed (AlertEngine::setSubject); an unknown
    // Subject is ignored. Additive: nothing else in this class changes.
    void updateSubjectSettings(const QUuid &subject, const AlertSubjectSettings &settings);

    // Rebuilds the threshold book and the engine from the journal, registers the Subjects, feeds
    // the first staleness information and any gap found by the lifecycle. Call once, after the
    // journal is open and the Subjects are added.
    void start();

    // Time passes: the engine's due events, and the retry of journal lines that could not be
    // written. Cheap when nothing is due.
    void tick();
    // The next time tick() has work to do; invalid when none.
    UtcTime nextWakeAt() const;

    // ServiceEventSink
    void dataChanged(const QUuid &subject);
    void stalenessChanged(const SubjectStatus &status);
    void subjectStateChanged(const SubjectStatus &status);
    void gapDetected(const UtcTime &from, const UtcTime &to);

    // The user's actions, for the service's D-Bus methods. Each returns the engine's result.
    ActionResult acknowledge(const QUuid &subject, AlertSlot slot);
    ActionResult snooze(const QUuid &subject, AlertSlot slot, int minutes);
    // `l` and `h` in whole mg/dL (convert an mmol/L edit with mmolEditToMgDl first). A change
    // carries no comment. `details` (may be null) gets the whole answer of the book: the
    // offending boundary, the allowed ranges and the standard values a screen needs to explain a
    // refusal in words. `details->result` is the returned word; a Subject the service
    // does not follow is `UnknownSubject`, a journal that could not take the line is `NotSaved`
    // (nothing changed in either case).
    EditResult setThresholds(const QUuid &subject, int l, int h, const Confirmation &confirmation,
                             EditOutcome *details = 0);

    // Silent mode. `minutes`: > 0 for that long (the offered durations are 60 and 240), 0 for
    // without limit, kSilentUntilTomorrow for the local-time boundary of SilentBook. False for an
    // unknown Subject or a length that cannot be one (more than a year). Turning the mode ON emits
    // one Update with `silent` for each active unacknowledged alert of the Subject so that a sounding
    // tone stops; a mode that is already on and is set again (longer or shorter) emits nothing, as
    // nothing changes for the ear. The mode is journaled (kind `silent`).
    bool setSilent(const QUuid &subject, int minutes);
    // Turns it off before its end. False when the Subject was not silent.
    bool endSilent(const QUuid &subject);
    bool isSilent(const QUuid &subject) const;
    // The end of a timed mode; invalid for a mode without limit or when the Subject is not silent.
    UtcTime silentUntil(const QUuid &subject) const { return m_silent.until(subject); }
    // For Alerts() and the resident notice: is a mode on, with or without an end.
    bool silentOn(const QUuid &subject) const { return m_silent.isOn(subject); }

    // Reading for Alerts() and the UI.
    QVector<AlertView> alerts() const { return m_engine.alerts(); }
    Thresholds thresholds(const QUuid &subject) const { return m_book.thresholdsFor(subject); }
    QVector<ThresholdChange> thresholdHistory(const QUuid &subject) const { return m_book.history(subject); }

    // Journal lines waiting for a retry (for tests and the status).
    int pendingJournalLines() const { return m_unwritten.size(); }

private:
    struct SubjectEntry
    {
        AlertSubjectSettings settings;
        StalenessInfo info;
        bool hasInfo;
    };

    SubjectAlertConfig configFor(const QUuid &subject, const AlertSubjectSettings &s) const;
    void feedStaleness(const QUuid &subject, const SubjectStatus &status);
    void feedReading(const QUuid &subject);
    void carryOut(const AlertEffects &effects);
    void writeLines(const AlertEffects &effects);
    void writeEntry(const JournalEntry &entry);

    Journal *m_journal;
    AlertReadingSource *m_readings;
    AlertNotifier *m_notifier;
    Clock *m_clock;
    AlertEngine m_engine;
    ThresholdBook m_book;
    SilentBook m_silent;
    AlertSettings m_settings;
    QHash<QUuid, SubjectEntry> m_subjects;
    QList<QUuid> m_order;                   // registration order, for a stable start
    QVector<JournalEntry> m_unwritten;
    UtcTime m_gapFrom, m_gapTo;
    bool m_started;
};

} // namespace lm

#endif
