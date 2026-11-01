// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_SUBJECTSTATE_H
#define LM_UI_SUBJECTSTATE_H

// What the screens say about one Subject: the headline state of the state inventory
// and which buttons go with it. Pure logic; the QML only
// shows the answer (REQ-UI-07).
//
// Pattern: one pure function from the service's report plus the UI's own freshness check to a
// state id. Inputs are the fields of Status() (daemon/serviceruntime.cpp) as plain values, so
// the tests can walk every row of the inventory without a service or a bus.
//
// Assumptions (to be re-checked when the service changes):
//  - the scheduler's state names and cause strings are those of core/scheduler (scheduleStateName,
//    StaleCause, StateCause); an unknown cause never makes the UI show a value as current
//    (state ST-X), because a later service may add causes (ST-X);
//  - the band comes from outside (core/alerts/band.h): this module never compares a value
//    with a threshold.

#include "alerts/band.h"

#include <QString>

namespace lm {
namespace ui {

// The band type lives in the core now (core/alerts/band.h, moved there):
// the alert engine and the screens use one definition. This alias keeps the UI's code and tests
// unchanged.
using Band = lm::Band;

// The headline ids The numbers of the staleness causes (1 to 7)
// are those of REQ-SCH-07.
enum class StateId {
    BandLL, BandL, BandN, BandH, BandHH, BandUnknown,   // B-LL .. B-HH, B-U   (fresh reading)
    NoDataYet,
    Unreachable,                                        // S
    NoNewReadings,                                      // S
    WarmUp,                                             // S
    StoppedByGuard,                                     // S
    ServiceGap,                                         // S
    CredentialsLocked,                                  // S
    LoginRefused,
    TermsMustBeAccepted,
    SourceChanged,
    CredentialsMissing,
    SubjectUnavailable,
    InternalProblem,
    NoCurrentReading                                    // ST-X (any other cause, or the UI's own age check)
};

// The inventory id of a state, for tests and logs: "B-LL", "ST-7a", ...
QString stateIdName(StateId id);
// True for the five band states and the unknown-band one: a reading shown as current.
bool isFresh(StateId id);

// The guard strip (G rows): independent of the headline, shown beside it.
enum class GuardStrip { None, ConnectionStopped, VersionStopped, Interrupted };

struct SubjectInput
{
    SubjectInput() : stale(false), hasReading(false), ageS(-1), staleSinceAgeS(-1),
                     band(Band::Unknown) {}

    // Fields of Status() for the Subject.
    QString state;          // "idle", "fetching", "backoff", "needs-attention", "disabled-by-guard"
    QString cause;          // why, in backoff / needs-attention / disabled-by-guard
    bool stale;
    QString staleCause;     // "source-unreachable", "no-new-readings", ...
    QString staleDetail;    // for example "rate-limited", "auth-failed"
    // The UI's own view of the newest `current` reading in the store.
    bool hasReading;
    qint64 ageS;            // now minus its measurement time; negative = the clock is unclear
    // How long the Subject has been stale, in seconds (-1 unknown): for the Retry offer.
    qint64 staleSinceAgeS;
    Band band;              // the band of that reading
};

struct SubjectView
{
    SubjectView() : id(StateId::NoCurrentReading), guard(GuardStrip::None), band(Band::Unknown),
                    showRetry(false), showReenable(false), showEditAccount(false),
                    showAskAgain(false), showSetUpAccount(false), rateLimited(false) {}

    StateId id;
    GuardStrip guard;
    Band band;              // the band to colour, only for a fresh reading; Unknown otherwise (REQ-UI-05)
    bool showRetry;         // REQ-GRD-14 and the other Retry offers
    bool showReenable;      // REQ-GRD-08
    bool showEditAccount;   // new password, new Subject list
    bool showAskAgain;      // the stored login waits for the system approval
    bool showSetUpAccount;
    bool rateLimited;       // Unreachable with the detail `rate-limited`: a different text
};

SubjectView resolveSubject(const SubjectInput &in);

// The text identifier of the headline, for example "ui-stale-unreachable".
// The QML maps it to the translated words; an identifier here is never display text.
QString stateTextKey(StateId id, bool rateLimited);
// The identifier of a band for the presentation: "ll", "l", "n", "h", "hh"; empty for Unknown. It names
// the badge file (design/export/ui/badge-<key>.svg) and the colour pair; it is never shown as text: the
// letters are not on screen any more, the band word is.
QString bandKey(Band band);

} // namespace ui
} // namespace lm

#endif
