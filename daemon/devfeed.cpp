// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "daemon/devfeed.h"

#ifdef LIBREMON_DEVSOURCE

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace lm {

namespace {

const char *const kSubjectNamespace = "{2b4f6e1a-0d7c-4b1e-9a55-6f3d1c7e8a40}";

QString slotKey(const QUuid &subject, AlertSlot slot) { return subject.toString() + QLatin1Char('/') + alertSlotName(slot); }

QString answer(bool ok, int applied, const QString &error = QString())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), ok);
    o.insert(QStringLiteral("applied"), applied);
    if (!error.isEmpty())
        o.insert(QStringLiteral("error"), error);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject alertOp(const char *kind, int subject, const char *slot, const char *type, int sinceMin = -1,
                    const char *state = 0, bool silent = false, bool neverAck = false)
{
    QJsonObject o;
    o.insert(QStringLiteral("op"), QStringLiteral("alert"));
    o.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
    o.insert(QStringLiteral("subject"), subject);
    o.insert(QStringLiteral("slot"), QString::fromLatin1(slot));
    if (type)
        o.insert(QStringLiteral("type"), QString::fromLatin1(type));
    if (sinceMin >= 0)
        o.insert(QStringLiteral("since_min"), sinceMin);
    if (state)
        o.insert(QStringLiteral("state"), QString::fromLatin1(state));
    if (silent)
        o.insert(QStringLiteral("silent"), true);
    if (neverAck)
        o.insert(QStringLiteral("never_ack"), true);
    return o;
}

// An operation that is skipped, not refused, when the slot it names does not exist (a ready-made
// scenario that touches "the low and the high slot of Subject 1" may find only one of them).
QJsonObject ifPresent(QJsonObject o) { o.insert(QStringLiteral("if_present"), true); return o; }
QJsonObject withSilent(QJsonObject o, bool silent) { o.insert(QStringLiteral("silent"), silent); return o; }

QJsonObject plainOp(const char *op)
{
    QJsonObject o;
    o.insert(QStringLiteral("op"), QString::fromLatin1(op));
    return o;
}

QJsonObject boolOp(const char *op, const char *key, bool value)
{
    QJsonObject o = plainOp(op);
    o.insert(QString::fromLatin1(key), value);
    return o;
}

} // namespace

QUuid DevFeed::subjectId(int number)
{
    if (number < 1 || number > 4)
        return QUuid();
    return QUuid::createUuidV5(QUuid(QString::fromLatin1(kSubjectNamespace)), QStringLiteral("dev-subject-%1").arg(number));
}

bool DevFeed::isSynthetic(const QUuid &subject)
{
    for (int i = 1; i <= 4; ++i)
        if (subjectId(i) == subject)
            return true;
    return false;
}

DevFeed::DevFeed(AlertNotificationPresenter *presenter, SubjectLabelBook *labels, Clock *now,
                 const EffectFactsReader *inner, const std::function<void(bool)> &dndChanged,
                 ServiceEventSink *events)
    : m_presenter(presenter), m_labels(labels), m_clock(now), m_inner(inner), m_dndChanged(dndChanged),
      m_events(events), m_dnd(false)
{
    const char *names[] = { "AAA", "BBB", "CCC", "DDD" };
    for (int i = 1; i <= 4; ++i)
        m_labelOf.insert(subjectId(i), QString::fromLatin1(names[i - 1]));
    if (m_labels)
        m_labels->setExtra(m_labelOf);
}

QStringList DevFeed::scenarioNames()
{
    return QStringList() << "urgent-low" << "low" << "high" << "urgent-high" << "no-data"
                         << "two-subjects-urgent-low-and-low" << "two-urgent-lows" << "urgent-high-then-urgent-low"
                         << "reminder" << "acknowledge" << "lower" << "silent" << "unsilent" << "ended"
                         << "interruption" << "approval" << "approval-done" << "guard" << "guard-done"
                         << "report" << "report-done" << "dnd-on" << "dnd-off" << "clear";
}

