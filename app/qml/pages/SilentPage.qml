// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StateText.js" as StateText
import "../components/AlertText.js" as AlertText

// Silent mode of one patient (REQ-ALR-09, REQ-UI-15):
// 1 hour, 4 hours, until tomorrow morning, without limit, and End silent mode. The page sends the
// choice as the service defines it and never computes the end (the service decides "until tomorrow",
// 06:00 local, and reports the end, which the line at the top then shows). "Without limit" asks
// for the confirmation first. The page also says what silent mode does not cover.
// Opened from the overview tile's menu and from the patient page's pull-down menu.
Page {
    id: page
    allowedOrientations: Orientation.All
    property string subjectId
    property var s: subjects.get(subjects.indexOfSubject(subjectId))

    Connections {
        target: subjects
        onChanged: page.s = subjects.get(subjects.indexOfSubject(page.subjectId))
    }

    function choose(choice) {
        if (alerts.requestSilent(page.subjectId, choice) === "confirm")
            pageStack.push(Qt.resolvedUrl("SilentConfirmDialog.qml"), { subjectId: page.subjectId })
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            SimulatedBar { }
            PageHeader {
                //% "Silent mode"
                title: qsTrId("ui-silent-title")
                description: StateText.displayName(s.name || "", s.shortLabel || "")
            }
            SilentLine {
                x: Theme.horizontalPageMargin
                silent: s.silent === true
                noLimit: s.silentNoLimit === true
                untilMs: s.silentUntilMs || 0
                untilToday: s.silentUntilToday === true
            }
            Label {
                visible: alerts.resultKey !== ""
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: AlertText.resultText(alerts.resultKey)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Silent mode withholds the sound and the vibration of this patient's alerts, the very low and very high ones included. The alerts still show, repeat and are recorded."
                text: qsTrId("ui-silent-explain")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "It does not cover the no data alerts, the notice that monitoring was interrupted, or the notices about approvals and connections."
                text: qsTrId("ui-silent-not-covered")
            }
            Item { width: 1; height: Theme.paddingMedium }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "1 hour"
                text: qsTrId("ui-silent-hour")
                onClicked: page.choose("hour")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "4 hours"
                text: qsTrId("ui-silent-four-hours")
                onClicked: page.choose("four-hours")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "Until tomorrow morning"
                text: qsTrId("ui-silent-tomorrow")
                onClicked: page.choose("tomorrow")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "Without limit"
                text: qsTrId("ui-silent-no-limit")
                onClicked: page.choose("no-limit")
            }
            Item { width: 1; height: Theme.paddingMedium }
            Button {
                visible: s.silent === true
                anchors.horizontalCenter: parent.horizontalCenter
                //% "End silent mode"
                text: qsTrId("ui-silent-end")
                onClicked: alerts.endSilent(page.subjectId)
            }
        }
        VerticalScrollDecorator { }
    }
}
