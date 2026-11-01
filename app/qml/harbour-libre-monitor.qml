// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "pages"

// The application window. The first run asks for the unit and the
// supplementary-monitor statement (REQ-UI-02, REQ-SAF-01) before anything else.
ApplicationWindow {
    id: app
    initialPage: Component { OverviewPage { } }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")
    allowedOrientations: defaultAllowedOrientations

    // The display tick: ages and the live time axis move while the window is visible. It touches
    // neither the store nor D-Bus, so it is not polling (REQ-ARC-03).
    Timer {
        interval: 15000
        running: true
        repeat: true
        onTriggered: { subjects.refresh(); alerts.refresh(); plot.tick() }
    }

    // a tap on a notification's banner opens the application and does not acknowledge. When an
    // alert that is not acknowledged arrived while the window was away, the window goes back to the
    // overview and its top, where the alert banner and the tiles' Acknowledge buttons are. A dialog
    // that is open (the user is in the middle of a choice) is left alone.
    Connections {
        target: alerts
        onLandOnOverview: {
            var current = pageStack.currentPage
            if (!current || pageStack.busy || current.hasOwnProperty("canAccept"))
                return
            var overview = pageStack.find(function(p) { return p.objectName === "overview" })
            if (overview) {
                if (overview !== current)
                    pageStack.pop(overview, PageStackAction.Immediate)
                overview.showTop()
            }
        }
    }

    // Development builds only: LM_DEV_ACTION does once what a tap would do, through the
    // same function the button calls. A release build never reaches it: its devAction is always empty.
    property bool devActionDone: false
    function runDevAction() {
        if (!devsource || devAction.length === 0 || devActionDone || !alerts.known)
            return
        var parts = devAction.split(":")
        if (parts[0] === "acknowledge-first") {
            for (var i = 0; i < alerts.count; ++i) {
                var a = alerts.get(i)
                if (a.canAcknowledge) {
                    devActionDone = true
                    alerts.acknowledge(a.subjectId, a.slot)         // what AlertLine's Acknowledge does
                    return
                }
            }
        } else if (parts[0] === "thr" && subjects.count > 0) {
            // "thr:<low>:<high>[:<typed again>]": the first patient's thresholds page, the
            // texts typed in the two fields and Save; with a fourth part the challenge screen is answered
            // with it, as a user typing it again would.
            devActionDone = true
            var tp = pageStack.push(Qt.resolvedUrl("pages/ThresholdsPage.qml"),
                                    { subjectId: subjects.get(0).subjectId, devTyped: parts[3] || "" },
                                    PageStackAction.Immediate)
            tp.devSave(parts[1], parts[2])                          // what the page's Save button does
        } else if (parts[0] === "purge" && configSettings.patients.length > 0) {
            // "purge:<row>[:<step>[:<typed>]]": the purge page of that patient of the
            // configuration; after the estimate, step "confirm" opens the confirmation, "gate" the typed-label
            // screen with <typed> already in the field (nothing accepted), "run" sends the purge with the
            // label, as the final button would.
            devActionDone = true
            var pp = configSettings.patients[parseInt(parts[1] || "0")]
            pageStack.push(Qt.resolvedUrl("pages/PurgePage.qml"),
                           { subjectId: pp.subjectId, patientName: pp.name, configuredLabel: pp.label,
                             devStep: parts[2] || "", devTyped: parts[3] || "" }, PageStackAction.Immediate)
        } else if (parts[0] === "compact-confirm") {
            // "compact-confirm": the storage page and the confirmation its Compact
            // button opens (REQ-STO-05); nothing is accepted, so nothing is started.
            devActionDone = true
            pageStack.push(Qt.resolvedUrl("pages/StoragePage.qml"), { devOpenConfirmation: true }, PageStackAction.Immediate)
        } else if (parts[0] === "report" && subjects.count > 0) {
            // "report:<row>": the report dialog of that patient's account and its
            // "Write the file" button's function (the consent a user would give).
            devActionDone = true
            pageStack.push(Qt.resolvedUrl("pages/DiagnosticReportDialog.qml"),
                           { connection: subjects.get(parseInt(parts[1] || "0")).connection }, PageStackAction.Immediate)
            report.write()
        } else if (parts[0] === "silent" && subjects.count > 0) {
            devActionDone = true
            var page = pageStack.push(Qt.resolvedUrl("pages/SilentPage.qml"),
                                      { subjectId: subjects.get(0).subjectId }, PageStackAction.Immediate)
            page.choose(parts[1])                                   // what the page's choice button does
        }
    }
    Connections {
        target: alerts
        onChanged: app.runDevAction()
    }
    // The scripted source of a development build knows its alerts before the page above is connected, and
    // may not change them again for a while: try once more shortly after the start.
    Timer {
        interval: 1500
        running: devsource && devAction.length > 0
        onTriggered: app.runDevAction()
    }

    // Development builds only: the cover drawn inside a page at the large cover size, so that the
    // emulator's screenshot can show it without a tap (devStartPage "Cover"). A release build never
    // reaches it: its devStartPage is always empty.
    Component {
        id: coverPreview
        Page {
            Rectangle {
                anchors.centerIn: parent
                width: Theme.coverSizeLarge.width + 2; height: Theme.coverSizeLarge.height + 2
                color: "transparent"
                border.color: Theme.primaryColor
                Loader {
                    x: 1; y: 1
                    width: Theme.coverSizeLarge.width; height: Theme.coverSizeLarge.height
                    source: Qt.resolvedUrl("cover/CoverPage.qml")
                }
            }
        }
    }

    // Development builds only: open the page named at start, for screenshots.
    // A last part "end" scrolls the page to its bottom once it is laid out, so that a
    // screenshot shows what a drag would (the settings' patients, the thresholds' history).
    property var devScrollPage: null
    function openDevStartPage(name) {
        var parts = name.split(":")
        var p = null
        if (parts[0] === "Cover")
            p = pageStack.push(coverPreview, {}, PageStackAction.Immediate)
        else if (parts[0] === "DiagnosticReportDialog")
            p = pageStack.push(Qt.resolvedUrl("pages/DiagnosticReportDialog.qml"),
                               { connection: subjects.get(parseInt(parts[1] || "0")).connection }, PageStackAction.Immediate)
        else if (parts[0] === "SubjectPage" || parts[0] === "ThresholdsPage")
            p = pageStack.push(Qt.resolvedUrl("pages/" + parts[0] + ".qml"),
                               { subjectId: subjects.get(parseInt(parts[1] || "0")).subjectId }, PageStackAction.Immediate)
        else
            p = pageStack.push(Qt.resolvedUrl("pages/" + parts[0] + ".qml"), {}, PageStackAction.Immediate)
        if (parts[parts.length - 1] === "end")
            devScrollPage = p
    }
    Timer {
        interval: 4000
        running: app.devScrollPage !== null
        onTriggered: {
            var kids = app.devScrollPage.children
            for (var i = 0; i < kids.length; ++i)
                if (kids[i].contentHeight !== undefined && kids[i].contentHeight > kids[i].height)
                    kids[i].contentY = kids[i].contentHeight - kids[i].height
            app.devScrollPage = null
        }
    }

    Component.onCompleted: {
        if (devsource && devStartPage.length > 0)
            openDevStartPage(devStartPage)
        else if (settings.firstRun)
            pageStack.push(Qt.resolvedUrl("pages/FirstRunDialog.qml"), {}, PageStackAction.Immediate)
    }
}
