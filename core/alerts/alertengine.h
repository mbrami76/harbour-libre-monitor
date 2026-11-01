// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTENGINE_H
#define LM_ALERTENGINE_H

// The alert engine (REQ-ALR-01 to 03, REQ-THR-01,
// REQ-SEN-03, REQ-SCH-06 and 07, REQ-DAT-12).
//
// WHAT IT DOES. For each Subject it keeps three slots: low (L or LL), high (H or HH) and missing
// data. A slot is normal, pending, active, acknowledged or snoozed. It is fed with newly stored
// `current` readings ( the graph feed never raises an alert), with the scheduler's staleness
// information, with the clock, and with the user's actions (acknowledge, snooze, a threshold
// change). It answers with a list of EFFECTS: journal entries to write, notifications to raise,
// repeat, replace or take down, and the interruption notice.
//
// PATTERN. Pure, in the style of the guard and the scheduler: no clock of its own, no
// Qt event loop, no I/O, no hidden randomness. Every call carries "now", and the effects say what
// the caller must do; the caller (the service, through a small adapter class in)
// does the writing and the notifying. A test therefore needs only a manual clock.
//
// TICK-RHYTHM INDEPENDENCE (asked the same of the guard). Everything that is due by time
// (a pending alert becoming active, a reminder, the end of a snooze, a missing-data limit) is kept
// as a DUE TIME and handled in time order when the clock is advanced, with the logical time of the
// event, not the time of the tick that noticed it. The caller can read the next due time with
// nextWakeup() and tick exactly then; a coarser tick makes the same decisions late, never
// different ones, and a reminder that was due several times in one coarse step is raised once.
//
// WHAT NO SETTING CAN DO. LL and HH are active the moment a reading is beyond the threshold, are
// never snoozed, and an acknowledgement only pauses their reminders for 15 minutes (REQ-THR-01).
// AlertSettings has no field that could change this.
//
// PRIVACY. An effect carries the Subject's opaque UUID, the slot, the level and the state. It never
// carries a glucose value or a name (REQ-UI-14); the reading stays in the
// store.
//
// ASSUMPTIONS the engine relies on: the thresholds are
// whole mg/dL; the scheduler's cause `sensor-warm-up` is how the engine learns that a
// warm-up is running; the newest stored
// `current` reading is the only input for glucose alerts.

#include "alerts/alertsettings.h"
#include "alerts/alerttypes.h"
#include "alerts/silentbook.h"
#include "alerts/silentbook.h"
#include "alerts/thresholdbook.h"
#include "alerts/thresholds.h"
#include "time/utctime.h"

#include <QMap>
#include <QString>
#include <QUuid>
#include <QVector>

namespace lm {

// One line of the journal kind `alert`: a transition of one slot of one Subject. Carries what
// REQ-ALR-05 needs ("what were the thresholds when this fired": `thresholdsId`) and enough to
// rebuild the slot at the next start ("Transitions across restarts").
// It carries NO glucose value; `measuredAt` points at the reading in the store.
struct AlertRecord
{
    AlertRecord() : slot(AlertSlot::Low), type(AlertType::L), state(AlertState::Normal),
                    from(AlertState::Normal), acknowledged(false), seq(0) {}

    QUuid id;               // the journal entry's own id
    QUuid subject;
    QUuid episode;          // one id from the first non-normal state to the clearing; stable across restarts
    AlertSlot slot;
    AlertType type;         // the level the slot has after this transition
    AlertState state;       // the state after it (Cleared and Normal records end an episode)
    AlertState from;
    QString event;          // pending, active, reverted, escalated, deescalated, acknowledged, snoozed,
                            // snooze-ended, re-alerted, cleared
    UtcTime time;           // the engine's logical time of the transition (wall clock)
    UtcTime measuredAt;     // measurement time of the reading that caused it, if a reading did
    UtcTime since;          // pending: when the condition began (the persistence runs from here)
    UtcTime until;          // snoozed: when the snooze ends; acknowledged LL/HH: when it re-alerts
    QUuid thresholdsId;     // the threshold history entry in force (null: the global defaults)
    // Facts the notification texts need, kept in the
    // journal so that a restart rebuilds them. Times are the measurement times of readings (a
    // pending H: when the pending began), never the time a line was written.
    UtcTime levelSince;     // first reading beyond the limit of THIS level in this stretch
    UtcTime episodeSince;   // first reading beyond the limit of the episode's first level
    bool acknowledged;      // the user acknowledged or snoozed at some point in the episode
    QString cause;          // missing data: the cause of REQ-SCH-07; cleared: why (recovered, reading, warm-up)
    qint64 seq;             // orders records with the same wall time
};

// What the caller has to do. The list is ordered; a transition produces its Journal effect first.
struct AlertEffect
{
    enum Kind {
        Journal,        // append `record` to the service journal (kind `alert`)
        Notify,         // raise the notification for the slot (new, escalated, re-alerted, snooze ended, restored)
        Remind,         // raise it again: the alert is still active and unacknowledged
        Update,         // replace the notification in place without sound (acknowledged, level lowered, cause changed)
        Withdraw,       // take the notification down (cleared, snoozed, suppressed)
        Interruption    // the notice "monitoring was interrupted from X to Y" (REQ-ALR-07); a notice, not an alert
    };

