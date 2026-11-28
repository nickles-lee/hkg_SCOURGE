// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

/* Diagnostics is reached from its own left-nav entry.  The
   main page carries no technical words; this pane keeps versions, connection
   state and boot safety, with a single destructive action at the bottom. */
Item {
    id: root
    width: parent ? parent.width : 780
    height: parent ? parent.height : 520

    property string versionsText: "App 1.0 · service 1.0 · firmware 260319"
    property string aaConnectionText: "Active"
    property string cpConnectionText: "Not connected"
    property string bootSafetyText: "Armed"
    property string problemText: "No problem recorded"
    property bool controlsEnabled: true

    signal restoreRequested()

    readonly property bool day: AppHomeModel.dayLightSuffix === "#GUI_DAY"
    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
    // Custom warn hexes from PATTERN_CATALOG §12 (stock has no warn asset).
    readonly property color warn: day ? "#a64b00" : "#ffb300"

    Column {
        x: 0
        y: 0
        width: parent.width

        // Same section-header style as Android Auto / CarPlay, aligned to the
        // 24 px left margin used by every row below it.
        ProjectionRow {
            width: parent.width
            height: 48
            title: "Diagnostics"
            titleSize: 30
            titleColor: root.accent
            pressable: false
            separator: true
        }

        ProjectionRow {
            width: parent.width
            height: 78
            title: "Android Auto connection"
            titleSize: 30
            subtitle: root.aaConnectionText
            subtitleSize: 24
            pressable: false
        }

        ProjectionRow {
            width: parent.width
            height: 78
            title: "CarPlay connection"
            titleSize: 30
            subtitle: root.cpConnectionText
            subtitleSize: 24
            pressable: false
        }

        ProjectionRow {
            width: parent.width
            height: 78
            title: "Boot safety"
            titleSize: 30
            subtitle: root.bootSafetyText
            subtitleSize: 24
            pressable: false
        }

        ProjectionRow {
            width: parent.width
            height: 78
            title: "Versions"
            titleSize: 30
            subtitle: root.versionsText
            subtitleSize: 24
            pressable: false
        }

        ProjectionRow {
            width: parent.width
            height: 78
            title: "Last problem"
            titleSize: 30
            subtitle: root.problemText
            subtitleSize: 24
            pressable: false
            separator: false
        }

        // One destructive row at the bottom of the list, with its own
        // one-line helper.  No Back button: the left-nav entry returns.
        Item {
            id: restoreRow
            width: parent.width
            height: 84
            opacity: root.controlsEnabled ? 1.0 : 0.4

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: 16
                color: Qt.rgba(root.warn.r, root.warn.g, root.warn.b, 0.16)
                border.width: 1
                border.color: Qt.rgba(root.warn.r, root.warn.g, root.warn.b, 0.55)
            }

            Column {
                x: 24
                y: 8
                width: parent.width - 48
                spacing: 2
                QText {
                    width: parent.width
                    height: 38
                    text: "Restore stock behaviour"
                    font.pixelSize: 30
                    textBold: true
                    color: root.warn
                    verticalAlignment: Text.AlignVCenter
                }
                QText {
                    width: parent.width
                    height: 32
                    text: "Turns everything off. The car works as it did before installing."
                    font.pixelSize: ConstantModel.FONT_SIZE_24
                    color: root.ink
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.controlsEnabled
                onClicked: {
                    QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                    root.restoreRequested()
                }
            }
        }
    }
}
