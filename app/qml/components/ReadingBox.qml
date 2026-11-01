// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "Style.js" as Style

// The glucose reading as one scalable object: a bordered box with
// a background and three cells of FIXED width, so the size of the box is the same for every reading
// and can be predicted before drawing:
//
//     [ band ][ value ][ trend ]      2 + 4 + 2 character widths
//
//  - band cell: the round band badge: its triangle glyph carries the band besides the colour;
//    a reading that is not current shows the unknown badge there
//  - value cell: the number, centred (up to 4 characters, "13.9" or "250"), monospace
//  - trend cell: the trend glyph (↓ ↘ ≈ ↗ ↑), in one font that has all five, so the diagonal
//    arrows are not drawn heavier from a fallback font
//
// Colour: a current reading keeps the band's own fill with its fixed text colour (REQ-UI-03). A reading that is not current has no band colour: a transparent box with a dash, the
// unknown badge and the trend cell empty (REQ-UI-05, REQ-UI-09). Everything scales with `size` (the
// text pixel size); `maxWidth` shrinks it to fit a narrow place such as the cover.
Item {
    id: box
    property string bandKey                 // "ll" .. "hh"; empty = not current
    property string valueText
    property string direction               // the trend glyph; empty = none
    property real size: Theme.fontSizeMedium
    property real maxWidth: 0               // 0 = no limit

    readonly property bool current: bandKey.length > 0
    readonly property int valueChars: 4
    readonly property real padUnits: 0.35   // in units of the text size
    readonly property real gapUnits: 0.30
    // Width of one monospace character per unit of text size, measured once.
    readonly property real charUnits: ruler.implicitWidth / 100
    readonly property real totalUnits: 2 * padUnits + 2 * gapUnits + charUnits * (2 + valueChars + 2)
    readonly property real effectiveSize: maxWidth > 0 ? Math.min(size, maxWidth / totalUnits) : size
    readonly property real cell: charUnits * effectiveSize

    width: totalUnits * effectiveSize
    height: effectiveSize * 1.75

    Text {
        id: ruler
        visible: false
        text: "0"
        font.family: "monospace"
        font.bold: true
        font.pixelSize: 100
    }

    Rectangle {
        anchors.fill: parent
        radius: box.effectiveSize * 0.3
        color: box.current ? Style.bandFill[box.bandKey] : "transparent"
        border.width: Math.max(1, Math.round(box.effectiveSize / 12))
        border.color: Theme.primaryColor
        opacity: box.current ? 1.0 : 0.7
    }

    Row {
        anchors.centerIn: parent
        spacing: box.gapUnits * box.effectiveSize
        Item {                                  // band cell: the round badge, or the unknown badge
            width: 2 * box.cell; height: box.height
            BandBadge {
                anchors.centerIn: parent
                bandKey: box.bandKey
                size: Math.min(2 * box.cell, box.height * 0.8)
            }
        }
        Item {                                  // value cell
            width: box.valueChars * box.cell; height: box.height
            Text {
                anchors.centerIn: parent
                font.family: "monospace"; font.bold: true; font.pixelSize: box.effectiveSize
                color: box.current ? Style.bandText[box.bandKey] : Theme.primaryColor
                text: box.current ? box.valueText : "—"
            }
        }
        Item {                                  // trend cell
            width: 2 * box.cell; height: box.height
            // A word instead of a glyph ("NULL" when no trend code arrived, an unknown code as a number,
            //) is shrunk to the cell instead of running over the box's border (seen on
            // the emulator after a restart of the service).
            Text {
                anchors.centerIn: parent
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
                font.family: "DejaVu Sans"
                font.pixelSize: box.effectiveSize * 0.8
                color: box.current ? Style.bandText[box.bandKey] : Theme.primaryColor
                text: box.current ? box.direction : ""
            }
        }
    }
}
