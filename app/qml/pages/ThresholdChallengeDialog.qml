// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0

// The challenge of a looser limit (REQ-THR-02): a screen of its own that warns that the
// alerts will come later and asks for the limit to be TYPED AGAIN. It does not show the value just asked
// for: it receives no number of it (the model has no property that carries it), and the fields start
// empty. The standard value, which is not the user's value, is shown so the warning has its number.
// The service enforces the match; a wrong or missing value is refused there and the thresholds page
// says which limit did not match, without the number. Leaving the screen sends nothing.
Dialog {
    id: dialog
    allowedOrientations: Orientation.All
    canAccept: (!thresholds.challengeLow || lowAgain.text.length > 0)
               && (!thresholds.challengeHigh || highAgain.text.length > 0)
    readonly property bool mmol: thresholds.unit === "mmol/L"
    property bool sent: false
    signal closed()

    onAccepted: {
        sent = true
        thresholds.confirmChallenge(lowAgain.text, highAgain.text)
        lowAgain.text = ""
        highAgain.text = ""
    }
    onRejected: thresholds.cancelChallenge()
    // Accepted or left, the dialog is popped and destroyed: the page may open a new challenge after it.
    Component.onDestruction: closed()

    // Development builds only: what the user's typing and the accept do.
    // The accept waits until the dialog is on screen (a push is animated).
    property string devPending: ""
    function devConfirm(typed) { devPending = typed }
    Timer {
        interval: 2500
        running: dialog.devPending.length > 0
        onTriggered: {
            if (thresholds.challengeLow) lowAgain.text = dialog.devPending
            if (thresholds.challengeHigh) highAgain.text = dialog.devPending
            dialog.devPending = ""
            dialog.accept()
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                //% "Save"
                acceptText: qsTrId("ui-thr-challenge-accept")
                //% "Type the value again"
                title: qsTrId("ui-thr-challenge-title")
            }
            Label {
                visible: thresholds.challengeLow
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                //% "The new low limit is lower than the standard value %1, so a low alert will come later. To confirm, type the new low limit again."
                text: qsTrId("ui-thr-challenge-low").arg(thresholds.standardLowText)
            }
            TextField {
                id: lowAgain
                visible: thresholds.challengeLow
                width: parent.width
                //% "Low limit, typed again (%1)"
                label: qsTrId("ui-thr-challenge-low-field").arg(thresholds.unit)
                placeholderText: label
                inputMethodHints: dialog.mmol ? (Qt.ImhFormattedNumbersOnly | Qt.ImhNoPredictiveText)
                                              : (Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText)
            }
            Label {
                visible: thresholds.challengeHigh
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                //% "The new high limit is higher than the standard value %1, so a high alert will come later. To confirm, type the new high limit again."
                text: qsTrId("ui-thr-challenge-high").arg(thresholds.standardHighText)
            }
            TextField {
                id: highAgain
                visible: thresholds.challengeHigh
                width: parent.width
                //% "High limit, typed again (%1)"
                label: qsTrId("ui-thr-challenge-high-field").arg(thresholds.unit)
                placeholderText: label
                inputMethodHints: dialog.mmol ? (Qt.ImhFormattedNumbersOnly | Qt.ImhNoPredictiveText)
                                              : (Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Very low and very high alerts do not change: they never wait."
                text: qsTrId("ui-thr-challenge-fixed")
            }
        }
    }
}
