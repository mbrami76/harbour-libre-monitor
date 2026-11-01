// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StateText.js" as StateText
import "../components/AgeText.js" as AgeText

// The application cover (REQ-UI-14). In the ambience's own colours,
// with a faint stylised rod of Asclepius behind: big, tilted towards the bottom left corner and
// cut by the edge, barely visible but there. In the centre, the connected
// Subjects with their names and readings: two at most, the most attentionable first (a low, then a
// Subject that is not monitored, then a high).
// A reading that is not current has no band badge: a dash and the reason (REQ-UI-05, REQ-UI-09).
// The cover exists only while this UI process runs and is no substitute for the monitoring
// service. A patient in silent mode shows the struck-through speaker with the end time (REQ-UI-15).
CoverBackground {
    id: cover
    // At most this many Subjects fit on the cover.
    readonly property int maxRows: 2
    property var rows: subjects.coverRows(maxRows)

    Connections {
        target: subjects
        onChanged: cover.rows = subjects.coverRows(cover.maxRows)
    }

    // The rod of Asclepius, from the author's picture (images/asclepius.png, a silhouette stored
    // as transparency only). It is painted through a canvas and filled in the theme's text colour,
    // so it follows any ambience, at a very low opacity: barely visible, but there. Centred at 80 % of
    // the width and 72 % of the height, tilted 12 degrees towards the bottom left, and BIG: 1.12 of
    // the cover's height, twice the first size, cut by the cover's edges.
    Canvas {
        id: rod
        anchors.fill: parent
        opacity: 0.13
        readonly property url source: Qt.resolvedUrl("../images/asclepius.png")
        Component.onCompleted: loadImage(source)
        onImageLoaded: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (!isImageLoaded(source))
                return
            var h = height * 1.12                  // the rod's height: 1.12 of the cover or more
            var w = h * 223 / 720                  // keeps the picture's proportions
            ctx.save()
            ctx.translate(width * 0.80, height * 0.72)
            ctx.rotate(12 * Math.PI / 180)         // clockwise: the foot leans to the bottom left
            ctx.drawImage(source, -w / 2, -h / 2, w, h)
            ctx.restore()
            // Colour the silhouette: keep the fill only where the picture has opacity.
            ctx.globalCompositeOperation = "source-in"
            ctx.fillStyle = String(Theme.primaryColor)
            ctx.fillRect(0, 0, width, height)
        }
    }

    // The application title at the top of the cover.
    Label {
        id: title
        anchors { top: parent.top; topMargin: Theme.paddingMedium; left: parent.left; right: parent.right }
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        font.pixelSize: Theme.fontSizeSmall
        font.bold: true
        color: Theme.primaryColor
        //% "Libre Monitor"
        text: qsTrId("ui-app-name")
    }

    // Monitoring stopped: no readings at all, only the state.
    Label {
        visible: monitoring.status === "stopped"
        anchors { centerIn: parent }
        width: parent.width - 2 * Theme.paddingMedium
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: Theme.primaryColor
        //% "Monitoring stopped"
        text: qsTrId("ui-cover-stopped")
    }

    // The Subjects, centred: name, then reading and direction, then the age or the reason.
    Column {
        visible: monitoring.status !== "stopped" && cover.rows.length > 0
        anchors { verticalCenter: parent.verticalCenter; verticalCenterOffset: title.height / 2; left: parent.left; right: parent.right
                  leftMargin: Theme.paddingMedium; rightMargin: Theme.paddingMedium }
        spacing: Theme.paddingMedium

        Repeater {
            model: cover.rows
            delegate: Column {
                width: parent.width
                spacing: 0
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.primaryColor
                    // The name unless the user turned names off; then the short label the notifications use:
                    // the user's label, or P1, P2 by the place in the configuration file, never the cover's own
                    // ranking ( lm::patientLabel, computed by the model).
                    text: settings.coverShowsName ? StateText.displayName(modelData.name, modelData.shortLabel)
                                                  : modelData.shortLabel
                }
                ReadingBox {
                    anchors.horizontalCenter: parent.horizontalCenter
                    bandKey: modelData.bandKey || ""
                    valueText: modelData.valueText || ""
                    direction: modelData.directionSymbol || ""
                    size: Theme.fontSizeLarge
                    maxWidth: parent.width
                }
                // Silent mode: the struck-through speaker and its end time, always visible while it lasts
                // (REQ-UI-15), here in the cover's short form.
                SilentLine {
                    anchors.horizontalCenter: parent.horizontalCenter
                    compact: true
                    pixelSize: Theme.fontSizeTiny
                    maxTextWidth: parent.width
                    silent: modelData.silent === true
                    noLimit: modelData.silentNoLimit === true
                    untilMs: modelData.silentUntilMs || 0
                    untilToday: modelData.silentUntilToday === true
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.primaryColor
                    text: modelData.fresh === true
                          ? AgeText.ageText(modelData.ageSeconds === undefined ? -1 : modelData.ageSeconds)
                          : StateText.stateText(modelData.stateKey || "")
                }
            }
        }
        Label {
            visible: subjects.count > cover.rows.length
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeTiny
            color: Theme.primaryColor
            text: "+" + (subjects.count - cover.rows.length)
        }
    }

    Label {
        visible: monitoring.status !== "stopped" && cover.rows.length === 0
        anchors { centerIn: parent }
        width: parent.width - 2 * Theme.paddingMedium
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: Theme.primaryColor
        text: qsTrId("ui-welcome-setup")
    }
    // The one cover action : Start, while the
    // monitoring is stopped; Silica's own cover icon `icon-cover-play` (it exists in the emulator's
    // theme, checked by tests/integration/check_emulator_pictograms.py). It does what the Start
    // monitoring button of the pages does. With monitoring running the cover has no action.
    CoverActionList {
        enabled: monitoring.status === "stopped"
        CoverAction {
            iconSource: "image://theme/icon-cover-play"
            onTriggered: monitoring.startMonitoring()
        }
    }

    Label {
        visible: devsource
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingSmall }
        font.pixelSize: Theme.fontSizeTiny
        color: Theme.primaryColor
        text: qsTrId("ui-simulated")
    }
}
