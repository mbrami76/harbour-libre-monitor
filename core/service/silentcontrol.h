// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SILENTCONTROL_H
#define LM_SILENTCONTROL_H

// SILENT MODE, SEEN FROM THE SERVICE.
//
// The mode itself (per Subject, silent or not, the end time, the journal kind `silent`, the one
// `Update` that stops a sounding tone when it starts) belongs engine
// (`AlertCoordinator::setSilent`, `endSilent`, `silentOn`, `silentUntil`, branch mod/alerts-2).
// The service only needs four questions answered, so it asks through this small interface and does
// not depend on the engine's classes: the D-Bus methods call set() and end(), `Alerts()` and
// `Status()` read isOn() and until(), and the notifier hears of the mode through the `silent` flag
// of the effects. Until the branch is merged the interface is filled by a fake in the tests
// and by nothing in the package (SetSilent then answers "unsupported"); the adapter over the real
// coordinator is a few lines and is the only thing to add when it is.
//
// A mode is never about the monitor itself: no-data alerts, the interruption notice, the approval
// notices, the guard notice and the Do not disturb notice are not covered, and
// that is enforced where the notifications are built, not here.

#include "alerts/alertcoordinator.h"
#include "alerts/silentbook.h"
#include "time/utctime.h"

#include <QUuid>

namespace lm {

// The special value of the `minutes` argument besides "that many minutes" and 0 (without limit),
// kSilentUntilTomorrow (-1: the next local-time boundary chosen), is defined
// SilentBook and used here.

class SilentControl
{
public:
    virtual ~SilentControl() {}
    // Turns the mode on for `minutes` (> 0), without limit (0) or until tomorrow (kSilentUntilTomorrow);
    // false for a Subject the service does not follow or a length that cannot be one.
    virtual bool set(const QUuid &subject, int minutes) = 0;
    // Turns it off before its end; false when the Subject was not silent.
    virtual bool end(const QUuid &subject) = 0;
    virtual bool isOn(const QUuid &subject) const = 0;
    // The end of a timed mode; invalid for a mode without limit or when the Subject is not silent.
    virtual UtcTime until(const QUuid &subject) const = 0;
};

// The real thing: the coordinator answers the four questions (the book, its journal kind
// `silent` and the one Update that stops a sounding tone when a mode starts are all inside it).
class CoordinatorSilentControl : public SilentControl
{
public:
    explicit CoordinatorSilentControl(AlertCoordinator *coordinator) : m_coordinator(coordinator) {}
    bool set(const QUuid &subject, int minutes) { return m_coordinator->setSilent(subject, minutes); }
    bool end(const QUuid &subject) { return m_coordinator->endSilent(subject); }
    bool isOn(const QUuid &subject) const { return m_coordinator->silentOn(subject); }
    UtcTime until(const QUuid &subject) const { return m_coordinator->silentUntil(subject); }

private:
    AlertCoordinator *m_coordinator;
};

} // namespace lm

#endif
