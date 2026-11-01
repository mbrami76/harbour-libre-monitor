// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0

// The one explicit confirmation before silent mode is set WITHOUT LIMIT (REQ-UI-15): it
// says that the very low and very high alerts will be silent as well. The text is the one approved in
// in English and Italian. Nothing is sent until the user accepts; going back sends nothing.
Dialog {
    id: dialog
    allowedOrientations: Orientation.All
    property string subjectId

    onAccepted: alerts.confirmSilentWithoutLimit(dialog.subjectId)

    Column {
        width: parent.width
        spacing: Theme.paddingLarge
        DialogHeader {
            //% "Silence"
            acceptText: qsTrId("ui-silent-confirm-accept")
        }
        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            wrapMode: Text.WordWrap
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
            //% "Without a limit, alerts for this patient stay silent until you end silent mode, very low and very high alerts included. Silence them?"
            text: qsTrId("ui-silent-confirm")
        }
    }
}
