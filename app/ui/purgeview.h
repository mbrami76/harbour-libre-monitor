// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PURGEVIEW_H
#define LM_UI_PURGEVIEW_H

// The pure part of the purge page (REQ-STO-17): the ages a
// user may choose, the label the typed text is compared with, the parameters sent with StartMaintenance,
// the reading of the service's estimate and result, and every answer turned into a text identifier.
// No QML, no D-Bus: tested on the host.
//
// The service's words are those: the kinds
// "purge-estimate" (a dry run that changes nothing, no label needed) and "purge" (with "typed_label"),
// the start refusals, the detail keys, the phases checking-space, purging, finalizing, done, and the
// repeat after a failed journal part (error io with store_part_done true).
//
// The gate is not authentication: it guards against a stray or repeated request. The service
// compares the typed text EXACTLY with the Subject's label in its configuration, which its reader trims
// once (daemon/serviceconfig.cpp); the window reads the same file and trims the same way
// (purgeLabelOf), so the label the page shows and asks for is the one the service accepts. The typed
// text itself is sent unchanged: no trimming, no case folding.

#include <QList>
#include <QString>
#include <QtGlobal>

namespace lm {
namespace ui {

// REQ-STO-17: 3, 6, 12 or 24 months; the page starts with the longest, which removes the least.
QList<int> purgeAgeChoices();
const int kDefaultPurgeAgeMonths = 24;

// The label the service compares with: the configuration's label, trimmed (as the service's reader does).
// Empty means the Subject has no label: the purge cannot be asked for (the service answers no-label).
QString purgeLabelOf(const QString &configuredLabel);
// True when the typed text equals the label exactly (and the label is not empty).
bool typedLabelMatches(const QString &typed, const QString &label);

// The JSON text of StartMaintenance: {"subject": "{uuid}", "age_months": N} for "purge-estimate", and
// the same with "typed_label" for "purge".
QString purgeEstimateParameters(const QString &subject, int ageMonths);
QString purgeParameters(const QString &subject, int ageMonths, const QString &typedLabel);

// The estimate or the result of a purge, read from the details of MaintenanceFinished. Numbers and keys only.
struct PurgeFacts
{
    PurgeFacts() : known(false), ageMonths(0), storeRows(0), journalEntries(0), bytesEstimated(0),
                   neededBytes(-1), freeBytes(-1), missingBytes(0), storePartDone(false), removedStore(0),
                   removedJournal(0), bytesFreed(0) {}

    bool known;                 // the details were readable and named a Subject
    QString subject;
    int ageMonths;
    qint64 storeRows;           // store_rows summed: readings and other records, marks, sensors, quarantine, ingest
    qint64 journalEntries;      // journal_entries summed: the Subject's alert, threshold and silent entries
    qint64 bytesEstimated;      // total_bytes_estimated: what the purge can recover (store and journal)
    qint64 neededBytes;         // needed_bytes: the temporary space needed (REQ-STO-16)
    qint64 freeBytes;           // -1 unknown
    QString space;              // "enough", "short", "unknown"
    qint64 missingBytes;
    // A purge's result only.
    bool storePartDone;
    qint64 removedStore, removedJournal, bytesFreed;
};

PurgeFacts purgeFactsFromDetails(const QString &detailsJson);

// The answer of StartMaintenance for "purge-estimate" or "purge": "" (started) gives an empty key; the
// purge's own refusals "ui-purge-start-<word>" (label-mismatch, no-label, unknown-subject, age-not-allowed,
// parameter-subject), the others as for compaction (storageview.h startAnswerKey).
QString purgeStartKey(const QString &result);
// The end of a purge: "ui-purge-done"; "ui-purge-repeat" after a failed journal part (io with
// store_part_done true); the refusals and failures as for compaction ("ui-maint-fail-<class>",
// "ui-maint-cancelled", "ui-maint-queue-stop", "ui-maint-shutdown"); the start words the core may also end
// with (no-subject, age-not-allowed) as "ui-purge-start-unknown-subject" and "ui-purge-start-age-not-allowed".
QString purgeFinishedKey(bool ok, const QString &errorClass, const QString &cancelReason, bool storePartDone);

} // namespace ui
} // namespace lm

#endif
