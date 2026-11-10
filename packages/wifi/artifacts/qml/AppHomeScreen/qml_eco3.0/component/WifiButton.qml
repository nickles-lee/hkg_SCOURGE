// WIFI_PAYLOAD_260128
// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import "common"

Item {
    id: root
    width: 180
    height: 54
    property string label: ""
    property bool selected: false
    property bool buttonEnabled: true
    signal clicked()

    opacity: buttonEnabled ? 1.0 : 0.35

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: root.selected ? "#2E7D32" : (mouse.pressed ? "#3A3A3A" : "#1F1F1F")
        border.color: root.selected ? "#4CAF50" : "#4A4A4A"
        border.width: 1
    }

    QText {
        anchors.centerIn: parent
        text: root.label
        font.pixelSize: 24
        color: "#FFFFFF"
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: root.buttonEnabled
        onClicked: root.clicked()
    }
}
