// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/alertjournal.h"

#include "store/dbutil.h"

namespace lm {

namespace {

void putTime(QJsonObject *x, const QString &key, const UtcTime &t)
{
    if (t.isValid())
        x->insert(key, t.toIso());
}

UtcTime getTime(const QJsonObject &x, const QString &key)
{
    return UtcTime::fromIso(x.value(key).toString());
}

QUuid getUuid(const QJsonObject &x, const QString &key)
{
    return dbutil::uuidFrom(x.value(key).toString());
}

} // namespace

JournalEntry toJournalEntry(const AlertRecord &r)
{
    JournalEntry e;
    e.id = r.id;
    e.kind = JournalKind::alert();
    e.wallTime = r.time;
    e.extra.insert(AlertKey::subject(), dbutil::uuidText(r.subject));
    e.extra.insert(AlertKey::slot(), alertSlotName(r.slot));
    e.extra.insert(AlertKey::level(), alertTypeName(r.type));
    e.extra.insert(AlertKey::state(), alertStateName(r.state));
    e.extra.insert(AlertKey::from(), alertStateName(r.from));
    e.extra.insert(AlertKey::event(), r.event);
    e.extra.insert(AlertKey::episode(), dbutil::uuidText(r.episode));
    e.extra.insert(AlertKey::seq(), double(r.seq));
    putTime(&e.extra, AlertKey::measuredAt(), r.measuredAt);
    putTime(&e.extra, AlertKey::since(), r.since);
    putTime(&e.extra, AlertKey::until(), r.until);
    putTime(&e.extra, AlertKey::levelSince(), r.levelSince);
    putTime(&e.extra, AlertKey::episodeSince(), r.episodeSince);
    e.extra.insert(AlertKey::acknowledged(), r.acknowledged);
    if (!r.thresholdsId.isNull())
        e.extra.insert(AlertKey::thresholds(), dbutil::uuidText(r.thresholdsId));
    if (!r.cause.isEmpty())
        e.extra.insert(AlertKey::reason(), r.cause);
    return e;
}

bool fromJournalEntry(const JournalEntry &e, AlertRecord *out)
{
    if (e.kind != JournalKind::alert())
        return false;
    AlertRecord r;
    const QJsonObject &x = e.extra;
    if (!alertSlotFromName(x.value(AlertKey::slot()).toString(), &r.slot)
        || !alertTypeFromName(x.value(AlertKey::level()).toString(), &r.type)
        || !alertStateFromName(x.value(AlertKey::state()).toString(), &r.state)
        || !alertStateFromName(x.value(AlertKey::from()).toString(), &r.from))
        return false;
    r.id = e.id;
    r.subject = getUuid(x, AlertKey::subject());
    r.episode = getUuid(x, AlertKey::episode());
    if (r.subject.isNull())
        return false;
    r.event = x.value(AlertKey::event()).toString();
    r.time = e.wallTime;
    r.seq = qint64(x.value(AlertKey::seq()).toDouble());
    r.measuredAt = getTime(x, AlertKey::measuredAt());
    r.since = getTime(x, AlertKey::since());
    r.until = getTime(x, AlertKey::until());
    r.levelSince = getTime(x, AlertKey::levelSince());
    r.episodeSince = getTime(x, AlertKey::episodeSince());
    r.acknowledged = x.value(AlertKey::acknowledged()).toBool();
    r.thresholdsId = getUuid(x, AlertKey::thresholds());
    r.cause = x.value(AlertKey::reason()).toString();
    *out = r;
    return true;
}

JournalEntry toJournalEntry(const ThresholdChange &c)
{
    JournalEntry e;
    e.id = c.id;
    e.kind = JournalKind::threshold();
    e.wallTime = c.time;
    e.extra.insert(ThresholdKey::subject(), dbutil::uuidText(c.subject));
    e.extra.insert(ThresholdKey::boundary(), boundaryName(c.boundary));
    e.extra.insert(ThresholdKey::value(), c.value);
    e.extra.insert(ThresholdKey::previous(), c.previous);
    return e;       // no comment
}

bool fromJournalEntry(const JournalEntry &e, ThresholdChange *out)
{
    if (e.kind != JournalKind::threshold())
        return false;
    ThresholdChange c;
    const QJsonObject &x = e.extra;
    if (!boundaryFromName(x.value(ThresholdKey::boundary()).toString(), &c.boundary))
        return false;
    c.id = e.id;
    c.subject = getUuid(x, ThresholdKey::subject());
    if (c.subject.isNull())
        return false;
    c.value = x.value(ThresholdKey::value()).toInt();
    c.previous = x.value(ThresholdKey::previous()).toInt();
    // An entry written by an older version may still hold the caregiver's note under "comment": it is
    // left in the journal as written and simply not read, since nothing shows it.
    c.time = e.wallTime;
    *out = c;
    return true;
}

JournalEntry toJournalEntry(const SilentChange &c)
{
    JournalEntry e;
    e.id = c.id;
    e.kind = JournalKind::silent();
    e.wallTime = c.time;
    e.extra.insert(SilentKey::subject(), dbutil::uuidText(c.subject));
    e.extra.insert(SilentKey::event(), c.event);
    putTime(&e.extra, SilentKey::until(), c.until);
    e.extra.insert(SilentKey::seq(), double(c.seq));
    return e;
}

bool fromJournalEntry(const JournalEntry &e, SilentChange *out)
{
    if (e.kind != JournalKind::silent())
        return false;
    SilentChange c;
    const QJsonObject &x = e.extra;
    c.event = x.value(SilentKey::event()).toString();
    if (c.event != SilentEvent::start() && c.event != SilentEvent::end() && c.event != SilentEvent::expired())
        return false;
    c.id = e.id;
    c.subject = getUuid(x, SilentKey::subject());
    if (c.subject.isNull())
        return false;
    c.until = getTime(x, SilentKey::until());
    c.time = e.wallTime;
    c.seq = qint64(x.value(SilentKey::seq()).toDouble());
    *out = c;
    return true;
}

bool writeSilentChange(Journal *journal, const SilentChange &c)
{
    return journal && journal->append(toJournalEntry(c));
}

QVector<SilentChange> loadSilentChanges(Journal *journal)
{
    QVector<SilentChange> out;
    if (!journal || !journal->isOpen())
        return out;
    const QVector<JournalEntry> all = journal->entries(JournalKind::silent());
    for (int i = 0; i < all.size(); ++i) {
        SilentChange c;
        if (fromJournalEntry(all.at(i), &c))
            out.append(c);
    }
    return out;
}

int writeJournalEffects(Journal *journal, const AlertEffects &effects)
{
    int failed = 0;
    for (int i = 0; i < effects.size(); ++i) {
        if (effects.at(i).kind != AlertEffect::Journal)
            continue;
        if (!journal || !journal->append(toJournalEntry(effects.at(i).record)))
            ++failed;
    }
    return failed;
}

bool writeThresholdChanges(Journal *journal, const QVector<ThresholdChange> &changes)
{
    bool ok = journal != 0;
    for (int i = 0; ok && i < changes.size(); ++i)
        ok = journal->append(toJournalEntry(changes.at(i)));
    return ok;
}

QVector<AlertRecord> loadAlertRecords(Journal *journal)
{
    QVector<AlertRecord> out;
    if (!journal || !journal->isOpen())
        return out;
    const QVector<JournalEntry> all = journal->entries(JournalKind::alert());
    for (int i = 0; i < all.size(); ++i) {
        AlertRecord r;
        if (fromJournalEntry(all.at(i), &r))
            out.append(r);
    }
    return out;
}

QVector<ThresholdChange> loadThresholdChanges(Journal *journal)
{
    QVector<ThresholdChange> out;
    if (!journal || !journal->isOpen())
        return out;
    const QVector<JournalEntry> all = journal->entries(JournalKind::threshold());
    for (int i = 0; i < all.size(); ++i) {
        ThresholdChange c;
        if (fromJournalEntry(all.at(i), &c))
            out.append(c);
    }
    return out;
}

} // namespace lm
