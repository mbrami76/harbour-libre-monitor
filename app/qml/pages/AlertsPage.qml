// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/AlertText.js" as AlertText

// The alerts page: every alert of every patient that is
// active, acknowledged or snoozed, in the order the service's arbiter ranks them (very low, very high,
// no data, low, high; the same rank by the earlier onset), each with its Acknowledge and Snooze buttons.
// Reached from the overview's alert banner and from its pull-down menu. A notification has no default
// action, so this page is not reached "from the notification": a banner tap opens the overview
// The page never says "no alerts" while it cannot know them.
Page {
    id: page
    allowedOrientations: Orientation.All

    SilicaListView {
        id: list
        anchors.fill: parent
        model: alerts
        spacing: Theme.paddingLarge

        header: Column {
            width: list.width
            spacing: Theme.paddingMedium
            SimulatedBar { }
            PageHeader {
                //% "Alerts"
                title: qsTrId("ui-alerts-title")
            }
            DndStrip { }
            Label {
                visible: alerts.resultKey !== ""
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AlertText.resultText(alerts.resultKey)
            }
        }

        delegate: Item {
            width: list.width
            height: line.height
            AlertLine {
                id: line
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                showPatient: true
                alert: model
            }
        }

        ViewPlaceholder {
            enabled: list.count === 0
            // An older service has no alerts to tell: say why and what to do, not "did not answer".
            text: alerts.known
                  //% "No active alerts"
                  ? qsTrId("ui-alerts-none")
                  : alerts.serviceOlder
                    //% "The monitoring service is outdated. Stop and Start monitoring again."
                    ? qsTrId("ui-service-older")
                    //% "The alerts are not known: the monitoring service did not answer"
                    : qsTrId("ui-alerts-unknown")
        }
        VerticalScrollDecorator { }
    }
}
