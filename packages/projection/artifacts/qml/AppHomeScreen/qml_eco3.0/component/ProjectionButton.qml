// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

/* Local copy of the themed push/segment button.  Press swaps the fill;
   beep plays on release only. */
Item {
    id: root
    width: 240
    height: 78
    property string label: ""
    property int labelSize: 33
    property bool selected: false
    property bool buttonEnabled: true
    signal clicked()

    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
    readonly property bool down: mouse.pressed && mouse.containsMouse

    opacity: buttonEnabled ? 1.0 : 0.4

    Rectangle {
        anchors.fill: parent
        radius: 16
        color: root.down
               ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.32)
               : (root.selected ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22)
                                : Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.12))
        border.width: 1
        border.color: root.down ? root.accent : Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.35)
    }

    QText {
        anchors.centerIn: parent
        width: parent.width - 12
        text: root.label
        font.pixelSize: root.labelSize
        color: root.ink
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: root.buttonEnabled
        onClicked: {
            QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
            root.clicked()
        }
    }
}
