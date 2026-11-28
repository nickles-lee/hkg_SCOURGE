// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"

/* The pane's first block: one plain sentence about what is happening now.
   Technical state stays one level deeper in Diagnostics. */
Item {
    id: root
    width: parent ? parent.width : 760
    height: 152

    property string statusText: ""
    property string sourceText: ""

    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE

    Rectangle {
        anchors.fill: parent
        anchors.margins: 4
        radius: 16
        color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.10)
        border.width: 1
        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22)
    }

    Column {
        x: 24
        y: 14
        width: parent.width - 48
        spacing: 4

        QText {
            width: parent.width
            height: 34
            text: "Right now"
            font.pixelSize: ConstantModel.FONT_SIZE_27
            textBold: true
            color: root.accent
            verticalAlignment: Text.AlignVCenter
        }
        QText {
            width: parent.width
            height: 45
            text: root.statusText
            font.pixelSize: ConstantModel.FONT_SIZE_36
            textBold: true
            color: root.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        QText {
            width: parent.width
            height: 34
            text: root.sourceText
            font.pixelSize: ConstantModel.FONT_SIZE_27
            color: root.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