// The ready-made steps of the phone protocol. Each starts from a
// clean tray where it needs to, so a step can be repeated.
QList<QJsonObject> DevFeed::scenario(const QString &name, bool *known) const
{
    *known = true;
    QList<QJsonObject> ops;
    const QJsonObject clear = plainOp("clear");
    if (name == QLatin1String("urgent-low")) ops << clear << alertOp("notify", 1, "low", "LL", 8);
    else if (name == QLatin1String("low")) ops << clear << alertOp("notify", 1, "low", "L", 8);
    else if (name == QLatin1String("high")) ops << clear << alertOp("notify", 1, "high", "H", 20);
    else if (name == QLatin1String("urgent-high")) ops << clear << alertOp("notify", 1, "high", "HH", 8);
    else if (name == QLatin1String("no-data")) ops << clear << alertOp("notify", 1, "missing", "MISSING", 18);
    else if (name == QLatin1String("two-subjects-urgent-low-and-low"))
        ops << clear << alertOp("notify", 1, "low", "LL", 8) << alertOp("notify", 2, "low", "L", 3);
    else if (name == QLatin1String("two-urgent-lows"))
        ops << clear << alertOp("notify", 1, "low", "LL", 12) << alertOp("notify", 2, "low", "LL", 5);
    else if (name == QLatin1String("urgent-high-then-urgent-low"))
        ops << clear << alertOp("notify", 1, "high", "HH", 14) << alertOp("notify", 2, "low", "LL", 2);
    else if (name == QLatin1String("reminder")) ops << ifPresent(alertOp("remind", 1, "low", 0)) << ifPresent(alertOp("remind", 1, "high", 0));
    else if (name == QLatin1String("acknowledge"))
        ops << ifPresent(alertOp("update", 1, "low", 0, -1, "acknowledged")) << ifPresent(alertOp("update", 1, "high", 0, -1, "acknowledged"));
    else if (name == QLatin1String("lower")) ops << alertOp("update", 1, "low", "L", -1, "active");
    else if (name == QLatin1String("silent"))
        ops << ifPresent(withSilent(alertOp("update", 1, "low", 0, -1, "active"), true))
            << ifPresent(withSilent(alertOp("update", 1, "high", 0, -1, "active"), true));
    else if (name == QLatin1String("unsilent"))
        ops << ifPresent(withSilent(alertOp("remind", 1, "low", 0), false)) << ifPresent(withSilent(alertOp("remind", 1, "high", 0), false));
    else if (name == QLatin1String("ended"))
        ops << clear << alertOp("notify", 1, "low", "L", 28) << alertOp("withdraw", 1, "low", "L", 28, "cleared", false, true);
    else if (name == QLatin1String("interruption")) { QJsonObject o = plainOp("interruption"); o.insert(QStringLiteral("from_min"), 95); o.insert(QStringLiteral("to_min"), 20); ops << o; }
    else if (name == QLatin1String("approval")) ops << plainOp("approval");
    else if (name == QLatin1String("approval-done")) ops << plainOp("approval_done");
    else if (name == QLatin1String("guard")) { QJsonObject o = plainOp("guard"); o.insert(QStringLiteral("attempts"), 3); o.insert(QStringLiteral("cause"), QStringLiteral("no-new-readings")); ops << o; }
    else if (name == QLatin1String("guard-done")) ops << plainOp("guard_done");
    else if (name == QLatin1String("report")) ops << plainOp("report");
    else if (name == QLatin1String("report-done")) ops << plainOp("report_done");
    else if (name == QLatin1String("dnd-on")) ops << boolOp("dnd", "on", true);
    else if (name == QLatin1String("dnd-off")) ops << boolOp("dnd", "on", false);
    else if (name == QLatin1String("clear")) ops << clear;
    else *known = false;
    return ops;
}

