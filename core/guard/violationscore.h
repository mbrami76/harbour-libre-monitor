// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_VIOLATIONSCORE_H
#define LM_VIOLATIONSCORE_H

// Violation score with time decay (REQ-GRD-04) and token bucket (REQ-GRD-01): the two small
// numeric helpers of the guard. Both are plain values driven by times passed in, so they are
// deterministic and testable without a clock.

#include "time/utctime.h"

#include <QtGlobal>

namespace lm {

// The score of one connection. Pattern: lazy exponential decay. Instead of a timer that
// lowers the score, the stored value is brought up to date only when it is read or changed:
// value * 2^(-dt / half-life). A soft violation that is long past thus no longer counts,
// while a hard one that just happened still does; that is what lets "two hard violations
// within about half an hour" trip and one never does.
class ViolationScore
{
public:
    explicit ViolationScore(qint64 halfLifeMs = 30LL * 60 * 1000);

    double value(const UtcTime &now) const;
    // Brings the value up to date, adds the weight and returns the new value.
    double add(const UtcTime &now, double weight);
    void reset();

private:
    qint64 m_halfLifeMs;
    double m_value;
    qint64 m_atMs;
    bool m_set;
};

// A token bucket: `capacity` tokens, refilled continuously at `refillPerS`. take() returns
// how long the caller must wait before the amount would have been available (0 = it was);
// the tokens are spent either way, so a pause by the caller equals the debt.
class TokenBucket
{
public:
    TokenBucket(double capacity = 1, double refillPerS = 1);

    void configure(double capacity, double refillPerS);
    // Tokens available at `now` (after refill), without spending.
    double level(const UtcTime &now) const;
    // Fraction of the capacity available at `now`.
    double fraction(const UtcTime &now) const;
    // Spends `amount`; returns milliseconds to wait, 0 when the bucket had enough.
    qint64 take(const UtcTime &now, double amount);
    // True when `amount` is available at `now`; spends nothing.
    bool has(const UtcTime &now, double amount) const { return level(now) >= amount; }
    // Milliseconds until `amount` is available, 0 if it is already.
    qint64 msUntil(const UtcTime &now, double amount) const;

private:
    void refill(const UtcTime &now) const;

    double m_capacity;
    double m_refillPerS;
    mutable double m_tokens;
    mutable qint64 m_atMs;
    mutable bool m_set;
};

} // namespace lm

#endif
