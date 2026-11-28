// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

/* Home-screen tile (306×249 cell).  Full-cell press swap, slide-off cancel
   and release-only beep follow RebootTile.qml, not the simpler TweaksTile. */
Item {
    id: root
    width: 306
    height: 249
    signal clicked()

    property bool pressActive: false
    // No extra panel: the stock icon backing (home_bg_icon) is the only plate, as on
    // RebootTile/TweaksTile (car 2026-11-27: an extra panel showed as a box).

    Q_ThemeImage {
        id: bg
        anchors.centerIn: parent
        themeSource: typeof defineModel.home_bg_icon_p !== "undefined"
                     ? (mouseArea.pressActive ? defineModel.home_bg_icon_p
                                              : defineModel.home_bg_icon_n)
                     : ""
    }

    Image {
        id: icon
        anchors.top: parent.top
        anchors.topMargin: 51
        anchors.horizontalCenter: parent.horizontalCenter
        width: 99
        height: 99
        smooth: true
        source: "file:///app/share/AppHomeScreen/projection/projection_icon_n.png"
    }

    QText {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: icon.bottom
        anchors.verticalCenterOffset: 38
        text: "Projection+"
        font.pixelSize: ConstantModel.FONT_SIZE_39
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        property bool pressActive: false
        onPressed: {
            mouse.accepted = true
            pressActive = true
            root.pressActive = true
        }
        onPositionChanged: {
            if (!containsMouse) {
                pressActive = false
                root.pressActive = false
            }
        }
        onCanceled: {
            pressActive = false
            root.pressActive = false
        }
        onReleased: {
            mouse.accepted = true
            var wasActive = pressActive
            pressActive = false
            root.pressActive = false
            if (wasActive) {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                root.clicked()
            }
        }
    }
}
