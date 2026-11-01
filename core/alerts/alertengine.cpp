// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/alertengine.h"

#include "alerts/band.h"
#include "scheduler/schedulemath.h"
#include "units/glucoseunits.h"

#include <QPair>

#include <algorithm>
#include <cmath>

namespace lm {

QString actionResultName(ActionResult r)
{
    switch (r) {
    case ActionResult::Done: return QStringLiteral("done");
    case ActionResult::UnknownSubject: return QStringLiteral("unknown-subject");
    case ActionResult::NotActive: return QStringLiteral("not-active");
    case ActionResult::AlreadyAcknowledged: return QStringLiteral("already-acknowledged");
    case ActionResult::NotSnoozable: return QStringLiteral("not-snoozable");
    case ActionResult::BadDuration: return QStringLiteral("bad-duration");
    }
    return QString();
}

namespace {

const AlertSlot kSlots[] = { AlertSlot::Low, AlertSlot::High, AlertSlot::Missing };
const int kSlotCount = 3;

qint64 msOf(const UtcTime &t) { return t.msecsSinceEpoch(); }

} // namespace

AlertEngine::AlertEngine(const AlertSettings &settings, const IdFactory &ids)
    : m_settings(settings),
      m_silentBook(0),
      m_ids(ids ? ids : IdFactory([]() { return QUuid::createUuid(); })),
      m_seq(1)
{
}

AlertEngine::SlotState &AlertEngine::slotOf(SubjectState &s, AlertSlot slot)
{
    return slot == AlertSlot::Low ? s.low : slot == AlertSlot::High ? s.high : s.missing;
}

const AlertEngine::SlotState &AlertEngine::slotOf(const SubjectState &s, AlertSlot slot) const
{
    return slot == AlertSlot::Low ? s.low : slot == AlertSlot::High ? s.high : s.missing;
}

// A configuration is read from a file and from a ledger, so it is not trusted: LL and HH are put
// back to the global values (nothing can move them, REQ-THR-01), an inconsistent set falls back to
// the defaults, and the times are kept inside their allowed ranges. The engine therefore never
// runs with a nonsensical number, whatever it was given.
SubjectAlertConfig AlertEngine::sanitized(const SubjectAlertConfig &c) const
{
    SubjectAlertConfig r = c;
    r.thresholds.ll = kGlobalLL;
    r.thresholds.hh = kGlobalHH;
    if (!r.thresholds.isConsistent())
        r.thresholds = Thresholds();
    r.persistenceLowMs = qBound<qint64>(0, r.persistenceLowMs, m_settings.persistenceLowMaxMs);
    r.persistenceHighMs = qBound<qint64>(0, r.persistenceHighMs, m_settings.persistenceHighMaxMs);
    r.missingLimitMs = qBound(m_settings.missingLimitMinMs, r.missingLimitMs, m_settings.missingLimitMaxMs);
    return r;
}

qint64 AlertEngine::persistenceFor(const SubjectState &s, AlertType type) const
{
    // LL and HH: always zero, by construction, not by a setting (REQ-THR-01).
    switch (type) {
    case AlertType::L: return s.config.persistenceLowMs;
    case AlertType::H: return s.config.persistenceHighMs;
    default: break;
    }
    return 0;
}

qint64 AlertEngine::staleLimitOf(const SubjectState &s) const
{
    return s.staleLimitMs > 0 ? s.staleLimitMs : m_settings.staleLimitDefaultMs;
}

bool AlertEngine::suppressed(const SubjectState &s) const
{
    // The scheduler names the cause of a stale Subject; during a sensor warm-up the cause is
    // `sensor-warm-up` (REQ-SCH-07 cause 3). That is how the engine learns about warm-up (REQ-ALR-03).
    return s.staleCause == StaleCause::warmUp();
}

// Where the missing-data clock starts: the newest reading, but never before the start of
// monitoring in this run (, option B: a gap that is already over
// when the service starts is the interruption notice's business, not a missing-data alert), and
// never before the last moment a warm-up was known to run (a Subject that had no reading during a
// warm-up has not been "missing" since the warm-up ended).
qint64 AlertEngine::missingBaseMs(const SubjectState &s) const
{
    const qint64 newest = std::max(s.lastMeasuredMs, s.infoReadingMs);
    return std::max(std::max(newest, s.monitoringSinceMs), s.floorMs);
}

// ---- due times ----------------------------------------------------------------------------

bool AlertEngine::nextDue(const SubjectState &s, AlertSlot slot, qint64 *when) const
{
    const SlotState &sl = slotOf(s, slot);
    if (slot == AlertSlot::Missing && sl.state == AlertState::Normal) {
        if (suppressed(s))
            return false;
        // "Older than the limit" is strict: the first instant with age > limit is limit + 1 ms.
        *when = missingBaseMs(s) + staleLimitOf(s) + 1;
        return true;
    }
    if (slot == AlertSlot::Missing && sl.state == AlertState::Pending) {
        const qint64 base = missingBaseMs(s);
        *when = std::max(base + staleLimitOf(s) + 1, base + s.config.missingLimitMs + 1);
        return true;
    }
    switch (sl.state) {
    case AlertState::Pending:
        if (sl.pendingDueMs < 0) return false;
        *when = sl.pendingDueMs;
        return true;
    case AlertState::Active:
        if (sl.nextReminderMs < 0) return false;
        *when = sl.nextReminderMs;
        return true;
    case AlertState::Snoozed:
        if (sl.snoozeUntilMs < 0) return false;
        *when = sl.snoozeUntilMs;
        return true;
    case AlertState::Acknowledged:
        if (sl.reAlertAtMs < 0) return false;
        *when = sl.reAlertAtMs;
        return true;
    default:
        break;
    }
    return false;
}

UtcTime AlertEngine::nextWakeup() const
{
    bool found = false;
    qint64 best = 0;
    for (SubjectMap::const_iterator it = m_subjects.constBegin(); it != m_subjects.constEnd(); ++it) {
        for (int i = 0; i < kSlotCount; ++i) {
            qint64 w;
            if (nextDue(it.value(), kSlots[i], &w) && (!found || w < best)) {
                best = w;
                found = true;
            }
        }
    }
    return found ? UtcTime::fromMSecsSinceEpoch(best) : UtcTime();
}

// Handles every due event up to `nowMs`, earliest first, each with its own logical time. This is
// the heart of the tick-rhythm independence: how often the caller ticks only decides how late an
// event is noticed.
void AlertEngine::advanceTo(qint64 nowMs, AlertEffects *out)
{
    // While a warm-up runs, the missing-data clock is held at "now" (see missingBaseMs).
    for (SubjectMap::iterator it = m_subjects.begin(); it != m_subjects.end(); ++it)
        if (suppressed(it.value()))
            it.value().floorMs = std::max(it.value().floorMs, nowMs);

    for (;;) {
        bool found = false;
        qint64 best = 0;
        QUuid bestSubject;
        AlertSlot bestSlot = AlertSlot::Low;
        for (SubjectMap::iterator it = m_subjects.begin(); it != m_subjects.end(); ++it) {
            for (int i = 0; i < kSlotCount; ++i) {
                qint64 w;
                if (nextDue(it.value(), kSlots[i], &w) && w <= nowMs && (!found || w < best)) {
                    found = true;
                    best = w;
                    bestSubject = it.key();
                    bestSlot = kSlots[i];
                }
            }
        }
        if (!found)
            break;
        fire(bestSubject, m_subjects[bestSubject], bestSlot, best, nowMs, out);
    }
}

void AlertEngine::fire(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 when,
                       qint64 nowMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    switch (sl.state) {
    case AlertState::Normal:        // missing data only: the display staleness limit passed
        enterPending(subject, s, slot, AlertType::Missing, when, -1, when, out);
        break;

    case AlertState::Pending:
        if (slot == AlertSlot::Missing) {
            activate(subject, s, slot, AlertType::Missing, QStringLiteral("active"), when, -1, out);
            break;
        }
        {
            // The persistence time ended: is the condition still there? Only a reading that is
            // fresh at this moment can say so; an old reading proves nothing, and missing data
            // is the other alert's business. Without one, wait for the next reading (judge()).
            AlertType raw;
            const bool fresh = s.hasReading && when - s.lastMeasuredMs <= staleLimitOf(s);
            if (fresh && rawLevel(slot, s.lastMgDl, s.config.thresholds, &raw))
                activate(subject, s, slot, raw, QStringLiteral("active"), when, -1, out);
            else
                sl.pendingDueMs = -1;
        }
        break;

    case AlertState::Active:
        {
            // A reminder. If several were due in one coarse step, only the latest one is raised:
            // the user needs one nudge, not a burst, and the schedule stays on its own lattice.
            const qint64 interval = m_settings.reminderMs(sl.type);
            qint64 at = when;
            if (nowMs > when)
                at = when + ((nowMs - when) / interval) * interval;
            ++sl.reminders;
            sl.nextReminderMs = at + interval;
            notification(AlertEffect::Remind, subject, s, slot, at, out);
        }
        break;

    case AlertState::Snoozed:
        activate(subject, s, slot, sl.type, QStringLiteral("snooze-ended"), when, -1, out);
        break;

    case AlertState::Acknowledged:  // LL or HH acknowledged, 15 minutes passed, condition holds
        activate(subject, s, slot, sl.type, QStringLiteral("re-alerted"), when, -1, out);
        break;

    case AlertState::Cleared:
        break;
    }
}

// ---- glucose levels -----------------------------------------------------------------------

bool AlertEngine::rawLevel(AlertSlot slot, double mgdl, const Thresholds &t, AlertType *type) const
{
    // The same rule as the screens use (band.cpp), so an alert and a coloured badge cannot differ.
    const Band b = bandOfMgDl(mgdl, t);
    if (slot == AlertSlot::Low) {
        if (b == Band::LL) { *type = AlertType::LL; return true; }
        if (b == Band::L) { *type = AlertType::L; return true; }
    } else if (slot == AlertSlot::High) {
        if (b == Band::HH) { *type = AlertType::HH; return true; }
        if (b == Band::H) { *type = AlertType::H; return true; }
    }
    return false;
}

// Hysteresis: a level still "holds" until the value is back inside its threshold by
// the margin. Looks at the current level and the one below it, because a value that left LL's
// margin may still be below L. Returns the highest level that still holds.
bool AlertEngine::holdingLevel(AlertSlot slot, double mgdl, AlertType current, const Thresholds &t,
                               AlertType *holding) const
{
    const double m = m_settings.hysteresisMgDl;
    if (slot == AlertSlot::Low) {
        if (current == AlertType::LL && mgdl < t.ll + m) { *holding = AlertType::LL; return true; }
        if ((current == AlertType::LL || current == AlertType::L) && mgdl < t.l + m) {
            *holding = AlertType::L;
            return true;
        }
    } else if (slot == AlertSlot::High) {
        if (current == AlertType::HH && mgdl > t.hh - m) { *holding = AlertType::HH; return true; }
        if ((current == AlertType::HH || current == AlertType::H) && mgdl > t.h - m) {
            *holding = AlertType::H;
            return true;
        }
    }
    return false;
}

// Judges the newest reading against one glucose slot. Used for a new reading and, with `sinceMs`
// = now, when the thresholds or a persistence time change under a reading already seen.
void AlertEngine::judge(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 sinceMs,
                        qint64 nowMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    const Thresholds &t = s.config.thresholds;
    AlertType raw = AlertType::L;
    const bool hasRaw = rawLevel(slot, s.lastMgDl, t, &raw);

    switch (sl.state) {
    case AlertState::Normal:
        if (!hasRaw)
            break;
        {
            const qint64 p = persistenceFor(s, raw);
            if (p == 0 || sinceMs + p <= nowMs)
                activate(subject, s, slot, raw, QStringLiteral("active"), nowMs, sinceMs, out);
            else
                enterPending(subject, s, slot, raw, sinceMs, sinceMs + p, nowMs, out);
        }
        break;

    case AlertState::Pending:
        if (!hasRaw) {                  // back within range before the persistence time ended
            revert(subject, s, slot, nowMs, out);
            break;
        }
        {
            // A more severe level of the slot takes over at once; its own "since" is this reading.
            const bool rose = severityOf(raw) > severityOf(sl.type);
            sl.type = raw;
            const qint64 p = persistenceFor(s, raw);
            if (p == 0 || sl.sinceMs + p <= nowMs)
                activate(subject, s, slot, raw, QStringLiteral("active"), nowMs, rose ? sinceMs : -1, out);
            else
                sl.pendingDueMs = sl.sinceMs + p;
        }
        break;

    case AlertState::Active:
    case AlertState::Acknowledged:
    case AlertState::Snoozed:
        if (hasRaw && severityOf(raw) > severityOf(sl.type)) {
            // The level rose (L to LL, H to HH): active again at once, whatever was acknowledged
            // or snoozed, with no confirmation count.
            activate(subject, s, slot, raw, QStringLiteral("escalated"), nowMs, sinceMs, out);
            break;
        }
        {
            AlertType holding = AlertType::L;
            const bool holds = holdingLevel(slot, s.lastMgDl, sl.type, t, &holding);
            if (holds && holding == sl.type) {
                sl.clearCount = 0;
                break;
            }
            if (++sl.clearCount < m_settings.confirmReadings)
                break;
            if (holds)
                lower(subject, s, slot, holding, nowMs, out);
            else
                clear(subject, s, slot, QStringLiteral("recovered"), nowMs, out);
        }
        break;

    case AlertState::Cleared:
        break;
    }
}

// ---- transitions --------------------------------------------------------------------------

void AlertEngine::journal(const QUuid &subject, const SubjectState &s, AlertSlot slot,
                          AlertState from, AlertState to, const QString &event, qint64 atMs,
                          const QString &cause, AlertEffects *out)
{
    const SlotState &sl = slotOf(s, slot);
    AlertRecord r;
    r.id = m_ids();
    r.subject = subject;
    r.episode = sl.episode;
    r.slot = slot;
    r.type = sl.type;
    r.state = to;
    r.from = from;
    r.event = event;
    r.time = timeOf(atMs);
    r.measuredAt = timeOf(s.lastMeasuredMs);
    r.thresholdsId = s.config.thresholdsId;
    r.cause = cause;
    r.seq = m_seq++;
    r.levelSince = timeOf(sl.levelSinceMs);
    r.episodeSince = timeOf(sl.episodeSinceMs);
    r.acknowledged = sl.acked;
    if (to == AlertState::Pending || to == AlertState::Active)
        r.since = timeOf(sl.sinceMs);
    if (to == AlertState::Snoozed)
        r.until = timeOf(sl.snoozeUntilMs);
    if (to == AlertState::Acknowledged)
        r.until = timeOf(sl.reAlertAtMs);

    AlertEffect e;
    e.kind = AlertEffect::Journal;
    e.subject = subject;
    e.slot = slot;
    e.type = r.type;
    e.state = to;
    e.episode = r.episode;
    e.at = r.time;
    e.cause = cause;
    e.record = r;
    out->append(e);
}

void AlertEngine::notification(AlertEffect::Kind kind, const QUuid &subject, const SubjectState &s,
                               AlertSlot slot, qint64 atMs, AlertEffects *out) const
{
    const SlotState &sl = slotOf(s, slot);
    AlertEffect e;
    e.kind = kind;
    e.subject = subject;
    e.slot = slot;
    e.type = sl.type;
    e.state = sl.state;
    e.episode = sl.episode;
    e.at = timeOf(atMs);
    e.cause = slot == AlertSlot::Missing ? sl.cause : QString();
    e.reminderNumber = sl.reminders;
    e.levelSince = timeOf(sl.levelSinceMs);
    e.episodeSince = timeOf(sl.episodeSinceMs);
    e.hasLimit = limitOf(s, sl.type, &e.limit);
    if (slot == AlertSlot::Missing)
        e.newestReadingAt = timeOf(std::max(s.lastMeasuredMs, s.infoReadingMs));
    // Silent mode withholds the feedback of a glucose alert; a Withdraw has none to withhold, and
    // missing data is not covered.
    e.silent = m_silentBook && slot != AlertSlot::Missing && kind != AlertEffect::Withdraw
        && m_silentBook->isSilent(subject, e.at);
    out->append(e);
}

// The boundary in force for a level (a setting, never a reading). None for missing data.
bool AlertEngine::limitOf(const SubjectState &s, AlertType type, int *limit) const
{
    switch (type) {
    case AlertType::LL: *limit = s.config.thresholds.ll; return true;
    case AlertType::L: *limit = s.config.thresholds.l; return true;
    case AlertType::H: *limit = s.config.thresholds.h; return true;
    case AlertType::HH: *limit = s.config.thresholds.hh; return true;
    case AlertType::Missing: break;
    }
    *limit = 0;
    return false;
}

void AlertEngine::enterPending(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type,
                               qint64 sinceMs, qint64 dueMs, qint64 atMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    sl = SlotState();
    sl.episode = m_ids();
    sl.state = AlertState::Pending;
    sl.type = type;
    sl.sinceMs = sinceMs;
    sl.pendingDueMs = dueMs;
    sl.levelSinceMs = sinceMs;
    sl.episodeSinceMs = sinceMs;
    if (slot == AlertSlot::Missing)
        sl.cause = s.staleCause;
    journal(subject, s, slot, AlertState::Normal, AlertState::Pending, QStringLiteral("pending"),
            atMs, sl.cause, out);
}

// Becomes (or stays) active: a new alert, an escalation, the end of a snooze, a re-alert. The
// notification is a fresh one with sound; the reminder lattice restarts from this moment.
void AlertEngine::activate(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type,
                           const QString &event, qint64 atMs, qint64 levelSinceMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    const AlertState from = sl.state;
    if (sl.episode.isNull())
        sl.episode = m_ids();
    if (levelSinceMs >= 0) {
        sl.levelSinceMs = levelSinceMs;
        if (sl.episodeSinceMs < 0)
            sl.episodeSinceMs = levelSinceMs;   // a new episode that starts active
    }
    sl.state = AlertState::Active;
    sl.type = type;
    sl.sinceMs = atMs;
    sl.pendingDueMs = -1;
    sl.snoozeUntilMs = -1;
    sl.reAlertAtMs = -1;
    sl.reminders = 0;
    sl.clearCount = 0;
    sl.nextReminderMs = atMs + m_settings.reminderMs(type);
    if (slot == AlertSlot::Missing)
        sl.cause = s.staleCause;
    journal(subject, s, slot, from, AlertState::Active, event, atMs, sl.cause, out);
    notification(AlertEffect::Notify, subject, s, slot, atMs, out);
}

void AlertEngine::revert(const QUuid &subject, SubjectState &s, AlertSlot slot, qint64 atMs,
                         AlertEffects *out)
{
    journal(subject, s, slot, AlertState::Pending, AlertState::Normal, QStringLiteral("reverted"),
            atMs, QString(), out);
    slotOf(s, slot) = SlotState();
}

// The level of an alert fell (LL to L, HH to H) while the condition of the lower level still
// holds. The state stays (an acknowledged LL becomes an acknowledged L: the user already knows),
// the notification is replaced in place without sound, and the reminder pace of the new level
// starts. The LL/HH re-alert no longer applies to the lower level.
void AlertEngine::lower(const QUuid &subject, SubjectState &s, AlertSlot slot, AlertType type,
                        qint64 atMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    sl.type = type;
    // The lower level's stretch began with the episode: the lowest level is the first one reached.
    sl.levelSinceMs = sl.episodeSinceMs;
    sl.clearCount = 0;
    sl.reminders = 0;
    sl.reAlertAtMs = -1;
    sl.nextReminderMs = sl.state == AlertState::Active ? atMs + m_settings.reminderMs(type) : -1;
    journal(subject, s, slot, sl.state, sl.state, QStringLiteral("deescalated"), atMs, QString(), out);
    notification(AlertEffect::Update, subject, s, slot, atMs, out);
}

void AlertEngine::clear(const QUuid &subject, SubjectState &s, AlertSlot slot, const QString &reason,
                        qint64 atMs, AlertEffects *out)
{
    SlotState &sl = slotOf(s, slot);
    const AlertState from = sl.state;
    journal(subject, s, slot, from, AlertState::Cleared, QStringLiteral("cleared"), atMs, reason, out);
    notification(AlertEffect::Withdraw, subject, s, slot, atMs, out);
    out->last().state = AlertState::Cleared;
    out->last().clearedAt = timeOf(atMs);
    // a glucose alert that cleared while nobody had acknowledged (or snoozed) it leaves
    // an "ended" entry; missing data never does.
    out->last().neverAcknowledged = slot != AlertSlot::Missing && !sl.acked;
    sl = SlotState();                   // cleared, then normal at once: the record is the journal line
}

// ---- inputs -------------------------------------------------------------------------------

AlertEffects AlertEngine::setSubject(const QUuid &subject, const SubjectAlertConfig &config,
                                     const UtcTime &now)
{
    AlertEffects out;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &out);
    const SubjectAlertConfig c = sanitized(config);

    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end()) {
        SubjectState s;
        s.config = c;
        s.monitoringSinceMs = nowMs;
        m_subjects.insert(subject, s);
        return out;
    }
    SubjectState &s = it.value();
    const bool judgeAgain = c.thresholds != s.config.thresholds
        || c.persistenceLowMs != s.config.persistenceLowMs
        || c.persistenceHighMs != s.config.persistenceHighMs;
    s.config = c;
    if (judgeAgain && s.hasReading) {
        // The newest reading is judged afresh against the new rules. The confirmation counters
        // start again; a condition that begins only because of the change begins now.
        s.low.clearCount = 0;
        s.high.clearCount = 0;
        judge(subject, s, AlertSlot::Low, nowMs, nowMs, &out);
        judge(subject, s, AlertSlot::High, nowMs, nowMs, &out);
    }
    return out;
}

