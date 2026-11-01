// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_SNAPSHOTBUILDER_H
#define LM_REAL_SNAPSHOTBUILDER_H

// Turns what the real sources say (the configuration file, the service's Status(), the store)
// into the SubjectSnapshot list the screens are built from (app/ui/backend.h). Pure: no D-Bus, no
// SQL, no clock of its own, so the host tests can walk every situation.
//
// Rules this function keeps:
//  - The Subjects are those of the configuration file, in its order (a stable order for the
//    overview); names and labels come only from there (REQ-SUB-02).
//  - When the service does not answer, every Subject is reported stale with the cause "service
//    not running": an old reading must not look current because the one who would say otherwise
//    is dead (REQ-UI-09). The screens add their own age check on top (kUiFreshMaxS).
//  - The trend arrow is attached only when it belongs to the newest stored reading (`trend_for`
//    equals its time); an arrow of another reading is dropped, never shown beside a different
//    value.

#include "snapshotbuilder.h"
#include "statusparser.h"
#include "storequeries.h"
#include "ui/backend.h"

#include <QHash>
#include <QList>

namespace lm {
namespace real {

// One Subject of the configuration file, flattened with its connection.
struct ConfigSubject
{
    QString id, name, label, connection;
};

// `report` is null when the service has not answered (stopped, not yet answered, call failed).
QList<ui::SubjectSnapshot> buildSnapshots(const QList<ConfigSubject> &subjects, const ServiceReport *report,
                                          const QHash<QString, SubjectData> &data,
                                          const QVector<QPair<qint64, qint64> > &serviceGaps, qint64 nowMs);

// How the running service's build relates to this window's.
enum class VersionRelation { Same, Older, Newer };

// How the strip reports the service: `answered` = at least one answer so far, `reachable` = the
// name has a holder now, `report` the last answer (may be null). `relation`: a service that runs, starts
// or waits for approval and is Older is reported as ServiceOlder (the strip asks the user to stop and
// start it), one that is Newer as ServiceNewer (the strip asks the user to close and reopen the
// application). A stopped or stopping service stays Stopped: there is nothing to restart, Start brings the
// installed binary. A failed call stays CallFailed: nothing is known.
ui::ServiceStatus serviceStatusOf(const ServiceReport *report, bool answered, bool reachable, bool callFailed,
                                  VersionRelation relation = VersionRelation::Same);

// Compares two RPM Version-Release strings such as "0.1-3" and "0.1-10": each is split at "-" and "."
// into numbers, compared one by one as numbers (a missing part counts as 0). Returns -1, 0 or 1, and -2
// when either is not made of numbers only (then no order is claimed). Pure.
int compareAppVersions(const QString &a, const QString &b);

// The service's build against this window's own `ownVersion` (LM_APP_VERSION), from its Status():
// Same when the strings are equal; Newer only when the service's version is known and compares higher;
// Older otherwise: lower, ABSENT or not
// comparable (the safe advice, stop and start, is then given). Same when `ownVersion` is empty (a build
// without the define) or nobody answered. Pure.
VersionRelation serviceVersionRelation(const ServiceReport *report, const QString &ownVersion);

} // namespace real
} // namespace lm

#endif