QString DevFeed::apply(const QString &json)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return answer(false, 0, QStringLiteral("not-json"));
    const QJsonObject root = doc.object();
    QList<QJsonObject> ops;
    if (root.contains(QStringLiteral("scenario"))) {
        bool known = false;
        ops = scenario(root.value(QStringLiteral("scenario")).toString(), &known);
        if (!known)
            return answer(false, 0, QStringLiteral("unknown-scenario"));
    } else if (root.value(QStringLiteral("ops")).isArray()) {
        const QJsonArray a = root.value(QStringLiteral("ops")).toArray();
        for (int i = 0; i < a.size(); ++i)
            ops << a.at(i).toObject();
    } else {
        return answer(false, 0, QStringLiteral("nothing-to-do"));
    }
    int applied = 0;
    for (int i = 0; i < ops.size(); ++i) {
        QString error;
        if (!applyOp(ops.at(i), &error))
            return answer(false, applied, error);
        ++applied;
    }
    return answer(true, applied);
}

bool DevFeed::applyOp(const QJsonObject &op, QString *error)
{
    const QString name = op.value(QStringLiteral("op")).toString();
    if (name == QLatin1String("alert"))
        return applyAlert(op, error);
    if (name == QLatin1String("interruption")) {
        const UtcTime now = m_clock->now();
        AlertEffect e;
        e.kind = AlertEffect::Interruption;
        e.gapFrom = UtcTime::fromMSecsSinceEpoch(now.msecsSinceEpoch() - qint64(op.value(QStringLiteral("from_min")).toInt(90)) * 60000);
        e.gapTo = UtcTime::fromMSecsSinceEpoch(now.msecsSinceEpoch() - qint64(op.value(QStringLiteral("to_min")).toInt(20)) * 60000);
        m_presenter->deliver(e);
        return true;
    }
    if (name == QLatin1String("approval")) { m_presenter->notifyApproval(QStringLiteral("dev")); return true; }
    if (name == QLatin1String("approval_done")) { m_presenter->approvalSettled(QStringLiteral("dev")); return true; }
    if (name == QLatin1String("guard")) {
        m_presenter->notifyGuardDisabled(QStringLiteral("dev"), op.value(QStringLiteral("attempts")).toInt(3),
                                         op.value(QStringLiteral("cause")).toString(QStringLiteral("score")));
        return true;
    }
    if (name == QLatin1String("guard_done")) { m_presenter->guardEnabled(QStringLiteral("dev")); return true; }
    if (name == QLatin1String("report")) { m_presenter->notifyReportProposed(QStringLiteral("dev")); return true; }
    if (name == QLatin1String("report_done")) { m_presenter->reportSettled(QStringLiteral("dev")); return true; }
    if (name == QLatin1String("dnd")) {
        m_dnd = op.value(QStringLiteral("on")).toBool();
        m_presenter->notifyDoNotDisturb(m_dnd);
        if (m_dndChanged)
            m_dndChanged(m_dnd);
        return true;
    }
    if (name == QLatin1String("label")) {
        const QUuid id = subjectId(op.value(QStringLiteral("subject")).toInt());
        const QString label = op.value(QStringLiteral("label")).toString().trimmed();
        if (id.isNull() || label.isEmpty() || label.size() > SubjectLabelBook::kMaxLabelChars) {
            *error = QStringLiteral("bad-label");
            return false;
        }
        m_labelOf.insert(id, label);
        if (m_labels)
            m_labels->setExtra(m_labelOf);
        return true;
    }
    if (name == QLatin1String("clear")) {
        // Every synthetic alert is taken down as a snoozed one (no "ended" entry is left), then the
        // notices and the entries of the synthetic Subjects, and Do not disturb is switched off.
        const QList<QString> keys = m_slots.keys();
        for (int i = 0; i < keys.size(); ++i) {
            const QString subject = keys.at(i).section(QLatin1Char('/'), 0, 0);
            AlertSlot slot = AlertSlot::Low;
            alertSlotFromName(keys.at(i).section(QLatin1Char('/'), 1, 1), &slot);
            AlertEffect e;
            e.kind = AlertEffect::Withdraw;
            e.subject = QUuid(subject);
            e.slot = slot;
            e.type = m_slots.value(keys.at(i)).type;
            e.state = AlertState::Snoozed;
            e.at = m_clock->now();
            m_presenter->deliver(e);
            if (m_events)
                m_events->alertChanged(e.subject, alertSlotName(e.slot), alertStateName(e.state));
        }
        m_slots.clear();
        QList<QUuid> subjects;
        for (int i = 1; i <= 4; ++i)
            subjects << subjectId(i);
        m_presenter->clearEndedEntriesAndInterruption(subjects);
        m_presenter->approvalSettled(QStringLiteral("dev"));
        m_presenter->guardEnabled(QStringLiteral("dev"));
        m_presenter->reportSettled(QStringLiteral("dev"));
        m_presenter->notifyDoNotDisturb(false);
        m_dnd = false;
        if (m_dndChanged)
            m_dndChanged(false);
        return true;
    }
    *error = QStringLiteral("unknown-op");
    return false;
}