AlertEffects AlertEngine::onReading(const QUuid &subject, double value, const QString &unit,
                                    const UtcTime &measuredAt, const UtcTime &now)
{
    AlertEffects out;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &out);
    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end() || !measuredAt.isValid())
        return out;
    SubjectState &s = it.value();
    const qint64 measuredMs = msOf(measuredAt);
    if (measuredMs <= s.lastMeasuredMs)
        return out;                     // not newer than what the alerts already follow
    s.lastMeasuredMs = measuredMs;

    // A new reading ends missing data at once, whatever its value.
    if (s.missing.state != AlertState::Normal)
        clear(subject, s, AlertSlot::Missing, QStringLiteral("reading"), nowMs, &out);

    double mg = 0;
    if (unit == QLatin1String("mg/dL"))
        mg = value;
    else if (unit == QLatin1String("mmol/L"))
        mg = value * kMgDlPerMmolL;     // exact, from the stored value (REQ-DAT-12)
    else
        return out;                     // an unknown unit proves the source is alive, not a value
    if (!std::isfinite(mg))
        return out;

    s.hasReading = true;
    s.lastMgDl = mg;
    judge(subject, s, AlertSlot::Low, measuredMs, nowMs, &out);
    judge(subject, s, AlertSlot::High, measuredMs, nowMs, &out);
    return out;
}

