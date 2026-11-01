// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The words of the purge page (REQ-STO-17):
// the answers of the service turned into sentences, from the text identifiers of app/ui/purgeview.h. The
// facts that the storage page also states use the approved texts of through the
// same identifiers (estimate, space, refusals); the purge's own sentences are as short as those.
.import "StorageText.js" as StorageText

// key: PurgeController.resultKey. removedStore, removedJournal: counts, already formatted; freed and missing:
// sizes, already formatted.
function resultText(key, removedStore, removedJournal, freed, missing) {
    switch (key) {
    case "": return ""
    //% "Removed: %1 records and %2 history entries. About %3 freed."
    case "ui-purge-done": return qsTrId("ui-purge-done").arg(removedStore).arg(removedJournal).arg(freed)
    //% "The readings were removed, the history was not. Repeat to finish."
    case "ui-purge-repeat": return qsTrId("ui-purge-repeat")
    //% "The label does not match. Nothing was removed."
    case "ui-purge-start-label-mismatch": return qsTrId("ui-purge-start-label-mismatch")
    //% "This patient has no label. Nothing was removed."
    case "ui-purge-start-no-label": return qsTrId("ui-purge-start-no-label")
    //% "This patient is not monitored any more. Nothing was removed."
    case "ui-purge-start-unknown-subject": return qsTrId("ui-purge-start-unknown-subject")
    //% "This age cannot be chosen. Nothing was removed."
    case "ui-purge-start-age-not-allowed": return qsTrId("ui-purge-start-age-not-allowed")
    }
    // The answers shared with compaction (approved texts #12 to #14, cancel, start refusals).
    return StorageText.resultText(key, missing)
}