bool DevFeed::applyAlert(const QJsonObject &op, QString *error)
{
    const QUuid subject = subjectId(op.value(QStringLiteral("subject")).toInt());
    if (subject.isNull()) {
        *error = QStringLiteral("not-a-synthetic-subject");         // only the four made-up Subjects, never a real one
        return false;
    }
    AlertSlot slot;
    if (!alertSlotFromName(op.value(QStringLiteral("slot")).toString(), &slot)) {
        *error = QStringLiteral("bad-slot");
        return false;
    }
    const QString kindName = op.value(QStringLiteral("kind")).toString();
    AlertEffect e;
    e.subject = subject;
    e.slot = slot;
    e.episode = QUuid::createUuid();
    e.at = m_clock->now();
    if (kindName == QLatin1String("notify")) e.kind = AlertEffect::Notify;
    else if (kindName == QLatin1String("remind")) e.kind = AlertEffect::Remind;
    else if (kindName == QLatin1String("update")) e.kind = AlertEffect::Update;
    else if (kindName == QLatin1String("withdraw")) e.kind = AlertEffect::Withdraw;
    else { *error = QStringLiteral("bad-kind"); return false; }

    const QString key = slotKey(subject, slot);
    const bool known = m_slots.contains(key);
    if (!known && op.value(QStringLiteral("if_present")).toBool())
        return true;
    Slot s = m_slots.value(key);
    // The type: given, else the one the slot already has.
    if (op.contains(QStringLiteral("type"))) {
        if (!alertTypeFromName(op.value(QStringLiteral("type")).toString(), &s.type)) {
            *error = QStringLiteral("bad-type");
            return false;
        }
    } else if (!known) {
        *error = QStringLiteral("type-needed");
        return false;
    }
    if (slotOf(s.type) != slot) {
        *error = QStringLiteral("type-does-not-fit-the-slot");
        return false;
    }
    e.type = s.type;
    const QString stateName = op.value(QStringLiteral("state")).toString();
    e.state = e.kind == AlertEffect::Withdraw ? AlertState::Cleared : AlertState::Active;
    if (!stateName.isEmpty() && !alertStateFromName(stateName, &e.state)) {
        *error = QStringLiteral("bad-state");
        return false;
    }
    e.cause = op.value(QStringLiteral("cause")).toString(e.type == AlertType::Missing ? QStringLiteral("source-unreachable") : QString());

    // The facts the texts and the arbiter read: when the level began, the limit, silence, and for a
    // withdrawal whether anybody had acknowledged it.
    EffectFacts f = s.facts;
    const qint64 now = e.at.msecsSinceEpoch();
    const bool sinceGiven = op.contains(QStringLiteral("since_min"));
    if (e.kind == AlertEffect::Notify || sinceGiven || !f.levelSince.isValid()) {
        const int sinceMin = op.value(QStringLiteral("since_min")).toInt(8);
        f.levelSince = UtcTime::fromMSecsSinceEpoch(now - qint64(sinceMin) * 60000);
        if (!known || (e.kind == AlertEffect::Notify && !f.episodeSince.isValid()))
            f.episodeSince = f.levelSince;
        if (e.type == AlertType::Missing)
            f.lastReadingAt = f.levelSince;
    }
    if (op.contains(QStringLiteral("limit")))
        f.limit = op.value(QStringLiteral("limit")).toInt();
    else if (e.type != AlertType::Missing)
        f.limit = e.type == AlertType::LL ? 54 : e.type == AlertType::L ? 70 : e.type == AlertType::H ? 180 : 250;
    if (op.contains(QStringLiteral("silent")))
        f.silent = e.type != AlertType::Missing && op.value(QStringLiteral("silent")).toBool();
    f.cause = e.cause;
    f.acknowledgementKnown = true;
    f.neverAcknowledged = op.value(QStringLiteral("never_ack")).toBool(false);
    f.clearedAt = e.at;
    s.facts = f;

    m_pending.insert(key, f);
    m_presenter->deliver(e);
    m_pending.remove(key);
    // Told like a real change, so the resident notice and the D-Bus signal follow it (also after the
    // user deleted a synthetic notification, which reaches this function through acknowledge()).
    if (m_events && (e.kind == AlertEffect::Notify || e.kind == AlertEffect::Update || e.kind == AlertEffect::Withdraw))
        m_events->alertChanged(e.subject, alertSlotName(e.slot), alertStateName(e.state));

    if (e.kind == AlertEffect::Withdraw) {
        m_slots.remove(key);
    } else {
        if (e.kind == AlertEffect::Notify)
            s.acknowledged = false;
        if (e.kind == AlertEffect::Update && e.state == AlertState::Acknowledged)
            s.acknowledged = true;
        m_slots.insert(key, s);
    }
    return true;
}

