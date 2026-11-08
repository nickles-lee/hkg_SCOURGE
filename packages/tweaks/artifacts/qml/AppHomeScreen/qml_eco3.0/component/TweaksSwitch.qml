// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// TWEAKS_PAYLOAD_260128
import QtQuick 2.0
import "common"

Item {
    id: root
    width: 120
    height: 60
    property bool checked: false
    property bool switchEnabled: true
    signal toggled()

    opacity: switchEnabled ? 1.0 : 0.35

    Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? "#2E7D32" : "#3A3A3A"
        border.color: root.checked ? "#4CAF50" : "#6A6A6A"
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
        onClicked: root.toggled()
    }
}
