// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_STORAGEVIEW_H
#define LM_UI_STORAGEVIEW_H

// The pure part of the storage and maintenance page (REQ-STO-02, 05, 06, 07, 13, 15, 16): the parameters
// sent with StartMaintenance and every answer of the service turned into a text identifier the page
// translates. No QML, no D-Bus: tested on the host.
//
// The application neither exports nor imports: the only maintenance kind is "compaction",
// which deletes permanently after one confirmation (REQ-STO-05). The words of the service are those of
// (StartMaintenance, the error classes and phases
// of core/store/compactor.h); an answer this version does not know becomes the general text with nothing
// invented.

#include <QList>
#include <QString>
#include <QtGlobal>

namespace lm {
namespace ui {

// The choices of the full-resolution window, in months; 0 is the shortest allowed, 14 days (REQ-STO-13).
// The standard is 3 months.
QList<int> windowChoices();
const int kStandardWindowMonths = 3;

// The JSON text StartMaintenance("compaction", ...) takes (daemon/maintenancejobs.cpp): the window only,
// {"window_months": N} or {"window_days": 14} for the shortest choice. Nothing else is sent.
QString compactionParameters(int windowMonths);

// The answer of StartMaintenance: "" (started) gives an empty key; every refusal its text identifier,
// "ui-maint-start-<word>" for the known words and "ui-maint-start-other" otherwise.
QString startAnswerKey(const QString &result);
// The end of a run (MaintenanceFinished): "ui-maint-done-compaction", "ui-maint-done-other",
// "ui-maint-cancelled", "ui-maint-queue-stop" (stopped to protect the readings), "ui-maint-shutdown",
// "ui-maint-fail-<class>" for the known classes (io, window-too-short, insufficient-space,
// free-space-unknown, aborted) and "ui-maint-fail-other".
QString finishedKey(const QString &kind, bool ok, const QString &errorClass, const QString &cancelReason);
// The text identifier of a progress phase of the compaction (core/store/compactor.h: checking-space,
// compacting, finalizing, done) and of the purge (purging): "ui-maint-phase-<phase>", "ui-maint-phase-other"
// for an unknown one.
QString phaseKey(const QString &phase);

// A number of bytes from the details of MaintenanceFinished ("missing_bytes", "bytes_freed"), or -1 when the
// key is absent or not a number. The details carry numbers and keys only, never a name or a value.
qint64 detailBytes(const QString &detailsJson, const char *key);

} // namespace ui
} // namespace lm

#endif