AlertEffects AlertEngine::onStaleness(const QUuid &subject, const StalenessInfo &info,
                                      const UtcTime &now)
{
    AlertEffects out;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &out);
    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end())
        return out;
    SubjectState &s = it.value();

    if (info.staleLimitMs > 0)
        s.staleLimitMs = info.staleLimitMs;
    if (info.lastReadingAt.isValid())
        s.infoReadingMs = std::max(s.infoReadingMs, msOf(info.lastReadingAt));
    const bool wasWarm = suppressed(s);
    s.staleCause = info.cause;
    const bool isWarm = suppressed(s);
    if (wasWarm || isWarm)
        s.floorMs = nowMs;              // the clock restarts when the warm-up ends

    SlotState &sl = s.missing;
    if (isWarm && sl.state != AlertState::Normal) {
        // A sensor was replaced: the caregiver knows why there is no data (REQ-ALR-03). The alert
        // is withdrawn and the clock restarts when the warm-up ends.
        clear(subject, s, AlertSlot::Missing, QStringLiteral("warm-up"), nowMs, &out);
    } else if (sl.state != AlertState::Normal && sl.cause != info.cause) {
        // The message names the cause (REQ-ALR-02); it follows the cause, quietly.
        sl.cause = info.cause;
        if (sl.state == AlertState::Active || sl.state == AlertState::Acknowledged)
            notification(AlertEffect::Update, subject, s, AlertSlot::Missing, nowMs, &out);
    }
    return out;
}

