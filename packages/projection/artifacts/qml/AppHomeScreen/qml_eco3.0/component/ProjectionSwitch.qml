// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

/* Local copy of the Gen5W-style pill toggle: accent when on, ink outline
   when off.  Beep is owned by the row when `interactive` is false. */
Item {
    id: root
    width: 96
    height: 54
    property bool checked: false
    property bool switchEnabled: true
    property bool interactive: true
    signal toggled()

    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE

    opacity: switchEnabled ? 1.0 : 0.4

    Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: root.checked
               ? (mouse.pressed ? Qt.darker(root.accent, 1.25) : root.accent)
               : Qt.rgba(root.ink.r, root.ink.g, root.ink.b, mouse.pressed ? 0.28 : 0.14)
        border.width: root.checked ? 0 : 2
        border.color: Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.6)
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    Rectangle {
        width: parent.height - 12
        height: width
        radius: width / 2
        y: 6
        x: root.checked ? parent.width - width - 6 : 6
        color: root.checked ? "#ffffff" : Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.9)
        Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: root.switchEnabled && root.interactive
        onClicked: {
            QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
            root.toggled()
        }
    }
}
