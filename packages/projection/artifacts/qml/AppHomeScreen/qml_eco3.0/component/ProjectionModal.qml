// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "../component/common"
import HMIEVENTS 1.0

/* Stock-style restore confirmation.  The fade, 972 px card, 78 px buttons and
   Yes-left ordering follow RebootPage.qml.  The stock 3-slice popup images are
   not available in the harness, so a themed surface sits behind them. */
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 100

    property bool pending: false
    signal confirmed()
    signal cancelled()

    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
    readonly property bool day: AppHomeModel.dayLightSuffix === "#GUI_DAY"

    function surfaceColor(a) {
        return Qt.tint(root.day ? "#ffffff" : "#0a1119",
                       Qt.rgba(root.ink.r, root.ink.g, root.ink.b, a))
    }

    Rectangle {
        anchors.fill: parent
        color: ConstantModel.GENERAL_COLOR_BLACK
        opacity: ConstantModel.GENERAL_OPACITY_PERCENT_70
    }

    MouseArea {
        anchors.fill: parent
        z: 1
        preventStealing: true
        acceptedButtons: Qt.AllButtons
        onPressed: mouse.accepted = true
        onReleased: {
            mouse.accepted = true
            if (!root.pending) {
                QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                root.dismiss()
            }
        }
        onWheel: wheel.accepted = true
    }

    QPopupAnimationItem {
        id: popup
        anchors.centerIn: parent
        z: 2
        width: 972
        height: 342

        Rectangle {
            anchors.fill: parent
            radius: 6
            color: root.surfaceColor(0.10)
            border.width: 1
            border.color: Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.40)
        }

        // The stock co_ic_question renders on the head unit; the local ring is
        // the harness-visible stand-in and also a clear confirmation cue.
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 24
            width: 54
            height: 54
            radius: 27
            color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22)
            border.width: 2
            border.color: root.accent
            QText {
                anchors.centerIn: parent
                text: "?"
                font.pixelSize: ConstantModel.FONT_SIZE_36
                textBold: true
                color: root.accent
            }
        }

        QText {
            id: title
            anchors.horizontalCenter: parent.horizontalCenter
            y: 92
            width: parent.width - 84
            text: "Restore stock behaviour?"
            font.pixelSize: ConstantModel.FONT_SIZE_39
            textBold: true
            horizontalAlignment: Text.AlignHCenter
        }

        QText {
            id: body
            x: 42
            y: 146
            width: parent.width - 84
            height: 100
            text: "This turns off HUD navigation and puts the car back the way it was. You can turn it on again here."
            font.pixelSize: 33
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignTop
            lineHeightMode: Text.FixedHeight
            lineHeight: 45
        }

        Item {
            x: 12
            width: (popup.width - 36) / 2
            height: 78
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            Rectangle {
                anchors.fill: parent
                radius: 8
                color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22)
                border.width: 1
                border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.55)
            }
            QHMI_ToggleButton {
                anchors.fill: parent
                label: "Restore"
                labelSize: ConstantModel.FONT_SIZE_39
                labelColor: root.ink
                onBtnRelease: {
                    if (!root.pending) root.pending = true
                    root.confirmed()
                }
            }
        }

        Item {
            x: popup.width - 12 - (popup.width - 36) / 2
            width: (popup.width - 36) / 2
            height: 78
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            Rectangle {
                anchors.fill: parent
                radius: 8
                color: Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.35)
            }
            QHMI_ToggleButton {
                anchors.fill: parent
                label: "Cancel"
                labelSize: ConstantModel.FONT_SIZE_39
                labelColor: root.ink
                onBtnRelease: root.dismiss()
            }
        }

        onClosePopupFinish: {
            if (root.pending)
                popup.opacity = 1
            else
                root.visible = false
        }
    }

    function open() {
        pending = false
        visible = true
        popup.opacity = 0
        popup.startOpenAnimation()
    }

    function dismiss() {
        if (pending) return
        popup.startCloseAnimation()
    }

    function finish() {
        pending = false
        popup.startCloseAnimation()
    }
}
