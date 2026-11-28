// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"

/* First-open / fresh-status-pending state.  The stock QLoadingComponent needs
   defineModel.img_loading frame images that are not present in the render
   harness, so this local ring is the prototype stand-in (see BUILD_NOTES). */
Item {
    id: root
    anchors.fill: parent

    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE

    Rectangle {
        id: ring
        width: 117
        height: 117
        radius: 58
        color: "transparent"
        border.width: 10
        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.25)
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round((parent.height - 220) / 2)
        RotationAnimation on rotation {
            running: root.visible
            from: 0
            to: 360
            duration: 1400
            loops: Animation.Infinite
        }
        Rectangle {
            x: -5
            y: -5
            width: 20
            height: 20
            radius: 10
            color: root.accent
        }
    }

    QText {
        anchors.horizontalCenter: parent.horizontalCenter
        y: ring.y + ring.height + 40
        width: parent.width - 48
        text: "Getting the latest status…"
        font.pixelSize: ConstantModel.FONT_SIZE_33
        horizontalAlignment: Text.AlignHCenter
    }
}
