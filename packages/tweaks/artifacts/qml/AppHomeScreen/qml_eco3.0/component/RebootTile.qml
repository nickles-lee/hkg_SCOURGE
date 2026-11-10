// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// TWEAKS_PAYLOAD_260128
// REBOOT_PAYLOAD_260128
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

Item {
    id: root
    width: 306
    height: 249

    // Stock interaction rules: beep on release and a slide-off cancels.
    property bool pressActive: false
    signal clicked()

    Q_ThemeImage {
        id: bg
        anchors.centerIn: parent
        themeSource: mouseArea.pressed && root.pressActive
                     ? defineModel.home_bg_icon_p : defineModel.home_bg_icon_n
    }

    Image {
        id: icon
        anchors.top: parent.top
        anchors.topMargin: 51
        anchors.horizontalCenter: parent.horizontalCenter
        width: 99
        height: 99
        smooth: true
        source: "file:///app/share/AppHomeScreen/tweaks/reboot_icon_n.png"
    }

    QText {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: icon.bottom
        anchors.verticalCenterOffset: 38
        text: "Reboot"
        font.pixelSize: ConstantModel.FONT_SIZE_39
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        onPressed: root.pressActive = true
        onPositionChanged: if (!containsMouse) root.pressActive = false
        onReleased: {
            if (root.pressActive) {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                root.clicked()
            }
            root.pressActive = false
        }
    }
}
