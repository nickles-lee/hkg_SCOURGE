// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"
import HMIEVENTS 1.0

/*
 * One list row: bold title, one or two secondary lines and an optional
 * trailing control declared by the caller.  `plate: true` makes it a left-nav
 * navigation card; `stacked: true` puts the trailing control (a segmented
 * picker) on its own line under the helper text, used by the per-source rows.
 */
Item {
    id: root
    width: parent ? parent.width : 800
    height: 108

    property string title: ""
    property string subtitle: ""
    property string detail: ""
    property color titleColor: ink
    property color subtitleColor: ink
    property color detailColor: ink
    property int titleSize: 39
    property int subtitleSize: 27
    property bool selected: false
    property bool pressable: true
    property bool rowEnabled: true
    property bool separator: true
    property bool plate: false
    property bool stacked: false
    property bool subtitleWrap: false
    property int trailingWidth: 0
    property int trailingHeight: 0
    property int radius: plate ? 18 : 0
    default property alias trailing: trailingHolder.data
    signal clicked()

    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
    readonly property bool down: mouse.pressed && mouse.containsMouse && pressable && rowEnabled

    opacity: rowEnabled ? 1.0 : 0.4

    function textHeight() {
        var h = Math.round(root.titleSize * 1.25)
        if (root.subtitle !== "")
            h += 3 + Math.round(root.subtitleSize * 1.3) * (root.subtitleWrap ? 2 : 1)
        if (root.detail !== "") h += 3 + Math.round(root.subtitleSize * 1.3)
        return h
    }

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.selected
               ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22)
               : (root.down ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                            : "transparent")
    }

    Item {
        id: textArea
        x: 24
        width: root.stacked
               ? Math.max(0, root.width - 48)
               : Math.max(0, root.width - 48 - (trailingHolder.width > 0 ? trailingHolder.width + 24 : 0))
        height: root.stacked ? Math.min(root.height - 8, textHeight() + 8) : root.height

        Column {
            id: col
            width: parent.width
            y: root.stacked ? 8 : Math.max(0, (parent.height - textHeight()) / 2)
            spacing: 3

            QText {
                width: parent.width
                text: root.title
                font.pixelSize: root.titleSize
                height: Math.round(root.titleSize * 1.25)
                verticalAlignment: Text.AlignVCenter
                textBold: true
                color: root.titleColor
                elide: Text.ElideRight
            }

            QText {
                visible: root.subtitle !== ""
                width: parent.width
                text: root.subtitle
                font.pixelSize: root.subtitleSize
                height: Math.round(root.subtitleSize * 1.3) * (root.subtitleWrap ? 2 : 1)
                verticalAlignment: Text.AlignVCenter
                color: root.subtitleColor
                wrapMode: root.subtitleWrap ? Text.WordWrap : Text.NoWrap
                elide: root.subtitleWrap ? Text.ElideNone : Text.ElideRight
            }

            QText {
                visible: root.detail !== ""
                width: parent.width
                height: Math.round(root.subtitleSize * 1.3)
                verticalAlignment: Text.AlignVCenter
                text: root.detail
                font.pixelSize: root.subtitleSize
                color: root.detailColor
                elide: Text.ElideRight
            }
        }
    }

    Item {
        id: trailingHolder
        anchors.right: parent.right
        anchors.rightMargin: 24
        y: root.stacked ? Math.max(0, parent.height - trailingSize() - 8)
                        : Math.max(0, (parent.height - trailingSize()) / 2)
        width: root.trailingWidth > 0 ? root.trailingWidth : childrenRect.width
        height: root.trailingHeight > 0 ? root.trailingHeight : childrenRect.height

        function trailingSize() {
            return root.trailingHeight > 0 ? root.trailingHeight : childrenRect.height
        }
    }

    Rectangle {
        visible: root.separator && !root.plate
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.bottom: parent.bottom
        height: 1
        color: Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.22)
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        z: -1                       // below trailing controls so they keep their own taps
        enabled: root.rowEnabled && root.pressable
        onClicked: {
            QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
            root.clicked()
        }
    }
}
