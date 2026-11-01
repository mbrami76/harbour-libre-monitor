// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"

// About: the statement that this is a supplementary monitor
// (REQ-SAF-01), the unofficial source (REQ-DOC-04) and the licence. The developer's contact has one
// source with the diagnostic report dialog (report.developerContact): the build define LM_DEVELOPER_CONTACT,
// filled from the spec's Packager line since 0.1-3.
Page {
    allowedOrientations: Orientation.All
    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge
        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge
            SimulatedBar { }
            PageHeader {
                //% "About"
                title: qsTrId("ui-menu-about")
            }
            // The version of this build (no stage word).
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Version %1"
                text: qsTrId("ui-about-version").arg(appVersion)
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                //% "Libre Monitor is a supplementary monitor. It is not a medical device. Do not base therapy decisions on it."
                text: qsTrId("ui-safety-statement")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "The data come from an unofficial, undocumented interface of the glucose service. It may change or stop working without notice."
                text: qsTrId("ui-about-unofficial")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "The direction arrow is the one the source reports; its meaning is not officially documented."
                text: qsTrId("ui-about-trend")
            }
            // The silent profile cannot be read by the application: the plain sentence instead.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTrId("ui-about-silent-profile")
            }
            SectionHeader {
                //% "Reading the plot"
                text: qsTrId("ui-about-plot-title")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Solid line: current feed. Dashed line: history feed. Dotted lines: the low and the high limit. Solid lines: the very low and the very high limit. Hatched: monitoring was not running."
                text: qsTrId("ui-plot-legend")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Free software under the GNU General Public License, version 3."
                text: qsTrId("ui-about-licence")
            }
            // The developer's contact: the same single source as the diagnostic report dialog (the build define
            // LM_DEVELOPER_CONTACT, read by the report controller), the address of the spec's Packager line
            // since 0.1-3. The second text is only for a build without an address.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: report.developerContact !== ""
                      //% "Developer: %1"
                      ? qsTrId("ui-about-developer").arg(report.developerContact)
                      //% "The developer's address is not in this package yet."
                      : qsTrId("ui-about-developer-none")
            }
        }
        VerticalScrollDecorator { }
    }
}
