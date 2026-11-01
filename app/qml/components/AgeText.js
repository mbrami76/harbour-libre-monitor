// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The age of a reading in words. Texts are identifier-based and
// plural-aware (%n); the English wording sits next to each identifier for lupdate.
function ageText(seconds) {
    if (seconds < 0)
        //% "time unclear"
        return qsTrId("ui-age-unclear")
    if (seconds < 60)
        //% "just now"
        return qsTrId("ui-age-just-now")
    var minutes = Math.floor(seconds / 60)
    if (minutes < 60)
        //% "%n min ago"
        return qsTrId("ui-age-minutes", minutes)
    var hours = Math.floor(minutes / 60)
    if (hours < 48)
        //% "%n h ago"
        return qsTrId("ui-age-hours", hours)
    //% "%n days ago"
    return qsTrId("ui-age-days", Math.floor(hours / 24))
}
