// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"

// The diagnostic report for the developer (REQ-GRD-15;
//). Opened from the patient's guard proposal after the second stop in a row. It says
// what the file holds, that NO glucose value is in it, and asks for consent; the file is written only
// after "Write the file", to the Documents folder, unchanged from the service's text; then the path and the
// developer's address. The application sends nothing. "Not now" ends the proposal without a file.
Page {
    id: page
    allowedOrientations: Orientation.All
    property string connection
    Component.onCompleted: report.open(connection)

    function errorText(key) {
        switch (key) {
        //% "The proposal has already ended: there is nothing to write."
        case "ui-report-err-not-proposed": return qsTrId("ui-report-err-not-proposed")
        //% "The monitoring service does not know this account any more."
        case "ui-report-err-unknown-connection": return qsTrId("ui-report-err-unknown-connection")
        //% "No details are available; the report will be offered again after the next problems."
        case "ui-report-err-no-report": return qsTrId("ui-report-err-no-report")
        //% "The monitoring service is not running. Start monitoring and try again."
        case "ui-report-err-service-not-running": return qsTrId("ui-maint-start-service-not-running")
        //% "The monitoring service did not answer. Try again."
        case "ui-report-err-no-answer": return qsTrId("ui-maint-start-no-answer")
        //% "The file could not be written to the Documents folder. Nothing was sent. Try again."
        case "ui-report-err-write": return qsTrId("ui-report-err-write")
        }
        //% "The monitoring service gave an answer this version cannot read. Nothing was written."
        return qsTrId("ui-report-err-malformed")
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge
        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge
            SimulatedBar { }
            PageHeader {
                //% "Report for the developer"
                title: qsTrId("ui-report-title")
            }
            Column {
                visible: report.stage === "consent"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingLarge
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    //% "Monitoring of this account was stopped twice in a row because the source sent data this app did not expect. A report can help the developer to fix it."
                    text: qsTrId("ui-report-why")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    //% "The file holds: the versions of the app and of the adapter, the kinds of problems found with their details, the type of request, counts, and the shape of the data that caused the stop."
                    text: qsTrId("ui-report-holds")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.highlightColor
                    //% "It holds no glucose value and no unit (each is replaced by the word \"number\"), no name, no account or patient identifier, no sensor identifier, no password or session, and no time of day."
                    text: qsTrId("ui-report-not-held")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    //% "The file is written as plain text to the folder %1. The app sends nothing: you can read the file and decide whether to send it."
                    text: qsTrId("ui-report-where").arg(report.folder)
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Write the file"
                    text: qsTrId("ui-report-write")
                    onClicked: report.write()
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTrId("ui-guard-report-later")
                    onClicked: { report.decline(); pageStack.pop() }
                }
            }
            BusyIndicator {
                visible: report.stage === "waiting"
                running: visible
                anchors.horizontalCenter: parent.horizontalCenter
                size: BusyIndicatorSize.Large
            }
            Column {
                visible: report.stage === "written" || report.stage === "error"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingLarge
                Label {
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    color: Theme.highlightColor
                    //% "The report was written: %1"
                    text: report.stage === "written" ? qsTrId("ui-report-written").arg(report.path) : page.errorText(report.errorKey)
                }
                Label {
                    visible: report.stage === "written"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    text: report.developerContact !== ""
                          //% "Send it to: %1"
                          ? qsTrId("ui-report-send-to").arg(report.developerContact)
                          //% "The developer's address is not in this package yet: send the file the way you agreed with the developer."
                          : qsTrId("ui-report-no-contact")
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Close"
                    text: qsTrId("ui-report-close")
                    onClicked: pageStack.pop()
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
