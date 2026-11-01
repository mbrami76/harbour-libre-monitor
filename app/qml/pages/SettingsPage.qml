// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/ThresholdText.js" as ThresholdText

// Settings. A settings page has no "OK": each control takes effect
// as soon as it changes (the settings object saves itself).
Page {
    allowedOrientations: Orientation.All
    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge
        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium
            SimulatedBar { }
            PageHeader {
                //% "Settings"
                title: qsTrId("ui-menu-settings")
            }
            BackgroundItem {
                width: parent.width
                onClicked: pageStack.push(Qt.resolvedUrl("AccountPage.qml"))
                Label {
                    anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: Theme.horizontalPageMargin }
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //% "Account"
                    text: qsTrId("ui-settings-account")
                }
            }
            ComboBox {
                width: parent.width
                //% "Glucose unit"
                label: qsTrId("ui-settings-unit")
                currentIndex: settings.displayUnit === "mg/dL" ? 1 : 0
                menu: ContextMenu {
                    MenuItem { text: "mmol/L"; onClicked: settings.displayUnit = "mmol/L" }
                    MenuItem { text: "mg/dL"; onClicked: settings.displayUnit = "mg/dL" }
                }
            }
            ComboBox {
                width: parent.width
                //% "Plot shows"
                label: qsTrId("ui-settings-feeds")
                currentIndex: settings.feeds === 0 ? 0 : settings.feeds === 1 ? 1 : 2
                menu: ContextMenu {
                    MenuItem {
                        //% "Current feed"
                        text: qsTrId("ui-settings-feed-current")
                        onClicked: settings.feeds = 0
                    }
                    MenuItem {
                        //% "History feed"
                        text: qsTrId("ui-settings-feed-history")
                        onClicked: settings.feeds = 1
                    }
                    MenuItem {
                        //% "Both feeds"
                        text: qsTrId("ui-settings-feed-both")
                        onClicked: settings.feeds = 2
                    }
                }
            }
            TextSwitch {
                checked: settings.valueAxisLeft
                //% "Value axis on the left"
                text: qsTrId("ui-settings-axis-left")
                //% "Move the numbers of the value axis to the left edge of the plot."
                description: qsTrId("ui-settings-axis-left-hint")
                onClicked: settings.valueAxisLeft = checked
            }
            TextSwitch {
                checked: settings.coverShowsName
                //% "Show the name on the cover"
                text: qsTrId("ui-settings-cover-name")
                //% "Anyone who sees the home screen can read it. Off: the cover shows a short label."
                description: qsTrId("ui-settings-cover-name-hint")
                onClicked: settings.coverShowsName = checked
            }

            // ---- What the monitoring service reads from the configuration file -------------
            SectionHeader {
                //% "Alerts and language"
                text: qsTrId("ui-settings-section-alerts")
            }
            ComboBox {
                width: parent.width
                enabled: configSettings.available
                //% "Language"
                label: qsTrId("ui-settings-language")
                //% "The language of the pages and of the alerts. The alerts change at once, the pages at the next start of the app."
                description: qsTrId("ui-settings-language-hint")
                currentIndex: configSettings.language === "en" ? 1 : configSettings.language === "it" ? 2 : 0
                menu: ContextMenu {
                    MenuItem {
                        //% "Phone language"
                        text: qsTrId("ui-settings-language-phone")
                        onClicked: configSettings.setLanguage("")
                    }
                    // The names of the languages are written in their own language and not translated.
                    MenuItem { text: "English"; onClicked: configSettings.setLanguage("en") }
                    MenuItem { text: "Italiano"; onClicked: configSettings.setLanguage("it") }
                }
            }
            Label {
                visible: configSettings.language !== configSettings.startLanguage
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Close and open the app again to see the pages in the new language."
                text: qsTrId("ui-settings-language-restart")
            }
            TextSwitch {
                enabled: configSettings.available
                checked: configSettings.alertsShowValue
                //% "Show the value in alerts"
                text: qsTrId("ui-settings-show-value")
                //% "Off: an alert shows only the patient's short label and the level, so a locked screen tells a passer-by nothing more. On: it also shows the newest value, its unit and the trend. The full name is never shown."
                description: qsTrId("ui-settings-show-value-hint")
                onClicked: configSettings.setAlertsShowValue(checked)
            }

            SectionHeader {
                visible: configSettings.patients.length > 0
                //% "Patients"
                text: qsTrId("ui-settings-section-patients")
            }
            Repeater {
                model: configSettings.patients
                Column {
                    width: column.width
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        truncationMode: TruncationMode.Fade
                        color: Theme.highlightColor
                        // The name is personal data: shown here only, never in a notification.
                        text: modelData.name && modelData.name.length > 0 ? modelData.name : modelData.shortLabel
                    }
                    TextField {
                        width: parent.width
                        enabled: configSettings.available
                        text: modelData.label
                        //% "Short label in alerts (1 to 3 characters)"
                        label: qsTrId("ui-settings-patient-label")
                        // The field's own name while it is empty (the P-name is in the line below): on the
                        // emulator a bare "P1" in an empty field did not say what the field is for.
                        placeholderText: label
                        inputMethodHints: Qt.ImhNoPredictiveText
                        EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                        EnterKey.onClicked: focus = false
                        onActiveFocusChanged: if (!activeFocus && text !== modelData.label) configSettings.setPatientLabel(modelData.subjectId, text)
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        wrapMode: Text.WordWrap
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        //% "Shown in the alerts and on a locked screen, and on the cover when names are off. Without a label: %1."
                        text: qsTrId("ui-settings-patient-label-hint").arg(modelData.defaultLabel)
                    }
                    BackgroundItem {
                        width: parent.width
                        onClicked: pageStack.push(Qt.resolvedUrl("ThresholdsPage.qml"), { subjectId: modelData.subjectId })
                        Label {
                            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: Theme.horizontalPageMargin }
                            color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                            text: qsTrId("ui-thr-title")
                        }
                    }
                }
            }

            SectionHeader {
                visible: configSettings.hasAccount
                //% "Advanced"
                text: qsTrId("ui-settings-section-advanced")
            }
            TextField {
                id: accountLabel
                visible: configSettings.hasAccount
                width: parent.width
                enabled: configSettings.available
                text: configSettings.connectionLabel
                //% "Name of the account in notices"
                label: qsTrId("ui-settings-account-label")
                placeholderText: qsTrId("ui-account-default-label")
                inputMethodHints: Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: focus = false
                onActiveFocusChanged: if (!activeFocus && text !== configSettings.connectionLabel)
                                          configSettings.setConnectionLabel(text, qsTrId("ui-account-default-label"))
            }
            Label {
                visible: configSettings.hasAccount
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                //% "Used instead of the account's e-mail address in the notices about the connection. At most 24 characters."
                text: qsTrId("ui-settings-account-label-hint")
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
        }
        VerticalScrollDecorator { }
    }
}
