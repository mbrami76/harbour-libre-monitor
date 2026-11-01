// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The words of the storage page: every answer of the service, every end of a compaction and every phase, from
// the text identifiers of app/ui/storageview.h. A refusal says what would help. There is no
// export and no import: compaction is the only kind.

function kindText(kind) {
    switch (kind) {
    //% "Compaction"
    case "compaction": return qsTrId("ui-maint-kind-compaction")
    //% "Removal of old data"
    case "purge": return qsTrId("ui-maint-kind-purge")
    //% "Removal estimate"
    case "purge-estimate": return qsTrId("ui-maint-kind-purge-estimate")
    }
    //% "Maintenance"
    return qsTrId("ui-maint-kind-other")
}

// The phases of a compaction (core/store/compactor.h), in the service's order.
function phaseText(key) {
    switch (key) {
    //% "checking the free space"
    case "ui-maint-phase-checking-space": return qsTrId("ui-maint-phase-checking-space")
    //% "removing old readings"
    case "ui-maint-phase-compacting": return qsTrId("ui-maint-phase-compacting")
    //% "removing old data"
    case "ui-maint-phase-purging": return qsTrId("ui-maint-phase-purging")
    //% "finishing"
    case "ui-maint-phase-finalizing": return qsTrId("ui-maint-phase-finalizing")
    //% "done"
    case "ui-maint-phase-done": return qsTrId("ui-maint-phase-done")
    }
    //% "working"
    return qsTrId("ui-maint-phase-other")
}

// arg: a size, already formatted: the bytes freed after a compaction, the bytes missing after a refusal
// for space (empty when the service did not say).
function resultText(key, arg) {
    switch (key) {
    case "": return ""
    //% "The store was compacted. About %1 were freed."
    case "ui-maint-done-compaction": return qsTrId("ui-maint-done-compaction").arg(arg)
    //% "Done."
    case "ui-maint-done-other": return qsTrId("ui-maint-done-other")
    //% "Cancelled. Nothing on the phone was changed."
    case "ui-maint-cancelled": return qsTrId("ui-maint-cancelled")
    //% "Stopped to protect the new readings, which were waiting while the store was busy. Nothing on the phone was changed. Try again later."
    case "ui-maint-queue-stop": return qsTrId("ui-maint-queue-stop")
    //% "Stopped because the monitoring service was stopping. Nothing on the phone was changed."
    case "ui-maint-shutdown": return qsTrId("ui-maint-shutdown")
    case "ui-maint-fail-insufficient-space":
        return arg !== ""
               //% "Not enough free space: about %1 more is needed. Nothing was changed."
               ? qsTrId("ui-maint-fail-insufficient-space-missing").arg(arg)
               //% "Not enough free space. Nothing was changed."
               : qsTrId("ui-maint-fail-insufficient-space")
    //% "Free space could not be checked. Nothing was removed. Try again later."
    case "ui-maint-fail-free-space-unknown": return qsTrId("ui-maint-fail-free-space-unknown")
    //% "The full-resolution window cannot be shorter than 14 days."
    case "ui-maint-fail-window-too-short": return qsTrId("ui-maint-start-parameter-window")
    //% "The data could not be read or written. Nothing was changed."
    case "ui-maint-fail-io": return qsTrId("ui-maint-fail-io")
    //% "The monitoring service is not running. Start monitoring and try again."
    case "ui-maint-start-service-not-running": return qsTrId("ui-maint-start-service-not-running")
    //% "Another maintenance task is running. Wait until it has finished."
    case "ui-maint-start-maintenance-running": return qsTrId("ui-maint-start-maintenance-running")
    //% "The full-resolution window cannot be shorter than 14 days."
    case "ui-maint-start-parameter-window": return qsTrId("ui-maint-start-parameter-window")
    //% "The monitoring service did not answer. Try again."
    case "ui-maint-start-no-answer": return qsTrId("ui-maint-start-no-answer")
    }
    //% "It did not work. Nothing on the phone was changed."
    return qsTrId("ui-maint-fail-other")
}

// The low-space notice (REQ-STO-02): it points to compaction, with the estimate when one is known
// (REQ-STO-13). size: the estimate, already formatted.
function lowText(estimateKnown, size) {
    return estimateKnown
           //% "Low storage. Compaction would free about %1."
           ? qsTrId("ui-storage-low").arg(size)
           //% "Low storage. Compaction can free space."
           : qsTrId("ui-storage-low-noestimate")
}

// The window as words inside a sentence ("older than 3 months"): no "(standard)" mark.
function windowName(months) {
    //% "14 days"
    if (months <= 0) return qsTrId("ui-storage-window-14d")
    //% "%n month(s)"
    return qsTrId("ui-storage-window-months", months)
}

// The window choices: 0 is 14 days, else months; the standard is marked.
function windowText(months) {
    var t = windowName(months)
    //% "%1 (standard)"
    return months === 3 ? qsTrId("ui-thr-time-standard").arg(t) : t
}
