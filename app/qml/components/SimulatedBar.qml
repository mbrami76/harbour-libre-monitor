// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0

// The permanent SIMULATED DATA indication of a development build (REQ-SIM-09). Present only
// when the C++ side says the build has the flag `devsource`; a release build never shows it
// because it has no simulated data to show.
Rectangle {
    visible: devsource
    width: parent ? parent.width : Screen.width
    height: visible ? Theme.itemSizeExtraSmall / 2 : 0
    color: "black"
    Label {
        anchors.centerIn: parent
        font.pixelSize: Theme.fontSizeExtraSmall
        color: "white"
        //% "SIMULATED DATA"
        text: qsTrId("ui-simulated")
    }
}
