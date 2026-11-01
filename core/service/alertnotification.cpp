// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/alertnotification.h"

namespace lm {

namespace {

const int kClosedMemory = 64;       // ids this service closed that it still remembers

QString settingsKey(const QString &key) { return QStringLiteral("note.id.") + key; }
const char *const kIndexKey = "note.index";

} // namespace

// ---- the labels -----------------------------------------------------------------------------

void SubjectLabelBook::set(const QList<QUuid> &order, const QHash<QUuid, QString> &labels)
{
    m_order = order;
    m_labels.clear();
    for (QHash<QUuid, QString>::const_iterator it = labels.constBegin(); it != labels.constEnd(); ++it) {
        if (isShortPatientLabel(it.value()))
            m_labels.insert(it.key(), it.value().trimmed());
    }
}

// The one rule of core/service/subjectlabel.h: the short label, else P1, P2 by the
// position in the configuration file. A Subject that is not in the file is one of the developer
// feed's made-up Subjects, whose label is kept apart; anything else is "?".
QString SubjectLabelBook::labelOf(const QUuid &subject) const
{
    const int index = m_order.indexOf(subject);
    if (index < 0)
        return m_extra.value(subject, QStringLiteral("?"));
    return patientLabel(m_labels.value(subject), index);
}

// ---- the relay ---------------------------------------------------------------------------------

void AlertChangeRelay::deliver(const AlertEffect &e)
{
    if (m_next)
        m_next->deliver(e);
    if (m_events && (e.kind == AlertEffect::Notify || e.kind == AlertEffect::Update
                     || e.kind == AlertEffect::Withdraw))
        m_events->alertChanged(e.subject, alertSlotName(e.slot), alertStateName(e.state));
}

// ---- the presenter -----------------------------------------------------------------------------

AlertNotificationPresenter::AlertNotificationPresenter(AlertNotificationBackend *backend,
                                                       ServiceSettingsStore *settings,
                                                       const SubjectLabelBook *labels, Clock *clock,
                                                       const NotificationCatalog &catalog)
    : m_backend(backend), m_settings(settings), m_labels(labels), m_clock(clock), m_catalog(catalog),
      m_facts(0), m_values(0), m_showValue(false)
{
    if (m_backend)
        m_backend->setClosedHandler([this](quint32 id, uint reason) { onClosed(id, reason); });
}

QString AlertNotificationPresenter::keyOf(const QUuid &subject, AlertSlot slot)
{
    return subject.toString() + QLatin1Char('/') + alertSlotName(slot);
}

quint32 AlertNotificationPresenter::idOf(const QUuid &subject, AlertSlot slot) const
{
    return m_alarms.value(keyOf(subject, slot)).id;
}

UtcTime AlertNotificationPresenter::levelSinceOf(const QUuid &subject, AlertSlot slot) const
{
    return m_alarms.value(keyOf(subject, slot)).facts.levelSince;
}

bool AlertNotificationPresenter::hasAlarm(const QUuid &subject, AlertSlot slot) const
{
    return m_alarms.contains(keyOf(subject, slot));
}

// The facts of an effect: from the reader when there is one, else what the effect itself knows.
EffectFacts AlertNotificationPresenter::factsOf(const AlertEffect &e) const
{
    if (m_facts)
        return m_facts->factsOf(e);
    EffectFacts f;
    if (e.kind == AlertEffect::Notify)
        f.levelSince = e.at;
    f.clearedAt = e.at;
    f.cause = e.cause;
    return f;
}

AlarmTextInput AlertNotificationPresenter::inputFor(const Alarm &a) const
{
    AlarmTextInput in;
    in.label = m_labels ? m_labels->labelOf(a.subject) : QStringLiteral("?");
    in.showValue = m_showValue;
    in.facts = a.facts;
    in.now = m_clock ? m_clock->now() : UtcTime();
    in.unit = QStringLiteral("mg/dL");
    // The value is read from the store when the notification is posted, and only when the setting
    // is on; the unit of the Subject's own readings is used for the limit as well (REQ-DAT-12).
    if (m_values) {
        double v = 0;
        QString unit;
        int trend = -1;
        UtcTime measured;
        if (m_values->newest(a.subject, &v, &unit, &trend, &measured)) {
            in.unit = unit;
            if (m_showValue) {
                in.haveValue = true;
                in.value = v;
                in.trendCode = trend;
            }
        }
    }
    return in;
}

NotificationRequest AlertNotificationPresenter::build(const Alarm &a, Sound sound, int /*reminder*/) const
{
    const NoteKind kind = NotificationCatalog::alarmKindOf(a.type);
    NotificationRequest r;
    const NoteText t = m_texts.alarm(kind, inputFor(a));
    r.summary = t.summary;
    r.body = t.body;
    r.hints = m_catalog.hints(kind, sound);
    return r;
}

quint32 AlertNotificationPresenter::storedId(const QString &key) const
{
    return m_settings ? m_settings->value(settingsKey(key)).toUInt() : 0;
}

// The id is kept so that a restart replaces the entry; the index lists the keys that have one, so
// that closeOrphans can find them without the settings store having to list its keys.
void AlertNotificationPresenter::storeId(const QString &key, quint32 id)
{
    if (!m_settings)
        return;
    m_settings->setValue(settingsKey(key), id ? QString::number(id) : QString());
    QStringList index = m_settings->value(QString::fromLatin1(kIndexKey)).split(QLatin1Char('|'), QString::SkipEmptyParts);
    const bool has = index.contains(key);
    if (id && !has)
        index << key;
    else if (!id && has)
        index.removeAll(key);
    else
        return;
    m_settings->setValue(QString::fromLatin1(kIndexKey), index.join(QLatin1Char('|')));
}

void AlertNotificationPresenter::rememberClosed(quint32 id)
{
    m_closedByUs.append(id);
    while (m_closedByUs.size() > kClosedMemory)
        m_closedByUs.removeFirst();
}

void AlertNotificationPresenter::post(Alarm *a, Sound sound, int reminder)
{
    const QString key = keyOf(a->subject, a->slot);
    // The id to replace: the one this run holds, else the one the last run stored (a restart).
    const quint32 replaces = a->id ? a->id : storedId(QStringLiteral("alarm/") + key);
    const quint32 now = m_backend->publish(build(*a, sound, reminder), replaces);
    if (now == 0)
        return;             // could not be posted: the next effect tries again
    a->id = now;
    if (now != replaces || storedId(QStringLiteral("alarm/") + key) != now)
        storeId(QStringLiteral("alarm/") + key, now);
}

void AlertNotificationPresenter::closeNotification(Alarm *a)
{
    const QString key = QStringLiteral("alarm/") + keyOf(a->subject, a->slot);
    const quint32 id = a->id ? a->id : storedId(key);
    if (id) {
        m_backend->close(id);
        rememberClosed(id);
    }
    a->id = 0;
    storeId(key, 0);
}

// The holder of the long tone: the best urgent alarm that is active, unacknowledged and not silent.
// Rank first (urgent low, then urgent high); the same rank is decided by the earlier onset of the
// level, then by the key so that the choice never flips between calls.
QString AlertNotificationPresenter::toneHolder() const
{
    QString best;
    int bestRank = 0;
    qint64 bestSince = 0;
    for (QMap<QString, Alarm>::const_iterator it = m_alarms.constBegin(); it != m_alarms.constEnd(); ++it) {
        const Alarm &a = it.value();
        const NoteKind kind = NotificationCatalog::alarmKindOf(a.type);
        if (!NotificationCatalog::isUrgentAlarm(kind) || a.acknowledged || a.state != AlertState::Active || a.facts.silent)
            continue;
        const int rank = NotificationCatalog::rankOf(kind);
        const qint64 since = a.facts.levelSince.isValid() ? a.facts.levelSince.msecsSinceEpoch() : Q_INT64_C(0x7fffffffffffffff);
        if (best.isEmpty() || rank < bestRank || (rank == bestRank && since < bestSince)) {
            best = it.key();
            bestRank = rank;
            bestSince = since;
        }
    }
    return best;
}

bool AlertNotificationPresenter::toneHolder(QUuid *subject, AlertSlot *slot) const
{
    const QString key = toneHolder();
    if (key.isEmpty())
        return false;
    const Alarm &a = m_alarms[key];
    *subject = a.subject;
    *slot = a.slot;
    return true;
}

// What an alarm may sound at a post or a reminder, given the holder of the tone. A silent Subject
// asks for nothing at all. The holder asks for the long tone. Any other alarm gets no sound
// while an urgent tone rings and owes its short sound for when it has ended; with no tone ringing
// an alarm's own sound stands.
Sound AlertNotificationPresenter::soundFor(Alarm *a, const QString &holder)
{
    a->owesShort = false;
    if (a->facts.silent)
        return Sound::None;
    const QString key = keyOf(a->subject, a->slot);
    const bool urgent = NotificationCatalog::isUrgentAlarm(NotificationCatalog::alarmKindOf(a->type));
    if (holder == key)
        return Sound::Own;
    if (!holder.isEmpty()) {
        a->owesShort = true;
        return Sound::None;
    }
    return urgent ? Sound::None : Sound::Own;       // an urgent alarm with no holder is silent or acknowledged
}

// After every effect: if the holder changed hands, the old holder is quietened (it was replaced
// without hints) and the new one is re-posted with the alarm hints; when no urgent tone rings any
// more, the alarms that were kept silent over it give their short sound once.
void AlertNotificationPresenter::settle()
{
    const QString holder = toneHolder();
    // 1. whoever still thinks it rings but is not the holder is stopped by a replacement without hints
    for (QMap<QString, Alarm>::iterator it = m_alarms.begin(); it != m_alarms.end(); ++it) {
        Alarm &a = it.value();
        if (a.toneRinging && it.key() != holder) {
            a.toneRinging = false;
            if (a.id && !a.acknowledged) {
                a.owesShort = !a.facts.silent;
                post(&a, Sound::None, 0);
            }
        }
    }
    // 2. the holder rings
    if (!holder.isEmpty()) {
        Alarm &h = m_alarms[holder];
        if (!h.toneRinging) {
            h.owesShort = false;
            post(&h, Sound::Own, 0);
            h.toneRinging = h.id != 0;
        }
        return;
    }
    // 3. no urgent tone: the short sounds owed are paid, once
    for (QMap<QString, Alarm>::iterator it = m_alarms.begin(); it != m_alarms.end(); ++it) {
        Alarm &a = it.value();
        if (!a.owesShort)
            continue;
        a.owesShort = false;
        if (a.id && !a.acknowledged && !a.facts.silent && a.state == AlertState::Active)
            post(&a, Sound::Short, 0);
    }
}

void AlertNotificationPresenter::deliver(const AlertEffect &e)
{
    if (!m_backend || e.kind == AlertEffect::Journal)
        return;
    if (e.kind == AlertEffect::Interruption) {
        // A notice, not an alert (REQ-ALR-07): page only, one entry, replaced by a newer one.
        postNotice(QStringLiteral("notice/interruption"), NoteKind::Interruption,
                   m_texts.interruption(e.gapFrom, e.gapTo), Sound::None);
        return;
    }

    const QString key = keyOf(e.subject, e.slot);
    const bool existed = m_alarms.contains(key);
    Alarm &a = m_alarms[key];
    a.subject = e.subject;
    a.slot = e.slot;

    if (e.kind == AlertEffect::Withdraw) {
        // Snoozed counts as handled; cleared ends the episode. A glucose alert that cleared while
        // nobody acknowledged or snoozed it leaves one page-only entry; no data never does.
        const EffectFacts f = factsOf(e);
        const bool acknowledgedInEpisode = f.acknowledgementKnown ? !f.neverAcknowledged : a.everAcknowledged;
        const AlertType type = e.type;
        const UtcTime episodeSince = a.facts.episodeSince;
        closeNotification(&a);
        m_alarms.remove(key);
        if (e.state == AlertState::Snoozed) {
            m_snoozed.insert(key);                     // the episode goes on: its end is not an unhandled one
        } else if (existed && e.state == AlertState::Cleared && !acknowledgedInEpisode && !m_snoozed.contains(key)) {
            NoteKind ended;
            if (NotificationCatalog::endedKindOf(type, &ended)) {
                EffectFacts t = f;
                if (!t.episodeSince.isValid())
                    t.episodeSince = episodeSince;
                if (!t.clearedAt.isValid())
                    t.clearedAt = e.at.isValid() ? e.at : (m_clock ? m_clock->now() : UtcTime());
                const QString label = m_labels ? m_labels->labelOf(e.subject) : QStringLiteral("?");
                postNotice(QStringLiteral("ended/") + key, ended, m_texts.ended(ended, label, t), Sound::None);
            }
        }
        if (e.state == AlertState::Cleared)
            m_snoozed.remove(key);
        settle();
        return;
    }
    // The slot is active again: its "ended" entry is out of date.
    if (e.kind == AlertEffect::Notify)
        withdrawNotice(QStringLiteral("ended/") + key);

    // The facts of this effect over what was noted before: a reader that does not know when a level
    // began (the effect predates the `levelSince`) leaves it invalid, and the time noted at the
    // raise stays; the episode begins with the first level of the slot.
    {
        EffectFacts f = factsOf(e);
        const bool known = a.facts.levelSince.isValid();
        if (!f.levelSince.isValid())
            f.levelSince = a.facts.levelSince;
        if (!f.episodeSince.isValid())
            f.episodeSince = known ? a.facts.episodeSince : f.levelSince;
        // No data is about the monitor itself and is never covered by a Subject's silent mode.
        if (e.type == AlertType::Missing)
            f.silent = false;
        a.type = e.type;
        a.state = e.state;
        a.facts = f;
    }

    switch (e.kind) {
    case AlertEffect::Notify:
    case AlertEffect::Remind: {
        if (e.kind == AlertEffect::Notify) {
            a.acknowledged = false;
            m_snoozed.remove(key);
        }
        // A raise or a repeat: the sound the arbiter allows. The new holder takes the tone from the
        // old one by the settling that follows (the old one is replaced without hints).
        const QString holder = toneHolder();
        const Sound sound = soundFor(&a, holder);
        if (sound == Sound::Own && holder == key) {
            // The old holder, if another, stops first, so the tone passes in one hand-over.
            for (QMap<QString, Alarm>::iterator it = m_alarms.begin(); it != m_alarms.end(); ++it)
                if (it.key() != key && it.value().toneRinging) {
                    Alarm &o = it.value();
                    o.toneRinging = false;
                    o.owesShort = !o.facts.silent;
                    if (o.id)
                        post(&o, Sound::None, 0);
                }
        }
        post(&a, sound, e.reminderNumber);
        a.toneRinging = sound == Sound::Own && a.id != 0 && NotificationCatalog::isUrgentAlarm(NotificationCatalog::alarmKindOf(a.type));
        break;
    }
    case AlertEffect::Update:
        if (e.state == AlertState::Acknowledged) {
            // Acknowledged: nothing is left to show ( the entry goes), the alarm stays in the
            // list of alerts with its flag.
            a.acknowledged = true;
            a.everAcknowledged = true;
            a.toneRinging = false;
            a.owesShort = false;
            closeNotification(&a);
        } else if (!a.acknowledged) {
            // News is quiet: a replacement without feedback stops a tone (U2b).
            a.toneRinging = false;
            post(&a, Sound::None, 0);
            // Replaced quietly: if another alarm rings the urgent tone this one is demoted by it and
            // owes its short sound for when the tone ends; with no tone to wait for it owes nothing.
            const QString holder = toneHolder();
            a.owesShort = !holder.isEmpty() && holder != key && !a.facts.silent;
        }
        break;
    default:
        break;
    }
    settle();
}

// The platform reports a notification closed by someone else (the user's deletion, an expiry) or
// by a call on an older object. Only a deletion of a notification this service did not close
// itself is the user's acknowledgement.
void AlertNotificationPresenter::onClosed(quint32 id, uint reason)
{
    if (m_closedByUs.contains(id))
        return;                                  // our own close, or an echo of it
    if (reason != 2)
        return;                                  // expired (1) or closed by call on an older object (3)
    // A notice or an "ended" entry the user deleted is only forgotten: deleting it acknowledges nothing.
    for (QMap<QString, quint32>::iterator it = m_noticeIds.begin(); it != m_noticeIds.end(); ++it)
        if (it.value() == id) {
            const QString key = it.key();
            m_noticeIds.erase(it);
            storeId(key, 0);
            return;
        }
    for (QMap<QString, Alarm>::iterator it = m_alarms.begin(); it != m_alarms.end(); ++it) {
        Alarm &a = it.value();
        if (a.id != id)
            continue;
        a.id = 0;
        a.toneRinging = false;
        storeId(QStringLiteral("alarm/") + it.key(), 0);
        if (m_acknowledge)
            m_acknowledge(a.subject, a.slot);
        return;
    }
}

void AlertNotificationPresenter::closeOrphans()
{
    if (!m_settings || !m_backend)
        return;
    const QStringList index = m_settings->value(QString::fromLatin1(kIndexKey)).split(QLatin1Char('|'), QString::SkipEmptyParts);
    for (int i = 0; i < index.size(); ++i) {
        const QString key = index.at(i);
        // The notices that describe a state of the moment (approval, guard, report, Do not disturb)
        // are posted again at start when they still hold; one the last run left and this run did not
        // post is out of date. The interruption notice and the "ended" entries are history and stay.
        // The Do not disturb notice is reconciled by its own watcher at start (it posts or withdraws
        // according to the state it reads), so it is left alone here.
        const bool transient = key.startsWith(QStringLiteral("notice/"))
                               && key != QLatin1String("notice/interruption")
                               && key != QLatin1String("notice/silent");
        if (transient) {
            if (!m_posted.contains(key)) {
                const quint32 id = storedId(key);
                if (id)
                    m_backend->close(id);
                storeId(key, 0);
            }
            continue;
        }
        if (!key.startsWith(QStringLiteral("alarm/")))
            continue;
        const QString slotKey = key.mid(6);
        const quint32 id = storedId(key);
        const Alarm alarm = m_alarms.value(slotKey);
        if (alarm.id == id && id != 0)
            continue;                            // re-notified by the restore: it replaced the old entry
        if (id)
            m_backend->close(id);
        storeId(key, 0);
    }
}

// ---- the notices ----------------------------------------------------------------------------------

void AlertNotificationPresenter::postNotice(const QString &key, NoteKind kind, const NoteText &text, Sound sound)
{
    if (!m_backend)
        return;
    NotificationRequest r;
    r.summary = text.summary;
    r.body = text.body;
    r.hints = m_catalog.hints(kind, sound);
    const quint32 replaces = m_noticeIds.value(key, storedId(key));
    const quint32 now = m_backend->publish(r, replaces);
    if (now == 0)
        return;
    m_noticeIds.insert(key, now);
    m_posted.insert(key);
    if (storedId(key) != now)
        storeId(key, now);
}

void AlertNotificationPresenter::withdrawNotice(const QString &key)
{
    if (!m_backend)
        return;
    const quint32 id = m_noticeIds.value(key, storedId(key));
    if (!id)
        return;
    m_backend->close(id);
    rememberClosed(id);
    m_noticeIds.remove(key);
    m_posted.remove(key);
    storeId(key, 0);
}

// The labels of a connection's Subjects, "ABC, DEF", for the words of the approval notice.
QString AlertNotificationPresenter::subjectLabelsOf(const QString &connection) const
{
    QStringList labels;
    if (m_subjectsOf && m_labels) {
        const QStringList ids = m_subjectsOf(connection);
        for (int i = 0; i < ids.size(); ++i)
            labels << m_labels->labelOf(QUuid(ids.at(i)));
    }
    return labels.isEmpty() ? QStringLiteral("?") : labels.join(QStringLiteral(", "));
}

void AlertNotificationPresenter::clearEndedEntriesAndInterruption(const QList<QUuid> &subjects)
{
    for (int i = 0; i < subjects.size(); ++i)
        for (int k = 0; k < 3; ++k)
            withdrawNotice(QStringLiteral("ended/") + keyOf(subjects.at(i), static_cast<AlertSlot>(k)));
    withdrawNotice(QStringLiteral("notice/interruption"));
}

void AlertNotificationPresenter::notifyApproval(const QString &connection)
{
    postNotice(QStringLiteral("notice/approval/") + connection, NoteKind::WaitingApproval,
               m_texts.waitingForApproval(subjectLabelsOf(connection)), Sound::Own);
}

void AlertNotificationPresenter::approvalSettled(const QString &connection)
{
    withdrawNotice(QStringLiteral("notice/approval/") + connection);
}

void AlertNotificationPresenter::notifyGuardDisabled(const QString &connection, int attempts, const QString &causeKey)
{
    const QString label = m_connectionLabel ? m_connectionLabel(connection) : connection;
    postNotice(QStringLiteral("notice/guard/") + connection, NoteKind::GuardDisabled,
               m_texts.guardDisabled(label, attempts, causeKey), Sound::None);
}

void AlertNotificationPresenter::guardEnabled(const QString &connection)
{
    withdrawNotice(QStringLiteral("notice/guard/") + connection);
}

void AlertNotificationPresenter::notifyReportProposed(const QString &connection)
{
    postNotice(QStringLiteral("notice/report/") + connection, NoteKind::ReportProposed, m_texts.reportProposed(), Sound::None);
}

void AlertNotificationPresenter::reportSettled(const QString &connection)
{
    withdrawNotice(QStringLiteral("notice/report/") + connection);
}

void AlertNotificationPresenter::notifyDoNotDisturb(bool on)
{
    const QString key = QStringLiteral("notice/silent");
    if (on)
        postNotice(key, NoteKind::AlertsSilent, m_texts.alertsWillBeSilent(), Sound::None);
    else
        withdrawNotice(key);
}

// The state at start. The entry the last run left is the one the system holds under the id stored in
// the settings; a phone that kept its notifications keeps it, so it is adopted and not posted again (a
// second post under the same id would raise a banner at every restart, which forbids: no nagging).
// UNVERIFIED: an entry the user deleted while the service was down is not known to be gone,
// so it stays absent until the next change; the service learns of a deletion only while it runs.
void AlertNotificationPresenter::adoptDoNotDisturb(bool on)
{
    const QString key = QStringLiteral("notice/silent");
    if (!on) {
        withdrawNotice(key);                     // went off while the service was down (nothing to do if none)
        return;
    }
    const quint32 id = storedId(key);
    if (id != 0) {
        m_noticeIds.insert(key, id);             // adopted: later withdrawn or replaced under this id
        m_posted.insert(key);
        return;
    }
    notifyDoNotDisturb(true);                    // came on while the service was down, or the entry is absent
}

} // namespace lm