AlertEffects AlertEngine::tick(const UtcTime &now)
{
    AlertEffects out;
    advanceTo(msOf(now), &out);
    return out;
}

ActionOutcome AlertEngine::acknowledge(const QUuid &subject, AlertSlot slot, const UtcTime &now)
{
    ActionOutcome o;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &o.effects);
    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end()) {
        o.result = ActionResult::UnknownSubject;
        return o;
    }
    SubjectState &s = it.value();
    SlotState &sl = slotOf(s, slot);
    if (sl.state == AlertState::Acknowledged) {
        o.result = ActionResult::AlreadyAcknowledged;
        return o;
    }
    if (sl.state != AlertState::Active) {
        o.result = ActionResult::NotActive;
        return o;
    }
    sl.state = AlertState::Acknowledged;
    sl.acked = true;
    sl.nextReminderMs = -1;             // the user has seen it: no more reminders
    // LL and HH cannot be silenced for good: they come back after the re-alert time if the
    // condition still holds. The others stay acknowledged until they clear or rise.
    sl.reAlertAtMs = isUrgent(sl.type) ? nowMs + m_settings.reAlertFor(sl.type) : -1;
    journal(subject, s, slot, AlertState::Active, AlertState::Acknowledged,
            QStringLiteral("acknowledged"), nowMs, sl.cause, &o.effects);
    notification(AlertEffect::Update, subject, s, slot, nowMs, &o.effects);
    return o;
}