    AlertEffect() : kind(Journal), slot(AlertSlot::Low), type(AlertType::L), state(AlertState::Normal),
                    reminderNumber(0), limit(0), hasLimit(false), neverAcknowledged(false),
                    silent(false) {}

    Kind kind;
    QUuid subject;
    AlertSlot slot;
    AlertType type;
    AlertState state;
    QUuid episode;
    UtcTime at;
    QString cause;              // missing data: the staleness cause shown in the message
    int reminderNumber;         // Remind: 1 for the first repeat
    AlertRecord record;         // Journal
    UtcTime gapFrom, gapTo;     // Interruption

    // ---- facts for the texts (Notify, Remind, Update, Withdraw) -------------------------
    // The notifier prints "below {limit} since {levelSince} ({minutes} min)" from these and from
    // its own clock. No glucose value is ever among them: a boundary is a setting, a time is the
    // time of a reading.
    UtcTime levelSince;         // first reading beyond the limit of the alert's CURRENT level in this
                                // stretch (a pending H: when the pending began)
    UtcTime episodeSince;       // the same for the first level of the slot's episode
    int limit;                  // the boundary in force, whole mg/dL: the Subject's L or H, the fixed LL
    bool hasLimit;              //  and HH; false for MISSING (a boundary is a setting, not a reading)
    UtcTime newestReadingAt;    // MISSING: the measurement time of the newest reading ("No reading since")
    // Withdraw only:
    bool neverAcknowledged;     // the alert cleared and nobody acknowledged (or snoozed) it in the
                                // episode: the notifier leaves the page-only "ended" entry. Always false for MISSING, which never gets one.
    UtcTime clearedAt;          // when the alert was cleared
    // Notify, Remind, Update: the Subject is in silent mode at this moment: the
    // service posts without feedback hint. Always false for MISSING.
    bool silent;
};

typedef QVector<AlertEffect> AlertEffects;

// What the scheduler knows about the freshness of a Subject (SubjectStatus). The engine does not
// read the store: the service passes the newest stored `current` reading's measurement time.
struct StalenessInfo
{
    StalenessInfo() : staleLimitMs(0) {}

    UtcTime lastReadingAt;      // newest stored `current` record, invalid = none (REQ-SCH-06)
    qint64 staleLimitMs;        // the display staleness limit (3 times the interval); 0 = keep the last
    QString cause;              // StaleCause of scheduler/schedulemath.h, empty when not stale
};

// The per-Subject inputs that come from the configuration and the threshold history.
struct SubjectAlertConfig
{
    SubjectAlertConfig()
        : persistenceLowMs(0), persistenceHighMs(15 * kAlertMinuteMs), missingLimitMs(15 * kAlertMinuteMs) {}

    Thresholds thresholds;      // LL and HH global, L and H of the Subject (ThresholdBook::thresholdsFor)
    QUuid thresholdsId;         // ThresholdBook::idInForce
    qint64 persistenceLowMs;    // editable, at most 15 min (the engine clamps)
    qint64 persistenceHighMs;   // editable
    qint64 missingLimitMs;      // 5 to 60 min (the engine clamps)
};

enum class ActionResult {
    Done,
    UnknownSubject,
    NotActive,          // nothing to acknowledge or snooze in that slot
    AlreadyAcknowledged,
    NotSnoozable,       // LL and HH have no snooze (REQ-THR-01)
    BadDuration         // not one of 15, 30, 60, 120 minutes
};

QString actionResultName(ActionResult r);

struct ActionOutcome
{
    ActionOutcome() : result(ActionResult::Done) {}

