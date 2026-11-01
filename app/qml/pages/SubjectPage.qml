// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import LibreMonitor 1.0
import "../components"
import "../components/StateText.js" as StateText
import "../components/AgeText.js" as AgeText
import "../components/Style.js" as Style
import "../components/AlertText.js" as AlertText

// The page of one Subject: the large current value in its
// badge (or the stale rendering), the direction and age, the state strip with the guard buttons,
// the plot with zoom and scrolling back (a tap on the value reading box returns to the current
// reading and resets the zoom; there is no play button), and the other patients as chips so
// trouble elsewhere stays visible (1.4). The big value is ALWAYS current: only the plot goes
// grey in a history view (4.7).
// The Do not disturb strip, silent mode with its
// struck-through speaker under the reading, a section with this patient's alerts and their buttons,
// and a pull-down menu with Silent mode and the alerts page.
Page {
    id: page
    allowedOrientations: Orientation.All
    // While value tracking is on, a drag on the plot must not take the page away: the edge
    // swipe that goes back to the previous page is switched off until tracking is turned off.
    // A drag on the plot must not turn into the swipe that goes back: off while tracking AND while a finger is on the plot.
    property bool plotTouched: false
    backNavigation: !plot.tracking && !plotTouched
    forwardNavigation: false
    property string subjectId
    // Silica's `Format` (a ready-made global, not an item to create) follows the phone's settings for dates and for 12 or 24 hour time.
    readonly property bool use24h: Format.formatDate(new Date(2000, 0, 1, 13, 0, 0), Formatter.TimeValue).indexOf("13") >= 0
    // The size of the plot's own text and of the window start and end: fixed, not following the user's
    // font size (they share a grid with the axis and would overlap), but readable. One place to tune.
    readonly property int plotLabelPx: Math.round(Theme.pixelRatio * 28)
    property var s: subjects.get(subjects.indexOfSubject(subjectId))

    Connections {
        target: subjects
        onChanged: page.s = subjects.get(subjects.indexOfSubject(page.subjectId))
    }
    Component.onCompleted: { plot.subjectId = subjectId; plot.backToNow() }
    onSubjectIdChanged: { plot.subjectId = subjectId; plot.backToNow() }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            MenuItem {
                text: qsTrId("ui-menu-alerts").arg(alerts.count)
                onClicked: pageStack.push(Qt.resolvedUrl("AlertsPage.qml"))
            }
            MenuItem {
                text: qsTrId("ui-menu-silent")
                onClicked: pageStack.push(Qt.resolvedUrl("SilentPage.qml"), { subjectId: page.subjectId })
            }
            // The patient's own limits and alert times.
            MenuItem {
                text: qsTrId("ui-thr-title")
                onClicked: pageStack.push(Qt.resolvedUrl("ThresholdsPage.qml"), { subjectId: page.subjectId })
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            SimulatedBar { }
            PageHeader { title: StateText.displayName(s.name, s.shortLabel) }
            MonitoringStrip { }
            DndStrip { }
            // Room between the monitoring notice and the value.
            Item { width: 1; height: Theme.paddingLarge * 2 }

            Item {
                width: parent.width
                height: bigBadge.height + Theme.paddingLarge
                Column {
                    id: bigBadge
                    anchors.horizontalCenter: parent.horizontalCenter
                    ReadingBox {
                        anchors.horizontalCenter: parent.horizontalCenter
                        bandKey: s.bandKey || ""
                        valueText: s.valueText || ""
                        direction: s.directionSymbol || ""
                        size: Theme.fontSizeHuge
                        maxWidth: page.width - 2 * Theme.horizontalPageMargin
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: Theme.primaryColor
                        text: s.unitText || ""
                    }
                }
                // A tap on the value reading box brings the plot back to the current reading and resets the
                // zoom: live, 12 hours. It is the only way back besides leaving the page;
                // there is no button and no icon for it.
                MouseArea {
                    anchors.fill: bigBadge
                    onClicked: plot.backToNow()
                }
            }
            // Wrapped inside the page margins: the Italian line with NULL after a restart of the service ran
            // past both edges on the emulator.
            Label {
                visible: s.fresh === true
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.primaryColor
                text: (s.directionSymbol || "") + "  " + StateText.directionText(s.directionKey || "") +
                      " · " + AgeText.ageText(s.ageSeconds === undefined ? -1 : s.ageSeconds)
            }
            Label {
                visible: s.fresh === true
                anchors.horizontalCenter: parent.horizontalCenter
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                text: StateText.stateText(s.stateKey || "")
            }
            // Silent mode, always visible while it lasts (REQ-UI-15).
            SilentLine {
                anchors.horizontalCenter: parent.horizontalCenter
                silent: s.silent === true
                noLimit: s.silentNoLimit === true
                untilMs: s.silentUntilMs || 0
                untilToday: s.silentUntilToday === true
            }
            // This patient's alerts, each with its buttons. Nothing when there is none.
            SectionHeader {
                visible: (s.alertList || []).length > 0
                text: qsTrId("ui-alerts-title")
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
            Repeater {
                model: s.alertList || []
                delegate: AlertLine {
                    x: Theme.horizontalPageMargin
                    width: column.width - 2 * Theme.horizontalPageMargin
                    alert: modelData
                }
            }

            // The state strip when the value is not current: pictogram, words, buttons.
            Column {
                visible: s.fresh === false
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    text: StateText.stateText(s.stateKey || "")
                }
                Label {
                    visible: s.measuredAtMs > 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryColor
                    //% "Last reading %1 %2, %3"
                    text: qsTrId("ui-age-last-reading").arg(s.valueText || "").arg(s.unitText || "").arg(AgeText.ageText(s.ageSeconds === undefined ? -1 : s.ageSeconds))
                }
                Button {
                    visible: s.showRetry === true
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Retry"
                    text: qsTrId("ui-guard-retry")
                    onClicked: subjects.retry(page.subjectId)
                }
                Button {
                    visible: s.showAskAgain === true
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Ask now"
                    text: qsTrId("ui-strip-ask-again")
                    onClicked: monitoring.askAgain()
                }
                Button {
                    visible: s.showReenable === true
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Re-enable"
                    text: qsTrId("ui-guard-reenable")
                    onClicked: subjects.reenable(page.subjectId)
                }
            }

            // The diagnostic report proposal (REQ-GRD-15): after a second guard stop in a row the
            // service proposes a report for the developer: prepare it (the dialog asks for consent and writes
            // the file) or decline it, which tells the service (ReportHandled).
            Column {
                visible: s.reportProposed === true
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    //% "It happened twice. You can prepare a report for the developer."
                    text: qsTrId("ui-guard-report-proposed")
                }
                // The dialog that writes the report after consent.
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Prepare a report"
                    text: qsTrId("ui-guard-report-prepare")
                    onClicked: pageStack.push(Qt.resolvedUrl("DiagnosticReportDialog.qml"), { connection: s.connection })
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //% "Not now"
                    text: qsTrId("ui-guard-report-later")
                    onClicked: subjects.declineReport(page.subjectId)
                }
            }

            // Room above the plot, then the window times on a row of their own.
            Item { width: 1; height: Theme.paddingLarge * 2 }
            // The window: the start (left) and the end (right), date and time, nothing else,
            // aligned with the drawn plot area, not with the page.
            Item {
                width: parent.width
                height: startLabel.height
                Label {
                    id: startLabel
                    x: glucosePlot.x + glucosePlot.plotAreaLeft
                    font.pixelSize: page.plotLabelPx      // fixed, like the plot's labels
                    color: Theme.primaryColor
                    text: Format.formatDate(new Date(plot.fromMs), Formatter.DateShort)   // date and time, as the phone writes them
                }
                Label {
                    x: glucosePlot.x + glucosePlot.plotAreaLeft + glucosePlot.plotAreaWidth - width
                    font.pixelSize: page.plotLabelPx
                    color: Theme.primaryColor
                    text: Format.formatDate(new Date(plot.toMs), Formatter.DateShort)
                }
            }
            Item { width: 1; height: Theme.paddingMedium }

            // The plot. A horizontal drag scrolls back in time, a pinch zooms; a vertical drag
            // is left to the page (gesture lock, to be tuned on the device). With value
            // tracking on, a drag moves the cursor instead of scrolling.
            Item {
                id: plotArea
                width: parent.width
                height: Math.max(Theme.itemSizeHuge * 2.2, page.height * 0.36)

                GlucosePlot {
                    id: glucosePlot
                    anchors { fill: parent
                              leftMargin: Theme.horizontalPageMargin; rightMargin: Theme.horizontalPageMargin }
                    controller: plot
                    displayUnit: settings.displayUnit
                    axisLeft: settings.valueAxisLeft
                    lineColor: Theme.primaryColor
                    // Axis numbers and times in the strong text colour: the pale secondary colour is
                    // too light on a light ambience.
                    labelColor: Theme.primaryColor
                    // The plot's labels keep their size whatever the user's font size is: they
                    // share a fixed grid with the axis and would overlap if they grew. The readout
                    // box of value tracking may follow the font size, from a smaller start.
                    labelPixelSize: page.plotLabelPx
                    bubblePixelSize: Theme.fontSizeExtraSmall
                    bubbleColor: Theme.rgba(Theme.highlightDimmerColor, 0.92)
                    hour24: page.use24h
                    // The threshold lines in the secondary colour, the symbol sizes in reference pixels
                    // scaled to the phone (uiconstants.h).
                    thresholdColor: Theme.secondaryColor
                    pixelRatio: Theme.pixelRatio
                    //% "no data"
                    noDataText: qsTrId("ui-plot-no-data")
                    //% "monitoring not running"
                    gapText: qsTrId("ui-plot-gap")
                    //% "monitoring stopped"
                    stoppedText: qsTrId("ui-plot-stopped")
                    //% "no new reading"
                    noNewText: qsTrId("ui-plot-no-new")
                }
                // The newest reading: the round band badge of that reading, 28
                // reference pixels, blinking between full and 45 % opacity over 2 s while it is current;
                // the unknown badge, still, when the patient is stale. It sits on the plot item's
                // newest point and is clipped to the plot.
                Item {
                    anchors.fill: glucosePlot
                    clip: true
                    BandBadge {
                        id: newestBadge
                        visible: glucosePlot.newestVisible
                        bandKey: glucosePlot.newestStale ? "" : (s.bandKey || "")
                        size: 28 * Theme.pixelRatio             // kUiNewestBadgePx
                        x: glucosePlot.newestX - width / 2
                        y: glucosePlot.newestY - height / 2
                        readonly property bool blinks: visible && !glucosePlot.newestStale && s.fresh === true
                                                       && page.status === PageStatus.Active && Qt.application.active
                        opacity: 1.0
                        SequentialAnimation on opacity {
                            running: newestBadge.blinks
                            loops: Animation.Infinite
                            // kUiBlinkPeriodMs = 2000 (1 s each way), kUiBlinkLowOpacity = 0.45
                            NumberAnimation { to: 0.45; duration: 1000 }
                            NumberAnimation { to: 1.0; duration: 1000 }
                            onRunningChanged: if (!running) newestBadge.opacity = 1.0
                        }
                    }
                }
                PinchArea {
                    anchors.fill: glucosePlot
                    onPinchUpdated: {
                        if (pinch.previousScale > 0)
                            plot.zoomBy(pinch.scale / pinch.previousScale,
                                        Math.max(0, Math.min(1, pinch.center.x / width)))
                    }
                    MouseArea {
                        id: drag
                        anchors.fill: parent
                        property real lastX: 0
                        property bool horizontal: false
                        // Once the drag is horizontal nothing may take it from us (the page or its edge swipe).
                        preventStealing: horizontal || plot.tracking
                        onPressed: {
                            page.plotTouched = true
                            lastX = mouse.x; horizontal = false
                            if (plot.tracking) { flick.interactive = false; glucosePlot.trackAt(mouse.x) }
                        }
                        onPositionChanged: {
                            if (plot.tracking) { glucosePlot.trackAt(mouse.x); return }
                            var dx = mouse.x - lastX
                            if (Math.abs(dx) > Theme.paddingSmall || horizontal) {
                                horizontal = true
                                flick.interactive = false
                                plot.panByFraction(dx / width)
                                lastX = mouse.x
                            }
                        }
                        onReleased: { flick.interactive = true; page.plotTouched = false }
                        onCanceled: { flick.interactive = true; page.plotTouched = false }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: !plot.hasData
                    color: Theme.secondaryColor
                    //% "No readings in this period"
                    text: qsTrId("ui-plot-empty")
                }
            }

            // Value tracking: on, a drag on the plot reads the values under the finger; off,
            // a drag scrolls in time.
            TextSwitch {
                checked: plot.tracking
                //% "Value tracking"
                text: qsTrId("ui-plot-tracking")
                //% "On: drag on the plot to read values. Off: drag to scroll in time."
                description: qsTrId("ui-plot-tracking-hint")
                onClicked: { plot.tracking = checked; if (checked) plot.setCursorFraction(1) }
            }
            // The other Subjects: trouble elsewhere stays visible (docs 1.4).
            Item { width: 1; height: Theme.paddingLarge }
            Flow {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Repeater {
                    model: subjects
                    delegate: BackgroundItem {
                        visible: subjectId !== page.subjectId
                        width: chipRow.width + 2 * Theme.paddingMedium
                        height: Theme.itemSizeExtraSmall
                        onClicked: pageStack.replace(Qt.resolvedUrl("SubjectPage.qml"), { subjectId: subjectId })
                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: Theme.paddingSmall
                            Label { text: model.shortLabel; color: Theme.primaryColor }   // the short label, as on the cover
                            // The round badge (the unknown badge when the reading is not current)
                            // and the value; a patient in trouble also shows the pictogram of its state,
                            // so trouble elsewhere is visible without leaving the page (docs 1.4).
                            BandBadge {
                                anchors.verticalCenter: parent.verticalCenter
                                bandKey: model.bandKey
                                size: Theme.iconSizeSmall * 0.8
                            }
                            Label {
                                visible: fresh
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.valueText
                                color: Theme.secondaryColor
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Icon {
                                visible: !fresh
                                anchors.verticalCenter: parent.verticalCenter
                                source: Style.pictogramSource(Style.iconForState(stateKey))
                                color: Theme.primaryColor
                                width: Theme.iconSizeSmall; height: width
                            }
                        }
                    }
                }
            }
            // The sensor the patient wears.
            Item { width: 1; height: Theme.paddingLarge }
            SensorBlock {
                width: parent.width
                sensor: s.sensor || ({})
            }
        }
        VerticalScrollDecorator { }
    }
}
