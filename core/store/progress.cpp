// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/progress.h"

namespace lm {

namespace {

const qint64 kMinIntervalMs = 100;

} // namespace

QString phaseKey(OperationProgress::Phase phase)
{
    switch (phase) {
    case OperationProgress::CheckingSpace: return QStringLiteral("checking-space");
    case OperationProgress::Compacting: return QStringLiteral("compacting");
    case OperationProgress::Purging: return QStringLiteral("purging");
    case OperationProgress::Finalizing: return QStringLiteral("finalizing");
    case OperationProgress::Done: return QStringLiteral("done");
    }
    return QString();
}

ProgressReporter::ProgressReporter(const ProgressFn &callback, OperationProgress::Phase work)
    : m_callback(callback), m_phase(OperationProgress::CheckingSpace),
      m_started(false), m_cancelled(false), m_lastFraction(0)
{
    // Weights (phone: dry run 2.5 s, the whole compaction 21 s, of which the
    // rebuild is a good part): an estimate; the bar only has to move steadily.
    // The same weights serve the purge: a dry run, the deletion, the rewrite of the file.
    m_phases << OperationProgress::CheckingSpace << work << OperationProgress::Finalizing;
    m_weights << 0.15 << 0.55 << 0.30;
}

bool ProgressReporter::report(OperationProgress::Phase phase, qint64 done, qint64 total, bool force)
{
    if (m_cancelled)
        return false;
    if (!m_callback)
        return true;

    const bool phaseChanged = !m_started || phase != m_phase;
    const bool phaseEnd = total > 0 && done >= total;
    if (!force && !phaseChanged && !phaseEnd && m_timer.isValid() && m_timer.elapsed() < kMinIntervalMs)
        return true;

    OperationProgress p;
    p.phase = phase;
    p.done = done;
    p.total = total;
    p.steps = m_phases.size();
    if (phase == OperationProgress::Done) {
        p.step = p.steps;
        p.fraction = 1.0;
    } else {
        double before = 0;
        int step = 0;
        double weight = 0;
        for (int i = 0; i < m_phases.size(); ++i) {
            if (m_phases[i] == int(phase)) {
                step = i + 1;
                weight = m_weights[i];
                break;
            }
            before += m_weights[i];
        }
        p.step = step > 0 ? step : 1;
        const double inPhase = total > 0 ? qMin(1.0, double(done) / double(total)) : 0.0;
        p.fraction = qMin(1.0, before + weight * inPhase);
    }
    if (p.fraction < m_lastFraction)
        p.fraction = m_lastFraction;       // never decreasing
    m_lastFraction = p.fraction;

    m_phase = phase;
    m_started = true;
    m_timer.restart();
    if (!m_callback(p))
        m_cancelled = true;
    return !m_cancelled;
}

} // namespace lm