ActionOutcome AlertEngine::snooze(const QUuid &subject, AlertSlot slot, int minutes, const UtcTime &now)
{
    ActionOutcome o;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &o.effects);
    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end()) {
        o.result = ActionResult::UnknownSubject;
        return o;
    }
    SubjectState &s = it.value();
    SlotState &sl = slotOf(s, slot);
    if (sl.state != AlertState::Active && sl.state != AlertState::Acknowledged) {
        o.result = ActionResult::NotActive;
        return o;
    }
    if (isUrgent(sl.type)) {            // no snooze for LL and HH, ever (REQ-THR-01)
        o.result = ActionResult::NotSnoozable;
        return o;
    }
    if (!isSnoozeMinutes(minutes)) {
        o.result = ActionResult::BadDuration;
        return o;
    }
    const AlertState from = sl.state;
    sl.acked = true;                    // a snooze is the user dealing with it: no "ended" entry later
    sl.state = AlertState::Snoozed;
    sl.snoozeUntilMs = nowMs + qint64(minutes) * kAlertMinuteMs;
    sl.nextReminderMs = -1;
    sl.reAlertAtMs = -1;
    journal(subject, s, slot, from, AlertState::Snoozed, QStringLiteral("snoozed"), nowMs, sl.cause,
            &o.effects);
    notification(AlertEffect::Withdraw, subject, s, slot, nowMs, &o.effects);
    return o;
}

