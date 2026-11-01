// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style
import "AlertText.js" as AlertText

// Silent mode of one patient, always visible while it lasts (REQ-UI-15): the struck-through
// speaker and "Silent until 14:30", "Silent until Tue 06:00" when the end is not today, or "Silent"
// without limit. The end time is the service's (the UI never computes it) in the phone's time
// format. Neutral colours, pictogram and text (REQ-UI-04). The pictogram is Silica's muted speaker until
// the author's own drawing exists. `compact` is the cover's form: a smaller
// speaker, the same words (REQ-UI-15 asks for the text there too), faded out if the cover is too narrow.
Row {
    id: line
    property bool silent: false
    property bool noLimit: false
    property real untilMs: 0
    property bool untilToday: true
    property bool compact: false
    property int pixelSize: Theme.fontSizeSmall
    // The widest the text may be (the cover sets it to its own width).
    property real maxTextWidth: 100000

    visible: silent
    spacing: Theme.paddingSmall

    Icon {
        id: pic
        anchors.verticalCenter: parent.verticalCenter
        source: Style.pictogramSource(Style.pictograms["silent"])
        color: Theme.primaryColor
        width: line.compact ? Theme.iconSizeExtraSmall : Theme.iconSizeSmall
        height: width
    }
    Label {
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, line.maxTextWidth - pic.width - line.spacing)
        truncationMode: TruncationMode.Fade
        font.pixelSize: line.pixelSize
        color: Theme.primaryColor
        text: AlertText.silentText(line.noLimit, line.untilMs, line.untilToday)
    }
}
