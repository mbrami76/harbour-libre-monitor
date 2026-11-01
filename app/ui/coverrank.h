// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_COVERRANK_H
#define LM_UI_COVERRANK_H

// Which Subjects the application cover shows when there are several.
// The rank answers "which Subject needs the caregiver first":
//   1  hypoglycaemia: a fresh reading in LL or L, the lower value first
//   2  not being monitored: stale, stopped by the guard, needs the user (longest first)
//   3  hyperglycaemia: a fresh reading in H or HH, the higher value first
//   4  everything else: in range or new, the one nearer a limit first
// Ties keep the configuration order. Pure functions, tested on the host.

#include "subjectstate.h"

#include <QList>

namespace lm {
namespace ui {

// 1 (most urgent) to 4.
int coverRank(StateId id);

struct CoverCandidate
{
    CoverCandidate() : id(StateId::NoDataYet), staleSinceAgeS(-1), valueMgdl(0), limitDistanceMgdl(1e9) {}

    StateId id;
    qint64 staleSinceAgeS;      // -1 = not stale or unknown
    double valueMgdl;           // the fresh reading, converted exactly; 0 when not fresh
    // How far a FRESH reading is from the nearest limit (LL, L, H or HH), in mg/dL; the smaller,
    // the nearer to a limit. Large for a reading that is not fresh.
    double limitDistanceMgdl;
};

// The index in `subjects` (in configuration order) of the Subject to show; -1 for an empty list.
int pickCoverSubject(const QList<CoverCandidate> &subjects);

// The Subjects the cover shows, most urgent first, at most `maxRows` (rank order above).
// Returns indices into `subjects`.
QList<int> orderForCover(const QList<CoverCandidate> &subjects, int maxRows);

} // namespace ui
} // namespace lm

#endif