AlertEffects AlertEngine::onSilentStarted(const QUuid &subject, const UtcTime &now)
{
    AlertEffects out;
    const qint64 nowMs = msOf(now);
    advanceTo(nowMs, &out);
    SubjectMap::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end())
        return out;
    const AlertSlot glucose[] = { AlertSlot::Low, AlertSlot::High };
    for (int i = 0; i < 2; ++i)
        if (slotOf(it.value(), glucose[i]).state == AlertState::Active)
            notification(AlertEffect::Update, subject, it.value(), glucose[i], nowMs, &out);
    return out;
}

AlertEffects AlertEngine::onServiceStart(const UtcTime &gapFrom, const UtcTime &gapTo,
                                         const UtcTime &now)
{
    AlertEffects out;
    advanceTo(msOf(now), &out);
    if (!gapFrom.isValid() || !gapTo.isValid() || msOf(gapTo) - msOf(gapFrom) <= m_settings.interruptionLimitMs)
        return out;
    AlertEffect e;
    e.kind = AlertEffect::Interruption;
    e.at = now;
    e.gapFrom = gapFrom;
    e.gapTo = gapTo;
    out.append(e);
    return out;
}

namespace {
bool recordBefore(const AlertRecord &a, const AlertRecord &b)
{
    if (a.time != b.time) return a.time < b.time;
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.id.toString() < b.id.toString();
}
} // namespace

