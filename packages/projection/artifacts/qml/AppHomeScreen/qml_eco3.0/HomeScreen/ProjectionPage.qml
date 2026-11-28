// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "../component"
import "../component/common"

/*
 * Projection settings overlay, deployment Model A: a page inside the stock
 * AppHomeScreen, opened by ProjectionTile.  The page owns open/close and all
 * foreground-loss hooks.  There is no driving lockout (PRODUCT.md,
 * 2026-11-27): passengers may change settings while the car moves.
 *
 * Strings are inline, matching the TW/WF reference pages.  Moving them to
 * TranslationsModel STR_* keys in all 26 ts files is an open point (Q10).
 */
Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    y: ConstantModel.MENU_BAR_Y
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
    visible: false
    focus: visible
    z: 100
    enabled: !(typeof AppHomeModel !== "undefined" && AppHomeModel.sysPopupShowing)

    signal closed()

    // ---- overlay ownership ----------------------------------------------
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                              ? AppHomeModel.isAtOtherApp : false
    property bool sysPopupShowing: typeof AppHomeModel !== "undefined"
                                   ? AppHomeModel.sysPopupShowing : false

    function open() {
        if (atOtherApp) { requested = false; visible = false; return }
        requested = true
        visible = true
        openedAt = Date.now()
        everFresh = false
        homeSeqSeeded = false
        uiState = "loading"
        view = "main"
        refresh()
    }

    function close() {
        if (!requested && !visible) return
        requested = false
        visible = false
        modal.close()
        view = "main"
        closed()
    }

    onVisibleChanged: {
        if (visible && atOtherApp) { close(); return }
        if (visible && !requested) {
            // A host may assign .visible directly instead of calling open().
            requested = true
            openedAt = Date.now()
            homeSeqSeeded = false
            uiState = "loading"
            refresh()
        } else if (!visible && requested) {
            requested = false
        }
    }
    onRequestedChanged: {
        if (requested && !atOtherApp && !visible) visible = true
        else if (!requested && visible) visible = false
    }
    onAtOtherAppChanged: if (atOtherApp) close()

    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.close()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.close()
        onCurrentScreenChanged: root.close()
        onCurrentHomeAdvancedPageIdxChanged: root.close()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.close()
        onSysPopupShowingChanged: if (AppHomeModel.sysPopupShowing) root.close()
    }
    Connections {
        target: (typeof AppHomeEngine !== "undefined") ? AppHomeEngine : null
        onNotifyHomeMainChanged: root.close()
    }

    // ---- daemon state ----------------------------------------------------
    readonly property string daemonBase: "http://127.0.0.1:8092"
    property bool serviceOk: false
    property bool everFresh: false
    property bool featureKnown: false
    property bool supported: false
    property bool masterEnabled: false
    property bool aaEnabled: true
    property bool cpEnabled: true
    property int aaSteps: 1               // 0 precise, 1 balanced, 2 calm
    property int cpSteps: 1
    property int aaUnits: 0               // 0 follow car, 1 metric, 2 imperial
    property int cpUnits: 0
    property bool guidanceActive: false
    property bool guidanceKnown: false
    property string source: "none"
    property bool sourceKnown: true
    property string health: "disabled"
    property string lastError: ""
    property string bootGuard: "armed"
    property int bootFailures: 0
    property string versionApp: "1.0"
    property string versionDaemon: "1.0"
    property string versionFirmware: "260319"
    property int nextTurnMeters: 0
    property int homeSeq: 0
    property bool homeSeqSeeded: false
    property double openedAt: 0
    property double suppressStatusUntil: 0

    // loading | ready | unavailable | unsupported
    property string uiState: "loading"
    property bool forceLoading: false     // harness hook: hold the loading frame

    function statusFileUrl() {
        return (typeof H_statusFile !== "undefined" && String(H_statusFile).length > 0)
               ? String(H_statusFile) : "file:///tmp/hudnav-status.json"
    }

    function applyStatus(d) {
        var upd = (typeof d.updated_ms === "number") ? d.updated_ms : 0
        serviceOk = upd > 0 && (Date.now() - upd) < 8000

        if (typeof d.home_seq === "number") {
            if (!homeSeqSeeded) { homeSeq = d.home_seq; homeSeqSeeded = true }
            else if (d.home_seq !== homeSeq) { close(); return }
        }

        if (forceLoading) { uiState = "loading"; return }
        if (!serviceOk) {
            if (!everFresh && (Date.now() - openedAt) < 5000) uiState = "loading"
            else uiState = "unavailable"
            return
        }
        everFresh = true
        featureKnown = d.feature_known === true
        supported = d.supported === true
        if (featureKnown && !supported) { uiState = "unsupported"; return }
        uiState = "ready"

        // A local write wins until the daemon has had time to echo it back.
        if (Date.now() < suppressStatusUntil) return

        masterEnabled = typeof d.hudnav_enabled !== "undefined" ? d.hudnav_enabled === true : false
        aaEnabled = typeof d.aa_enabled !== "undefined" ? d.aa_enabled === true : true
        cpEnabled = typeof d.cp_enabled !== "undefined" ? d.cp_enabled === true : true
        aaSteps = typeof d.aa_distance_steps === "string" ? stepIndex(d.aa_distance_steps) : 1
        cpSteps = typeof d.cp_distance_steps === "string" ? stepIndex(d.cp_distance_steps) : 1
        aaUnits = typeof d.aa_units === "string" ? unitIndex(d.aa_units) : 0
        cpUnits = typeof d.cp_units === "string" ? unitIndex(d.cp_units) : 0

        guidanceActive = d.guidance_active === true
        guidanceKnown = d.guidance_known === true
        source = typeof d.source === "string" ? d.source : "none"
        sourceKnown = typeof d.source_known !== "undefined" ? d.source_known === true : true
        health = typeof d.health === "string" ? d.health : "disabled"
        lastError = typeof d.last_error === "string" ? d.last_error : ""
        bootGuard = typeof d.boot_guard === "string" ? d.boot_guard : "armed"
        bootFailures = typeof d.boot_failures === "number" ? d.boot_failures : 0
        versionApp = typeof d.version_app === "string" ? d.version_app : "1.0"
        versionDaemon = typeof d.version_daemon === "string" ? d.version_daemon : "1.0"
        versionFirmware = typeof d.version_firmware === "string" ? d.version_firmware : "260319"
        nextTurnMeters = typeof d.next_turn_m === "number" ? d.next_turn_m : 0
    }

    function refreshFromFile() {
        var fx = new XMLHttpRequest()
        fx.onreadystatechange = function() {
            if (fx.readyState === XMLHttpRequest.DONE) {
                if (fx.status === 200 || fx.status === 0) {
                    try { applyStatus(JSON.parse(fx.responseText)); return } catch (e) {}
                }
                if (forceLoading) uiState = "loading"
                else uiState = everFresh ? "unavailable" : "loading"
            }
        }
        fx.open("GET", statusFileUrl())
        fx.send()
    }

    function refresh() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    try { applyStatus(JSON.parse(xhr.responseText)); return } catch (e) {}
                }
                refreshFromFile()
            }
        }
        xhr.open("GET", daemonBase + "/status")
        xhr.send()
    }

    function postConfig(patch) {
        suppressStatusUntil = Date.now() + 1500
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) refresh()
        }
        xhr.open("POST", daemonBase + "/config")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.send(JSON.stringify(patch))
    }

    function stepIndex(v) { return v === "precise" ? 0 : (v === "calm" ? 2 : 1) }
    function unitIndex(v) { return v === "metric" ? 1 : (v === "imperial" ? 2 : 0) }
    function stepName(i) { return i === 0 ? "precise" : (i === 2 ? "calm" : "balanced") }
    function unitName(i) { return i === 1 ? "metric" : (i === 2 ? "imperial" : "car") }

    function setMaster(on) {
        masterEnabled = on
        postConfig({"hudnav_enabled": on})
    }
    function setSourceEnabled(src, on) {
        if (src === "aa") aaEnabled = on
        else cpEnabled = on
        var patch = {}
        patch[src === "aa" ? "aa_enabled" : "cp_enabled"] = on
        postConfig(patch)
    }
    function setSteps(src, i) {
        if (src === "aa") aaSteps = i
        else cpSteps = i
        var patch = {}
        patch[src === "aa" ? "aa_distance_steps" : "cp_distance_steps"] = stepName(i)
        postConfig(patch)
    }
    function setUnits(src, i) {
        if (src === "aa") aaUnits = i
        else cpUnits = i
        var patch = {}
        patch[src === "aa" ? "aa_units" : "cp_units"] = unitName(i)
        postConfig(patch)
    }
    function restoreStock() {
        postConfig({"restore_stock": true, "hudnav_enabled": false,
                    "aa_enabled": false, "cp_enabled": false})
    }

    function statusSentence() {
        if (uiState === "loading") return "Getting the latest status…"
        if (uiState === "unavailable") return "Can't reach the projection service."
        if (uiState === "unsupported") return "Not available on this car."
        if (health === "disabled") return "Off - the car shows its own navigation only."
        if (health === "waiting_phone") return "On - no route from a phone right now."
        if (health === "guiding")
            return source === "carplay" ? "Showing turns from CarPlay."
                                        : "Showing turns from Android Auto."
        if (health === "not_guiding") return "Phone connected, but no turns right now."
        if (health === "error") return "Turns are not reaching the display."
        return "On - no route from a phone right now."
    }
    function statusDetailLine() {
        if (uiState !== "ready") return ""
        if (health === "guiding") {
            var metres = nextTurnMeters > 0 ? nextTurnMeters : 350
            return "Next turn: " + metres + " m"
        }
        if (health === "not_guiding") return "Start a route on your phone."
        if (health === "waiting_phone")
            return "Start a route on your phone."
        return ""
    }
    function groupCaption(key) {
        if (key !== "hudnav") return ""
        if (uiState === "unavailable") return "Service unavailable"
        if (uiState === "unsupported") return "Not available on this car"
        if (uiState === "loading") return "Checking service…"
        if (!masterEnabled) return "Off · stock behaviour"
        if (health === "guiding")
            return source === "carplay" ? "Turns · CarPlay"
                                        : "Turns · Android Auto"
        if (health === "waiting_phone") return "On · no route right now"
        if (health === "not_guiding") return "No turns right now"
        if (health === "error") return "Turns are not reaching the display"
        return "On · head-up display"
    }
    function versionsText() {
        return "App " + versionApp + " · service " + versionDaemon
               + " · firmware " + versionFirmware
    }
    function problemText() { return lastError !== "" ? lastError : "No problem recorded" }
    function aaConnectionText() {
        if (!aaEnabled) return "Off"
        return sourceKnown && source === "android_auto" ? "Active" : "Not connected"
    }
    function cpConnectionText() {
        if (!cpEnabled) return "Off"
        return sourceKnown && source === "carplay" ? "Active" : "Not connected"
    }
    function bootSafetyText() {
        if (bootGuard === "rolled_back")
            return "Rolled back after " + bootFailures + " failed starts"
        return "Armed"
    }

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }
    Timer {
        interval: 5000
        running: root.visible && uiState === "loading"
        repeat: false
        onTriggered: if (!everFresh) uiState = "unavailable"
    }

    // ---- layout ----------------------------------------------------------
    readonly property bool wide: AppHomeModel.isWideScr
    readonly property bool day: AppHomeModel.dayLightSuffix === "#GUI_DAY"
    readonly property color ink: HomeThemeHelper.normalTextColor
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE
    readonly property int navW: wide ? 400 : 340
    readonly property int gap: 36
    readonly property int paneW: Math.min(width - 96 - navW - gap, 1040)
    readonly property int blockW: navW + gap + paneW
    readonly property int blockX: Math.round((width - blockW) / 2)
    readonly property int bodyY: 90
    readonly property int bodyH: 538

    property string view: "main"           // main | diagnostics
    property string groupKey: "hudnav"

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    // Opaque themed panel so a loading/error state never shows stale rows
    // bleeding through.  Qt.tint keeps the fill derived from the theme ink.
    function surfaceColor(a) {
        return Qt.tint(root.day ? "#ffffff" : "#0a1119", tint(root.ink, a))
    }
    function selectGroup(key) { view = "main"; groupKey = key }
    function showDiagnostics() { view = "diagnostics" }
    function requestRestore() { modal.open() }
    function scrollToBottom() {
        Qt.callLater(function() {
            flick.contentY = Math.max(0, flick.contentHeight - flick.height)
        })
    }
    function scrollToSection(src) {
        Qt.callLater(function() {
            var target = src === "cp" ? cpSection.y : 0
            flick.contentY = Math.max(0, target - 4)
        })
    }

    // Feature groups come from a model: adding a group is one entry here and
    // one right-pane Item below.  The example entry is deliberately hidden.
    ListModel {
        id: groupModel
        ListElement {
            groupKey: "hudnav"
            label: "HUD navigation"
            caption: "Turn-by-turn on the head-up display"
        }
        // ListElement {
        //     groupKey: "evrouting"
        //     label: "EV routing"
        //     caption: "Report EV details to the phone"
        // }
    }

    Rectangle {
        anchors.fill: parent
        color: ConstantModel.GENERAL_COLOR_BLACK
    }
    Q_ThemeImage {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        themeSource: root.wide ? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
    }

    // Full-page shield: presses, releases, drags and wheel never fall through.
    MouseArea {
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
        anchors.fill: parent
        z: 2

        // ---- title bar ---------------------------------------------------
        Image {
            x: root.blockX
            y: 15
            width: 60
            height: 60
            smooth: true
            source: "file:///app/share/AppHomeScreen/projection/projection_icon_n.png"
        }
        QText {
            x: root.blockX + 84
            y: 0
            height: 90
            text: "Projection+"
            font.pixelSize: ConstantModel.FONT_SIZE_48
            textBold: true
            verticalAlignment: Text.AlignVCenter
        }
        ProjectionButton {
            id: closeButton
            x: root.blockX + root.blockW - width
            y: 12
            width: 180
            height: 66
            label: "Close"
            onClicked: root.close()
        }

        // ---- left group list ---------------------------------------------
        // Diagnostics is a navigation entry here too (not a feature group):
        // the user can always see where they are and tap HUD navigation to
        // return.  Feature groups still come from groupModel below.
        Column {
            id: nav
            x: root.blockX
            y: root.bodyY
            width: root.navW
            spacing: 8
            Repeater {
                model: groupModel
                delegate: ProjectionRow {
                    width: nav.width
                    height: 88
                    plate: true
                    title: model.label
                    titleSize: 33
                    subtitle: root.groupCaption(model.groupKey)
                    subtitleSize: 24
                    selected: root.view === "main" && root.groupKey === model.groupKey
                    onClicked: root.selectGroup(model.groupKey)
                }
            }
            ProjectionRow {
                width: nav.width
                height: 88
                plate: true
                title: "Diagnostics"
                titleSize: 33
                subtitle: "Versions and status"
                subtitleSize: 24
                selected: root.view === "diagnostics"
                onClicked: root.showDiagnostics()
            }
        }

        // ---- right pane surface ------------------------------------------
        Rectangle {
            id: plate
            x: root.blockX + root.navW + root.gap
            y: root.bodyY
            width: root.paneW
            height: root.bodyH
            radius: 24
            color: root.tint(root.ink, 0.07)
            border.width: 1
            border.color: root.tint(root.ink, 0.16)
        }

        // ---- HUD navigation group: scrollable content -------------------
        Item {
            x: plate.x
            y: plate.y
            width: plate.width
            height: plate.height
            visible: root.view === "main" && root.groupKey === "hudnav"

            Flickable {
                id: flick
                x: 8
                y: 8
                width: parent.width - 16
                height: parent.height - 16
                clip: true
                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: root.paneW - 40
                contentHeight: contentCol.height

                Column {
                    id: contentCol
                    width: parent.width - 24

                    ProjectionStatusBlock {
                        width: parent.width
                        statusText: root.statusSentence()
                        sourceText: root.statusDetailLine()
                    }

                    ProjectionRow {
                        width: parent.width
                        height: 108
                        title: "HUD navigation"
                        subtitle: "When off, the car behaves as stock."
                        trailingWidth: 96
                        trailingHeight: 54
                        rowEnabled: root.serviceOk
                        onClicked: root.setMaster(!root.masterEnabled)
                        ProjectionSwitch {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: root.masterEnabled
                            interactive: false
                        }
                    }

                    ProjectionSourceSection {
                        id: aaSection
                        width: parent.width
                        source: "aa"
                        sourceEnabled: root.aaEnabled
                        controlsEnabled: root.serviceOk
                        distanceSteps: root.aaSteps
                        units: root.aaUnits
                        onSourceToggled: root.setSourceEnabled("aa", !root.aaEnabled)
                        onDistanceChanged: root.setSteps("aa", index)
                        onUnitsPicked: root.setUnits("aa", index)
                    }

                    ProjectionSourceSection {
                        id: cpSection
                        width: parent.width
                        source: "cp"
                        sourceEnabled: root.cpEnabled
                        controlsEnabled: root.serviceOk
                        distanceSteps: root.cpSteps
                        units: root.cpUnits
                        onSourceToggled: root.setSourceEnabled("cp", !root.cpEnabled)
                        onDistanceChanged: root.setSteps("cp", index)
                        onUnitsPicked: root.setUnits("cp", index)
                    }
                }
            }

            // Thin stock-style thumb, visible only while content overflows.
            Rectangle {
                x: flick.x + flick.width - 10
                y: flick.y + flick.visibleArea.yPosition * flick.height
                width: 6
                height: Math.max(36, flick.visibleArea.heightRatio * flick.height)
                radius: 3
                visible: flick.visibleArea.heightRatio < 1
                color: root.tint(root.ink, 0.5)
            }
        }

        // ---- Diagnostics group (one level deeper) ------------------------
        ProjectionDiagnosticsPane {
            x: plate.x + 8
            y: plate.y + 8
            width: plate.width - 16
            height: plate.height - 16
            visible: root.view === "diagnostics"
            versionsText: root.versionsText()
            aaConnectionText: root.aaConnectionText()
            cpConnectionText: root.cpConnectionText()
            bootSafetyText: root.bootSafetyText()
            problemText: root.problemText()
            controlsEnabled: root.serviceOk
            onRestoreRequested: root.requestRestore()
        }

        // ---- empty / error states of the pane ----------------------------
        Item {
            x: plate.x
            y: plate.y
            width: plate.width
            height: plate.height
            visible: root.view === "main" && root.groupKey === "hudnav"
                     && root.uiState !== "ready"
            z: 5

            Rectangle {
                anchors.fill: parent
                radius: 24
                color: root.surfaceColor(0.16)
                border.width: 1
                border.color: root.tint(root.ink, 0.16)
            }

            ProjectionLoading {
                anchors.fill: parent
                visible: root.uiState === "loading"
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 96
                spacing: 12
                visible: root.uiState === "unavailable" || root.uiState === "unsupported"

                QText {
                    width: parent.width
                    height: 52
                    text: root.uiState === "unsupported" ? "Not available on this car."
                                                         : "Can't reach the projection service."
                    font.pixelSize: ConstantModel.FONT_SIZE_39
                    textBold: true
                    horizontalAlignment: Text.AlignHCenter
                }
                QText {
                    width: parent.width
                    height: 92
                    text: root.uiState === "unsupported"
                          ? "This feature needs a newer firmware. Nothing has changed on your car."
                          : "Check that the update is installed, then try again."
                    font.pixelSize: ConstantModel.FONT_SIZE_27
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                ProjectionButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 220
                    height: 66
                    label: "Try again"
                    visible: root.uiState === "unavailable"
                    onClicked: root.refresh()
                }
            }
        }
    }


    ProjectionModal {
        id: modal
        onConfirmed: {
            root.restoreStock()
            root.view = "main"
            modal.finish()
        }
        onCancelled: modal.dismiss()
    }
}
