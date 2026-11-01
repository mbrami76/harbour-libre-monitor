// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBRETIMESTAMP_H
#define LM_LIBRETIMESTAMP_H

#include "time/utctime.h"

#include <QString>

namespace lm {

// Parses the source's timestamp format "M/D/YYYY h:mm:ss AM|PM" (REQ-DAT-11,
//) and returns it as UTC. The caller passes FactoryTimestamp,
// which is UTC; this function does not know the zone and applies none.
//
// Strict and independent of any locale (no QLocale): month, day and hour have one or two
// digits, the year four, minutes and seconds two; single spaces; the marker is exactly
// "AM" or "PM" in capitals; 12 AM is hour 0 and 12 PM is hour 12; impossible dates are
// rejected. Anything else, including leading or trailing text, gives false and leaves
// *out unchanged. Nothing is guessed.
bool parseLibreTimestamp(const QString &text, UtcTime *out);

} // namespace lm

#endif
