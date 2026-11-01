// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DONOTDISTURB_H
#define LM_DONOTDISTURB_H

// DO NOT DISTURB, WATCHED (closed).
//
// While Do not disturb is on, the phone silences every sound and vibration a notification can ask
// for, the urgent alarms included. The application cannot change that, but it can say
// so: when Do not disturb turns on the service posts the notice "alerts will be silent", withdraws it
// when it ends, and the resident notice carries a line while it holds. One notification per change,
// no repeating. The state is read from the dconf value `/lipstick/do_not_disturb` through the QML
// type `Nemo.Configuration` `ConfigurationValue` in a headless engine (the Nemo implementation is in daemon/phoneports); the profile (general, silent) cannot be read from the
// sandbox, which is why a static sentence in the texts tells the user about it, and
// nothing here builds that. There is NO system alarm in this round (the `timed` library fails the
// Harbour validator).
//
// Pattern: a port for the state (DoNotDisturbSource) and a small watcher that turns changes into
// notices and into a callback for the resident notice. Tests use a fake source.

#include "service/alertnotification.h"

#include <functional>

namespace lm {

class DoNotDisturbSource
{
public:
    typedef std::function<void(bool on)> ChangeHandler;
    virtual ~DoNotDisturbSource() {}
    // The state now; false when it is unknown (the value is unset or the source is not available).
    virtual bool isOn() const = 0;
    // Called when the state changes, from the event loop; an empty handler stops the calls.
    virtual void setChangeHandler(const ChangeHandler &handler) = 0;
};

class DoNotDisturbWatcher
{
public:
    // Neither pointer is owned. `changed` is called with the new state, also once at start().
    DoNotDisturbWatcher(DoNotDisturbSource *source, AlertNotificationPresenter *presenter,
                        const std::function<void(bool on)> &changed);
    ~DoNotDisturbWatcher();

    // Reads the state, posts or withdraws the notice to match it, and starts listening.
    void start();
    void stop();
    bool isOn() const { return m_on; }

private:
    void apply(bool on, bool force);

    DoNotDisturbSource *m_source;
    AlertNotificationPresenter *m_presenter;
    std::function<void(bool)> m_changed;
    bool m_on;
    bool m_known;
};

} // namespace lm

#endif
