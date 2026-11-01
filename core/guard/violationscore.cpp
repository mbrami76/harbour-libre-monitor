// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "guard/violationscore.h"

#include <cmath>

namespace lm {

ViolationScore::ViolationScore(qint64 halfLifeMs)
    : m_halfLifeMs(halfLifeMs > 0 ? halfLifeMs : 1), m_value(0), m_atMs(0), m_set(false) {}

double ViolationScore::value(const UtcTime &now) const
{
    if (!m_set || !now.isValid())
        return m_value;
    const qint64 dt = now.msecsSinceEpoch() - m_atMs;
    if (dt <= 0)
        return m_value;     // a clock that steps back never raises the score
    return m_value * std::pow(0.5, double(dt) / double(m_halfLifeMs));
}

double ViolationScore::add(const UtcTime &now, double weight)
{
    m_value = value(now) + weight;
    if (now.isValid()) {
        // Never move the reference point backwards, or a later read would decay twice.
        if (!m_set || now.msecsSinceEpoch() > m_atMs)
            m_atMs = now.msecsSinceEpoch();
        m_set = true;
    }
    return m_value;
}

void ViolationScore::reset()
{
    m_value = 0;
    m_set = false;
}

TokenBucket::TokenBucket(double capacity, double refillPerS)
    : m_capacity(capacity), m_refillPerS(refillPerS), m_tokens(capacity), m_atMs(0), m_set(false) {}

void TokenBucket::configure(double capacity, double refillPerS)
{
    m_capacity = capacity;
    m_refillPerS = refillPerS;
    m_tokens = capacity;
    m_set = false;
}

void TokenBucket::refill(const UtcTime &now) const
{
    if (!now.isValid())
        return;
    if (!m_set) {
        m_atMs = now.msecsSinceEpoch();
        m_set = true;
        return;
    }
    const qint64 dt = now.msecsSinceEpoch() - m_atMs;
    if (dt <= 0)
        return;
    m_tokens += m_refillPerS * double(dt) / 1000.0;
    if (m_tokens > m_capacity)
        m_tokens = m_capacity;
    m_atMs = now.msecsSinceEpoch();
}

double TokenBucket::level(const UtcTime &now) const
{
    refill(now);
    return m_tokens;
}

double TokenBucket::fraction(const UtcTime &now) const
{
    return m_capacity > 0 ? level(now) / m_capacity : 1.0;
}

qint64 TokenBucket::take(const UtcTime &now, double amount)
{
    refill(now);
    if (m_tokens >= amount) {
        m_tokens -= amount;
        return 0;
    }
    const double missing = amount - m_tokens;
    m_tokens = 0;
    if (m_refillPerS <= 0)
        return 0;       // a bucket that never refills cannot say when; callers cap the pause
    return qint64(std::ceil(missing / m_refillPerS * 1000.0));
}

qint64 TokenBucket::msUntil(const UtcTime &now, double amount) const
{
    refill(now);
    if (m_tokens >= amount || m_refillPerS <= 0)
        return 0;
    return qint64(std::ceil((amount - m_tokens) / m_refillPerS * 1000.0));
}

} // namespace lm
