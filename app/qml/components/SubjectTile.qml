// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style
import "StateText.js" as StateText
import "AgeText.js" as AgeText

// One Subject on the overview: the value badge, the name with
// the direction, the age, the state line. The guard buttons (Retry, Re-enable) are in the tile,
// on the main page, never only in a menu (REQ-GRD-08, REQ-GRD-14). Everything shown was
// decided by the model: this item only lays it out.
// An alert that is not acknowledged is on the tile with its level word and an Acknowledge
// button ( a banner tap opens the application and does not acknowledge); silent mode shows the
// struck-through speaker and its end (REQ-UI-15); the tile's menu (long press) sets silent mode.
ListItem {
    id: tile
    contentHeight: content.height + Theme.paddingLarge * 2
    onClicked: pageStack.push(Qt.resolvedUrl("../pages/SubjectPage.qml"), { subjectId: subjectId })
    menu: ContextMenu {
        MenuItem {
            //% "Silent mode"
            text: qsTrId("ui-menu-silent")
            onClicked: pageStack.push(Qt.resolvedUrl("../pages/SilentPage.qml"), { subjectId: subjectId })
        }
    }

    Column {
        id: content
        x: Theme.horizontalPageMargin
        y: Theme.paddingMedium
        width: parent.width - 2 * Theme.horizontalPageMargin
        spacing: Theme.paddingMedium

        Row {
            spacing: Theme.paddingLarge
            Column {
                id: badge
                anchors.verticalCenter: parent.verticalCenter
                ReadingBox {
                    bandKey: model.bandKey
                    valueText: model.valueText
                    direction: directionSymbol
                    size: Theme.fontSizeMedium
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.primaryColor
                    text: model.unitText
                }
            }
            Column {
                width: content.width - badge.width - parent.spacing
                anchors.verticalCenter: parent.verticalCenter
                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeLarge
                    color: Theme.primaryColor
                    text: StateText.displayName(name, shortLabel)
                }
                Label {
                    width: parent.width
                    visible: fresh
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryColor
                    text: StateText.directionText(directionKey) + " · " + AgeText.ageText(ageSeconds)
                }
                Row {
                    visible: !fresh
                    width: parent.width
                    spacing: Theme.paddingSmall
                    Icon {
                        id: pic
                        source: Style.pictogramSource(Style.iconForState(stateKey))
                        color: Theme.primaryColor
                        width: Theme.iconSizeSmall; height: width
                    }
                    Label {
                        width: parent.width - pic.width - parent.spacing
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.secondaryColor
                        text: StateText.stateText(stateKey) + (ageSeconds >= 0 ? " · " + AgeText.ageText(ageSeconds) : "")
                    }
                }
                // The band word. At the extra small size in the secondary colour it was the weakest text of
                // the tile on a dark ambience (colour review); it carries the band for
                // someone who cannot tell the colours apart, so it is at the small size.
                Label {
                    visible: fresh
                    width: parent.width
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryColor
                    text: StateText.stateText(stateKey)
                }
                SilentLine {
                    silent: model.silent
                    noLimit: model.silentNoLimit
                    untilMs: model.silentUntilMs
                    untilToday: model.silentUntilToday
                }
            }
        }
        // The alerts not acknowledged yet, each with its Acknowledge button.
        Repeater {
            model: alertList
            delegate: AlertLine {
                visible: modelData.canAcknowledge === true
                height: visible ? implicitHeight : 0
                width: content.width
                tile: true
                alert: modelData
            }
        }
        Button {
            visible: showRetry
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Retry"
            text: qsTrId("ui-guard-retry")
            onClicked: subjects.retry(subjectId)
        }
        Button {
            visible: showAskAgain
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Ask now"
            text: qsTrId("ui-strip-ask-again")
            onClicked: monitoring.askAgain()
        }
        Column {
            visible: reportProposed
            width: parent.width
            spacing: Theme.paddingSmall
            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                //% "It happened twice. You can prepare a report for the developer."
                text: qsTrId("ui-guard-report-proposed")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "Prepare a report"
                text: qsTrId("ui-guard-report-prepare")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/DiagnosticReportDialog.qml"), { connection: connection })
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "Not now"
                text: qsTrId("ui-guard-report-later")
                onClicked: subjects.declineReport(subjectId)
            }
        }
        Button {
            visible: showReenable
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Re-enable"
            text: qsTrId("ui-guard-reenable")
            onClicked: subjects.reenable(subjectId)
        }
    }
}
