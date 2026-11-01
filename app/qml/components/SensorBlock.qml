// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style

// The sensor block of the patient page (REQ-SEN-01 to 03). It shows what the model decided (app/ui/sensorview.h): the identifier only in its short hashed
// form, never as stored; the start; the end of the warm-up while it runs, with the sentence that the
// no-data alerts wait meanwhile (REQ-SEN-03); when the monitoring first and last saw the sensor. The
// pictogram is the author's own drawing `sensor`, tinted like Silica's icons; no colour (REQ-UI-04).
Column {
    id: block
    property var sensor: ({})
    spacing: Theme.paddingSmall

    function when(ms) {
        if (ms === undefined || ms < 0)
            //% "not known"
            return qsTrId("ui-sensor-unknown-time")
        var d = new Date(ms)
        return Format.formatDate(d, Formatter.DateMedium) + " " + Format.formatDate(d, Formatter.TimeValue)
    }

    Row {
        x: Theme.horizontalPageMargin
        spacing: Theme.paddingMedium
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            source: Style.pictogramSource(Style.pictograms["sensor"])
            color: Theme.highlightColor
            width: Theme.iconSizeSmall; height: width
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeSmall
            //% "Sensor"
            text: qsTrId("ui-sensor-title")
        }
    }
    Label {
        visible: block.sensor.known !== true
        x: Theme.horizontalPageMargin
        width: block.width - 2 * Theme.horizontalPageMargin
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.secondaryColor
        //% "No sensor is known yet for this patient."
        text: qsTrId("ui-sensor-none")
    }
    Column {
        visible: block.sensor.known === true
        x: Theme.horizontalPageMargin
        width: block.width - 2 * Theme.horizontalPageMargin
        spacing: Theme.paddingSmall
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.primaryColor
            //% "Identifier (shortened): %1"
            text: qsTrId("ui-sensor-id").arg(block.sensor.shortId || "")
        }
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.primaryColor
            //% "Started: %1"
            text: qsTrId("ui-sensor-activated").arg(block.when(block.sensor.activatedMs))
        }
        Label {
            visible: block.sensor.warmingUp === true
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.primaryColor
            //% "Warming up until %1. No-data alerts wait until then."
            text: qsTrId("ui-sensor-warmup").arg(block.when(block.sensor.warmupEndMs))
        }
        Label {
            visible: block.sensor.warmingUp !== true && block.sensor.warmupEndMs >= 0
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            //% "Warm-up ended: %1"
            text: qsTrId("ui-sensor-warmup-ended").arg(block.when(block.sensor.warmupEndMs))
        }
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            //% "First seen: %1"
            text: qsTrId("ui-sensor-first").arg(block.when(block.sensor.firstSeenMs))
        }
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            //% "Last seen: %1"
            text: qsTrId("ui-sensor-last").arg(block.when(block.sensor.lastSeenMs))
        }
    }
}