    ActionResult result;
    AlertEffects effects;
};

// A read-only picture of one slot for the service's `Alerts()` and the UI.
struct AlertView
{
    AlertView() : slot(AlertSlot::Low), type(AlertType::L), state(AlertState::Normal),
                  acknowledged(false), limit(0), hasLimit(false) {}

    QUuid subject;
    AlertSlot slot;
    AlertType type;
    AlertState state;
    QUuid episode;
    UtcTime since;
    UtcTime snoozeUntil;
    QString cause;
    // For the resident notice and Alerts() (REQ-ALR-06): the service filters and
    // translates; nothing here is a name or a value.
    UtcTime levelSince;
    UtcTime episodeSince;
    bool acknowledged;          // state == Acknowledged
    int limit;
    bool hasLimit;
    UtcTime newestReadingAt;    // MISSING only
};

class AlertEngine
{
public:
    explicit AlertEngine(const AlertSettings &settings = AlertSettings(),
                         const IdFactory &ids = IdFactory());

    // Registers a Subject, or applies a new configuration to a known one. A new Subject starts to
    // be monitored at `now` (the missing-data clock starts there when it has no reading yet). A
    // change of the thresholds or of a persistence time re-judges the newest reading at once
    // (: a threshold change during a pending and an active alert).
    AlertEffects setSubject(const QUuid &subject, const SubjectAlertConfig &config, const UtcTime &now);

    // A newly stored `current` reading. `unit` is the stored unit. A reading that is not newer
    // than the newest one already seen is ignored (alerts follow the newest reading only).
    AlertEffects onReading(const QUuid &subject, double value, const QString &unit,
                           const UtcTime &measuredAt, const UtcTime &now);

    // The scheduler's staleness information changed (or is repeated). Drives the missing-data
    // slot and the warm-up suppression.
    AlertEffects onStaleness(const QUuid &subject, const StalenessInfo &info, const UtcTime &now);

    // Time passes. Handles everything that has become due, in time order.
    AlertEffects tick(const UtcTime &now);

    ActionOutcome acknowledge(const QUuid &subject, AlertSlot slot, const UtcTime &now);
    ActionOutcome snooze(const QUuid &subject, AlertSlot slot, int minutes, const UtcTime &now);

    // Silent mode. The engine only READS the book (it is not owned and may be null): every
    // Notify, Remind and Update of a glucose slot carries `silent` = the Subject is silent at that
    // moment. Missing data is never silent.
    void setSilentBook(const SilentBook *book) { m_silentBook = book; }

    // The Subject's silent mode has just been turned on (the caller has already told the book).
    // Emits one `Update` with `silent` = true for every ACTIVE, unacknowledged glucose slot of the
    // Subject, so that the notifier can replace the notification without a feedback hint and a
    // sounding tone stops. Acknowledged, snoozed and pending slots and missing data
    // are unaffected. Nothing is emitted when a mode ends: the next reminder simply sounds.
    AlertEffects onSilentStarted(const QUuid &subject, const UtcTime &now);

    // The service has started after a gap (REQ-ALR-07, REQ-SCH-10): produces the interruption
    // notice when the gap is longer than the limit.
    AlertEffects onServiceStart(const UtcTime &gapFrom, const UtcTime &gapTo, const UtcTime &now);

    // Rebuilds the slots from the journal's `alert` records after a restart: the last record per
    // (Subject, slot) wins. Call it after setSubject for every Subject. An active alert stays
    // active and is notified again; the counters of hysteresis start from zero.
    AlertEffects restore(const QVector<AlertRecord> &records, const UtcTime &now);

    // The next time something is due, or an invalid time when nothing is. The service arms its
    // timer with it.
    UtcTime nextWakeup() const;

    // Every active, acknowledged and snoozed alert, ordered by Subject and slot:
    // the list of the resident notice and of Alerts(). A pending alert is not in it: nobody has
    // been told yet.
    QVector<AlertView> alerts() const;
    AlertView view(const QUuid &subject, AlertSlot slot) const;

    const AlertSettings &settings() const { return m_settings; }

private:
    struct SlotState
    {
        SlotState() : state(AlertState::Normal), type(AlertType::L), sinceMs(-1), pendingDueMs(-1),
                      nextReminderMs(-1), snoozeUntilMs(-1), reAlertAtMs(-1), reminders(0),
                      clearCount(0), levelSinceMs(-1), episodeSinceMs(-1), acked(false) {}

