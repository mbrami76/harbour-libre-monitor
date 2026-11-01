// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0

// First run: the unit (REQ-UI-02) and the statement that this is a
// supplementary monitor, not a medical device (REQ-SAF-01), which the user must accept.
Dialog {
    id: dialog
    canAccept: understood.checked
    allowedOrientations: Orientation.All

    onAccepted: {
        settings.displayUnit = unit.currentIndex === 0 ? "mmol/L" : "mg/dL"
        settings.safetyAccepted = true
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height
        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge
            DialogHeader {
                //% "Welcome"
                title: qsTrId("ui-firstrun-title")
            }
            // The welcome emblem is the application icon (the rod of Asclepius stays on the
            // cover only). The 172 px drawing installed by app/app.pro as icons/ui/app-icon.png.
            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "../../icons/ui/app-icon.png"
                width: Theme.iconSizeLauncher * 1.5
                height: width
                sourceSize.width: width
                sourceSize.height: height
                smooth: true
            }
            // The version of this build (no stage word).
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTrId("ui-about-version").arg(appVersion)
            }
            ComboBox {
                id: unit
                width: parent.width
                //% "Glucose unit"
                label: qsTrId("ui-settings-unit")
                menu: ContextMenu {
                    MenuItem { text: "mmol/L" }
                    MenuItem { text: "mg/dL" }
                }
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                //% "Libre Monitor is a supplementary monitor. It is not a medical device. Do not base therapy decisions on it."
                text: qsTrId("ui-safety-statement")
            }
            // What can silence an alert, said once before anything is monitored (//): the silent profile cannot be read by the
            // application, so it is a plain sentence here and on About.
            SectionHeader {
                //% "Alerts"
                text: qsTrId("ui-alerts-title")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "To acknowledge an alert, tap its entry in Events or use Acknowledge in this app. A tap on the banner only opens the app; the alarm goes on."
                text: qsTrId("ui-firstrun-acknowledge")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "In the silent profile only very low and very high alerts sound; the others vibrate. Do not disturb silences every alert, the very low and very high ones included."
                text: qsTrId("ui-about-silent-profile")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Silent mode, which you can set for each patient, silences the very low and very high alerts as well."
                text: qsTrId("ui-firstrun-silent-mode")
            }
            TextSwitch {
                id: understood
                //% "I understand"
                text: qsTrId("ui-firstrun-understand")
            }
        }
    }
}
