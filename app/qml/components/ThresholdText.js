// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The words of the thresholds page (REQ-THR-04). The model
// (app/ui/thresholdeditor.h) returns each message as sentences, a text key with its arguments already
// written in the user's unit; the sentences are written here once, each as a literal so the translation
// tool finds it (the same reason as StateText.js and AlertText.js). The levels use the words of the
// notifications. A refused challenge never says the value asked for or the value typed again.

function partText(p) {
    var a = p.args || []
    switch (p.key) {
    case "ui-thr-saved":
        //% "Saved: low limit %1, high limit %2."
        return qsTrId("ui-thr-saved").arg(a[0]).arg(a[1])
    case "ui-thr-saved-plain":
        //% "Saved."
        return qsTrId("ui-thr-saved-plain")
    case "ui-thr-no-change":
        //% "Nothing changed: these are the limits in force."
        return qsTrId("ui-thr-no-change")
    case "ui-thr-err-invalid-low":
        //% "The low limit is not a possible value. Nothing was sent."
        return qsTrId("ui-thr-err-invalid-low")
    case "ui-thr-err-invalid-high":
        //% "The high limit is not a possible value. Nothing was sent."
        return qsTrId("ui-thr-err-invalid-high")
    case "ui-thr-hint-mgdl":
        //% "Type a whole number of mg/dL."
        return qsTrId("ui-thr-hint-mgdl")
    case "ui-thr-hint-mmol":
        //% "Type a number of mmol/L, with one decimal."
        return qsTrId("ui-thr-hint-mmol")
    case "ui-thr-err-order-low":
        //% "The low limit breaks the order of the limits: very low, low, high, very high. Nothing was changed."
        return qsTrId("ui-thr-err-order-low")
    case "ui-thr-err-order-high":
        //% "The high limit breaks the order of the limits: very low, low, high, very high. Nothing was changed."
        return qsTrId("ui-thr-err-order-high")
    case "ui-thr-range-low":
        //% "Allowed low limit: from %1 to %2."
        return qsTrId("ui-thr-range-low").arg(a[0]).arg(a[1])
    case "ui-thr-range-high":
        //% "Allowed high limit: from %1 to %2."
        return qsTrId("ui-thr-range-high").arg(a[0]).arg(a[1])
    case "ui-thr-err-not-confirmed":
        //% "Not saved: a limit looser than the standard value must be typed again to confirm it."
        return qsTrId("ui-thr-err-not-confirmed")
    case "ui-thr-err-mismatch-low":
        //% "The low limit typed again does not match the one asked for. Nothing was changed."
        return qsTrId("ui-thr-err-mismatch-low")
    case "ui-thr-err-mismatch-high":
        //% "The high limit typed again does not match the one asked for. Nothing was changed."
        return qsTrId("ui-thr-err-mismatch-high")
    case "ui-thr-err-unknown-patient":
        //% "The monitoring service does not follow this patient. Nothing was changed."
        return qsTrId("ui-thr-err-unknown-patient")
    case "ui-thr-err-not-saved":
        //% "The monitoring service could not record the change. Nothing was changed."
        return qsTrId("ui-thr-err-not-saved")
    case "ui-thr-err-no-service":
        //% "The monitoring service did not answer. Nothing was changed."
        return qsTrId("ui-thr-err-no-service")
    case "ui-service-older":
        // the same sentence as the strip's "older service" state.
        //% "The monitoring service is outdated. Stop and Start monitoring again."
        return qsTrId("ui-service-older")
    case "ui-thr-err-refused":
    default:
        //% "The monitoring service refused the change. Nothing was changed."
        return qsTrId("ui-thr-err-refused")
    }
}

// A whole message, its sentences joined.
function messageText(m) {
    if (!m || !m.parts || m.parts.length === 0)
        return ""
    var out = []
    for (var i = 0; i < m.parts.length; ++i)
        out.push(partText(m.parts[i]))
    return out.join(" ")
}

// An alert time in words: "At once" for 0, else whole minutes (the service's ranges are whole minutes;
// a time that is not is written in seconds rather than rounded).
function timeText(seconds) {
    if (seconds === 0)
        //% "At once"
        return qsTrId("ui-thr-time-now")
    if (seconds % 60 === 0)
        //% "%1 min"
        return qsTrId("ui-thr-time-min").arg(seconds / 60)
    //% "%1 s"
    return qsTrId("ui-thr-time-s").arg(seconds)
}

// A choice of an alert time, with "(standard)" after the service's default.
function choiceText(c) {
    if (c.isDefault)
        //% "%1 (standard)"
        return qsTrId("ui-thr-time-standard").arg(timeText(c.seconds))
    return timeText(c.seconds)
}

// One line of the history: when, which limit, the new and the previous value.
function historyText(r) {
    var when = Format.formatDate(new Date(r.timeMs), Formatter.DateMedium) + " " + Format.formatDate(new Date(r.timeMs), Formatter.TimeValue)
    if (r.boundary === "high")
        //% "%1: high limit %2 (was %3)"
        return qsTrId("ui-thr-history-high").arg(when).arg(r.valueText).arg(r.previousText)
    //% "%1: low limit %2 (was %3)"
    return qsTrId("ui-thr-history-low").arg(when).arg(r.valueText).arg(r.previousText)
}

// What went wrong with a settings change written to the configuration file (ConfigSettings.errorKey).
function configErrorText(key) {
    switch (key) {
    case "":
        return ""
    case "label-too-long":
        //% "A label has at most 3 characters. Nothing was changed."
        return qsTrId("ui-cfg-err-label-long")
    case "account-label-too-long":
        //% "The account's name has at most 24 characters. Nothing was changed."
        return qsTrId("ui-cfg-err-account-label-long")
    case "config-unreadable":
        //% "The settings file cannot be read, so nothing can be changed here."
        return qsTrId("ui-cfg-err-unreadable")
    case "config-write-failed":
        //% "The change could not be written. Nothing was changed."
        return qsTrId("ui-cfg-err-write")
    case "service-config-invalid":
        //% "Saved, but the monitoring service could not read the settings file."
        return qsTrId("ui-cfg-err-service-invalid")
    case "service-no-answer":
        //% "Saved. The monitoring service did not answer; it reads the change when it starts."
        return qsTrId("ui-cfg-err-service-no-answer")
    default:
        //% "The change could not be made."
        return qsTrId("ui-cfg-err-other")
    }
}
