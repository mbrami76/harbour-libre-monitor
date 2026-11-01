// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SUBJECTLABEL_H
#define LM_SUBJECTLABEL_H

// How a patient (a "Subject" in the code) is named where a stranger may read it: the first line of
// an alarm, the resident notice's list of alerts, the "waiting for approval" notice, and the cover of
// the application when the user switched names off (REQ-ALR-10).
//
// The rule, in this order:
//   1. the short `label` the user gave the patient in the configuration file (one to three
//      characters, spaces at the ends ignored), because that is the name the user chose to show;
//   2. otherwise "P1", "P2", ... by the patient's position in the configuration file (connections in
//      file order, their Subjects in file order), so that a notification is never blank and never
//      shows the full name. A label of more than three characters is not a short label (it may be a
//      name typed in the wrong field) and is treated as absent, never shortened, so that no part of a
//      name can leak onto the lock screen.
// The full name is never used here: this code never sees it (the service does not read the `name`
// key). Showing the name is a separate, explicit choice of the user.
//
// Pattern: one pure function, shared by the service (SubjectLabelBook, core/service/alertnotification.h)
// and the user interface, so the cover and the notifications cannot drift apart (asks
// for "the same derivation, one place"). It reads no file and no clock.
//
// The letter P is not translated: it is a short code like the label itself, the same in English and
// Italian ("patient", "paziente"), and a translated code would change when the language changes,
// which would make the same patient look like two.

#include <QString>

namespace lm {

// Longest text that counts as a short label ( "a short label of up to three characters").
const int kPatientLabelMaxChars = 3;

// True when `label`, trimmed, is a usable short label (1 to kPatientLabelMaxChars characters).
bool isShortPatientLabel(const QString &label);

// The text that names a patient. `position` is the 0-based place of the patient in the
// configuration file; a negative position (a patient that is not in the file) gives "?".
QString patientLabel(const QString &configuredLabel, int position);

} // namespace lm

#endif