        AlertState state;
        AlertType type;
        QUuid episode;
        qint64 sinceMs;         // pending: start of the condition; otherwise when the slot became active
        qint64 pendingDueMs;    // pending: when the persistence ends, -1 = waiting for a fresh reading
        qint64 nextReminderMs;
        qint64 snoozeUntilMs;
        qint64 reAlertAtMs;
        int reminders;
        int clearCount;         // consecutive readings back inside by the margin
        qint64 levelSinceMs;    // see AlertEffect::levelSince
        qint64 episodeSinceMs;  // see AlertEffect::episodeSince; also the since of the lower level
        bool acked;             // acknowledged or snoozed in this episode
        QString cause;
    };

    struct SubjectState
    {
        SubjectState() : hasReading(false), lastMgDl(0), lastMeasuredMs(-1), infoReadingMs(-1),
                         monitoringSinceMs(0), floorMs(-1), staleLimitMs(0) {}

        SubjectAlertConfig config;
        SlotState low, high, missing;
        bool hasReading;
        double lastMgDl;
        qint64 lastMeasuredMs;      // newest reading seen by onReading
        qint64 infoReadingMs;       // newest reading the scheduler reported, -1 none
        qint64 monitoringSinceMs;
        qint64 floorMs;             // the missing-data clock cannot start before this (warm-up end)
        qint64 staleLimitMs;
        QString staleCause;
    };

    typedef QMap<QUuid, SubjectState> SubjectMap;

    SlotState &slotOf(SubjectState &s, AlertSlot slot);
    const SlotState &slotOf(const SubjectState &s, AlertSlot slot) const;
    SubjectAlertConfig sanitized(const SubjectAlertConfig &c) const;

    // Due times.
    bool nextDue(const SubjectState &s, AlertSlot slot, qint64 *when) const;
    qint64 missingBaseMs(const SubjectState &s) const;
    qint64 staleLimitOf(const SubjectState &s) const;
    qint64 persistenceFor(const SubjectState &s, AlertType type) const;
    void advanceTo(qint64 nowMs, AlertEffects *out);
    void fire(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 when, qint64 nowMs,
              AlertEffects *out);

    // Glucose slots.
    bool rawLevel(AlertSlot slot, double mgdl, const Thresholds &t, AlertType *type) const;
    bool holdingLevel(AlertSlot slot, double mgdl, AlertType current, const Thresholds &t,
                      AlertType *holding) const;
    void judge(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 sinceMs, qint64 nowMs,
               AlertEffects *out);

    // Transitions. Each one writes the Journal effect and the notification effect, in that order.
    // `atMs` is the logical time of the transition (the reading's arrival, or the due time).
    void enterPending(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type,
                      qint64 sinceMs, qint64 dueMs, qint64 atMs, AlertEffects *out);
    // `levelSinceMs`: measured time of the first reading beyond the limit of `type`, or -1 to keep
    // the one the slot already has (a snooze that ends, a re-alert, a pending alert that becomes
    // active at the same level).
    void activate(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type,
                  const QString &event, qint64 atMs, qint64 levelSinceMs, AlertEffects *out);
    bool limitOf(const SubjectState &s, AlertType type, int *limit) const;
    void revert(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 atMs, AlertEffects *out);
    void lower(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type, qint64 atMs,
               AlertEffects *out);
    void clear(const QUuid &subject, SubjectState &s, AlertSlot slot, const QString &reason,
               qint64 atMs, AlertEffects *out);
    bool suppressed(const SubjectState &s) const;

    void journal(const QUuid &subject, const SubjectState &s, AlertSlot slot, AlertState from,
                 AlertState to, const QString &event, qint64 atMs, const QString &cause,
                 AlertEffects *out);
    void notification(AlertEffect::Kind kind, const QUuid &subject, const SubjectState &s,
                      AlertSlot slot, qint64 atMs, AlertEffects *out) const;
    static UtcTime timeOf(qint64 ms) { return ms < 0 ? UtcTime() : UtcTime::fromMSecsSinceEpoch(ms); }

    AlertSettings m_settings;
    const SilentBook *m_silentBook;
    IdFactory m_ids;
    SubjectMap m_subjects;
    qint64 m_seq;
};

} // namespace lm

#endif
