// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/linkpolicy.h"

namespace lm {

NullPolicy::NullPolicy()
{
}

LinkLimits NullPolicy::limits() const
{
    return LinkLimits();
}

MayStartResult NullPolicy::mayStart(const StartRequest &)
{
    return MayStartResult::allowed();
}

void NullPolicy::onSpawned(const LinkId &)
{
}

void NullPolicy::onExited(const LinkId &, const ExitStatus &)
{
}

MayStartResult NullPolicy::onAdmitted(const LinkId &)
{
    return MayStartResult::allowed();
}

Action NullPolicy::onLine(const LinkId &, qint64, const UtcTime &)
{
    return Action::proceed();
}

Action NullPolicy::onBatch(const LinkId &, const QString &, int, qint64)
{
    return Action::proceed();
}

Action NullPolicy::onStderr(const LinkId &, qint64)
{
    return Action::proceed();
}

// A hard violation of the contract ends the adapter at once: the command-line
// program has no guard, and an adapter that breaks the protocol is not trusted any further
// (REQ-ADP-08). A soft one is only recorded. The driver applies the answer immediately, so
// this also covers an adapter that sends a hard violation and then goes quiet.
Action NullPolicy::onViolation(const Violation &violation)
{
    m_violations.append(violation);
    return violation.severity == Severity::Hard ? Action::kill() : Action::proceed();
}

void NullPolicy::onCommandFinished(const CommandOutcome &)
{
}

bool NullPolicy::sawHardViolation() const
{
    for (int i = 0; i < m_violations.size(); ++i)
        if (m_violations.at(i).severity == Severity::Hard)
            return true;
    return false;
}

} // namespace lm
