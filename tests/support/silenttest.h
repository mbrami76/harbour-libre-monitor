// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SILENTTEST_H
#define LM_SILENTTEST_H

// A silent mode in memory for the tests of the service: the same
// four questions the coordinator answers (core/service/silentcontrol.h), with a clock the
// test moves. It is not the engine's book: the durations are the ones the offered buttons use, and
// "until tomorrow" is the next midnight UTC. Synthetic only.

#include "service/silentcontrol.h"

#include <QHash>
#include <functional>

namespace lm {
namespace testing {

class FakeSilentControl : public SilentControl
{
public:
    explicit FakeSilentControl(const std::function<qint64()> &nowMs) : m_now(nowMs) {}

    bool set(const QUuid &subject, int minutes)
    {
        if (minutes < kSilentUntilTomorrow || minutes > 366 * 24 * 60)
            return false;
        Mode m;
        m.on = true;
        if (minutes > 0)
            m.untilMs = m_now() + qint64(minutes) * 60000;
        else if (minutes == kSilentUntilTomorrow)
            m.untilMs = (m_now() / 86400000 + 1) * 86400000;
        else
            m.untilMs = 0;                       // without limit
        m_modes.insert(subject, m);
        ++sets;
        return true;
    }
    bool end(const QUuid &subject)
    {
        if (!isOn(subject))
            return false;
        m_modes.remove(subject);
        return true;
    }
    bool isOn(const QUuid &subject) const
    {
        const Mode m = m_modes.value(subject);
        return m.on && (m.untilMs == 0 || m_now() < m.untilMs);
    }
    UtcTime until(const QUuid &subject) const
    {
        const Mode m = m_modes.value(subject);
        return isOn(subject) && m.untilMs ? UtcTime::fromMSecsSinceEpoch(m.untilMs) : UtcTime();
    }

    int sets = 0;

private:
    struct Mode { Mode() : on(false), untilMs(0) {} bool on; qint64 untilMs; };
    std::function<qint64()> m_now;
    QHash<QUuid, Mode> m_modes;
};

} // namespace testing
} // namespace lm

#endif
