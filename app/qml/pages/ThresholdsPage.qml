// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StateText.js" as StateText
import "../components/ThresholdText.js" as ThresholdText

// The thresholds and alert times of one patient (REQ-THR-01 to 04). The low and high limits are edited in the user's unit, every
// number carries the unit; the standard values and the very low and very high limits (the same for every
// patient) are shown; a limit looser than the standard asks for the CHALLENGE on a screen of its own,
// which does not show the value just asked for; every refusal is said in words with numbers; the history
// of the edits comes from the journal. The alert times are offered inside the service's own ranges and
// written to the configuration file (the service applies them at once). The page decides nothing: the
// service does (thresholds, ThresholdEditor; configSettings, ConfigSettings).
// Opened from the patient page's pull-down menu and from the settings (each patient).
Page {
    id: page
    allowedOrientations: Orientation.All
    property string subjectId
    property var s: subjects.get(subjects.indexOfSubject(subjectId))
    // Development builds only: the text typed again on the challenge screen.
    property string devTyped: ""
    // Re-evaluates the function-based bindings (the alert times) when the model changes.
    property int rev: 0
    property bool challengeOpen: false
    readonly property bool mmol: thresholds.unit === "mmol/L"

    Component.onCompleted: {
        thresholds.open(subjectId)
        configSettings.clearError()
    }
    Connections {
        target: subjects
        onChanged: page.s = subjects.get(subjects.indexOfSubject(page.subjectId))
    }
    Connections {
        target: thresholds
        onChanged: {
            page.rev++
            if (ThresholdText.messageText(thresholds.message).length > 0)
                showMessage.restart()
            if (thresholds.state === "challenge" && !page.challengeOpen && thresholds.subjectId === page.subjectId) {
                page.challengeOpen = true
                var d = pageStack.push(Qt.resolvedUrl("ThresholdChallengeDialog.qml"))
                d.closed.connect(function() { page.challengeOpen = false })
                if (devsource && page.devTyped.length > 0)
                    d.devConfirm(page.devTyped)
            }
        }
    }

    // Development builds only: what the Save button does, with given texts.
    function devSave(low, high) {
        lowField.text = low
        highField.text = high
        save()
    }
    function save() {
        thresholds.save(lowField.text, highField.text)   // no note
    }

    // The field shows the limit in force again when it changes (after an accepted edit, a change of unit),
    // unless the user is typing in it.
    property string inForceLow: thresholds.lowText
    property string inForceHigh: thresholds.highText
    onInForceLowChanged: if (!lowField.activeFocus) lowField.text = inForceLow
    onInForceHighChanged: if (!highField.activeFocus) highField.text = inForceHigh

    // The answer sits under the Save button, which is near the bottom of the first screen: scroll it into
    // view once it is laid out (on the emulator the refusal ran below the screen's edge).
    Timer {
        id: showMessage
        interval: 50
        onTriggered: {
            var bottom = messageLabel.y + messageLabel.height + Theme.paddingLarge
            if (bottom > flick.contentY + flick.height)
                flick.contentY = Math.min(bottom - flick.height, flick.contentHeight - flick.height)
        }
    }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            SimulatedBar { }
            PageHeader {
                //% "Thresholds and alert times"
                title: qsTrId("ui-thr-title")
                description: s ? StateText.displayName(s.name || "", s.shortLabel || "") : ""
            }

            SectionHeader {
                //% "Limits"
                text: qsTrId("ui-thr-section-limits")
            }
            Label {
                visible: !thresholds.known
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "The limits in force are not known: the monitoring service did not answer."
                text: qsTrId("ui-thr-not-known")
            }
            TextField {
                id: lowField
                width: parent.width
                enabled: thresholds.known && thresholds.state === "idle"
                //% "Low limit (%1)"
                label: qsTrId("ui-thr-low-field").arg(thresholds.unit)
                placeholderText: label
                text: thresholds.lowText
                inputMethodHints: page.mmol ? Qt.ImhFormattedNumbersOnly : Qt.ImhDigitsOnly
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: highField.focus = true
            }
            TextField {
                id: highField
                width: parent.width
                enabled: thresholds.known && thresholds.state === "idle"
                //% "High limit (%1)"
                label: qsTrId("ui-thr-high-field").arg(thresholds.unit)
                placeholderText: label
                text: thresholds.highText
                inputMethodHints: page.mmol ? Qt.ImhFormattedNumbersOnly : Qt.ImhDigitsOnly
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Standard values: low %1, high %2. A lower low limit or a higher high limit makes the alerts come later; you are then asked to type it again."
                text: qsTrId("ui-thr-standard").arg(thresholds.standardLowText).arg(thresholds.standardHighText)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Very low: below %1. Very high: above %2. These two are the same for every patient and cannot be changed; their alerts never wait and cannot be snoozed."
                text: qsTrId("ui-thr-fixed").arg(thresholds.veryLowText).arg(thresholds.veryHighText)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "The standard values are common targets, not rules for everybody: children, pregnancy, older or high-risk people may be given other targets by their doctor."
                text: qsTrId("ui-thr-not-for-everybody")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: thresholds.known && thresholds.state === "idle"
                //% "Save the limits"
                text: qsTrId("ui-thr-save")
                onClicked: { lowField.focus = false; highField.focus = false; page.save() }
            }
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                size: BusySize.Small
                running: thresholds.state === "sending"
                visible: running
            }
            Label {
                id: messageLabel
                visible: text.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: ThresholdText.messageText(thresholds.message)
            }

            SectionHeader {
                //% "Alert times"
                text: qsTrId("ui-thr-section-times")
            }
            Label {
                visible: !thresholds.timesKnown
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "The alert times are not known: the monitoring service did not answer."
                text: qsTrId("ui-thr-times-not-known")
            }
            AlertTimeBox {
                subjectId: page.subjectId
                rev: page.rev
                kind: "low"
                //% "Wait before a low alert"
                label: qsTrId("ui-thr-time-low")
                //% "A low alert sounds only after the reading has stayed below the low limit this long. Very low alerts never wait."
                description: qsTrId("ui-thr-time-low-hint")
            }
            AlertTimeBox {
                subjectId: page.subjectId
                rev: page.rev
                kind: "high"
                //% "Wait before a high alert"
                label: qsTrId("ui-thr-time-high")
                //% "A high alert sounds only after the reading has stayed above the high limit this long. Very high alerts never wait."
                description: qsTrId("ui-thr-time-high-hint")
            }
            AlertTimeBox {
                subjectId: page.subjectId
                rev: page.rev
                kind: "missing"
                //% "No data alert after"
                label: qsTrId("ui-thr-time-missing")
                //% "How long without a new reading before the no data alert sounds."
                description: qsTrId("ui-thr-time-missing-hint")
            }
            Label {
                visible: text.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: ThresholdText.configErrorText(configSettings.errorKey)
            }

            SectionHeader {
                //% "Changes of the limits"
                text: qsTrId("ui-thr-section-history")
            }
            Label {
                visible: thresholds.historyState !== "ready" || thresholds.history.length === 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: thresholds.historyState === "unreadable"
                      //% "The history cannot be read now."
                      ? qsTrId("ui-thr-history-unreadable")
                      : thresholds.historyState === "loading" ? ""
                      //% "No change recorded: the standard values apply."
                      : qsTrId("ui-thr-history-empty")
            }
            Repeater {
                model: thresholds.history
                Label {
                    x: Theme.horizontalPageMargin
                    width: column.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    text: ThresholdText.historyText(modelData)
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