bool DevFeed::acknowledge(const QUuid &subject, AlertSlot slot)
{
    if (!isSynthetic(subject))
        return false;
    const QString key = slotKey(subject, slot);
    if (!m_slots.contains(key) || m_slots.value(key).acknowledged)
        return false;
    QJsonObject op = alertOp("update", 1, alertSlotName(slot).toLatin1().constData(), 0, -1, "acknowledged");
    for (int i = 1; i <= 4; ++i)
        if (subjectId(i) == subject)
            op.insert(QStringLiteral("subject"), i);
    QString error;
    return applyAlert(op, &error);
}

QList<ResidentAlert> DevFeed::residentAlerts() const
{
    QList<ResidentAlert> list;
    for (QHash<QString, Slot>::const_iterator it = m_slots.constBegin(); it != m_slots.constEnd(); ++it) {
        ResidentAlert a;
        a.kind = NotificationCatalog::alarmKindOf(it.value().type);
        a.label = m_labelOf.value(QUuid(it.key().section(QLatin1Char('/'), 0, 0)), QStringLiteral("?"));
        a.acknowledged = it.value().acknowledged;
        list << a;
    }
    return list;
}

QStringList DevFeed::silentLabels() const
{
    QStringList labels;
    for (QHash<QString, Slot>::const_iterator it = m_slots.constBegin(); it != m_slots.constEnd(); ++it)
        if (it.value().facts.silent) {
            const QString l = m_labelOf.value(QUuid(it.key().section(QLatin1Char('/'), 0, 0)), QStringLiteral("?"));
            if (!labels.contains(l))
                labels << l;
        }
    return labels;
}

EffectFacts DevFeed::factsOf(const AlertEffect &effect) const
{
    if (isSynthetic(effect.subject)) {
        const QString key = slotKey(effect.subject, effect.slot);
        if (m_pending.contains(key))
            return m_pending.value(key);
        // A synthetic effect that did not come through apply() (the user's deletion): the facts noted.
    }
    return m_inner ? m_inner->factsOf(effect) : EffectFacts();
}

} // namespace lm

#endif // LIBREMON_DEVSOURCE
