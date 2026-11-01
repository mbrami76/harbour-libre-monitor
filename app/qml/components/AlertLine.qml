// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style
import "StateText.js" as StateText
import "AlertText.js" as AlertText

// One alert: the level word, since when and how long, the state,
// and the buttons Acknowledge and Snooze (15, 30, 60, 120 minutes; never for very low and very high,
// REQ-THR-01). A glucose alert that is still active shows its round band badge with the band's colour;
// an acknowledged, snoozed or no-data alert is neutral, with a pictogram and words (REQ-UI-04). A no-data
// alert says why in the UI's own words and never shows a value. Everything was decided by the model
// (alertview.h): this item lays it out and sends the button presses to the model.
//
// `showPatient` puts the patient's name (or short label) first and adds the Silent mode button: the
// alerts page lists every patient.
// `tile` is the overview tile's form: the level word, since when, and Acknowledge only.
Column {
    id: line
    property var alert: ({})
    property bool showPatient: false
    property bool tile: false
    property bool snoozeOpen: false

    width: parent ? parent.width : Screen.width
    spacing: Theme.paddingSmall

    Row {
        width: parent.width
        spacing: Theme.paddingMedium
        Item {
            id: mark
            width: Theme.iconSizeSmall; height: width
            anchors.verticalCenter: parent.verticalCenter
            // The band badge only for an active glucose alert (bandKey is empty otherwise).
            BandBadge {
                anchors.centerIn: parent
                visible: (line.alert.bandKey || "") !== ""
                bandKey: line.alert.bandKey || ""
                size: parent.width
            }
            Icon {
                anchors.fill: parent
                visible: (line.alert.bandKey || "") === ""
                source: Style.pictogramSource(line.alert.levelKey === "missing" ? Style.pictograms["interrupted"]
                                              : (line.alert.acknowledged ? Style.pictograms["running"]
                                                                         : Style.pictograms["problem"]))
                color: Theme.primaryColor
            }
        }
        Column {
            width: parent.width - mark.width - parent.spacing
            anchors.verticalCenter: parent.verticalCenter
            Label {
                width: parent.width
                truncationMode: TruncationMode.Fade
                color: Theme.primaryColor
                font.pixelSize: line.tile ? Theme.fontSizeSmall : Theme.fontSizeMedium
                text: (line.showPatient ? StateText.displayName(line.alert.name || "", line.alert.shortLabel || "") + " · " : "")
                      + AlertText.levelText(line.alert.levelKey || "")
            }
            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AlertText.sinceText(line.alert.levelSinceMs || 0, line.alert.minutesSince === undefined ? -1 : line.alert.minutesSince)
            }
            Label {
                visible: (line.alert.causeKey || "") !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: StateText.stateText(line.alert.causeKey || "")
            }
            Label {
                visible: !line.tile
                width: parent.width
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AlertText.stateText(line.alert)
            }
        }
    }
    // A Flow: three buttons may not fit on one line of a narrow screen.
    Flow {
        visible: line.alert.canAcknowledge === true || (!line.tile && line.alert.canSnooze === true) || line.showPatient
        width: parent.width
        spacing: Theme.paddingMedium
        Button {
            visible: line.alert.canAcknowledge === true
            //% "Acknowledge"
            text: qsTrId("ui-alert-acknowledge")
            onClicked: alerts.acknowledge(line.alert.subjectId, line.alert.slot)
        }
        Button {
            visible: !line.tile && line.alert.canSnooze === true
            //% "Snooze"
            text: qsTrId("ui-alert-snooze")
            onClicked: line.snoozeOpen = !line.snoozeOpen
        }
        // On the alerts page every line also leads to the patient's silent mode (Acknowledge, Snooze and Silent on the alert page).
        Button {
            visible: line.showPatient
            text: qsTrId("ui-menu-silent")
            onClicked: pageStack.push(Qt.resolvedUrl("../pages/SilentPage.qml"), { subjectId: line.alert.subjectId })
        }
    }
    // The snooze durations, from the model (the one list the service accepts).
    Row {
        id: snoozeRow
        visible: line.snoozeOpen && !line.tile && line.alert.canSnooze === true
        width: parent.width
        spacing: Theme.paddingSmall
        Repeater {
            id: snoozeChoices
            model: alerts.snoozeChoices()
            delegate: Button {
                width: (snoozeRow.width - (snoozeChoices.count - 1) * snoozeRow.spacing) / Math.max(1, snoozeChoices.count)
                //% "%1 min"
                text: qsTrId("ui-alert-snooze-minutes").arg(modelData)
                onClicked: { line.snoozeOpen = false; alerts.snooze(line.alert.subjectId, line.alert.slot, modelData) }
            }
        }
    }
}
