// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICEPLATFORM_H
#define LM_SERVICEPLATFORM_H

// The set of ports that differ between the phone and the PC, handed to the service as one
// value so the wiring (ServiceCore, ServiceRuntime) is the same code in both places. Pattern:
// ports and adapters; this struct is the "platform" half. Nothing in it is owned.

#include "alerts/alertcoordinator.h"
#include "daemon/provisioning.h"
#include "guard/ports.h"
#include "scheduler/schedulerports.h"
#include "service/alertnotification.h"
#include "service/donotdisturb.h"
#include "service/residentnotice.h"
#include "service/serviceports.h"
#include "service/versionwatch.h"

namespace lm {

struct ServicePlatform
{
    ServicePlatform()
        : clock(0), random(0), wake(0), notifier(0), approval(0), secrets(0), credentials(0),
          connectionSettings(0), names(0), alerts(0), labels(0), presenter(0), resident(0), dnd(0),
          stamp(0), activator(0), serviceSettings(0) {}

    ServiceClock *clock;
    RandomSource *random;
    WakeSource *wake;
    Notifier *notifier;
    ApprovalSource *approval;
    SecretStore *secrets;
    // Removes a secret (the probe's). The part of the secret store the service only needs
    // for that one purpose; may be null, then `Discover("probe")` is refused.
    CredentialWriter *credentials;
    ConnectionSettingsStore *connectionSettings;
    NameOwner *names;
    // Where alert notifications go. May be null: the alerts are then still evaluated and
    // journalled, nothing is shown. On the phone it is the AlertNotificationPresenter over the
    // platform's notification library (hints: core/service/alerthints.cpp, retuned after S9).
    AlertNotifier *alerts;
    // The short labels the alert notification texts use; the service fills it from the
    // configuration file at start and when the file changes. May be null.
    SubjectLabelBook *labels;
    // The presenter behind `alerts` (the same object), for the parts only the service can fill: the
    // acknowledge handler, the value source, the facts reader, the language and the settings.
    AlertNotificationPresenter *presenter;
    // The words of the resident notice; the service core gives it the facts of the moment.
    ResidentNoticeComposer *resident;
    // Where the state of Do not disturb comes from; null = not watched (the PC host).
    DoNotDisturbSource *dnd;
    // The package's version stamp (core/service/versionwatch.h). Null = not watched: the PC
    // host and the tests that do not ask for it, where no package is installed and a missing stamp
    // would otherwise stop the service at its first heartbeat.
    VersionStampReader *stamp;
    // Starts the new binary after a stop for an upgrade; null = nothing is started (the next ping of
    // the add-on, a tap on the notice or the window then starts it).
    ServiceActivator *activator;
    // The service's own settings, for the time of the last self-restart (the loop guard). May be null.
    ServiceSettingsStore *serviceSettings;
};

} // namespace lm

#endif
