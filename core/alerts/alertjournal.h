// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTJOURNAL_H
#define LM_ALERTJOURNAL_H

// The bridge between the engine and the service journal:
// turns an AlertRecord or a ThresholdChange into a JournalEntry of the kind `alert` or
// `threshold` and back, writes the Journal effects the engine produced, and loads the entries at
// start so that the engine and the threshold book can be rebuilt ("the last entry per slot wins",
// as does for the connection state).
//
// Why a separate file: the engine and the book are pure and know nothing of the database; this
// is the only place in core/alerts/ that touches Journal, so a change of the storage stays here.
// Entries that cannot be read back (a damaged line, a kind from a newer format) are skipped, not
// guessed: the fold then falls back to the previous entry of that slot, which is the safe side
// (an alert that cannot be proven cleared is kept).

#include "alerts/alertengine.h"
#include "alerts/silentbook.h"
#include "alerts/thresholdbook.h"
#include "store/journal.h"

namespace lm {

JournalEntry toJournalEntry(const AlertRecord &record);
bool fromJournalEntry(const JournalEntry &entry, AlertRecord *record);

JournalEntry toJournalEntry(const ThresholdChange &change);
bool fromJournalEntry(const JournalEntry &entry, ThresholdChange *change);

// Appends the Journal effects of a list, in order. Returns the number that could NOT be written
// (0 = all written); the others are still attempted so one failure does not hide the rest.
int writeJournalEffects(Journal *journal, const AlertEffects &effects);

// Appends the entries of an accepted threshold edit. False if one was refused.
bool writeThresholdChanges(Journal *journal, const QVector<ThresholdChange> &changes);

// Silent mode (kind `silent`).
JournalEntry toJournalEntry(const SilentChange &change);
bool fromJournalEntry(const JournalEntry &entry, SilentChange *change);
bool writeSilentChange(Journal *journal, const SilentChange &change);
QVector<SilentChange> loadSilentChanges(Journal *journal);

QVector<AlertRecord> loadAlertRecords(Journal *journal);
QVector<ThresholdChange> loadThresholdChanges(Journal *journal);

} // namespace lm

#endif
