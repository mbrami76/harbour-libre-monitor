// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"
import "../components/StateText.js" as StateText
import "../components/Style.js" as Style

// The account page: enter the account, TEST the login, choose the
// patients to follow and check their names and units, then SAVE. Nothing is stored until Save, and
// Save works only for the account name and password that were tested; changing either field
// throws the test away. The logic is in AccountSetup (tested); this page only presents it.
Page {
    id: page
    allowedOrientations: Orientation.All
    // Leaving the page forgets the test and the password.
    onStatusChanged: if (status === PageStatus.Deactivating) account.reset()
    Component.onCompleted: account.reset()

    Connections {
        target: account
        onChanged: if (account.state === "saved") pageStack.pop()
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            // Room between every element (things were placed too near each other).
            spacing: Theme.paddingLarge

            SimulatedBar { }
            PageHeader {
                //% "Account"
                title: qsTrId("ui-acct-title")
            }

            TextField {
                id: userField
                width: parent.width
                enabled: !account.busy
                //% "Account name (e-mail)"
                label: qsTrId("ui-acct-user")
                placeholderText: label
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: passField.focus = true
                onTextChanged: account.fieldsChanged(text, passField.text)
            }
            PasswordField {
                id: passField
                width: parent.width
                enabled: !account.busy
                //% "Password"
                label: qsTrId("ui-acct-pass")
                placeholderText: label
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
                onTextChanged: account.fieldsChanged(userField.text, text)
            }

            Item { width: 1; height: Theme.paddingMedium }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: account.canTest
                //% "Test login"
                text: qsTrId("ui-acct-test")
                onClicked: { passField.focus = false; account.test() }
            }
            Item { width: 1; height: Theme.paddingSmall }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.primaryColor
                //% "Nothing is saved until you press Save."
                text: qsTrId("ui-acct-nothing-saved")
            }

            // Working.
            Row {
                visible: account.state === "testing" || account.state === "saving"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                BusyIndicator { running: parent.visible; size: BusyIndicatorSize.Small }
                Label {
                    width: parent.width - Theme.itemSizeSmall
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    anchors.verticalCenter: parent.verticalCenter
                    text: account.state === "testing"
                          //% "Contacting the source… this can take several seconds."
                          ? qsTrId("ui-acct-testing")
                          //% "Saving…"
                          : qsTrId("ui-acct-saving")
                }
            }

            // A failure: pictogram and words, neutral colour (REQ-UI-04).
            Row {
                visible: account.state === "test-failed" || account.state === "save-failed"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Icon {
                    source: Style.pictogramSource(Style.pictograms["problem"])
                    color: Theme.primaryColor
                    width: Theme.iconSizeSmall; height: width
                }
                Label {
                    width: parent.width - Theme.iconSizeSmall - parent.spacing
                    wrapMode: Text.WordWrap
                    color: Theme.primaryColor
                    text: StateText.accountError(account.errorKey)
                }
            }

            // The people found, after a successful test.
            Label {
                visible: account.state === "test-ok"
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                //% "The login works. Choose the patients to follow and check their names and units."
                text: qsTrId("ui-acct-ok")
            }
            Repeater {
                model: account.state === "test-ok" ? account.subjects : []
                delegate: Column {
                    width: column.width
                    spacing: Theme.paddingMedium
                    TextSwitch {
                        checked: modelData.follow
                        //% "Follow"
                        text: qsTrId("ui-acct-follow")
                        onClicked: account.setFollow(index, checked)
                    }
                    TextField {
                        width: parent.width
                        visible: modelData.follow
                        //% "Name"
                        label: qsTrId("ui-acct-name")
                        text: modelData.name
                        onTextChanged: if (activeFocus) account.setName(index, text)
                    }
                    ComboBox {
                        width: parent.width
                        visible: modelData.follow
                        label: StateText.unitBasisText(modelData.unitBasis, modelData.unit)
                        currentIndex: modelData.unit === "mg/dL" ? 1 : 0
                        menu: ContextMenu {
                            MenuItem { text: "mmol/L"; onClicked: account.setUnit(index, "mmol/L") }
                            MenuItem { text: "mg/dL"; onClicked: account.setUnit(index, "mg/dL") }
                        }
                    }
                }
            }

            Button {
                visible: account.state === "test-ok"
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: account.canSave
                //% "Save"
                text: qsTrId("ui-acct-save")
                onClicked: {
                    // The connection's default label, the name the notices about this account use
                    // instead of an account name; changeable in the settings.
                    //% "Account"
                    account.setDefaultConnectionLabel(qsTrId("ui-account-default-label"))
                    account.save()
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
