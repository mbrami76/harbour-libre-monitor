// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0

// The round band badge: an outer ring for the very low and very high
// bands, a white ring, the disc in the band colour and a glyph of triangles (three down, one down, two
// facing, one up, three up). The glyph tells the band without colour, so low and high stay apart for
// someone who cannot tell orange from yellow. The unknown badge (grey disc,
// question mark) stands for a reading that is not current: the stale badge.
//
// The drawings are the author's (design/export/ui/badge-<key>.svg), installed by app/app.pro into
// the application's icons/ui folder and never copied into the source tree; this item only shows one.
// They are plain SVG shapes, which Qt's SVG image plugin draws.
Item {
    id: badge
    // "ll", "l", "n", "h", "hh" (the model's bandKey); anything else draws the unknown badge.
    property string bandKey: ""
    property real size: Theme.iconSizeSmall
    // A thin dark outline that would keep the badge visible on a light ambience is still an open design
    // point: turning it on is this one line.
    readonly property bool outline: false

    readonly property string fileKey: ["ll", "l", "n", "h", "hh"].indexOf(bandKey) >= 0 ? bandKey : "unknown"
    width: size
    height: size

    Image {
        anchors.fill: parent
        source: Qt.resolvedUrl("../../icons/ui/badge-" + badge.fileKey + ".svg")
        sourceSize.width: Math.ceil(badge.size)
        sourceSize.height: Math.ceil(badge.size)
        smooth: true
    }
    Rectangle {
        visible: badge.outline
        anchors.fill: parent
        radius: width / 2
        color: "transparent"
        border.width: 1
        border.color: "#202020"
    }
}