AlertEffects AlertEngine::restore(const QVector<AlertRecord> &records, const UtcTime &now)
{
    AlertEffects out;
    const qint64 nowMs = msOf(now);
    QVector<AlertRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(), recordBefore);

    // The last record of each (Subject, slot) wins (way).
    QMap<QPair<QUuid, int>, AlertRecord> last;
    for (int i = 0; i < sorted.size(); ++i) {
        const AlertRecord &r = sorted.at(i);
        last.insert(qMakePair(r.subject, int(r.slot)), r);
        m_seq = std::max(m_seq, r.seq + 1);
    }

    for (QMap<QPair<QUuid, int>, AlertRecord>::const_iterator it = last.constBegin();
         it != last.constEnd(); ++it) {
        const AlertRecord &r = it.value();
        SubjectMap::iterator sit = m_subjects.find(r.subject);
        if (sit == m_subjects.end())
            continue;                   // a Subject no longer configured
        if (r.state == AlertState::Normal || r.state == AlertState::Cleared)
            continue;
        SubjectState &s = sit.value();
        SlotState &sl = slotOf(s, r.slot);
        sl = SlotState();
        sl.state = r.state;
        sl.type = r.type;
        sl.episode = r.episode.isNull() ? m_ids() : r.episode;
        sl.sinceMs = r.since.isValid() ? msOf(r.since) : msOf(r.time);
        // The facts for the texts come back from the journal; a line written before they existed
        // falls back to the reading it was judged on, then to its own time.
        sl.levelSinceMs = r.levelSince.isValid() ? msOf(r.levelSince)
            : (r.measuredAt.isValid() ? msOf(r.measuredAt) : msOf(r.time));
        sl.episodeSinceMs = r.episodeSince.isValid() ? msOf(r.episodeSince) : sl.levelSinceMs;
        sl.acked = r.acknowledged || r.state == AlertState::Acknowledged || r.state == AlertState::Snoozed;
        if (r.slot == AlertSlot::Missing) {
            sl.cause = r.cause;
            // the newest reading the alert was judged on, for "No reading since"
            if (r.measuredAt.isValid())
                s.lastMeasuredMs = std::max(s.lastMeasuredMs, msOf(r.measuredAt));
        }

        switch (r.state) {
        case AlertState::Pending:
            if (r.slot != AlertSlot::Missing)
                sl.pendingDueMs = sl.sinceMs + persistenceFor(s, r.type);
            break;
        case AlertState::Active:
            // Stays active and is notified again; the reminders start from this start.
            sl.sinceMs = nowMs;
            sl.nextReminderMs = nowMs + m_settings.reminderMs(r.type);
            notification(AlertEffect::Notify, r.subject, s, r.slot, nowMs, &out);
            break;
        case AlertState::Acknowledged:
            sl.reAlertAtMs = isUrgent(r.type)
                ? (r.until.isValid() ? msOf(r.until) : nowMs + m_settings.reAlertFor(r.type)) : -1;
            notification(AlertEffect::Update, r.subject, s, r.slot, nowMs, &out);
            break;
        case AlertState::Snoozed:
            sl.snoozeUntilMs = r.until.isValid() ? msOf(r.until) : nowMs;
            break;
        default:
            break;
        }
    }
    return out;
}

