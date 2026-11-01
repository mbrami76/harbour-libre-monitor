// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.Configuration 1.0
import "Style.js" as Style

// The Do not disturb strip: while the phone's Do not disturb is on, no alert makes a sound, the very low and very
// high ones included, so the overview and the patient page say so with a pictogram and a text, in the
// neutral colours (REQ-UI-04). The value is read live from the phone's own setting with
// `ConfigurationValue` on /lipstick/do_not_disturb (no permission needed; the Harbour check accepts the import): a change shows or hides the strip within about a second, nothing polls.
// The silent profile cannot be read from the sandbox; it has a static sentence on the first-run page
// and on About instead.
Column {
    id: strip
    width: parent ? parent.width : Screen.width
    visible: dnd.value === true
    height: visible ? implicitHeight : 0

    ConfigurationValue {
        id: dnd
        key: "/lipstick/do_not_disturb"
        defaultValue: false
    }

    Row {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * Theme.horizontalPageMargin
        spacing: Theme.paddingMedium
        Icon {
            id: pic
            source: Style.pictogramSource(Style.pictograms["dnd"])
            color: Theme.primaryColor
            width: Theme.iconSizeSmall; height: width
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            width: parent.width - pic.width - parent.spacing
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.primaryColor
            //% "Do not disturb is on: no alert will make a sound"
            text: qsTrId("ui-dnd-strip")
        }
    }
}
