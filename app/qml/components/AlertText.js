// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The words of the alerts and of silent mode. The models
// return keys and times (app/ui/alertview.h); the texts are written here once, each as a literal so the
// translation tool finds it (the same reason as StateText.js). The levels use the words of the
// notifications: very low, low, high, very high, no data. Times are written in the phone's own
// format with Silica's Format (12 or 24 hours as the phone is set).

// The time of day of an instant, in the phone's format.
function timeText(ms) {
    return Format.formatDate(new Date(ms), Formatter.TimeValue)
}

// The time, with the short day name in front when the instant is not today.
function dayTimeText(ms, today) {
    var d = new Date(ms)
    var t = Format.formatDate(d, Formatter.TimeValue)
    return today ? t : Qt.locale().dayName(d.getDay(), Locale.ShortFormat) + " " + t
}

function isToday(ms) {
    var d = new Date(ms)
    var n = new Date()
    return d.getFullYear() === n.getFullYear() && d.getMonth() === n.getMonth() && d.getDate() === n.getDate()
}

// The word of an alert's level ("ll", "l", "h", "hh" are the band keys; "missing" is no data).
function levelText(levelKey) {
    switch (levelKey) {
    case "ll":
        return qsTrId("ui-band-ll")
    case "l":
        return qsTrId("ui-band-l")
    case "h":
        return qsTrId("ui-band-h")
    case "hh":
        return qsTrId("ui-band-hh")
    case "missing":
        //% "No data"
        return qsTrId("ui-alert-level-missing")
    default:
        //% "Alert"
        return qsTrId("ui-alert-level-unknown")
    }
}

// Since when the level holds and for how many minutes ("Since 14:02 (12 min)").
function sinceText(levelSinceMs, minutes) {
    if (levelSinceMs <= 0)
        return ""
    //% "Since %1 (%2 min)"
    return qsTrId("ui-alert-since").arg(dayTimeText(levelSinceMs, isToday(levelSinceMs))).arg(minutes < 0 ? 0 : minutes)
}

// The state of an alert in words.
function stateText(a) {
    if (a.snoozed)
        //% "Snoozed until %1"
        return qsTrId("ui-alert-snoozed-until").arg(dayTimeText(a.snoozeUntilMs, isToday(a.snoozeUntilMs)))
    if (a.isPause)
        //% "Acknowledged. This is a pause: the alert returns if the level persists."
        return qsTrId("ui-alert-pause")
    if (a.acknowledged)
        //% "Acknowledged"
        return qsTrId("ui-alert-acknowledged")
    //% "Not acknowledged"
    return qsTrId("ui-alert-not-acknowledged")
}

// The answer of an action, when there is something to say (AlertModel.resultKey).
function resultText(key) {
    switch (key) {
    case "ui-alert-err-not-active":
        //% "The alert is no longer active."
        return qsTrId("ui-alert-err-not-active")
    case "ui-alert-err-already":
        //% "The alert was already acknowledged."
        return qsTrId("ui-alert-err-already")
    case "ui-alert-err-not-silent":
        //% "Silent mode was already off."
        return qsTrId("ui-alert-err-not-silent")
    case "ui-alert-err-no-service":
        //% "The monitoring service did not answer. Try again."
        return qsTrId("ui-alert-err-no-service")
    case "ui-service-older":
        // the same sentence as the strip's "older service" state.
        //% "The monitoring service is outdated. Stop and Start monitoring again."
        return qsTrId("ui-service-older")
    case "":
        return ""
    default:
        //% "The monitoring service refused the request."
        return qsTrId("ui-alert-err-refused")
    }
}

// Silent mode, in full ("Silent until 14:30", "Silent").
function silentText(noLimit, untilMs, untilToday) {
    if (noLimit || untilMs <= 0)
        //% "Silent"
        return qsTrId("ui-silent-on")
    //% "Silent until %1"
    return qsTrId("ui-silent-until").arg(dayTimeText(untilMs, untilToday))
}