// ---- reading the state --------------------------------------------------------------------

AlertView AlertEngine::view(const QUuid &subject, AlertSlot slot) const
{
    AlertView v;
    v.subject = subject;
    v.slot = slot;
    const SubjectMap::const_iterator it = m_subjects.constFind(subject);
    if (it == m_subjects.constEnd())
        return v;
    const SlotState &sl = slotOf(it.value(), slot);
    v.type = sl.type;
    v.state = sl.state;
    v.episode = sl.episode;
    v.since = timeOf(sl.sinceMs);
    v.snoozeUntil = timeOf(sl.snoozeUntilMs);
    v.cause = sl.cause;
    v.levelSince = timeOf(sl.levelSinceMs);
    v.episodeSince = timeOf(sl.episodeSinceMs);
    v.acknowledged = sl.state == AlertState::Acknowledged;
    v.hasLimit = limitOf(it.value(), sl.type, &v.limit);
    if (slot == AlertSlot::Missing && sl.state != AlertState::Normal)
        v.newestReadingAt = timeOf(std::max(it.value().lastMeasuredMs, it.value().infoReadingMs));
    return v;
}

QVector<AlertView> AlertEngine::alerts() const
{
    QVector<AlertView> out;
    for (SubjectMap::const_iterator it = m_subjects.constBegin(); it != m_subjects.constEnd(); ++it)
        for (int i = 0; i < kSlotCount; ++i) {
            const AlertView v = view(it.key(), kSlots[i]);
            if (v.state != AlertState::Normal && v.state != AlertState::Pending)
                out.append(v);
        }
    return out;
}

} // namespace lm
