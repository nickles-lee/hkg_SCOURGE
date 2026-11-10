// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// TWEAKS_PAYLOAD_260128
// REBOOT_PAYLOAD_260128
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0

/*
 * Reboot confirmation overlay.  The tile opens this page, which immediately
 * shows a stock-style yes/no modal.  Visibility is owned by this page; hosts
 * only call open()/close() (and onGoHome for the Home-key contract).
 */
Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal goHome()

    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    property bool sysPopupShowing: typeof AppHomeModel !== "undefined"
                                    ? AppHomeModel.sysPopupShowing : false
    readonly property bool driving: typeof AppHomeModel !== "undefined"
                                    ? AppHomeModel.drivingRestriction : false

    visible: requested && !atOtherApp
    focus: visible
    enabled: !sysPopupShowing
    opacity: sysPopupShowing ? 0 : 1

    property string mode: "confirm"       // confirm | pending | blocked | unavailable | error
    property bool serviceOk: false
    property bool serviceError: false
    property string rebootState: "idle"
    property string rebootError: ""
    property int homeSeq: 0
    property bool homeSeqSeeded: false
    property string homeSource: "none"
    property bool smackOk: true

    readonly property string daemonBase: "http://127.0.0.1:8091"
    readonly property bool pending: mode === "pending"

    function open() {
        requested = true
        homeSeqSeeded = false
        rebootError = ""
        serviceError = false
        if (rebootState === "rebooting") {
            mode = "pending"
        } else if (driving) {
            mode = "blocked"
        } else {
            mode = "confirm"
        }
        serviceOk = false
        popup.startOpenAnimation()
        refresh()
    }

    function close() {
        requested = false
        mode = "confirm"
        popup.opacity = 0
    }

    function dismiss() {
        if (mode === "pending")
            return
        popup.startCloseAnimation()
    }

    function applyStatus(d) {
        serviceOk = true
        serviceError = false
        rebootState = d.reboot_state ? d.reboot_state : "idle"
        rebootError = d.reboot_error ? d.reboot_error : ""
        homeSource = d.home_source ? d.home_source : "none"
        smackOk = d.smack_ok !== false

        if (typeof d.home_seq === "number") {
            if (!homeSeqSeeded) {
                homeSeq = d.home_seq
                homeSeqSeeded = true
            } else if (d.home_seq !== homeSeq) {
                homeSeq = d.home_seq
                close()
                return
            }
        }

        if (mode === "pending") {
            if (rebootState === "error") {
                mode = "error"
                if (!rebootError)
                    rebootError = "The restart request failed. Try again, or use the reset button."
            }
            return
        }
        if (driving) {
            mode = "blocked"
        } else if (mode === "blocked") {
            mode = "confirm"
        }
        if (rebootState === "rebooting")
            mode = "pending"
    }

    function refreshFromFile() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200 || xhr.status === 0) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                        return
                    } catch (e) {
                        // fall through
                    }
                }
                serviceOk = false
                serviceError = true
                if (mode !== "pending" && !driving)
                    mode = "unavailable"
            }
        }
        xhr.open("GET", "file:///rw_data/tweaks/status.json")
        xhr.send()
    }

    function refresh() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                        return
                    } catch (e) {
                        // fall through
                    }
                }
                refreshFromFile()
            }
        }
        xhr.open("GET", daemonBase + "/status")
        xhr.send()
    }

    function confirm() {
        if (driving) {
            mode = "blocked"
            return
        }
        if (!serviceOk) {
            mode = "unavailable"
            return
        }
        mode = "pending"
        pendingTimer.restart()

        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 202 || xhr.status === 200) {
                    mode = "pending"
                    pendingTimer.restart()
                    return
                }
                var reason = ""
                try {
                    var d = JSON.parse(xhr.responseText)
                    reason = d.reason ? d.reason : ""
                } catch (e) {
                    // ignore
                }
                if (reason === "already_rebooting") {
                    mode = "pending"
                    pendingTimer.restart()
                } else if (reason === "update_in_progress") {
                    mode = "error"
                    rebootError = "An update is in progress. Do not restart the head unit."
                } else if (reason === "forbidden") {
                    mode = "error"
                    rebootError = "The restart request was rejected."
                } else {
                    mode = "error"
                    rebootError = "The restart request failed. Try again, or use the reset button."
                }
            }
        }
        xhr.open("POST", daemonBase + "/reboot")
        xhr.setRequestHeader("X-Reboot-App", "1")
        xhr.send()
    }

    readonly property string bodyText: {
        if (mode === "blocked")
            return TranslationsModel.STR_HOME_POP_TRY_AGAIN_PARKING_VEHICLE
        if (mode === "pending")
            return "Restarting the head unit\u2026\n\nThe screen will return in about a minute."
        if (mode === "error")
            return (rebootError !== "" ? rebootError
                    : "The restart request failed. Try again, or use the reset button.")
        if (mode === "unavailable")
            return "The restart service is not available. Reinstall Tweaks, or use the reset button."
        if (!serviceOk)
            return "Checking restart service\u2026"
        return "Restart the head unit?\n\nThis restarts the infotainment system and takes about a minute. Do not restart while driving or during an update."
    }

    onDrivingChanged: {
        if (driving && mode === "confirm")
            mode = "blocked"
        else if (!driving && mode === "blocked")
            mode = "confirm"
    }

    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.close()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.close()
        onCurrentScreenChanged: root.close()
        onCurrentHomeAdvancedPageIdxChanged: root.close()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.close()
    }

    Connections {
        target: (typeof AppHomeEngine !== "undefined") ? AppHomeEngine : null
        onNotifyHomeMainChanged: root.close()
    }

    Timer {
        interval: 500
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Timer {
        id: pendingTimer
        interval: 60000
        repeat: false
        onTriggered: {
            if (root.mode === "pending") {
                root.mode = "error"
                root.rebootError = "The restart did not complete. Use the reset button."
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ConstantModel.GENERAL_COLOR_BLACK
        opacity: ConstantModel.GENERAL_OPACITY_PERCENT_70
    }

    // Full-page shield.  It is below the card, so outside taps cancel while
    // the card and its buttons keep their own input.
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
        width: ConstantModel.APP_HOME_POPUP_WIDTH_MID
        height: Math.max(ConstantModel.APP_HOME_POPUP_1LINE_HEIGHT,
                         ConstantModel.APP_HOME_POPUP_1LINE_HEIGHT
                         + (bodyTextItem.lineCount - 1)
                           * ConstantModel.APP_HOME_POPUP_TEXT_LINE_SPACING)

        QVerticalButton {
            id: panel
            btnWidth: parent.width
            btnHeight: parent.height
            topImages: [defineModel.co_bg_popup_mid_t]
            middleImages: [defineModel.co_bg_popup_mid_m]
            botImages: [defineModel.co_bg_popup_mid_b]
        }

        // Block the shield from receiving taps that land on the card.
        MouseArea {
            anchors.fill: parent
        }

        Q_ThemeImage {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 27
            themeSource: root.mode === "confirm"
                         ? defineModel.co_ic_question : defineModel.co_ic_info
        }

        QText {
            id: bodyTextItem
            anchors.centerIn: parent
            width: parent.width - 84
            text: root.bodyText + AppHomeModel.textUpdate
            font.pixelSize: ConstantModel.FONT_SIZE_39
            color: HomeThemeHelper.normalTextColor
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            lineHeightMode: Text.FixedHeight
            lineHeight: ConstantModel.APP_HOME_POPUP_TEXT_LINE_SPACING
        }

        Item {
            id: yesWrap
            visible: root.mode === "confirm"
            width: (popup.width - 36) / 2
            height: ConstantModel.APP_HOME_POPUP_BUTTON_HEIGHT
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12

            QHMI_ToggleButton {
                id: yesButton
                width: parent.width
                height: parent.height
                label: TranslationsModel.STR_HOME_YES + AppHomeModel.textUpdate
                labelSize: ConstantModel.FONT_SIZE_39
                labelColor: HomeThemeHelper.normalTextColor
                enabled: root.serviceOk && !root.driving
                opacity: enabled ? 1 : 0.4
                onBtnRelease: root.confirm()
            }
        }

        Item {
            id: noWrap
            visible: root.mode === "confirm"
            width: (popup.width - 36) / 2
            height: ConstantModel.APP_HOME_POPUP_BUTTON_HEIGHT
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12

            QHMI_ToggleButton {
                width: parent.width
                height: parent.height
                label: TranslationsModel.STR_HOME_NO + AppHomeModel.textUpdate
                labelSize: ConstantModel.FONT_SIZE_39
                labelColor: HomeThemeHelper.normalTextColor
                onBtnRelease: root.dismiss()
            }
        }

        Item {
            id: okWrap
            visible: root.mode === "error" || root.mode === "unavailable"
                     || root.mode === "blocked"
            width: popup.width - 24
            height: ConstantModel.APP_HOME_POPUP_BUTTON_HEIGHT
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12

            QHMI_ToggleButton {
                width: parent.width
                height: parent.height
                label: TranslationsModel.STR_HOME_OK + AppHomeModel.textUpdate
                labelSize: ConstantModel.FONT_SIZE_39
                labelColor: HomeThemeHelper.normalTextColor
                onBtnRelease: root.dismiss()
            }
        }

        onClosePopupFinish: {
            if (root.mode === "pending")
                popup.opacity = 1
            else
                root.close()
        }
    }
}
