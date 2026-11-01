// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style

// The monitoring-state strip (REQ-UI-10): pictogram plus text,
// neutral colour, never colour alone (REQ-UI-04). It has the two buttons of the service level
// in the page body, not in a menu: Start monitoring and Ask again.
Column {
    id: strip
    width: parent ? parent.width : Screen.width
    spacing: Theme.paddingMedium

    Row {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * Theme.horizontalPageMargin
        spacing: Theme.paddingMedium
        Icon {
            id: pic
            source: Style.pictogramSource(Style.icons[monitoring.status] || Style.pictograms["problem"])
            color: Theme.primaryColor
            width: Theme.iconSizeSmall; height: width
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            width: parent.width - pic.width - parent.spacing
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            text: {
                switch (monitoring.status) {
                case "running":
                    //% "Monitoring is running"
                    return qsTrId("ui-strip-running")
                case "stopped":
                    //% "Monitoring is not running"
                    return qsTrId("ui-strip-stopped")
                case "starting":
                    //% "The monitoring service is starting…"
                    return qsTrId("ui-strip-starting")
                case "approval":
                    //% "Waiting for your approval to unlock the stored login"
                    return qsTrId("ui-strip-approval")
                case "call-failed":
                    //% "The monitoring service did not answer"
                    return qsTrId("ui-strip-call-failed")
                case "service-older":
                    // the service runs, but it is another build than this window
                    // (an upgrade left the old process running). It still monitors; the window does not
                    // restart it by itself: the user stops it here and starts it with the button that follows.
                    //% "The monitoring service is outdated. Stop and Start monitoring again."
                    return qsTrId("ui-service-older")
                case "service-newer":
                    // the service is newer than this window, which was left
                    // open across an upgrade. Nothing to restart: the window itself is the old one.
                    //% "The app is older than the service. Reopen it."
                    return qsTrId("ui-service-newer")
                default:
                    //% "Connecting to the monitoring service…"
                    return qsTrId("ui-strip-connecting")
                }
            }
        }
    }
    // Only in the "older service" state: the Stop half of "Stop and Start monitoring". Once the old process
    // is gone the strip shows "not running" and the Start button below, which starts the installed binary.
    Button {
        visible: monitoring.status === "service-older"
        anchors.horizontalCenter: parent.horizontalCenter
        //% "Stop monitoring"
        text: qsTrId("ui-strip-stop-button")
        onClicked: monitoring.stopMonitoring()
    }
    Button {
        visible: monitoring.status === "stopped" || monitoring.status === "call-failed"
        anchors.horizontalCenter: parent.horizontalCenter
        //% "Start monitoring"
        text: qsTrId("ui-strip-start-button")
        onClicked: monitoring.startMonitoring()
    }
    // After a phone restart the system asks once whether the app may read the stored login. If the
    // dialog went away unanswered the service raises it again every 15 minutes; this button
    // raises it at once. Nothing is monitored until it is approved.
    Label {
        visible: monitoring.status === "approval"
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * Theme.horizontalPageMargin
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.primaryColor
        //% "Nothing is monitored until you approve. The system asks again every 15 minutes, or ask now."
        text: qsTrId("ui-strip-approval-hint")
    }
    Button {
        visible: monitoring.status === "approval"
        anchors.horizontalCenter: parent.horizontalCenter
        //% "Ask now"
        text: qsTrId("ui-strip-ask-again")
        onClicked: monitoring.askAgain()
    }
}
