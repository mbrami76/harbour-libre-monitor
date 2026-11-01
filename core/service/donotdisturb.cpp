// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/donotdisturb.h"

namespace lm {

DoNotDisturbWatcher::DoNotDisturbWatcher(DoNotDisturbSource *source, AlertNotificationPresenter *presenter,
                                         const std::function<void(bool on)> &changed)
    : m_source(source), m_presenter(presenter), m_changed(changed), m_on(false), m_known(false)
{
}

DoNotDisturbWatcher::~DoNotDisturbWatcher() { stop(); }

void DoNotDisturbWatcher::start()
{
    if (!m_source)
        return;
    m_source->setChangeHandler([this](bool on) { apply(on, false); });
    apply(m_source->isOn(), true);          // the state at start: posted or withdrawn to match, whatever the last run left
}

void DoNotDisturbWatcher::stop()
{
    if (m_source)
        m_source->setChangeHandler(DoNotDisturbSource::ChangeHandler());
}

// One notification per change: a repeated report of the same state does nothing, so the notice is
// never raised again while Do not disturb stays on ( no nagging).
void DoNotDisturbWatcher::apply(bool on, bool force)
{
    if (!force && m_known && on == m_on)
        return;
    m_on = on;
    m_known = true;
    if (m_presenter) {
        // `force` is the reading at start: a start is not a change, so the entry the last run left is
        // adopted and nothing is raised again.
        if (force)
            m_presenter->adoptDoNotDisturb(on);
        else
            m_presenter->notifyDoNotDisturb(on);
    }
    if (m_changed)
        m_changed(on);
}

} // namespace lm
