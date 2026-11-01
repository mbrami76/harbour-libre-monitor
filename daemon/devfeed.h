// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DEVFEED_H
#define LM_DEVFEED_H

// A DEVELOPER-ONLY WAY TO FEED SYNTHETIC EFFECTS.
//
// A real reading rarely crosses a threshold, and a developer must be able to SEE and HEAR every kind
// of notification on the phone: the five alarms, two Subjects at once, a reminder, a silent mode, a
// Do not disturb change, the notices. This feed builds the same effects the alert engine would
// build and hands them to the same notifier (the presenter), so what appears on the phone is exactly
// what a real alert would show, without waiting for glucose and without the engine, the store or the
// source being involved. The notifier's own state (ids, arbiter, "ended" entries, resident list) is
// the real one; only the cause of the effect is made up.
//
// It exists ONLY when the code is built with the development flag `devsource` (qmake
// CONFIG+=devsource, implied by hostbuild): the whole of this file and the D-Bus method that calls it
// are compiled out of a release package, which has neither the code nor a switch (REQ-SIM-06). The
// Harbour check runs on the release package. Synthetic Subjects only: four fixed identifiers made
// up for this purpose, with labels "AAA" to "DDD"; a call naming anything else is refused, so the
// feed can never touch the real Subject or put a real label into a notification.
//
// The call is one JSON text: {"scenario":"<name>"} for a ready-made step of the phone protocol, or
// {"ops":[...]} for a list of operations. Operations (all optional fields have defaults):
//   {"op":"alert","kind":"notify|remind|update|withdraw","subject":1..4,"slot":"low|high|missing",
//    "type":"LL|L|H|HH|MISSING","state":"active|acknowledged|cleared|snoozed","since_min":8,
//    "limit":54,"silent":false,"never_ack":false,"cause":"source-unreachable"}
//   {"op":"interruption","from_min":90,"to_min":20}     {"op":"approval"} {"op":"approval_done"}
//   {"op":"guard","attempts":3,"cause":"score"}  {"op":"guard_done"}  {"op":"report"}  {"op":"report_done"}
//   {"op":"dnd","on":true}   {"op":"label","subject":1,"label":"AAA"}   {"op":"clear"}
// A deletion of a synthetic notification by the user acknowledges the synthetic slot, as it would a real one.

#ifdef LIBREMON_DEVSOURCE

#include "service/alertnotification.h"
#include "service/residentnotice.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>

namespace lm {

class DevFeed : public EffectFactsReader
{
public:
    // Neither pointer is owned. `now` is the service's clock; `inner` is the reader used for every
    // effect that is not synthetic.
    DevFeed(AlertNotificationPresenter *presenter, SubjectLabelBook *labels, Clock *now,
            const EffectFactsReader *inner, const std::function<void(bool)> &dndChanged,
            ServiceEventSink *events = 0);

    // Runs a scenario or a list of operations. The answer is JSON: {"ok":true,"applied":N} or
    // {"ok":false,"error":"<fixed key>","applied":N}.
    QString apply(const QString &json);

    // The names of the ready-made scenarios, for the protocol and the tests.
    static QStringList scenarioNames();

    static QUuid subjectId(int number);             // 1..4, the synthetic Subjects; null otherwise
    static bool isSynthetic(const QUuid &subject);

    // The user deleted a synthetic notification: the slot is acknowledged here, as the engine would.
    bool acknowledge(const QUuid &subject, AlertSlot slot);

    // For the resident notice and the tests: the synthetic alerts that are up, and the silent ones.
    QList<ResidentAlert> residentAlerts() const;
    QStringList silentLabels() const;
    bool doNotDisturb() const { return m_dnd; }

    // EffectFactsReader: the facts the last operation set for a synthetic slot, else the inner reader's.
    EffectFacts factsOf(const AlertEffect &effect) const;

private:
    struct Slot { Slot() : acknowledged(false), type(AlertType::L) {} AlertType type; bool acknowledged; EffectFacts facts; };

    bool applyOp(const QJsonObject &op, QString *error);
    bool applyAlert(const QJsonObject &op, QString *error);
    QList<QJsonObject> scenario(const QString &name, bool *known) const;

    AlertNotificationPresenter *m_presenter;
    SubjectLabelBook *m_labels;
    Clock *m_clock;
    const EffectFactsReader *m_inner;
    QHash<QUuid, QString> m_labelOf;
    QHash<QString, Slot> m_slots;                   // by subject + slot name, the synthetic alerts that exist
    QHash<QString, EffectFacts> m_pending;          // facts for the effect being delivered
    std::function<void(bool)> m_dndChanged;
    ServiceEventSink *m_events;             // told of each synthetic change, like the relay tells it of a real one
    bool m_dnd;
};

} // namespace lm

#endif // LIBREMON_DEVSOURCE

#endif
