// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// BATTERY_PAYLOAD_260128
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal goHome()

    // ---- foreground / home-key handling --------------------------------
    // The overlay must only be visible while the home screen itself is the
    // foreground app.  AppHomeModel.isAtOtherApp tracks the app-manager
    // foreground: when another app owns the Wayland surface, clicks would
    // fall through to it, so the overlay closes itself instead of trying to
    // raise the window (which QML cannot do here).
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    property int homeSeq: 0
    property string homeSource: "none"
    property int lastAppKey: -1
    property bool homeMonitor: false

    visible: requested && !atOtherApp
    focus: visible
    /* Stock rule: never fight a system popup for input (styleguide §4.6). */
    enabled: !(typeof AppHomeModel !== "undefined"
               && AppHomeModel.sysPopupShowing)

    function open() {
        requested = true
        refresh()
    }
    function close() {
        requested = false
    }

    // The steering-wheel Home/Star key is delivered through the app manager
    // (session-bus NotifyRequestHomeScreen / custom_key_value), never as a
    // QML key event.  The daemon tails the session bus and bumps home_seq;
    // any navigation event also closes the overlay.
    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.close()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.close()
        onCurrentScreenChanged: root.close()
        onCurrentHomeAdvancedPageIdxChanged: root.close()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.close()
    }
    onHomeSeqChanged: if (homeSeq > 0) close()

    // ---- daemon state (I-CAN) ----------------------------------------
    property bool serviceOk: false
    property string heaterState: "idle"
    property int precondRaw: 0
    property int battMin: 0
    property bool battValid: false
    property string battRaw: ""
    property string tempSource: ""
    property real daemonSocPct: 0
    property bool daemonSocValid: false
    property bool daemonReady: false
    property bool daemonUtility: false
    property bool dashOn: false
    property bool factoryNav: false
    property int retries: 0
    property int etaS: 0
    property string daemonMessage: ""

    // ---- charge door (daemon + QML gear) -----------------------------
    property bool daemonPark: false
    property bool daemonParkKnown: false
    property int daemonGear: -1
    property bool cable: false
    property bool cableKnown: false
    property bool charging: false
    property int doorState: 0
    property bool doorKnown: false
    property string doorSource: "unknown"
    property int doorWarn: 0
    property int doorOpt: 0
    property bool doorBusy: false
    property string doorMessage: ""
    property string doorBlockReason: ""
    property string lastTx: ""
    property string daemonVer: ""
    property int sendOk: 0
    property int sendFail: 0
    property int battCond: 0
    property bool battCondKnown: false
    property string gearRaw: ""
    property int gearB3: -1
    property int gearB4: -1

    // ---- optimistic preheat ------------------------------------------
    property string blockReason: ""
    property bool preheatPending: false
    property bool preheatRequested: false
    property double preheatRequestedAt: 0
    property int preheatRetries: 0
    property int rawGear: -1
    property string lastGearText: ""

    // ---- factory models ----------------------------------------------
    readonly property string liveGearText: {
        if (typeof AppNModeModel !== "undefined") {
            var g = AppNModeModel.gearPosRightTxtImg
            if (!g) g = AppNModeModel.gearPosLeftTxtImg
            if (g) return g
        }
        if (rawGear >= 0) {
            if (rawGear === 0) return "P"
            if (rawGear === 5) return "D"
            if (rawGear === 6) return "N"
            if (rawGear === 7) return "R"
        }
        if (daemonParkKnown) {
            if (daemonGear === 0) return "P"
            if (daemonGear === 5) return "D"
            if (daemonGear === 6) return "N"
            if (daemonGear === 7) return "R"
        }
        return ""
    }
    readonly property string gearText: liveGearText !== "" ? liveGearText : lastGearText
    onLiveGearTextChanged: if (liveGearText !== "") lastGearText = liveGearText
    readonly property bool gearKnown: gearText !== ""
    readonly property bool park: gearText === "P"

    readonly property bool ready: {
        if (typeof AppPHEVModel !== "undefined") return AppPHEVModel.vcuEvReady === true
        return daemonReady
    }
    readonly property bool utility: {
        if (typeof AppPHEVModel !== "undefined") return AppPHEVModel.utilityMode === 2
        return daemonUtility
    }
    readonly property real socPct: {
        if (typeof AppPHEVModel !== "undefined" && AppPHEVModel.batLevel >= 0)
            return AppPHEVModel.batLevel
        return daemonSocPct
    }
    readonly property bool socValid: {
        if (typeof AppPHEVModel !== "undefined" && AppPHEVModel.batLevel >= 0) return true
        return daemonSocValid
    }
    readonly property int heaterPercent: {
        if (typeof AppPHEVModel !== "undefined" && AppPHEVModel.heatBatteryPercent >= 0)
            return AppPHEVModel.heatBatteryPercent
        return -1
    }
    readonly property bool conditioningMode: {
        if (typeof AppPHEVModel !== "undefined")
            return AppPHEVModel.batteryConditionMode ? true : false
        return false
    }
    readonly property bool heaterActive: heaterPercent > 0 || heaterState === "preparing" || heaterState === "active"
    readonly property bool preheatOn: preheatPending || preheatRequested || heaterActive
    readonly property bool socLow: socValid && socPct < 20
    readonly property bool tempHigh: battValid && battMin >= 21
    readonly property bool doorAllowed: serviceOk && gearKnown && park
                                        && !(cableKnown && cable)
    readonly property bool doorOpen: doorKnown && doorState === 1

    readonly property string outsideTemp: {
        if (typeof appClimateModel !== "undefined" && appClimateModel.ambienTemp)
            return appClimateModel.ambienTemp
        return "--"
    }

    readonly property var warnings: {
        var w = []
        if (!ready)
            w.push("Vehicle is not ready \u2014 the BMS may refuse preheating.")
        if (utility)
            w.push("Utility mode is active.")
        if (socLow)
            w.push("Battery is below 20%.")
        if (tempHigh)
            w.push("Battery is above 21\u00b0C \u2014 preheating is not needed.")
        if (blockReason)
            w.push("Preheat request blocked: " + blockReason + ".")
        if (doorBlockReason)
            w.push("Charge door blocked: " + doorBlockReason + ".")
        if (heaterState === "timeout")
            w.push("Preheating did not start. Retry or check the vehicle.")
        if (heaterState === "stopped_by_vehicle")
            w.push("Preheating stopped by the vehicle.")
        if (doorMessage === "no_response")
            w.push("Charge door did not respond. Check the door and retry.")
        if (doorMessage === "send_failed")
            w.push("Charge door command could not be sent.")
        return w
    }

    readonly property var warningsShown: {
        var w = warnings
        if (w.length <= 3)
            return w
        return [w[0], w[1], "+" + (w.length - 2) + " more warnings"]
    }

    Component.onCompleted: {
        if (liveGearText !== "")
            lastGearText = liveGearText
    }

    // ---- daemon I/O ---------------------------------------------------
    function applyStatus(d) {
        heaterState = d.state
        precondRaw = d.precond_raw
        battMin = d.batt_min_c
        battValid = d.batt_valid
        battRaw = d.batt_raw || ""
        tempSource = d.temp_source || ""
        daemonSocPct = d.soc_pct
        daemonSocValid = d.soc_valid
        daemonReady = d.ready
        daemonUtility = d.utility
        dashOn = d.dash
        factoryNav = d.factory_nav
        retries = d.retries
        etaS = d.eta_s
        daemonMessage = d.message

        daemonPark = d.park === true
        daemonParkKnown = d.park_known === true
        daemonGear = (typeof d.gear === "number") ? d.gear : -1
        cable = d.cable === true
        cableKnown = d.cable_known === true
        charging = d.charging === true
        doorState = (typeof d.door_state === "number") ? d.door_state : 0
        doorKnown = d.door_known === true
        doorSource = d.door_source || "unknown"
        doorWarn = (typeof d.door_warn === "number") ? d.door_warn : 0
        doorOpt = (typeof d.door_opt === "number") ? d.door_opt : 0
        doorBusy = d.door_busy === true
        doorMessage = d.door_message || ""
        lastTx = d.last_tx || ""
        daemonVer = d.ver || ""
        sendOk = (typeof d.send_ok === "number") ? d.send_ok : 0
        sendFail = (typeof d.send_fail === "number") ? d.send_fail : 0
        battCond = (typeof d.batt_cond === "number") ? d.batt_cond : 0
        battCondKnown = d.batt_cond_known === true
        gearRaw = d.gear_raw || ""
        gearB3 = (typeof d.gear_b3 === "number") ? d.gear_b3 : -1
        gearB4 = (typeof d.gear_b4 === "number") ? d.gear_b4 : -1
        homeSeq = (typeof d.home_seq === "number") ? d.home_seq : 0
        homeSource = d.home_source || "none"
        lastAppKey = (typeof d.last_app_key === "number") ? d.last_app_key : -1
        homeMonitor = d.home_monitor === true
    }

    function refreshFromFile() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200 || xhr.status === 0) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                        serviceOk = true
                    } catch (e) {
                        serviceOk = false
                    }
                } else {
                    serviceOk = false
                }
            }
        }
        xhr.open("GET", "file:///rw_data/battery/status.json")
        xhr.send()
    }

    function refresh() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                        serviceOk = true
                        return
                    } catch (e) {
                        // fall through to the file mirror
                    }
                }
                refreshFromFile()
            }
        }
        xhr.open("GET", "http://127.0.0.1:8088/status")
        xhr.send()
    }

    function post(path) {
        var isPreheat = path === "/start" || path === "/stop"
        var isDoor = path.indexOf("/chargedoor/") === 0
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (isPreheat || isDoor) {
                    var reason = ""
                    if (xhr.status === 200) {
                        reason = ""
                    } else {
                        try {
                            var r = JSON.parse(xhr.responseText)
                            reason = r.reason || r.error
                                   || ("http " + xhr.status)
                        } catch (e) {
                            reason = "http " + xhr.status
                        }
                    }
                    if (isPreheat) root.blockReason = reason
                    if (isDoor) root.doorBlockReason = reason
                }
                refresh()
            }
        }
        xhr.open("POST", "http://127.0.0.1:8088" + path)
        xhr.send()
    }

    function togglePreheat() {
        if (!serviceOk || preheatPending || tempHigh) return
        preheatPending = true
        preheatRetries = 0
        if (preheatOn) {
            preheatRequested = false
            post("/stop")
        } else {
            preheatRequested = true
            preheatRequestedAt = Date.now()
            post("/start")
        }
        preheatClearTimer.restart()
    }

    function toggleDash() {
        if (!serviceOk) return
        post(dashOn ? "/dash/off" : "/dash/on")
    }

    function toggleDoor() {
        if (!doorAllowed || doorBusy) return
        if (doorOpen) post("/chargedoor/close?park=1")
        else post("/chargedoor/open?park=1")
    }

    function heaterText() {
        if (preheatPending) return "Requesting"
        if (heaterState === "preparing") return "Preparing"
        if (heaterState === "active") return "Active"
        if (heaterPercent > 0) return "Heating"
        if (preheatRequested) return "Requested"
        if (heaterState === "starting") return "Starting"
        if (heaterState === "stopping") return "Stopping"
        if (heaterState === "timeout") return "Not started"
        return "Off"
    }

    function heaterSubText() {
        if (preheatPending) return "Sending request\u2026"
        if (heaterState === "preparing" && etaS > 0)
            return "Battery heater \u00b7 about " + etaS + " s"
        if (heaterPercent >= 0)
            return "Heater power " + heaterPercent + "%"
        if (preheatRequested) return "Waiting for the BMS"
        return "Battery heater"
    }

    function doorSubText() {
        if (!serviceOk) return "Service unavailable"
        if (doorBlockReason) return "Blocked: " + doorBlockReason
        if (!gearKnown) return "Gear position unavailable"
        if (!park) return "Shift to Park to use the charge door"
        if (cableKnown && cable) return "Unplug the charging cable first"
        if (!cableKnown) return "Cable state unknown \u2014 allowed"
        if (doorBusy) return doorMessage === "closing" ? "Closing\u2026" : "Opening\u2026"
        if (doorKnown) {
            if (doorState === 1) return doorSource === "command" ? "Open (commanded)" : "Open"
            if (doorState === 2) return doorSource === "command" ? "Closed (commanded)" : "Closed"
        }
        return "Tap to open"
    }

    Connections {
        target: (typeof AppNModeModel !== "undefined") ? AppNModeModel : null
        onSignalGEAR_TYPE: function(value) { root.rawGear = value }
    }

    Timer {
        interval: 500
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Timer {
        id: preheatClearTimer
        interval: 3000
        onTriggered: root.preheatPending = false
    }

    Timer {
        interval: 10000
        repeat: true
        running: root.preheatRequested && !root.heaterActive
        onTriggered: {
            if (root.preheatRetries < 4) {
                root.preheatRetries++
                root.post("/start")
            }
        }
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: {
            if (root.preheatRequested && !root.heaterActive &&
                Date.now() - root.preheatRequestedAt > 180000)
                root.preheatRequested = false
        }
    }

    // ---- background: the stock themed home background (styleguide §3) ----
    Q_ThemeImage {
        anchors.fill: parent
        themeSource: AppHomeModel.isWideScr
                      ? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
    }

    // Full-screen input shield: accept every press/release/wheel so no
    // event reaches the home grid or another app's surface underneath.
    MouseArea {
        id: shield
        anchors.fill: parent
        z: 1
        preventStealing: true
        acceptedButtons: Qt.AllButtons
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
        onClicked: mouse.accepted = true
        onWheel: wheel.accepted = true
    }

    Item {
        id: content
        z: 2
        width: 1180
        height: 630
        anchors.horizontalCenter: parent.horizontalCenter

        Image {
            id: headerIcon
            x: 0
            y: 8
            width: 56
            height: 56
            smooth: true
            source: "file:///app/share/AppHomeScreen/battery/battery_icon_n.png"
        }

        QText {
            id: title
            x: 74
            y: 2
            text: "Battery"
            font.pixelSize: ConstantModel.FONT_SIZE_42
            textBold: true
            color: HomeThemeHelper.normalTextColor
        }

        QText {
            id: subtitle
            x: 76
            y: 56
            text: {
                var svc = root.serviceOk ? "connected" : "unavailable"
                var raw = "0x" + ("0" + root.precondRaw.toString(16)).slice(-2)
                return "Battery preconditioning   \u00b7   Service " + svc
                       + "   \u00b7   BMS " + raw
            }
            font.pixelSize: ConstantModel.FONT_SIZE_21
            color: HomeThemeHelper.normalTextColor
        }

        Rectangle {
            id: homeButton
            property bool pressActive: false
            anchors.right: parent.right
            y: 8
            width: 140
            height: 50
            radius: 25
            color: homeMouse.pressed && pressActive
                   ? ConstantModel.GENERAL_COLOR_FOCUS_GREY
                   : ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                anchors.centerIn: parent
                text: "\u2039  Home"
                font.pixelSize: ConstantModel.FONT_SIZE_24
                color: HomeThemeHelper.normalTextColor
            }

            MouseArea {
                id: homeMouse
                anchors.fill: parent
                onPressed: homeButton.pressActive = true
                onPositionChanged:
                    if (!containsMouse) homeButton.pressActive = false
                onReleased: {
                    if (homeButton.pressActive) {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)
                        root.goHome()
                    }
                    homeButton.pressActive = false
                }
            }
        }

        // ---- battery temperature -------------------------------------
        Rectangle {
            x: 0
            y: 92
            width: 570
            height: 132
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 28
                y: 14
                text: "Battery temperature"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 44
                text: root.battValid ? (root.battMin + "\u00b0C") : "--"
                font.pixelSize: ConstantModel.FONT_SIZE_44
                textBold: true
                color: root.tempHigh ? "#FFB300" : HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 104
                text: "Minimum cell temperature"
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: HomeThemeHelper.disableTextColor
            }
        }

        // ---- exterior temperature ------------------------------------
        Rectangle {
            x: 610
            y: 92
            width: 570
            height: 132
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 28
                y: 14
                text: "Exterior temperature"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 44
                text: root.outsideTemp
                font.pixelSize: ConstantModel.FONT_SIZE_44
                textBold: true
                color: HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 104
                text: "From the climate controller"
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: HomeThemeHelper.disableTextColor
            }
        }

        // ---- state of charge -----------------------------------------
        Rectangle {
            x: 0
            y: 236
            width: 570
            height: 132
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 28
                y: 14
                text: "State of charge"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 42
                text: root.socValid ? root.socPct.toFixed(1) + "%" : "--"
                font.pixelSize: ConstantModel.FONT_SIZE_42
                textBold: true
                color: root.socLow ? "#FFB300" : HomeThemeHelper.normalTextColor
            }

            Rectangle {
                x: 28
                y: 92
                width: 514
                height: 14
                radius: 7
                color: "#2C2C2C"

                Rectangle {
                    width: root.socValid ? Math.max(0, Math.min(1, root.socPct / 100)) * 514 : 0
                    height: parent.height
                    radius: 7
                    color: root.socLow ? "#E53935" : "#4CAF50"
                }
            }
            QText {
                x: 28
                y: 110
                text: "Preheating needs at least 20%"
                font.pixelSize: ConstantModel.FONT_SIZE_18
                color: HomeThemeHelper.disableTextColor
            }
        }

        // ---- battery heater ------------------------------------------
        Rectangle {
            x: 610
            y: 236
            width: 570
            height: 132
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: root.preheatOn ? "#2E7D32" : ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: root.preheatOn ? 2 : 1

            QText {
                x: 28
                y: 14
                text: "Battery heater"
                font.pixelSize: ConstantModel.FONT_SIZE_27
                color: HomeThemeHelper.normalTextColor
            }
            QText {
                x: 28
                y: 44
                text: root.heaterText()
                      + (root.battCondKnown && root.battCond > 0 ? "  \u00b7  0x25D=" + root.battCond : "")
                font.pixelSize: ConstantModel.FONT_SIZE_42
                textBold: true
                color: root.preheatOn ? "#4CAF50"
                     : (root.heaterState === "timeout" ? "#E53935" : HomeThemeHelper.normalTextColor)
            }
            QText {
                x: 28
                y: 104
                text: root.heaterSubText()
                font.pixelSize: ConstantModel.FONT_SIZE_21
                color: HomeThemeHelper.disableTextColor
            }
        }

        // ---- preheat toggle ------------------------------------------
        Rectangle {
            x: 0
            y: 380
            width: 570
            height: 64
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 28
                y: 8
                text: "Engage battery preheater"
                font.pixelSize: ConstantModel.FONT_SIZE_24
                color: root.serviceOk ? HomeThemeHelper.normalTextColor : HomeThemeHelper.disableTextColor
            }
            QText {
                x: 28
                y: 36
                text: root.tempHigh ? "Battery is above 21\u00b0C"
                      : (root.blockReason ? "Blocked: " + root.blockReason
                      : (root.preheatPending ? "Sending request\u2026"
                         : (root.preheatOn ? "On" : "Off")))
                font.pixelSize: ConstantModel.FONT_SIZE_18
                color: (root.tempHigh || root.blockReason) ? "#FFB300" : "#7A7A7A"
            }

            BatterySwitch {
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                checked: root.preheatOn
                switchEnabled: root.serviceOk && !root.preheatPending && !root.tempHigh
                onToggled: root.togglePreheat()
            }
        }

        // ---- dash toggle ---------------------------------------------
        Rectangle {
            x: 610
            y: 380
            width: 570
            height: 64
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: 1

            QText {
                x: 28
                y: 8
                text: "Cluster display test"
                font.pixelSize: ConstantModel.FONT_SIZE_24
                color: root.serviceOk ? HomeThemeHelper.normalTextColor : HomeThemeHelper.disableTextColor
            }
            QText {
                x: 28
                y: 36
                text: root.dashOn
                      ? "0x4E8 + 0x4CC countdown \u2014 sending"
                      : "0x4E8 + 0x4CC countdown \u2014 test without preheat"
                font.pixelSize: ConstantModel.FONT_SIZE_18
                color: HomeThemeHelper.disableTextColor
            }

            BatterySwitch {
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                checked: root.dashOn
                switchEnabled: root.serviceOk
                onToggled: root.toggleDash()
            }
        }

        // ---- charge door ---------------------------------------------
        Rectangle {
            x: 0
            y: 456
            width: 1180
            height: 64
            radius: 14
            color: ConstantModel.GENERAL_COLOR_BUTTON_GREY
            border.color: root.doorOpen ? "#2E7D32" : ConstantModel.GENERAL_COLOR_FOCUS_GREY
            border.width: root.doorOpen ? 2 : 1

            QText {
                x: 28
                y: 6
                text: "Open/Close charge door"
                font.pixelSize: ConstantModel.FONT_SIZE_24
                color: root.doorAllowed ? HomeThemeHelper.normalTextColor : HomeThemeHelper.disableTextColor
            }
            QText {
                x: 28
                y: 34
                text: root.doorSubText()
                font.pixelSize: ConstantModel.FONT_SIZE_18
                color: root.doorAllowed ? HomeThemeHelper.normalTextColor : "#FFB300"
            }

            BatterySwitch {
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                checked: root.doorOpen
                switchEnabled: root.doorAllowed && !root.doorBusy
                onToggled: root.toggleDoor()
            }
        }

        // ---- warnings ------------------------------------------------
        Column {
            x: 4
            y: 528
            width: 1172
            spacing: 2

            Repeater {
                model: root.warningsShown
                QText {
                    text: "\u26a0  " + modelData
                    font.pixelSize: ConstantModel.FONT_SIZE_18
                    color: "#FFB300"
                }
            }
        }

        // ---- debug footer --------------------------------------------
        QText {
            x: 4
            y: 596
            text: {
                var gear = root.gearKnown ? root.gearText : "?"
                if (root.liveGearText === "" && root.lastGearText !== "")
                    gear += " (last)"
                var svc = root.serviceOk ? "ok" : "down"
                var src = root.tempSource ? root.tempSource : "?"
                var cab = root.cableKnown ? (root.cable ? "yes" : "no") : "?"
                var raw = "0x" + ("0" + root.precondRaw.toString(16)).slice(-2)
                var fg = root.atOtherApp ? "other" : "home"
                var mon = root.homeMonitor ? "on" : "off"
                var cond = root.battCondKnown ? ("Cond " + root.battCond) : "Cond ?"
                var line = "FG " + fg + "   \u00b7   " + cond
                           + "   \u00b7   Gear " + gear
                           + " (raw " + root.rawGear
                           + " b3=" + root.gearB3 + " b4=" + root.gearB4 + ")"
                           + "   \u00b7   Service " + svc
                           + "   \u00b7   Temp " + src + "   \u00b7   Cable " + cab
                           + "   \u00b7   BMS " + raw
                if (root.homeSeq > 0)
                    line += "   \u00b7   Home " + root.homeSeq
                            + " (" + root.homeSource + ")"
                if (root.lastAppKey >= 0)
                    line += "   \u00b7   Key " + root.lastAppKey
                if (root.battRaw) line += "   \u00b7   2FC " + root.battRaw
                if (root.gearRaw) line += "   \u00b7   036 " + root.gearRaw
                if (root.lastTx) line += "   \u00b7   TX " + root.lastTx
                line += "   \u00b7   Send " + root.sendOk + " ok"
                if (root.sendFail > 0)
                    line += "/" + root.sendFail + " FAIL"
                if (root.daemonVer) line += "   \u00b7   v" + root.daemonVer
                line += "   \u00b7   Bus " + mon
                return line
            }
            font.pixelSize: ConstantModel.FONT_SIZE_15
            color: HomeThemeHelper.disableTextColor
        }
    }
}
