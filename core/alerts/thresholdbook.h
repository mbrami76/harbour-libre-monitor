// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_THRESHOLDBOOK_H
#define LM_THRESHOLDBOOK_H

// The per-Subject thresholds and their append-only history (REQ-THR-01, REQ-THR-02, REQ-ALR-05,
//).
//
// Pattern: a small in-memory ledger folded from journal entries of the kind `threshold` (as the
// guard folds `connection-state`). A change is first CHECKED (no side effect, so a refused
// change leaves nothing behind), then the caller writes the returned entries to the journal and
// APPLIES them; `edit` does both checks and applying for tests and for callers that write first
// and cannot fail. History is never rewritten: the current value of a boundary is the newest
// entry for it, and "what were the thresholds when this fired" is answered by the id of the entry
// in force, which every alert journal entry carries.
//
// Only L and H are editable, per Subject. LL and HH are the global values, always, and no edit
// reaches them. A change that makes L LOWER or H HIGHER than the global value (looser: the Subject
// is alerted later) needs the caller's confirmation AND a challenge answered: the caregiver types
// the new value again, without it being shown, and it must match (REQ-THR-02: the typed repetition replaces a mandatory comment, because it
// stops a blind "OK" better than a sentence does). Enforced here and not only in the UI. A stricter
// change needs neither. A change carries no comment: the optional note and
// had no reader, so it was removed; older journal entries that still hold one are read
// without it.

#include "alerts/thresholds.h"
#include "time/utctime.h"

#include <QHash>
#include <QString>
#include <QUuid>
#include <QVector>

#include <functional>

namespace lm {

enum class Boundary { L, H };

QString boundaryName(Boundary b);                       // "L" or "H" (the journal value)
bool boundaryFromName(const QString &name, Boundary *b);

// One entry of the history: the boundary of a Subject moved from `previous` to `value` (mg/dL).
struct ThresholdChange
{
    ThresholdChange() : boundary(Boundary::L), value(0), previous(0) {}

    QUuid id;               // the entry's own id; alert entries cite it as "thresholds in force"
    QUuid subject;
    Boundary boundary;
    int value;
    int previous;
    UtcTime time;           // no comment: removed
};

enum class EditResult {
    Accepted,
    NoChange,           // both numbers equal the current ones; nothing to record
    InvalidValue,       // not a whole positive number of mg/dL in a plausible range
    Inconsistent,       // would break LL < L < H < HH
    NeedsConfirmation,  // looser than the global value and not confirmed
    ConfirmationMismatch, // looser, confirmed, but the value typed again is missing or differs
    // The next two are never answered by the book itself; the coordinator answers them when the
    // question cannot be put to the book at all (alerts in the service, D-Bus layer).
    UnknownSubject,     // the Subject is not one the service follows
    NotSaved            // the edit was fine but the journal could not take it; nothing changed
};

// The caregiver's answer to the challenge of a looser change. `typedL` and `typedH` are the values
// typed AGAIN, in whole mg/dL (convert an mmol/L entry with mmolEditToMgDl first), -1 = nothing
// typed. Only the boundaries that are loosened and move are checked.
struct Confirmation
{
    Confirmation() : confirmed(false), typedL(-1), typedH(-1) {}
    static Confirmation typed(int l, int h) { Confirmation c; c.confirmed = true; c.typedL = l; c.typedH = h; return c; }

    bool confirmed;
    int typedL, typedH;
};

QString editResultName(EditResult r);                   // stable words for the D-Bus layer

struct EditOutcome
{
    EditOutcome() : result(EditResult::NoChange), hasOffending(false), offending(Boundary::L),
                    lMin(0), lMax(0), hMin(0), hMax(0), globalL(kGlobalL), globalH(kGlobalH) {}

    EditResult result;
    // What a screen needs to EXPLAIN a refusal in words with numbers ( the page shows the
    // units and says what was violated). All in whole mg/dL; the page shows them in the Subject's
    // unit next to the unit's name (mgDlToMmolForDisplay for mmol/L).
    //  - Inconsistent: the allowed range of L (above LL and below the H asked for) and of H (above
    //    the L asked for and below HH), so the message can say "L must be between 55 and 179".
    //  - ConfirmationMismatch: `offending` is the boundary whose repetition was missing or wrong.
    //  - NeedsConfirmation / ConfirmationMismatch: `globalL` and `globalH`, the standard values the
    //    change is looser than.
    //  - InvalidValue: `offending` is the boundary with an impossible number.
    bool hasOffending;
    Boundary offending;
    int lMin, lMax, hMin, hMax;
    int globalL, globalH;
    QVector<ThresholdChange> changes;   // to journal; empty unless Accepted
    Thresholds thresholds;              // what the Subject's thresholds will be after applying
};

// Creates the ids of new entries. The default is a random UUID; tests inject a counter so that
// runs are repeatable (the engine and the book have no hidden randomness or clock).
typedef std::function<QUuid()> IdFactory;

class ThresholdBook
{
public:
    explicit ThresholdBook(const IdFactory &ids = IdFactory());

    // The thresholds in force for a Subject: its newest L and H entries over the global defaults.
    Thresholds thresholdsFor(const QUuid &subject) const;

    // The id of the newest entry that touches the Subject (null: the Subject still has the global
    // defaults). Alert entries carry it (REQ-ALR-05).
    QUuid idInForce(const QUuid &subject) const;

    // Validates a request for new L and H values (whole mg/dL; use mmolEditToMgDl first for an
    // edit typed in mmol/L). Changes nothing.
    EditOutcome check(const QUuid &subject, int l, int h, const Confirmation &confirmation,
                      const UtcTime &now) const;

    // Appends entries to the ledger and makes them current. Used after the journal accepted them
    // and when folding the journal at start. Entries of an id already seen are ignored, so
    // applying twice is harmless.
    void apply(const QVector<ThresholdChange> &changes);

    // check + apply in one step.
    EditOutcome edit(const QUuid &subject, int l, int h, const Confirmation &confirmation,
                     const UtcTime &now);

    // The history of a Subject, oldest first (by time, then id).
    QVector<ThresholdChange> history(const QUuid &subject) const;

    // Rebuilds the ledger from journal entries, in any order (replaces the content).
    void restore(const QVector<ThresholdChange> &entries);

private:
    struct SubjectLedger
    {
        QVector<ThresholdChange> entries;   // sorted by (time, id)
    };

    static bool before(const ThresholdChange &a, const ThresholdChange &b);
    void insertSorted(SubjectLedger *ledger, const ThresholdChange &c);

    QHash<QUuid, SubjectLedger> m_subjects;
    IdFactory m_ids;
};

} // namespace lm

#endif
