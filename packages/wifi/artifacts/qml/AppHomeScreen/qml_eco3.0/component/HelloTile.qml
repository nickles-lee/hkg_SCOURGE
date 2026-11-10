// HELLO_PAYLOAD_260128
// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item {
    id: root
    width: 306
    height: 249
    signal clicked()

    Q_ThemeImage {
        id: bg
        anchors.centerIn: parent
        themeSource: mouseArea.pressed ? defineModel.home_bg_icon_p : defineModel.home_bg_icon_n
    }

    Q_ThemeImage {
        id: icon
        anchors.top: parent.top
        anchors.topMargin: 51
        anchors.horizontalCenter: parent.horizontalCenter
        themeSource: "file:///app/share/AppHomeScreen/hello/hello_icon_n.png"
    }

    QText {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: icon.bottom
        anchors.verticalCenterOffset: 38
        text: "Hello"
        font.pixelSize: 39
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
