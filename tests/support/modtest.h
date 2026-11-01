// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_MODTEST_H
#define LM_MODTEST_H

// Test doubles shared by the guard and scheduler tests: a manual clock, scripted
// random numbers and a recording ingest-log sink. They exist so that no test lets real time
// pass: a test moves the clock by hand and calls the logic, phase 2).

#include "guard/ports.h"

#include <QList>

namespace lm {
namespace testing {

class ManualClock : public Clock
{
public:
    ManualClock() : m_ms(1768000000000LL) {}      // 2026-01-09, an arbitrary fixed start

    UtcTime now() const { return UtcTime::fromMSecsSinceEpoch(m_ms); }
    void advanceMs(qint64 ms) { m_ms += ms; }
    void advanceS(qint64 s) { m_ms += s * 1000; }
    void setMs(qint64 ms) { m_ms = ms; }
    qint64 ms() const { return m_ms; }

private:
    qint64 m_ms;
};

// Returns the scripted numbers in turn, then a fixed fallback. 0.5 makes jitter neutral.
class ScriptedRandom : public RandomSource
{
public:
    ScriptedRandom() : m_fallback(0.5) {}

    double next()
    {
        if (!m_values.isEmpty())
            return m_values.takeFirst();
        return m_fallback;
    }
    void script(const QList<double> &values) { m_values = values; }
    void setFallback(double v) { m_fallback = v; }

private:
    QList<double> m_values;
    double m_fallback;
};

class RecordingIngestSink : public IngestLogSink
{
public:
    RecordingIngestSink() : m_accepting(true) {}

    bool append(const IngestLogEntry &entry)
    {
        if (!m_accepting)
            return false;
        entries.append(entry);
        return true;
    }
    void setAccepting(bool accepting) { m_accepting = accepting; }

    int count(const QString &event) const
    {
        int n = 0;
        for (int i = 0; i < entries.size(); ++i)
            if (entries.at(i).event == event) ++n;
        return n;
    }

    QList<IngestLogEntry> entries;

private:
    bool m_accepting;
};

} // namespace testing
} // namespace lm

#endif
