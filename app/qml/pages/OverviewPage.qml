// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/Style.js" as Style
import "../components/AlertText.js" as AlertText
import "../components/StorageText.js" as StorageText

// The main page: the monitoring strip, the attention banner and
// one tile per Subject in a stable order, so a tile does not jump under the finger. The
// settings, the "about" page and (development builds) the scenario switch are in the pull-down.
// The Do not disturb strip under the monitoring
// strip, the alert banner at the top (a tap on a notification's banner opens the application here and
// does not acknowledge, so the alerts and their Acknowledge buttons are the first thing seen),
// and the entry to the alerts page in the pull-down with the count.
Page {
    id: page
    objectName: "overview"
    allowedOrientations: Orientation.All
    // The window brings the overview back to the top when an alert arrived while it was away.
    function showTop() { list.positionViewAtBeginning() }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: subjects

        PullDownMenu {
            MenuItem {
                visible: devsource
                text: "Scenario: " + (fakeBackend ? fakeBackend.scenarioName : "")
                onClicked: if (fakeBackend) fakeBackend.nextScenario()
            }
            MenuItem {
                //% "Alerts (%1)"
                text: qsTrId("ui-menu-alerts").arg(alerts.count)
                onClicked: pageStack.push(Qt.resolvedUrl("AlertsPage.qml"))
            }
            MenuItem {
                //% "Set up account"
                text: qsTrId("ui-menu-account")
                onClicked: pageStack.push(Qt.resolvedUrl("AccountPage.qml"))
            }
            // Storage and maintenance.
            MenuItem {
                //% "Storage"
                text: qsTrId("ui-menu-storage")
                onClicked: pageStack.push(Qt.resolvedUrl("StoragePage.qml"))
            }
            MenuItem {
                //% "About"
                text: qsTrId("ui-menu-about")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                //% "Settings"
                text: qsTrId("ui-menu-settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
        }

        header: Column {
            width: list.width
            spacing: Theme.paddingMedium
            SimulatedBar { }
            PageHeader {
                //% "Libre Monitor"
                title: qsTrId("ui-app-name")
            }
            MonitoringStrip { }
            DndStrip { }
            // Store maintenance is running (REQ-STO-15: monitoring goes on meanwhile) and little space
            // is left (REQ-STO-02, from the service's last weekly check): one line each, pictogram and
            // text, no colour; a tap opens the storage page.
            BackgroundItem {
                visible: storage.running || storage.low
                width: parent.width
                height: visible ? Math.max(Theme.itemSizeExtraSmall, storageLine.height + Theme.paddingMedium) : 0
                onClicked: pageStack.push(Qt.resolvedUrl("StoragePage.qml"))
                Row {
                    id: storageLine
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    spacing: Theme.paddingMedium
                    Icon {
                        id: storagePic
                        anchors.verticalCenter: parent.verticalCenter
                        source: Style.pictogramSource(Style.pictograms[storage.running ? "busy" : "storage"])
                        color: Theme.primaryColor
                        width: Theme.iconSizeSmall; height: width
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - storagePic.width - parent.spacing
                        wrapMode: Text.WordWrap
                        color: Theme.primaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        // REQ-STO-02: the low-space notice points to compaction with its estimate (REQ-STO-13).
                        text: storage.running
                              //% "Store maintenance is running. Monitoring goes on."
                              ? qsTrId("ui-strip-maintenance")
                              : StorageText.lowText(storage.estimateKnown, Format.formatFileSize(storage.estimateBytes))
                    }
                }
            }
            // The alert banner: how many alerts are not acknowledged; a tap opens the alerts page.
            // Neutral, pictogram and text (REQ-UI-04); the tiles below carry the Acknowledge buttons.
            BackgroundItem {
                visible: alerts.unacknowledgedCount > 0
                width: parent.width
                height: visible ? Math.max(Theme.itemSizeExtraSmall, alertBanner.height + Theme.paddingMedium) : 0
                onClicked: pageStack.push(Qt.resolvedUrl("AlertsPage.qml"))
                Row {
                    id: alertBanner
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    spacing: Theme.paddingMedium
                    Icon {
                        id: alertPic
                        anchors.verticalCenter: parent.verticalCenter
                        source: Style.pictogramSource(Style.pictograms["problem"])
                        color: Theme.highlightColor
                        width: Theme.iconSizeSmall; height: width
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - alertPic.width - parent.spacing
                        wrapMode: Text.WordWrap
                        color: Theme.highlightColor
                        //% "%n alert(s) not acknowledged"
                        text: qsTrId("ui-banner-alerts", alerts.unacknowledgedCount)
                    }
                }
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
            Item { width: 1; height: Theme.paddingLarge }
            // The attention banner is neutral: pictogram and text, never colour (REQ-UI-04).
            Item { width: 1; height: subjects.attentionCount > 0 ? Theme.paddingMedium : 0 }
            Row {
                visible: subjects.attentionCount > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Icon {
                    source: Style.pictogramSource(Style.pictograms["problem"])
                    color: Theme.primaryColor
                    width: Theme.iconSizeSmall; height: width
                }
                Label {
                    //% "%n patient(s) need attention"
                    text: qsTrId("ui-banner-attention", subjects.attentionCount)
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.primaryColor
                }
            }
        }

        delegate: SubjectTile { }

        // The empty state. Not Silica's ViewPlaceholder: its hint line is drawn in a pale
        // highlight colour that was very faint on the default ambience of the emulator (the service's
        // open point 4; colour review of the user interface). Both lines are in
        // the theme's strong colours, which Silica adapts to light and dark ambiences.
        // It is drawn over the list (parent: the page), centred in the view, so the pull-down still works.
        Column {
            parent: page
            visible: list.count === 0
            anchors.centerIn: parent
            width: page.width - 2 * Theme.horizontalPageMargin
            spacing: Theme.paddingLarge
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraLarge
                color: Theme.highlightColor
                //% "No account set up yet"
                text: qsTrId("ui-welcome-setup")
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeLarge
                color: Theme.primaryColor
                //% "Pull down and choose Set up account"
                text: qsTrId("ui-welcome-hint")
            }
        }
        VerticalScrollDecorator { }
    }
}
