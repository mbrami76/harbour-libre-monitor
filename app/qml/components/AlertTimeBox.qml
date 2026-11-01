// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "ThresholdText.js" as ThresholdText

// One alert time of a patient on the thresholds page:
// the choices come from the service's own range (Alerts() `limits`, thresholds.timeChoices), never from
// a number written here; a choice is written to the configuration file at once (configSettings.setAlertTime,
// the key) and the service applies it without a restart; the value in force comes back with
// the next Alerts() answer. `rev` changes whenever the thresholds model changes.
//
// The choices are menu items made by a Repeater, which exist only after the menu is filled: the current
// choice is set after that (sync), from -1 to the index, so that the combo box finds its item and shows the
// value (with a plain binding it showed the label alone on the emulator).
ComboBox {
    id: box
    property string subjectId
    property string kind
    property int rev: 0
    property var choices: { rev; return thresholds.timeChoices(kind) }

    width: parent ? parent.width : 0
    visible: choices.length > 0
    function sync() {
        var i = thresholds.timeIndex(kind)
        currentIndex = -1
        currentIndex = i
    }
    onRevChanged: syncLater.restart()
    Component.onCompleted: syncLater.restart()
    Timer {
        id: syncLater
        interval: 0
        onTriggered: box.sync()
    }
    menu: ContextMenu {
        Repeater {
            model: box.choices
            onCountChanged: syncLater.restart()
            MenuItem {
                text: ThresholdText.choiceText(modelData)
                onClicked: configSettings.setAlertTime(box.subjectId, box.kind, modelData.seconds)
            }
        }
    }
}
