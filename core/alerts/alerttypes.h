// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTTYPES_H
#define LM_ALERTTYPES_H

// The vocabulary of the alert engine (REQ-ALR-01): the five
// alert types, the three slots they live in, and the states of a slot. Plain values and their
// names; no logic beyond naming, so every other file in core/alerts/ can share them.
//
// Why "slots": for one Subject and one direction there is room for ONE alert, whose level may
// change (low is L or LL, high is H or HH, and missing data is a slot of its own). The more
// severe level takes the slot, so the user never gets an L and an LL notification at once.
//
// The names below are written into the journal (kinds `alert` and `threshold`) and
// so they are a file format: change them only together with a format version.

#include <QString>

namespace lm {

// The alert types LL and HH are "urgent": no setting can delay or
// silence them (REQ-THR-01).
enum class AlertType { LL, L, H, HH, Missing };

// One slot per Subject and direction. Missing data has its own slot because it is independent of
// the glucose value.
enum class AlertSlot { Low, High, Missing };

// REQ-ALR-01 plus the snooze. `Cleared` is never a resting state: it is the name of the transition
// that ends an alert (the engine goes to Normal at once and the journal keeps the "cleared" line
// as the record).
enum class AlertState { Normal, Pending, Active, Acknowledged, Snoozed, Cleared };

AlertSlot slotOf(AlertType type);

// 0 for no alert; within one slot a higher number is the more severe level (L < LL, H < HH).
int severityOf(AlertType type);

// LL and HH: never delayed, never snoozed, never silenced by a setting (REQ-THR-01).
bool isUrgent(AlertType type);

// Journal and display names. The `...FromName` functions return false for a name they do not know
// and leave the result untouched, so a corrupt journal line is skipped, not guessed.
QString alertTypeName(AlertType type);                  // "LL", "L", "H", "HH", "MISSING"
bool alertTypeFromName(const QString &name, AlertType *type);
QString alertSlotName(AlertSlot slot);                  // "low", "high", "missing"
bool alertSlotFromName(const QString &name, AlertSlot *slot);
QString alertStateName(AlertState state);               // "normal", "pending", "active", ...
bool alertStateFromName(const QString &name, AlertState *state);

} // namespace lm

#endif
