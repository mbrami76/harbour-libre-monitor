// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StorageText.js" as StorageText
import "../components/PurgeText.js" as PurgeText

// Removal of one patient's old data (REQ-STO-17). The page binds to `purge` (app/ui/purgecontroller.h). The order: the age (3, 6, 12, 24 months), the
// service's ESTIMATE (what can be recovered, the temporary space needed, the space check, the counts),
// then Continue opens the confirmation naming the patient by its label and the age, which is replaced by
// the gate: the user types the label and presses Remove, enabled only when the text equals the label the
// service compares with (PurgeController.label). Then the progress and the result, or the repeat offer
// after a failed journal part. A patient without a label cannot be purged: the page says so.
// No colour: words and the theme's controls only (REQ-UI-04). Counts and sizes only, never a value.
Page {
    id: page
    allowedOrientations: Orientation.All
    property string subjectId
    property string patientName
    property string configuredLabel
    // Development builds only (the action "purge:<row>:<step>:<typed>"): after the estimate, open the
    // confirmation ("confirm"), the gate with a typed text ("gate"), or send the purge with the label
    // ("run"), as the user's taps would. Always empty in a release build (devAction is empty there).
    property string devStep
    property string devTyped

    Component.onCompleted: purge.open(subjectId, patientName, configuredLabel)

    function size(bytes) { return bytes >= 0 ? Format.formatFileSize(bytes) : qsTrId("ui-sensor-unknown-time") }
    // Counts in the application's language (the one chosen in the settings, else the phone's).
    function count(n) {
        var loc = configSettings.startLanguage !== "" ? Qt.locale(configSettings.startLanguage) : Qt.locale()
        return Number(n).toLocaleString(loc, "f", 0)
    }
    function openConfirmation(operation) {
        if (purge.canContinue)
            pageStack.push(confirmDialog, {}, operation === undefined ? PageStackAction.Animated : operation)
    }

    Connections {
        target: purge
        onChanged: {
            if (page.devStep === "" || !purge.canContinue)
                return
            var step = page.devStep
            page.devStep = ""
            if (step === "confirm")
                page.openConfirmation(PageStackAction.Immediate)
            else if (step === "gate")
                pageStack.push(gateDialog, { initialText: page.devTyped }, PageStackAction.Immediate)
            else if (step === "run")
                purge.purge(purge.label)
        }
    }

    // ---- the confirmation, naming the patient by its label and the age ----
    Component {
        id: confirmDialog
        Dialog {
            acceptDestination: gateDialog
            acceptDestinationAction: PageStackAction.Replace
            SilicaFlickable {
                anchors.fill: parent
                contentHeight: confirmColumn.height
                Column {
                    id: confirmColumn
                    width: parent.width
                    spacing: Theme.paddingLarge
                    DialogHeader {
                        //% "Continue"
                        acceptText: qsTrId("ui-purge-continue")
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        //% "Remove the readings and the alert and threshold history of %1 older than %2?"
                        text: qsTrId("ui-purge-confirm").arg(purge.label).arg(StorageText.windowName(purge.ageMonths))
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        text: qsTrId("ui-storage-estimate").arg(page.size(purge.estimateBytes)).arg(page.size(purge.neededBytes))
                    }
                    // Approved text #5: removal is permanent.
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        font.bold: true
                        text: qsTrId("ui-storage-compact-permanent")
                    }
                }
            }
        }
    }

    // ---- the gate: the label typed, then Remove ----
    Component {
        id: gateDialog
        Dialog {
            id: gate
            property string initialText
            canAccept: purge.typedMatches(field.text)
            onAccepted: purge.purge(field.text)
            Column {
                width: parent.width
                spacing: Theme.paddingLarge
                DialogHeader {
                    //% "Remove"
                    acceptText: qsTrId("ui-purge-remove")
                }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    color: Theme.highlightColor
                    //% "Type %1 to confirm."
                    text: qsTrId("ui-purge-gate").arg(purge.label)
                }
                TextField {
                    id: field
                    width: parent.width
                    text: gate.initialText
                    //% "Label"
                    label: qsTrId("ui-purge-gate-field")
                    placeholderText: label
                    // The text is sent as typed: no correction, no capital added by the keyboard.
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData
                    EnterKey.enabled: gate.canAccept
                    EnterKey.onClicked: gate.accept()
                }
                Label {
                    visible: field.text.length > 0 && !gate.canAccept
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    color: Theme.secondaryHighlightColor
                    font.pixelSize: Theme.fontSizeSmall
                    //% "The text does not match the label."
                    text: qsTrId("ui-purge-gate-mismatch")
                }
            }
        }
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
                //% "Remove old data"
                title: qsTrId("ui-purge-title")
                description: page.patientName
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: purge.hasLabel
                      //% "Label: %1"
                      ? qsTrId("ui-purge-label").arg(purge.label)
                      //% "No label"
                      : qsTrId("ui-purge-label-none")
            }
            // a patient without a label cannot be purged (the service answers no-label).
            Label {
                visible: !purge.hasLabel
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                //% "Removing needs a label for this patient. Set one in Settings."
                text: qsTrId("ui-purge-no-label")
            }
            ComboBox {
                id: ageBox
                enabled: purge.stage !== "running" && purge.stage !== "starting"
                //% "Remove data older than"
                label: qsTrId("ui-purge-age")
                menu: ContextMenu {
                    Repeater {
                        model: purge.ageChoices
                        MenuItem {
                            text: StorageText.windowName(modelData)
                            onClicked: purge.ageMonths = modelData
                        }
                    }
                }
                Component.onCompleted: currentIndex = purge.ageChoices.indexOf(purge.ageMonths)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                //% "Readings and the alert and threshold history older than this age are removed. The patient and its settings stay."
                text: qsTrId("ui-purge-what")
            }

            // ---- the estimate (REQ-STO-17: before anything; REQ-STO-16: the temporary space) ----
            Label {
                visible: purge.stage === "estimating" && !purge.estimateKnown
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                color: Theme.secondaryColor
                //% "Estimating…"
                text: qsTrId("ui-purge-estimating")
            }
            DetailItem {
                visible: purge.estimateKnown
                //% "Readings and records"
                label: qsTrId("ui-purge-rows")
                value: page.count(purge.storeRows)
            }
            DetailItem {
                visible: purge.estimateKnown
                //% "Alert and threshold history"
                label: qsTrId("ui-purge-history")
                value: page.count(purge.journalEntries)
            }
            Label {
                visible: purge.estimateKnown
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                text: qsTrId("ui-storage-estimate").arg(page.size(purge.estimateBytes)).arg(page.size(purge.neededBytes))
            }
            Label {
                visible: purge.estimateKnown && (purge.space === "short" || purge.space === "unknown"
                                                 || purge.storeRows + purge.journalEntries <= 0)
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: purge.space === "short" ? qsTrId("ui-storage-space-short").arg(page.size(purge.missingBytes))
                      : purge.space === "unknown" ? qsTrId("ui-storage-space-unknown")
                        //% "Nothing to remove for this age."
                        : qsTrId("ui-purge-nothing")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTrId("ui-storage-compact-permanent")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: purge.canContinue
                text: qsTrId("ui-purge-continue")
                onClicked: page.openConfirmation()
            }

            // ---- a purge in progress (phases checking-space, purging, finalizing, done) ----
            Column {
                visible: purge.stage === "running" || purge.stage === "starting"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.highlightColor
                    text: qsTrId("ui-storage-running").arg(StorageText.kindText("purge"))
                          .arg(StorageText.phaseText(purge.phaseKey))
                }
                ProgressBar {
                    width: parent.width
                    indeterminate: purge.fraction < 0
                    minimumValue: 0; maximumValue: 1
                    value: purge.fraction < 0 ? 0 : purge.fraction
                    label: purge.steps > 0 ? qsTrId("ui-storage-step").arg(purge.step).arg(purge.steps) : ""
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTrId("ui-storage-cancel")
                    onClicked: purge.cancel()
                }
            }
            // ---- the result, or a refusal, in words; the repeat offer ----
            Label {
                visible: purge.resultKey !== ""
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                text: PurgeText.resultText(purge.resultKey, page.count(purge.removedStore), page.count(purge.removedJournal),
                                           page.size(purge.bytesFreed),
                                           purge.resultMissingBytes > 0 ? page.size(purge.resultMissingBytes) : "")
            }
            Button {
                visible: purge.canRepeat
                anchors.horizontalCenter: parent.horizontalCenter
                //% "Repeat"
                text: qsTrId("ui-purge-repeat-button")
                onClicked: purge.repeat()
            }
        }
        VerticalScrollDecorator { }
    }
}
