// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROGRESS_H
#define LM_PROGRESS_H

#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace lm {

// Progress of the long store operations, compaction (compactor.h) and the purge of one
// Subject's old data (purge.h), so the UI can show which phase is running, how far it is and
// that the wait is expected: a year of one Subject takes tens of seconds on the phone.
//
// The callback runs in the thread that runs the operation. It must return quickly and must
// not touch the store; a UI forwards the value with a queued signal. Returning false asks
// the operation to stop: it rolls everything back (the store is untouched) and fails with the
// class "cancelled".
struct OperationProgress
{
    // The phases, in the order they happen.
    enum Phase {
        CheckingSpace,      // the dry run that decides what goes and checks the space (REQ-STO-16); records scanned
        Compacting,         // compaction: scanning again and removing; records scanned
        Purging,            // purge: removing the Subject's old rows; rows done
        Finalizing,         // commit and the rebuild of the file
        Done
    };

    OperationProgress() : phase(CheckingSpace), step(1), steps(1), done(0), total(0), fraction(0) {}

    Phase phase;
    int step;               // 1-based position of the phase among the steps ("step 2 of 3")
    int steps;              // number of steps
    qint64 done;            // within the phase; 0 and total 0 when the size is not known
    qint64 total;
    double fraction;        // 0..1 over the whole operation: an estimate, never decreasing
};

// A stable key for a phase ("checking-space", "compacting", ...) that the UI translates.
QString phaseKey(OperationProgress::Phase phase);

typedef std::function<bool(const OperationProgress &)> ProgressFn;

// Turns the work of an operation into throttled callbacks. A phase change and the end of a
// phase are always reported; within a phase at most one report every 100 ms, so a fast scan
// does not flood the queued signal of the UI.
class ProgressReporter
{
public:
    // "work" is the main phase: Compacting for a compaction, Purging for a purge. The steps are
    // CheckingSpace, work, Finalizing.
    explicit ProgressReporter(const ProgressFn &callback,
                              OperationProgress::Phase work = OperationProgress::Compacting);

    // Returns false if the callback asked to cancel (and keeps returning false).
    bool report(OperationProgress::Phase phase, qint64 done, qint64 total, bool force = false);
    bool cancelled() const { return m_cancelled; }
    bool active() const { return bool(m_callback); }

private:
    QList<int> m_phases;        // the steps, in order
    QList<double> m_weights;    // their share of the whole, summing to 1
    ProgressFn m_callback;
    QElapsedTimer m_timer;
    OperationProgress::Phase m_phase;
    bool m_started;
    bool m_cancelled;
    double m_lastFraction;
};

} // namespace lm

#endif
