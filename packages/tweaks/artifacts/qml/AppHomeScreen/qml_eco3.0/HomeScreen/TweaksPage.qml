// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// TWEAKS_PAYLOAD_260128
import QtQuick 2.0
import "../component"
import "../component/common"

Item {
    id: root
    width: AppHomeModel.isWideScr ? 1920 : 1280
    y: ConstantModel.MENU_BAR_Y
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
    visible: false
    z: 100

    signal closed()

    property bool serviceOk: false
    property bool clusterFix: true
    property bool infotainmentFix: true
    property bool deepRed: false
    property bool filterActive: false
    property int warmthLevel: 0
    property string gamma: "128:128:128"
    property bool projectionActive: false
    property string projectionSource: ""
    property int clusterCrtc: 1
    property int mainCrtc: 0
    property bool clusterApplied: false
    property bool mainApplied: false
    property bool nightForced: false
    property bool disclaimerAuto: true
    property int disclaimerTimeout: 2000
    property bool autoDst: true
    property string dstZone: "Europe/Amsterdam"
    property bool dstActive: false
    property int dstRaw: -1
    property int dstExpected: -1
    property string dstState: ""
    property string dstError: ""
    property string tzdataVersion: ""
    property bool otaBlock: false
    property string otaState: ""
    property string otaError: ""
    property string daemonMessage: ""
    property string lastError: ""

    readonly property string daemonBase: "http://127.0.0.1:8091"
    readonly property int disclaimerSeconds: Math.round(disclaimerTimeout / 1000)

    function applyStatus(d) {
        serviceOk = true
        clusterFix = d.cluster_fix
        infotainmentFix = d.infotainment_fix
        if (typeof d.deep_red !== "undefined") deepRed = d.deep_red === 1
        filterActive = d.filter_active
        warmthLevel = d.warmth_level
        gamma = d.gamma
        projectionActive = d.projection_active
        projectionSource = d.projection_source
        clusterCrtc = d.cluster_crtc
        mainCrtc = d.main_crtc
        clusterApplied = d.cluster_applied
        mainApplied = d.main_applied
        if (typeof d.night_forced !== "undefined") nightForced = d.night_forced === 1
        if (typeof d.disclaimer_auto !== "undefined") disclaimerAuto = d.disclaimer_auto === 1
        if (typeof d.disclaimer_timeout_ms !== "undefined") disclaimerTimeout = d.disclaimer_timeout_ms
        if (typeof d.auto_dst !== "undefined") autoDst = d.auto_dst === 1
        if (typeof d.dst_zone !== "undefined") dstZone = d.dst_zone
        if (typeof d.dst_active !== "undefined") dstActive = d.dst_active === 1
        if (typeof d.dst_raw !== "undefined") dstRaw = d.dst_raw
        if (typeof d.dst_expected !== "undefined") dstExpected = d.dst_expected
        if (typeof d.dst_state !== "undefined") dstState = d.dst_state
        if (typeof d.dst_error !== "undefined") dstError = d.dst_error
        if (typeof d.tzdata_version !== "undefined") tzdataVersion = d.tzdata_version
        if (typeof d.ota_block !== "undefined") otaBlock = d.ota_block === 1
        if (typeof d.ota_state !== "undefined") otaState = d.ota_state
        if (typeof d.ota_error !== "undefined") otaError = d.ota_error
        daemonMessage = d.message
        lastError = d.last_error
    }

    function refreshFromFile() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200 || xhr.status === 0) {
                    try {
                        applyStatus(JSON.parse(xhr.responseText))
                    } catch (e) {
                        serviceOk = false
                    }
                } else {
                    serviceOk = false
                }
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

    function post(path) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                refresh()
            }
        }
        xhr.open("POST", daemonBase + path)
        xhr.send()
    }

    function setClusterFix(on) {
        clusterFix = on
        post("/set?cluster_fix=" + (on ? "1" : "0"))
    }

    function setInfotainmentFix(on) {
        infotainmentFix = on
        post("/set?infotainment_fix=" + (on ? "1" : "0"))
    }

    function setDeepRed(on) {
        deepRed = on
        post("/set?deep_red=" + (on ? "1" : "0"))
    }

    function setDisclaimerAuto(on) {
        disclaimerAuto = on
        post("/set?disclaimer_auto=" + (on ? "1" : "0"))
    }

    function setDisclaimerTimeout(ms) {
        var next = Math.max(1000, Math.min(10000, Math.round(ms / 1000) * 1000))
        disclaimerTimeout = next
        post("/set?disclaimer_timeout_ms=" + next)
    }

    function setAutoDst(on) {
        autoDst = on
        post("/set?auto_dst=" + (on ? "1" : "0"))
    }

    function setDstZone(zone) {
        dstZone = zone
        post("/set?dst_zone=" + encodeURIComponent(zone))
    }

    function setOtaBlock(on) {
        otaBlock = on
        post("/set?ota_block=" + (on ? "1" : "0"))
    }

    function warmthText() {
        if (!filterActive) return nightForced ? "Kept on during projection" : "Blue light filter is off"
        if (warmthLevel > 0) return "Warmth level " + warmthLevel + " (from Setup)"
        return "Warmth from Setup"
    }

    function projectionText() {
        if (projectionActive) return "Projection active"
        return "Waiting for CarPlay / Android Auto"
    }

    function disclaimerText() {
        if (!disclaimerAuto) return "Off"
        return "Auto-accepts after " + disclaimerSeconds + "s \u00b7 touch cancels"
    }

    function dstText() {
        if (!autoDst) return "Off \u00b7 manual DST"
        if (dstState === "error") return "Error \u00b7 " + (dstError || "DST unavailable")
        if (dstState === "" || dstState === "unknown") return dstZone + " \u00b7 checking\u2026"
        var state = dstState === "active" ? "DST on" : "DST off"
        var raw = dstRaw >= 0 ? " \u00b7 flag " + dstRaw : ""
        return dstZone + " \u00b7 " + state + raw
    }

    function otaText() {
        if (!otaBlock) return "Off \u00b7 Hyundai OTA allowed"
        if (otaState === "blocked") return "Blocked \u00b7 DM client offline"
        if (otaState === "pending") return "Enabling \u00b7 waiting for OTA client"
        if (otaState === "error") return "Block pending \u00b7 " + (otaError || "OTA client unavailable")
        return "On \u00b7 " + (otaState || "waiting")
    }

    onVisibleChanged: {
        if (!visible && zonePicker)
            zonePicker.visible = false
    }

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Rectangle {
        anchors.fill: parent
        color: "#0E0E0E"
    }

    // Swallow taps that land outside the controls so they cannot reach the
    // home screen below the overlay.
    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    TweaksZonePicker {
        id: zonePicker
        visible: false
        currentZone: root.dstZone
        onSelected: {
            root.setDstZone(zone)
            zonePicker.visible = false
        }
        onClosed: zonePicker.visible = false
    }

    Item {
        id: content
        width: 1180
        height: 630
        anchors.horizontalCenter: parent.horizontalCenter

        Image {
            id: headerIcon
            x: 0
            y: 4
            width: 56
            height: 56
            smooth: true
            source: "file:///app/share/AppHomeScreen/tweaks/tweaks_icon_n.png"
        }

        QText {
            id: title
            x: 72
            y: 0
            text: "Tweaks"
            font.pixelSize: 44
            textBold: true
            color: "#FFFFFF"
        }

        QText {
            id: subtitle
            x: 74
            y: 50
            text: "Night mode, startup, time and update controls"
            font.pixelSize: 20
            color: "#9A9A9A"
        }

        TweaksButton {
            id: closeButton
            width: 150
            height: 46
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 6
            label: "\u2039  Close"
            onClicked: {
                root.visible = false
                root.closed()
            }
        }

        // ---- Left column: night mode fixes -----------------------------
        Rectangle {
            id: clusterCard
            x: 0
            y: 78
            width: 570
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: root.clusterFix ? "#2E7D32" : "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Cluster night mode"
                font.pixelSize: 26
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                x: 20
                y: 44
                width: 380
                text: "Keeps the cluster amber while CarPlay or Android Auto is on, via the vehicle's own blue-light CAN signal."
                font.pixelSize: 17
                color: "#B0B0B0"
                wrapMode: Text.WordWrap
            }

            QText {
                x: 20
                y: 108
                width: 390
                text: root.clusterFix
                      ? (root.filterActive
                         ? (root.clusterApplied ? "Active \u00b7 " + root.warmthText() : "Waiting for cluster gamma")
                         : "Enabled \u00b7 " + root.warmthText())
                      : "Off \u00b7 stock behaviour"
                font.pixelSize: 16
                color: root.clusterFix ? "#4CAF50" : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                checked: root.clusterFix
                switchEnabled: root.serviceOk
                onToggled: root.setClusterFix(!root.clusterFix)
            }
        }

        Rectangle {
            id: infotainmentCard
            x: 0
            y: 226
            width: 570
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: root.infotainmentFix ? "#2E7D32" : "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Projection night mode"
                font.pixelSize: 26
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                x: 20
                y: 44
                width: 380
                text: "Applies the same amber warmth to the CarPlay and Android Auto screen."
                font.pixelSize: 17
                color: "#B0B0B0"
                wrapMode: Text.WordWrap
            }

            QText {
                x: 20
                y: 108
                width: 390
                text: root.infotainmentFix
                      ? (root.projectionActive
                         ? (root.mainApplied ? "Active \u00b7 " + root.warmthText() : "Projection active \u00b7 main display not applied")
                         : "Enabled \u00b7 " + root.projectionText())
                      : "Off \u00b7 projection screen stays neutral"
                font.pixelSize: 16
                color: root.infotainmentFix && root.projectionActive ? "#4CAF50" : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                checked: root.infotainmentFix
                switchEnabled: root.serviceOk
                onToggled: root.setInfotainmentFix(!root.infotainmentFix)
            }
        }

        Rectangle {
            id: settingCard
            x: 0
            y: 374
            width: 570
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Vehicle blue-light setting"
                font.pixelSize: 20
                color: "#9A9A9A"
            }

            QText {
                x: 20
                y: 44
                width: 520
                text: root.warmthText() + "  \u00b7  Setup > Display > Blue Light Filter"
                font.pixelSize: 19
                color: "#FFFFFF"
                elide: Text.ElideRight
            }

            QText {
                x: 20
                y: 78
                width: 520
                text: root.serviceOk ? "Daemon online" : "Daemon unavailable"
                font.pixelSize: 16
                color: root.serviceOk ? "#4CAF50" : "#FF7043"
            }
        }

        // ---- Deep red night mode row ------------------------------------
        Rectangle {
            id: deepRedCard
            x: 0
            y: 522
            width: 570
            height: 44
            radius: 10
            color: "#1B1B1B"
            border.color: root.deepRed ? "#8E2424" : "#333333"
            border.width: 1

            QText {
                x: 16
                y: 4
                width: 360
                text: "Deep red night mode"
                font.pixelSize: 19
                textBold: true
                color: root.deepRed ? "#FF8A80" : "#FFFFFF"
                verticalAlignment: Text.AlignVCenter
                height: parent.height - 8
            }

            QText {
                x: 16
                y: 24
                width: 360
                text: root.deepRed ? "On \u00b7 front 128:32:16 \u00b7 cluster level 9"
                                  : "Off \u00b7 follows the vehicle filter"
                font.pixelSize: 13
                color: root.deepRed ? "#FF8A80" : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                scale: 0.7
                checked: root.deepRed
                switchEnabled: root.serviceOk
                onToggled: root.setDeepRed(!root.deepRed)
            }
        }

        // ---- Right column: startup, time, updates ---------------------
        Rectangle {
            id: disclaimerCard
            x: 590
            y: 78
            width: 590
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: root.disclaimerAuto ? "#2E7D32" : "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Safety disclaimer"
                font.pixelSize: 26
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                x: 20
                y: 44
                width: 360
                text: "Auto-accept the boot disclaimer. Any touch cancels it for that boot."
                font.pixelSize: 17
                color: "#B0B0B0"
                wrapMode: Text.WordWrap
            }

            QText {
                x: 20
                y: 108
                width: 335
                text: root.disclaimerText()
                font.pixelSize: 16
                color: root.disclaimerAuto ? "#4CAF50" : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.top: parent.top
                anchors.topMargin: 18
                checked: root.disclaimerAuto
                switchEnabled: root.serviceOk
                onToggled: root.setDisclaimerAuto(!root.disclaimerAuto)
            }

            Row {
                x: 372
                y: 86
                spacing: 8

                TweaksButton {
                    width: 48
                    height: 38
                    label: "\u2212"
                    buttonEnabled: root.serviceOk && root.disclaimerAuto && root.disclaimerTimeout > 1000
                    onClicked: root.setDisclaimerTimeout(root.disclaimerTimeout - 1000)
                }

                QText {
                    width: 54
                    height: 38
                    text: root.disclaimerSeconds + "s"
                    font.pixelSize: 22
                    color: root.disclaimerAuto ? "#FFFFFF" : "#666666"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                TweaksButton {
                    width: 48
                    height: 38
                    label: "+"
                    buttonEnabled: root.serviceOk && root.disclaimerAuto && root.disclaimerTimeout < 10000
                    onClicked: root.setDisclaimerTimeout(root.disclaimerTimeout + 1000)
                }
            }
        }

        Rectangle {
            id: dstCard
            x: 590
            y: 226
            width: 590
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: root.autoDst ? "#2E7D32" : "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Automatic DST"
                font.pixelSize: 26
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                x: 20
                y: 44
                width: 360
                text: "Flips the vehicle DST switch using the selected IANA timezone."
                font.pixelSize: 17
                color: "#B0B0B0"
                wrapMode: Text.WordWrap
            }

            QText {
                x: 20
                y: 108
                width: 335
                text: root.dstText()
                font.pixelSize: 16
                color: root.autoDst ? (root.dstState === "error" ? "#FFB300" : "#4CAF50") : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.top: parent.top
                anchors.topMargin: 18
                checked: root.autoDst
                switchEnabled: root.serviceOk
                onToggled: root.setAutoDst(!root.autoDst)
            }

            TweaksButton {
                x: 374
                y: 88
                width: 194
                height: 38
                label: "Change timezone"
                buttonEnabled: root.serviceOk
                onClicked: zonePicker.visible = true
            }
        }

        Rectangle {
            id: otaCard
            x: 590
            y: 374
            width: 590
            height: 140
            radius: 14
            color: "#1B1B1B"
            border.color: root.otaBlock ? "#2E7D32" : "#333333"
            border.width: 1

            QText {
                x: 20
                y: 12
                text: "Block Hyundai OTA"
                font.pixelSize: 26
                textBold: true
                color: "#FFFFFF"
            }

            QText {
                x: 20
                y: 44
                width: 370
                text: "Blocks remote firmware pushes. Local USB updates remain available."
                font.pixelSize: 17
                color: "#B0B0B0"
                wrapMode: Text.WordWrap
            }

            QText {
                x: 20
                y: 108
                width: 540
                text: root.otaText()
                font.pixelSize: 16
                color: root.otaBlock ? (root.otaState === "error" ? "#FFB300" : "#4CAF50") : "#7A7A7A"
                elide: Text.ElideRight
            }

            TweaksSwitch {
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.top: parent.top
                anchors.topMargin: 18
                checked: root.otaBlock
                switchEnabled: root.serviceOk
                onToggled: root.setOtaBlock(!root.otaBlock)
            }
        }

        // ---- Actions and diagnostics ----------------------------------
        Row {
            id: actions
            x: 0
            y: 522
            spacing: 8

            TweaksButton {
                width: 140
                height: 44
                label: "Apply now"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/apply")
            }
            TweaksButton {
                width: 120
                height: 44
                label: "Refresh"
                buttonEnabled: root.serviceOk
                onClicked: root.refresh()
            }
            TweaksButton {
                width: 160
                height: 44
                label: "Restore stock"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/restore")
            }
            TweaksButton {
                width: 170
                height: 44
                label: "Identify CRTC 2"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/identify?crtc=2&seconds=3")
            }
            TweaksButton {
                width: 160
                height: 44
                label: "Identify CRTC 1"
                buttonEnabled: root.serviceOk
                onClicked: root.post("/identify?crtc=1&seconds=3")
            }
        }

        QText {
            id: diag
            x: 0
            y: 572
            width: 1180
            text: "Cluster CAN 0x063F \u00b7 Front CRTC " + root.mainCrtc
                  + " \u00b7 projection " + (root.projectionActive ? "active" : "idle")
                  + " [" + (root.projectionSource || "?") + "]"
                  + " \u00b7 gamma " + root.gamma
                  + " \u00b7 tzdata " + (root.tzdataVersion || "unknown")
                  + (root.lastError.length ? " \u00b7 " + root.lastError : "")
            font.pixelSize: 16
            color: root.lastError.length ? "#FFB300" : "#7A7A7A"
            elide: Text.ElideRight
        }

        QText {
            id: message
            x: 0
            y: 596
            width: 1180
            text: root.daemonMessage
            font.pixelSize: 15
            color: "#7A7A7A"
            elide: Text.ElideRight
        }
    }
}
