// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StorageText.js" as StorageText
import "../components/Style.js" as Style

// Storage and maintenance (REQ-STO-02, 05, 06, 07, 13, 15, 16). The page binds to `storage` (app/ui/storagecontroller.h). Since
// the application neither exports nor imports: the page shows the sizes, the full-resolution window and
// the core's estimate for it with the rebuild need, and offers Compact, which deletes permanently after ONE
// confirmation that shows the estimate and says so in plain words (REQ-STO-05). Compact is not offered
// when the core's space check says the rebuild would not fit or the free space cannot be read: the page
// says why and what would help (REQ-STO-16). The work runs in the monitoring service, which keeps reading
// meanwhile (REQ-STO-15). No colour: every state is words and the theme's pictograms (REQ-UI-04).
Page {
    id: page
    allowedOrientations: Orientation.All
    Component.onCompleted: storage.refresh()
    // Development builds only (the action "compact-confirm"): open the confirmation once the estimate allows
    // it, as a tap on Compact would. Always false in a release build (devAction is empty there).
    property bool devOpenConfirmation: false
    Connections {
        target: storage
        onStateChanged: {
            if (page.devOpenConfirmation && storage.canCompact) {
                page.devOpenConfirmation = false
                page.openConfirmation(PageStackAction.Immediate)
            }
        }
    }

    function size(bytes) { return bytes >= 0 ? Format.formatFileSize(bytes) : qsTrId("ui-sensor-unknown-time") }
    // The Compact button's confirmation (also used by the development action "compact-confirm").
    function openConfirmation(operation) {
        if (storage.canCompact)
            pageStack.push(compactDialog, {}, operation === undefined ? PageStackAction.Animated : operation)
    }

    Component {
        id: compactDialog
        Dialog {
            onAccepted: storage.compact()
            SilicaFlickable {
                anchors.fill: parent
                contentHeight: confirmColumn.height
                Column {
                    id: confirmColumn
                    width: parent.width
                    spacing: Theme.paddingLarge
                    DialogHeader {
                        //% "Compact"
                        acceptText: qsTrId("ui-storage-compact")
                    }
                    // What is removed, by the rule (REQ-STO-07): one per-minute current reading in every
                    // 5 minutes is kept; the history graph, the logbook, marks, events and sensors stay.
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        //% "Of the current readings older than %1, one every 5 minutes is kept. Everything else (history graph, logbook, marks, events, sensors) is untouched."
                        text: qsTrId("ui-storage-compact-confirm").arg(StorageText.windowName(storage.windowMonths))
                    }
                    // REQ-STO-13: the estimate is shown before the confirmation, labelled as one.
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        text: qsTrId("ui-storage-estimate").arg(page.size(storage.estimateBytes)).arg(page.size(storage.rebuildNeedBytes))
                    }
                    // REQ-STO-05, word for word: compaction deletes permanently.
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        font.bold: true
                        //% "Removed data cannot be restored by the app. The vendor's service may still hold the readings."
                        text: qsTrId("ui-storage-compact-permanent")
                    }
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
                //% "Storage and maintenance"
                title: qsTrId("ui-storage-title")
            }

            // ---- a run in progress, with Cancel ----
            Column {
                visible: storage.running
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.highlightColor
                    //% "%1 is running: %2. Monitoring goes on meanwhile."
                    text: qsTrId("ui-storage-running").arg(StorageText.kindText(storage.runningKind))
                          .arg(StorageText.phaseText(storage.phaseKey))
                }
                ProgressBar {
                    width: parent.width
                    indeterminate: storage.fraction < 0
                    minimumValue: 0; maximumValue: 1
                    value: storage.fraction < 0 ? 0 : storage.fraction
                    //% "Step %1 of %2"
                    label: storage.steps > 0 ? qsTrId("ui-storage-step").arg(storage.step).arg(storage.steps) : ""
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Cancel"
                    text: qsTrId("ui-storage-cancel")
                    onClicked: storage.cancel()
                }
            }
            // ---- the last answer, in words ----
            Label {
                visible: storage.resultKey !== ""
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                // The argument is a number of bytes (freed, or missing after a refusal for space), or empty.
                text: StorageText.resultText(storage.resultKey,
                          storage.resultArg !== "" ? Format.formatFileSize(Number(storage.resultArg)) : "")
            }
            Label {
                visible: storage.lostReadings > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                //% "%n reading(s) could not be kept while the store was busy."
                text: qsTrId("ui-storage-lost", storage.lostReadings)
            }

            // ---- this phone ----
            SectionHeader {
                //% "On this phone"
                text: qsTrId("ui-storage-phone")
            }
            Label {
                visible: storage.factsLoaded && !storage.factsOk
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                //% "The data cannot be read now. Monitoring is not affected."
                text: qsTrId("ui-storage-unreadable")
            }
            DetailItem {
                //% "Readings and events"
                label: qsTrId("ui-storage-store")
                value: page.size(storage.storeBytes)
            }
            DetailItem {
                //% "Service journal"
                label: qsTrId("ui-storage-journal")
                value: page.size(storage.journalBytes)
            }
            DetailItem {
                //% "Free space"
                label: qsTrId("ui-storage-free")
                value: page.size(storage.freeBytes)
            }
            DetailItem {
                //% "Last weekly check"
                label: qsTrId("ui-storage-check")
                //% "none yet"
                value: storage.checkKnown && storage.checkTimeMs > 0
                       ? Format.formatDate(new Date(storage.checkTimeMs), Formatter.DateMedium) : qsTrId("ui-storage-check-none")
            }
            Row {
                visible: storage.low
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Icon {
                    id: lowPic
                    source: Style.pictogramSource(Style.pictograms["storage"])
                    color: Theme.primaryColor
                    width: Theme.iconSizeSmall; height: width
                }
                Label {
                    width: parent.width - lowPic.width - parent.spacing
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    text: StorageText.lowText(storage.estimateKnown, page.size(storage.estimateBytes))
                }
            }

            // ---- the full-resolution window and the estimate (REQ-STO-13, REQ-STO-16) ----
            SectionHeader {
                //% "Older readings"
                text: qsTrId("ui-storage-window-section")
            }
            ComboBox {
                id: windowBox
                //% "Keep every reading for"
                label: qsTrId("ui-storage-window")
                menu: ContextMenu {
                    Repeater {
                        model: storage.windowChoices
                        MenuItem {
                            text: StorageText.windowText(modelData)
                            onClicked: storage.windowMonths = modelData
                        }
                    }
                }
                // Set after the menu is filled (the same reason as AlertTimeBox).
                Component.onCompleted: currentIndex = storage.windowChoices.indexOf(storage.windowMonths)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                //% "Estimate: frees about %1. Temporary space needed: %2."
                text: storage.estimateKnown ? qsTrId("ui-storage-estimate").arg(page.size(storage.estimateBytes)).arg(page.size(storage.rebuildNeedBytes))
                      //% "No estimate is available now."
                      : qsTrId("ui-storage-estimate-none")
            }
            // REQ-STO-16: the core's space check. The service refuses in both cases, with no override.
            Label {
                visible: storage.space === "short" || storage.space === "unknown"
                         || (storage.estimateKnown && storage.estimateRecords <= 0)
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: storage.space === "short"
                      //% "Not enough free space: about %1 more is needed."
                      ? qsTrId("ui-storage-space-short").arg(page.size(storage.missingBytes))
                      : storage.space === "unknown"
                        //% "Free space cannot be read. Nothing was removed."
                        ? qsTrId("ui-storage-space-unknown")
                        //% "Nothing to compact for this window."
                        : qsTrId("ui-storage-nothing")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //% "Readings of the last 14 days are never thinned. Removed readings cannot be restored."
                text: qsTrId("ui-storage-window-hint")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: storage.canCompact
                text: qsTrId("ui-storage-compact")
                onClicked: page.openConfirmation()
            }

            // ---- the purge, one patient at a time (REQ-STO-17): each patient opens P13 ----
            SectionHeader {
                //% "Remove a patient's old data"
                text: qsTrId("ui-purge-section")
            }
            Repeater {
                model: configSettings.patients
                BackgroundItem {
                    width: column.width
                    height: Math.max(Theme.itemSizeSmall, patientColumn.height + Theme.paddingMedium)
                    onClicked: pageStack.push(Qt.resolvedUrl("PurgePage.qml"),
                                              { subjectId: modelData.subjectId, patientName: modelData.name,
                                                configuredLabel: modelData.label })
                    Column {
                        id: patientColumn
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            color: highlighted ? Theme.highlightColor : Theme.primaryColor
                            text: modelData.name
                        }
                        Label {
                            width: parent.width
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryColor
                            // The label the purge asks for, trimmed as the service reads it (purgeview.h).
                            text: purge.labelOf(modelData.label) !== "" ? qsTrId("ui-purge-label").arg(purge.labelOf(modelData.label))
                                                                         : qsTrId("ui-purge-label-none")
                        }
                    }
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
