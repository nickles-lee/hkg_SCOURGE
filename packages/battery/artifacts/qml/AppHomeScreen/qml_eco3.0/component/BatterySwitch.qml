// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// BATTERY_PAYLOAD_260128
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

Item {
    id: root
    width: 120
    height: 60
    property bool checked: false
    property bool switchEnabled: true
    // Stock interaction rules (styleguide §4): beep on release, and a
    // slide-off press cancels the click.
    property bool pressActive: false
    signal toggled()

    opacity: switchEnabled ? 1.0 : 0.35

    Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? "#2E7D32" : ConstantModel.GENERAL_COLOR_BUTTON_GREY
        border.color: root.checked ? "#4CAF50" : ConstantModel.GENERAL_COLOR_FOCUS_GREY
        border.width: 2
    }

    Rectangle {
        id: knob
        width: parent.height - 12
        height: width
        radius: width / 2
        y: 6
        x: root.checked ? parent.width - width - 6 : 6
        color: "#F5F5F5"

        Behavior on x {
            NumberAnimation { duration: 120 }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.switchEnabled
        onPressed: root.pressActive = true
        onPositionChanged: if (!containsMouse) root.pressActive = false
        onReleased: {
            if (root.pressActive) {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                root.toggled()
            }
            root.pressActive = false
        }
    }
}
