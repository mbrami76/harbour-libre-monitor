// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_RESIDENTNOTICE_H
#define LM_RESIDENTNOTICE_H

// THE WORDS OF THE RESIDENT NOTICE (REQ-ALR-06).
//
// The resident notice is the one notification that stays in the tray and survives the service: its
// summary names the state of the monitor (active, waiting for approval, cannot start, stopped) and
// its body gives the facts. In the active state the body lists the alerts that are active or
// acknowledged (the label and the level word, never a value), says which Subjects are in silent
// mode, and says when Do not disturb is on, so the notice is the one place a user can always read
// that alerts will not sound. The lifecycle decides WHEN the notice is shown (at a change of state and at the wake-ups); this class decides what it SAYS, from the state of the notice
// and the facts of the moment, so a test can check the words in every state without a phone.
//
// Pattern: a composer behind a port. The facts of the moment (the alerts, the silent Subjects, the
// Do not disturb flag, the number of Subjects) are asked of a ResidentFactsSource, implemented by the
// service core; the words come from the presenter's current texts, so the notice follows the
// language of the configuration file like every other notification (REQ-ALR-10). Nothing here is
// a glucose value, a name or a credential.

#include "service/alertnotification.h"

namespace lm {

class ResidentFactsSource
{
public:
    virtual ~ResidentFactsSource() {}
    // The alerts that are active or acknowledged (snoozed ones are not listed), with the label of
    // the Subject; the silent Subjects' labels; whether Do not disturb is on; how many Subjects the
    // service monitors. The fields that belong to the notice itself are left empty.
    virtual ResidentInput facts() const = 0;
};

class ResidentNoticeComposer
{
public:
    // The presenter supplies the texts (and with them the language) and the Subjects' labels; not owned.
    explicit ResidentNoticeComposer(const AlertNotificationPresenter *presenter) : m_presenter(presenter), m_source(0) {}

    void setSource(const ResidentFactsSource *source) { m_source = source; }

    // The summary and body of the notice in its state. Total: without a source the facts are empty.
    NoteText compose(const Notice &notice) const;

private:
    const AlertNotificationPresenter *m_presenter;
    const ResidentFactsSource *m_source;
};

} // namespace lm

#endif
