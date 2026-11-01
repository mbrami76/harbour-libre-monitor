// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICECONFIG_H
#define LM_SERVICECONFIG_H

// The part of the configuration file the service needs: which connections exist, which adapter
// each uses and which Subjects it follows (REQ-SUB-01, REQ-SUB-02). PROVISIONAL: the
// user interface writes the file. The shape below is the minimum that lets the service run.
//
//   { "format": 1,
//     "connections": [
//       { "id": "c1", "adapter": "libre",
//         "subjects": [ { "id": "{uuid}", "ref": "opaque source reference" } ],
//         "current_interval_s": 60, "graph_interval_s": 300, "logbook_interval_s": 21600 } ] }
//
// Names of people are personal data and live in the configuration file only (REQ-SUB-01,
// REQ-SUB-02); this reader ignores every key it does not know, except the short `label` of a
// Subject (at most three characters: what a notification shows on a locked
// screen), so a name present in the file is never loaded into the service, never reaches the scheduler and cannot reach the store, a
// log or a D-Bus message. Credentials are never in this file (REQ-SEC-01).

#include "scheduler/schedulersettings.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QUuid>

namespace lm {

// The three alert keys a Subject's entry may carry, each in whole seconds. The
// value -1 means "absent": the engine's default applies. Nothing is clamped here: the service reads the
// number as written (a negative or non-integer value counts as absent), and the engine clamps it to its
// range so that the one range lives in one place.
struct SubjectAlertKeys
{
    SubjectAlertKeys() : persistenceLowS(-1), persistenceHighS(-1), missingLimitS(-1) {}
    qint64 persistenceLowS;     // `alert_persistence_low_s`: how long L must hold before it alerts
    qint64 persistenceHighS;    // `alert_persistence_high_s`: the same for H
    qint64 missingLimitS;       // `missing_limit_s`: how long without a reading before the no-data alert
};

struct ServiceConfig
{
    ServiceConfig() : ok(false) {}

    bool ok;
    QString error;      // a fixed key, never a path or a value from the file
    QList<ConnectionConfig> connections;
    // The Subjects in file order, and the SHORT LABEL (at most three characters) the user gave
    // some of them. The label is what an alert notification shows on a locked screen instead of a
    // name; it is the one text from the Subject's entry that the service reads.
    QList<QUuid> subjectOrder;
    QHash<QUuid, QString> subjectLabels;
    // The per-Subject alert keys written by the thresholds page; a Subject without any has no entry.
    QHash<QUuid, SubjectAlertKeys> subjectAlertKeys;
    // The application's language, written by the user interface (REQ-UI-13): "it", "en", or
    // empty for the phone's language. The notifier follows it at ConfigChanged without a restart.
    // The display label of each connection (optional key `label` of a connection, at most 24 characters), used
    // by the guard notice instead of an account name or an e-mail address, which the service never loads.
    QHash<QString, QString> connectionLabels;
    QString language;
    // The setting "Show name and value in alerts" (off by default): with it the alarm texts
    // show the newest value, its unit and its trend after the label. The key is `alerts_show_value`.
    bool alertsShowValue = false;
};

ServiceConfig parseServiceConfig(const QByteArray &json);
ServiceConfig loadServiceConfig(const QString &path);

} // namespace lm

#endif
