// WIFI_PAYLOAD_260128
// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y

    signal closed()

    // ---- overlay / foreground ownership ------------------------------
    // The page owns its visibility: callers use open()/close(), never
    // .visible.  Never paint while another app owns the Wayland surface,
    // otherwise taps fall through to it (UI_CONTEXT.md Rule 2).
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    property int homeSeq: 0
    property double homeEpoch: 0
    property string homeSource: "none"
    property bool homeMonitor: false
    property bool homeFileSeen: false
    property double openedAt: 0
    property bool homeBaselineValid: false
    property double homeEpochBaseline: 0
    property bool homeFallback: false
    property int openSerial: 0
    property int homeReadSerial: 0
    property int homeAcceptedRead: 0

    // Do not bind `visible` to `requested`.  Older Tweaks/Battery host
    // variants assign wifiPage.visible directly; a direct assignment
    // destroys a QML binding, which made Exit set requested=false while the
    // page stayed painted.  Keep the two properties synchronized explicitly
    // so both open()/close() and legacy host assignments work.
    visible: false
    focus: visible
    enabled: !(typeof AppHomeModel !== "undefined"
               && AppHomeModel.sysPopupShowing)

    function open() {
        if (atOtherApp) {
            requested = false
            visible = false
            return
        }
        openSerial++
        requested = true
        visible = true
        openedAt = Date.now()
        homeBaselineValid = false
        refresh()
        refreshHome()
    }
    function close() {
        if (!requested && !visible) return
        requested = false
        visible = false
        openSerial++
        homeBaselineValid = false
        closed()
    }

    onVisibleChanged: {
        console.log("[WifiPage] visible=" + visible + " requested=" + requested
                    + " atOtherApp=" + atOtherApp + " serial=" + openSerial)
        if (visible && atOtherApp) {
            close()
            return
        }
        if (visible && !requested) {
            // Compatibility for a host that assigns .visible = true instead
            // of calling open().  Re-arm the page exactly once.
            requested = true
            openSerial++
            openedAt = Date.now()
            homeBaselineValid = false
            refresh()
            refreshHome()
        } else if (!visible && requested) {
            requested = false
        }
    }

    onRequestedChanged: {
        if (requested && !atOtherApp && !visible)
            visible = true
        else if (!requested && visible)
            visible = false
    }

    onAtOtherAppChanged: if (atOtherApp) close()

    // Steering-wheel Home/Star is a session-bus event; the daemon tails it
    // and bumps home_seq.  Any navigation or foreground change also closes
    // the overlay.  AppHomeModel.onButtonBackClicked is only the home
    // screen's own back affordance, not the steering-wheel key.
    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.close()
        onSwitchToOtherApp: root.close()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.close()
        onCurrentScreenChanged: root.close()
        onCurrentHomeAdvancedPageIdxChanged: root.close()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.close()
        onSysPopupShowingChanged: if (AppHomeModel.sysPopupShowing) root.close()
    }

    function acceptHome(d) {
        var hs = (typeof d.seq === "number") ? d.seq : 0
        var ep = (typeof d.epoch === "number") ? d.epoch : 0
        // Snapshot the first *fresh* response after open. A new daemon epoch
        // is a new counter domain, not a Home press or an unreachable count.
        if (!homeBaselineValid || ep !== homeEpochBaseline) {
            homeSeqBaseline = hs
            homeEpochBaseline = ep
            homeBaselineValid = requested
        } else if (requested && hs > homeSeqBaseline) {
            close()
        }
        homeSeq = hs
        homeEpoch = ep
        if (d.source) homeSource = d.source
        homeMonitor = d.monitor === true
    }

    // Live log console on the right; only wide (1920) layouts have room.
    readonly property bool showConsole: width >= 1700
    property string consoleSource: "daemon"

    // ---- daemon state -------------------------------------------------
    property bool serviceOk: false
    property bool observationsFresh: false
    property string mode: "off"
    property bool wlanOn: false
    property bool driver: false
    property string ip: ""
    property string ssid: ""
    property int signalLevel: 0
    property bool apActive: false
    property string apSsid: ""
    property int apClients: 0
    property bool carplayReady: false
    property bool carplayIe: false
    property bool carplayIap2: false
    property bool carplayMdns: false
    property bool sshActive: false
    property bool sshEnabled: false
    property int sshPort: 2222
    property bool sshPortFallback: false
    property bool vendorSsh22: false
    property int secureConsole: -1
    property bool debugConsole: false
    property bool imported: false
    property string importedFrom: ""
    property string clientSsid: ""
    property string clientNetworks: ""
    property int clientNetCount: 1
    property string carplaySsid: ""
    property string carplayPsk: ""
    property bool carplayPskSet: false
    property int carplayBand: 2
    property int carplayChannel: 36
    property string carplayBtaddr: ""
    property string carplayId: ""
    property string daemonMessage: ""
    property bool btDiscoverable: false
    property bool btPairable: false
    property string pairMessage: ""

    property int currentTab: 0
    property var networks: []
    property bool scanning: false
    property string selectedSsid: ""
    property double statusUpdatedAt: 0
    property int homeSeqBaseline: 0

    readonly property string daemonBase: "http://127.0.0.1:8090"
    readonly property bool carplayMode: mode === "carplay"

    function applyStatus(d) {
        // A parseable status file is not proof the daemon is alive: require a
        // fresh timestamp so a crashed daemon cannot leave stale controls.
        var upd = (typeof d.updated_ms === "number") ? d.updated_ms : 0
        serviceOk = upd > 0 && (Date.now() - upd) < 8000
        statusUpdatedAt = upd
        observationsFresh = d.observations_fresh === true
        mode = d.mode
        wlanOn = d.wlan_on
        driver = d.driver
        ip = d.ip
        ssid = d.ssid
        signalLevel = d.signal
        apActive = d.ap_active
        apSsid = d.ap_ssid
        apClients = d.ap_clients
        carplayReady = d.carplay_ready
        carplayIe = d.carplay_ie === true
        carplayIap2 = d.carplay_iap2
        carplayMdns = d.carplay_mdns
        sshActive = d.ssh_active
        sshEnabled = d.ssh_enabled
        sshPort = d.ssh_port
        sshPortFallback = d.ssh_port_fallback
        vendorSsh22 = d.vendor_ssh_22
        secureConsole = d.secure_console
        debugConsole = d.debug_console
        imported = d.imported
        importedFrom = d.imported_from
        clientSsid = d.client_ssid
        if (d.client_networks !== undefined) clientNetworks = d.client_networks
        if (d.client_net_count !== undefined) clientNetCount = d.client_net_count
        carplaySsid = d.carplay_ssid
        carplayPsk = d.carplay_psk
        carplayPskSet = d.carplay_psk_set
        carplayBand = d.carplay_band
        carplayChannel = d.carplay_channel
        carplayBtaddr = d.carplay_btaddr
        carplayId = d.carplay_id
        daemonMessage = d.message
        btDiscoverable = d.bt_discoverable === true
        btPairable = d.bt_pairable === true
        pairMessage = d.pair_message || ""
        // home.json is the fast path; /status is only a fallback for a
        // daemon that predates it.  Once home.json has been read, ignore
        // the slower status snapshot so the two cannot flap.
        if (homeFallback && serviceOk)
            acceptHome({seq: d.home_seq, epoch: d.home_epoch,
                        source: d.home_source, monitor: d.home_monitor})
        homeSource = d.home_source || homeSource
        homeMonitor = d.home_monitor === true
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
        xhr.open("GET", "file:///rw_data/wifi/status.json")
        xhr.send()
    }

    function refresh() {
        // The daemon writes status.json every 2 s; reading the file avoids
        // running its expensive build_status() on every UI poll.  HTTP is
        // the fallback while the file is missing or unreadable.
        var fx = new XMLHttpRequest()
        fx.onreadystatechange = function() {
            if (fx.readyState === XMLHttpRequest.DONE) {
                if (fx.status === 200 || fx.status === 0) {
                    try {
                        applyStatus(JSON.parse(fx.responseText))
                        if (serviceOk) return
                    } catch (e) {
                        // fall through to HTTP
                    }
                }
                var xhr = new XMLHttpRequest()
                xhr.onreadystatechange = function() {
                    if (xhr.readyState === XMLHttpRequest.DONE &&
                        xhr.status === 200) {
                        try {
                            applyStatus(JSON.parse(xhr.responseText))
                        } catch (e) {
                            serviceOk = false
                        }
                    } else if (xhr.readyState === XMLHttpRequest.DONE) {
                        serviceOk = false
                    }
                }
                xhr.open("GET", daemonBase + "/status")
                xhr.send()
            }
        }
        fx.open("GET", "file:///rw_data/wifi/status.json")
        fx.send()
    }

    function post(path, done) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (done) done()
                refresh()
            }
        }
        xhr.open("POST", daemonBase + path)
        xhr.send()
    }

    function get(path, done) {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (done) done(xhr)
                refresh()
            }
        }
        xhr.open("GET", daemonBase + path)
        xhr.send()
    }

    function refreshHome() {
        // The daemon writes this tiny file immediately on a Home/Star
        // event, so the overlay can close within ~0.5 s without forcing
        // the expensive full status build at 1 Hz.
        var xhr = new XMLHttpRequest()
        var serial = openSerial
        var readSerial = ++homeReadSerial
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (serial !== openSerial || readSerial < homeAcceptedRead) return
            if (xhr.status !== 200 && xhr.status !== 0) {
                homeFallback = true
                return
            }
            try {
                var d = JSON.parse(xhr.responseText)
                homeAcceptedRead = readSerial
                acceptHome(d)
                homeFileSeen = true
                homeFallback = false
            } catch (e) { homeFallback = true }
        }
        xhr.open("GET", "file:///rw_data/wifi/home.json")
        xhr.send()
    }

    function refreshConsole() {
        if (!showConsole) return
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 200) return
            try {
                var d = JSON.parse(xhr.responseText)
                var t = d.log || ""
                var lines = t.split("\n")
                var start = Math.max(0, lines.length - 40)
                consoleModel.clear()
                for (var i = start; i < lines.length; i++) {
                    if (lines[i].length > 0)
                        consoleModel.append({ line: lines[i] })
                }
            } catch (e) {}
        }
        xhr.open("GET", daemonBase + "/logs?source=" + consoleSource)
        xhr.send()
    }

    function scan() {
        networks = []
        scanning = true
        get("/scan", function(xhr) {
            if (xhr.status !== 200 && xhr.status !== 202) { scanning = false; return }
            try {
                var d = JSON.parse(xhr.responseText)
                networks = d.networks || []
                scanning = d.scanning === true
            } catch (e) {
                networks = []
                scanning = false
            }
        })
    }

    function maskKey(s) {
        if (!s) return "(not imported)"
        return "\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022"
    }

    function bandText(b) {
        if (b === 1) return "2.4 GHz"
        if (b === 2) return "5 GHz"
        return "Auto"
    }

    function modeText() {
        if (!wlanOn || mode === "off") return "Off"
        return mode === "carplay" ? "CarPlay AP" : "Client"
    }

    function ipText() {
        if (ip && ip.length) return ip
        return apActive ? "10.42.0.1" : "--"
    }

    function sshCommand() {
        return "ssh car@" + ipText() + " -p " + sshPort
    }

    Timer {
        interval: 500
        running: root.requested
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refreshHome()
    }

    Timer {
        interval: 750
        running: root.requested && root.scanning
        repeat: true
        onTriggered: root.get("/scan?poll=1", function(xhr) {
            if (xhr.status !== 200) { root.scanning = false; return }
            try {
                var d = JSON.parse(xhr.responseText)
                root.scanning = d.scanning === true
                root.networks = d.networks || []
            } catch (e) { root.scanning = false }
        })
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    // ---- background ---------------------------------------------------
    Rectangle {
        anchors.fill: parent
        color: "#0E0E0E"
    }

    // Full-screen input shield (APP_OVERLAY_CHECKLIST.md).  A bare
    // onClicked handler is not enough: presses, releases, drags and the
    // wheel must all be owned, and the real content must sit above it.
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

    // Local close control is above both content and console. It never waits
    // for a daemon request, association, scan or log operation to finish.
    Rectangle {
        id: closeButton
        objectName: "wifiExit"
        z: 10
        x: content.x + content.width - width
        y: 26
        width: 170
        height: 52
        radius: 26
        color: closeMouse.pressed ? "#4A4A4A" : "#2E2E2E"
        border.color: "#777777"
        border.width: 2
        QText {
            anchors.centerIn: parent
            text: "\u2715  Exit"
            font.pixelSize: 26
            textBold: true
            color: "#FFFFFF"
        }
        MouseArea {
            id: closeMouse
            anchors.fill: parent
            preventStealing: true
            onClicked: root.close()
        }
    }

    Item {
        id: content
        z: 2
        width: 1180
        height: root.height
        x: root.showConsole ? 16 : (root.width - width) / 2

        Image {
            id: headerIcon
            x: 0
            y: 24
            width: 56
            height: 56
            smooth: true
            source: "file:///app/share/AppHomeScreen/wifi/wifi_icon_n.png"
        }

        QText {
            id: title
            x: 76
            y: 18
            text: "WiFi"
            font.pixelSize: 46
            textBold: true
            color: "#FFFFFF"
        }

        QText {
            id: subtitle
            x: 78
            y: 72
            text: "WLAN \u00b7 Wireless CarPlay \u00b7 SSH"
            font.pixelSize: 22
            color: "#9A9A9A"
        }

        // ---- tabs -----------------------------------------------------
        Row {
            id: tabRow
            x: 0
            y: 110
            spacing: 10

            WifiButton {
                width: 190
                label: "Status"
                selected: root.currentTab === 0
                onClicked: root.currentTab = 0
            }
            WifiButton {
                width: 190
                label: "Client"
                selected: root.currentTab === 1
                onClicked: root.currentTab = 1
            }
            WifiButton {
                width: 210
                label: "CarPlay AP"
                selected: root.currentTab === 2
                onClicked: root.currentTab = 2
            }
            WifiButton {
                width: 190
                label: "SSH"
                selected: root.currentTab === 3
                onClicked: root.currentTab = 3
            }
        }

        // ================= STATUS ====================================
        Item {
            id: statusTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 0

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 130
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 28
                    y: 20
                    text: "WLAN interface"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 28
                    y: 56
                    text: root.wlanOn ? "On" : "Off"
                    font.pixelSize: 46
                    textBold: true
                    color: root.wlanOn ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 28
                    y: 108
                    text: !root.observationsFresh ? "Refreshing interface state..."
                                      : root.driver ? "bcmdhd loaded \u00b7 wlan0 present"
                                      : "driver / wlan0 not detected"
                    font.pixelSize: 21
                    color: root.driver ? "#7A7A7A" : "#FFB300"
                }

                WifiSwitch {
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.wlanOn
                    switchEnabled: root.serviceOk
                    onToggled: root.post("/wlan?on=" + (root.wlanOn ? "0" : "1"))
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 130
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 28
                    y: 20
                    text: "Current mode"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 28
                    y: 56
                    text: root.modeText()
                    font.pixelSize: 46
                    textBold: true
                    color: "#FFFFFF"
                }
                QText {
                    x: 28
                    y: 108
                    text: "IP " + root.ipText()
                    font.pixelSize: 21
                    color: "#7A7A7A"
                }
            }

            Row {
                x: 0
                y: 140
                spacing: 12

                WifiButton {
                    width: 220
                    label: "Off"
                    selected: root.mode === "off" || !root.wlanOn
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=off")
                }
                WifiButton {
                    width: 220
                    label: "Client"
                    selected: root.mode === "client" && root.wlanOn
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=client")
                }
                WifiButton {
                    width: 240
                    label: "CarPlay AP"
                    selected: root.mode === "carplay" && root.wlanOn
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/mode?value=carplay")
                }
                WifiButton {
                    width: 220
                    label: "Import USB"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/import")
                }
                WifiButton {
                    width: 200
                    label: "Save logs"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/logs/dump")
                }
            }

            Rectangle {
                x: 0
                y: 212
                width: 1180
                height: 96
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 14
                    text: "Imported credentials"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 48
                    text: root.imported
                          ? ("Client nets (" + root.clientNetCount + ", priority order): " +
                             (root.clientNetworks || root.clientSsid || "--") +
                             "   \u00b7   CarPlay: " + (root.carplaySsid || "--") +
                             "   \u00b7   " + root.importedFrom)
                          : "No wifi.txt imported yet. Put wifi.txt on the USB root, then press Import USB."
                    font.pixelSize: 21
                    color: root.imported ? "#FFFFFF" : "#FFB300"
                    width: 1130
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                x: 0
                y: 318
                width: 570
                height: 96
                radius: 14
                color: "#1B1B1B"
                border.color: root.apActive ? "#2E7D32" : "#333333"
                border.width: root.apActive ? 2 : 1

                QText {
                    x: 24
                    y: 14
                    text: "Access point"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 48
                    text: root.apActive
                          ? (root.apSsid + "  \u00b7  " + root.apClients + " client(s)")
                          : "Not running"
                    font.pixelSize: 24
                    color: root.apActive ? "#4CAF50" : "#FFFFFF"
                }
            }

            Rectangle {
                x: 610
                y: 318
                width: 570
                height: 96
                radius: 14
                color: "#1B1B1B"
                border.color: root.sshActive ? "#2E7D32" : "#333333"
                border.width: root.sshActive ? 2 : 1

                QText {
                    x: 24
                    y: 14
                    text: "SSH"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 48
                    text: root.sshActive
                          ? ("Listening on " + root.sshPort + "  \u00b7  " + root.sshCommand())
                          : "Stopped"
                    font.pixelSize: root.sshActive ? 21 : 24
                    color: root.sshActive ? "#4CAF50" : "#FFFFFF"
                    width: 530
                    elide: Text.ElideRight
                }
            }

            QText {
                x: 4
                y: 428
                width: 1172
                text: root.daemonMessage
                font.pixelSize: 20
                color: "#7A7A7A"
                elide: Text.ElideRight
            }
        }

        // ================= CLIENT ====================================
        Item {
            id: clientTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 1

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Client network"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: root.clientSsid ? root.clientSsid : "(no SSID imported)"
                    font.pixelSize: 40
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 112
                    text: (root.clientNetCount > 1 && root.clientNetworks
                           ? ("Priority: " + root.clientNetworks + "   \u00b7   ")
                           : "") +
                          "Key: " + root.maskKey(root.clientSsid)
                    font.pixelSize: 22
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 142
                    text: root.ssid ? ("Connected to " + root.ssid + "  \u00b7  " +
                                     root.signalLevel + " dBm  \u00b7  " + root.ip)
                                   : "Not connected"
                    font.pixelSize: 21
                    color: root.ssid ? "#4CAF50" : "#FFB300"
                    width: 520
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "How it works"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    width: 520
                    text: "Put wifi.txt on the USB root with ssid= and psk=, then press Import USB. "
                          + "The client uses the imported SSID/key. Passwords are not typed on screen."
                    font.pixelSize: 22
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 192
                spacing: 12

                WifiButton {
                    width: 220
                    label: "Scan"
                    buttonEnabled: root.serviceOk
                    onClicked: root.scan()
                }
                WifiButton {
                    width: 260
                    label: "Connect imported"
                    buttonEnabled: root.serviceOk && root.clientSsid !== ""
                    onClicked: root.post("/client/connect")
                }
                WifiButton {
                    width: 220
                    label: "Disconnect"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/client/disconnect")
                }
                WifiButton {
                    width: 220
                    label: "Import USB"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/import")
                }
            }

            Rectangle {
                x: 0
                y: 264
                width: 1180
                height: 186
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 10
                    text: "Scan results"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }

                ListView {
                    x: 18
                    y: 44
                    width: 1144
                    height: 132
                    clip: true
                    model: root.networks
                    delegate: Rectangle {
                        width: 1120
                        height: 44
                        color: "transparent"
                        QText {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 760
                            text: modelData.ssid
                            font.pixelSize: 24
                            color: "#FFFFFF"
                            elide: Text.ElideRight
                        }
                        QText {
                            x: 780
                            anchors.verticalCenter: parent.verticalCenter
                            width: 120
                            text: modelData.security
                            font.pixelSize: 22
                            color: "#9A9A9A"
                        }
                        QText {
                            x: 910
                            anchors.verticalCenter: parent.verticalCenter
                            width: 180
                            text: modelData.signal + " dBm"
                            font.pixelSize: 22
                            color: "#9A9A9A"
                        }
                    }
                }

                QText {
                    x: 24
                    y: 152
                    width: 1130
                    text: root.daemonMessage !== "" ? root.daemonMessage
                          : (root.networks.length === 0 ? "Press Scan to list nearby networks." : "")
                    font.pixelSize: 20
                    color: root.daemonMessage !== "" ? "#FFB300" : "#7A7A7A"
                    elide: Text.ElideRight
                }
            }
        }

        // ================= CARPLAY ===================================
        Item {
            id: carplayTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 2

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "CarPlay access point"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: root.carplaySsid
                    font.pixelSize: 40
                    textBold: true
                    color: "#FFFFFF"
                    width: 520
                    elide: Text.ElideRight
                }
                QText {
                    x: 24
                    y: 112
                    text: "Key: " + (root.carplayPsk ? root.carplayPsk : "(not set)")
                    font.pixelSize: 22
                    color: "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 142
                    text: "Band: " + root.bandText(root.carplayBand) +
                          "   \u00b7   Channel: " + root.carplayChannel +
                          "   \u00b7   Max clients: " + (root.apActive ? root.apClients : 2)
                    font.pixelSize: 21
                    color: "#B0B0B0"
                }
                QText {
                    x: 24
                    y: 176
                    text: root.apActive ? "AP running" : "AP stopped"
                    font.pixelSize: 24
                    textBold: true
                    color: root.apActive ? "#4CAF50" : "#FFB300"
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 240
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "CarPlay transport"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 50
                    text: "iAP2: " + (root.carplayIap2 ? "running" : "stopped")
                    font.pixelSize: 24
                    color: root.carplayIap2 ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 82
                    text: "mDNS: " + (root.carplayMdns ? "running" : "stopped")
                    font.pixelSize: 24
                    color: root.carplayMdns ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 114
                    text: "Ready: " + (!root.observationsFresh ? "checking" : (root.carplayReady ? "yes" : "no"))
                    font.pixelSize: 24
                    color: root.carplayReady ? "#4CAF50" : "#FFB300"
                }
                QText {
                    x: 24
                    y: 140
                    width: 520
                    text: "Press Start CarPlay AP, then pick the car on the iPhone: "
                          + "Settings > General > CarPlay > Available Cars."
                    font.pixelSize: 19
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 192
                    width: 520
                    text: "iPhone already paired via Bluetooth? Just pick the car "
                          + "under Settings > General > CarPlay once the AP is running."
                    font.pixelSize: 17
                    color: "#8FD18F"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 252
                spacing: 10

                WifiButton {
                    width: 230
                    label: "Start CarPlay AP"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/pair")
                }
                WifiButton {
                    width: 210
                    label: "Stop pairing"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/pair/stop")
                }
                WifiButton {
                    width: 220
                    label: "Restart stack"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/restart")
                }
                WifiButton {
                    width: 170
                    label: "Stop AP"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/carplay/stop")
                }
                WifiButton {
                    width: 210
                    label: "Import USB"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/import")
                }
            }

            QText {
                x: 0
                y: 308
                width: 1180
                text: (root.pairMessage !== "" && root.daemonMessage !== "")
                      ? (root.pairMessage + "   \u00b7   " + root.daemonMessage)
                      : (root.pairMessage !== "" ? root.pairMessage : root.daemonMessage)
                font.pixelSize: 18
                color: (root.pairMessage !== "" || root.daemonMessage !== "") ? "#FFB300" : "#7A7A7A"
                elide: Text.ElideRight
            }

            Rectangle {
                x: 0
                y: 326
                width: 1180
                height: 124
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 8
                    text: "Identity sent in the CarPlay information element"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 40
                    text: "Manufacturer: Hyundai   \u00b7   Model: Ioniq 5   \u00b7   Name: Ioniq 5"
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 68
                    text: "Bluetooth: " + (root.carplayBtaddr ? root.carplayBtaddr : "(not detected)")
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 96
                    text: "Device ID: " + (root.carplayId ? root.carplayId : "(not detected)")
                    font.pixelSize: 21
                    color: "#FFFFFF"
                }
            }
        }

        // ================= SSH =======================================
        Item {
            id: sshTab
            x: 0
            y: 170
            width: 1180
            height: 450
            visible: root.currentTab === 3

            Rectangle {
                x: 0
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "SSH server"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 56
                    text: root.sshActive ? "Running" : "Stopped"
                    font.pixelSize: 46
                    textBold: true
                    color: root.sshActive ? "#4CAF50" : "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 116
                    text: "Port " + root.sshPort + "  \u00b7  user car"
                    font.pixelSize: 22
                    color: "#B0B0B0"
                }
                QText {
                    x: 24
                    y: 146
                    text: root.sshCommand()
                    font.pixelSize: 21
                    color: "#7A7A7A"
                }

                WifiSwitch {
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    checked: root.sshEnabled
                    switchEnabled: root.serviceOk
                    onToggled: root.post("/ssh?enabled=" + (root.sshEnabled ? "0" : "1") + "&port=" + root.sshPort)
                }
            }

            Rectangle {
                x: 610
                y: 0
                width: 570
                height: 180
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 16
                    text: "Credentials"
                    font.pixelSize: 28
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 58
                    text: "User: car"
                    font.pixelSize: 24
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 92
                    text: "Password: electric"
                    font.pixelSize: 24
                    color: "#FFFFFF"
                }
                QText {
                    x: 24
                    y: 132
                    width: 520
                    text: "Password auth is enabled at your request. Change it after install. "
                          + "You can also import authorized_keys from the USB root."
                    font.pixelSize: 20
                    color: "#FFB300"
                    wrapMode: Text.WordWrap
                }
            }

            Row {
                x: 0
                y: 192
                spacing: 12

                WifiButton {
                    width: 260
                    label: "Import SSH keys"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh/import")
                }
                WifiButton {
                    width: 220
                    label: "Restart SSH"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh?enabled=1&port=" + root.sshPort)
                }
                WifiButton {
                    width: 220
                    label: "Stop SSH"
                    buttonEnabled: root.serviceOk
                    onClicked: root.post("/ssh?enabled=0&port=" + root.sshPort)
                }
            }

            Rectangle {
                x: 0
                y: 264
                width: 1180
                height: 186
                radius: 14
                color: "#1B1B1B"
                border.color: "#333333"
                border.width: 1

                QText {
                    x: 24
                    y: 10
                    text: "Notes"
                    font.pixelSize: 24
                    color: "#9A9A9A"
                }
                QText {
                    x: 24
                    y: 44
                    width: 1120
                    text: "SSH is only reachable over the head unit's WLAN. In CarPlay AP mode "
                          + "the address is normally 10.42.0.1. In client mode use the IP shown "
                          + "on the Status tab. The app adds a firewall rule for wlan0 and the "
                          + "selected port. Dropbear is static and does not depend on the "
                          + "head unit's glibc."
                    font.pixelSize: 20
                    color: "#B0B0B0"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 108
                    width: 1120
                    visible: root.sshPortFallback || root.vendorSsh22
                    text: root.vendorSsh22
                          ? ("Port 22 is already in use (vendor/debug SSH). The app uses port " + root.sshPort + ".")
                          : ("The configured SSH port was busy. The app uses port " + root.sshPort + ".")
                    font.pixelSize: 18
                    color: "#FFB300"
                    wrapMode: Text.WordWrap
                }
                QText {
                    x: 24
                    y: 146
                    text: "Secure console: " + (root.debugConsole ? "debug (serial/root)" :
                          (root.secureConsole === 1 ? "secure" : "unknown"))
                    font.pixelSize: 18
                    color: root.debugConsole ? "#4CAF50" : "#7A7A7A"
                }
                QText {
                    x: 24
                    y: 164
                    text: "Authorized keys: " + (root.sshActive ? "loaded from /rw_data/wifi/ssh/authorized_keys" : "not loaded")
                    font.pixelSize: 18
                    color: "#7A7A7A"
                }
            }
        }
    }

    // ---- live log console (wide layouts) ---------------------------
    Rectangle {
        id: consolePanel
        visible: root.showConsole
        x: root.width - 436
        y: 20
        width: 420
        height: 600
        radius: 12
        color: "#0B0B0B"
        border.color: "#333333"
        border.width: 1
        z: 5

        QText {
            x: 16
            y: 18
            text: "Live log"
            font.pixelSize: 22
            color: "#9A9A9A"
        }

        Row {
            x: 16
            y: 56
            spacing: 8

            WifiButton {
                width: 130
                height: 42
                label: "Daemon"
                selected: root.consoleSource === "daemon"
                onClicked: { root.consoleSource = "daemon"; root.refreshConsole() }
            }
            WifiButton {
                width: 150
                height: 42
                label: "Supplicant"
                selected: root.consoleSource === "wpa"
                onClicked: { root.consoleSource = "wpa"; root.refreshConsole() }
            }
            WifiButton {
                width: 90
                height: 42
                label: "Dump"
                onClicked: root.post("/logs/dump")
            }
        }

        Rectangle {
            x: 12
            y: 104
            width: 396
            height: 484
            radius: 8
            color: "#050505"
            border.color: "#222222"
            border.width: 1
            clip: true

            ListView {
                id: consoleView
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                model: consoleModel
                delegate: QText {
                    width: consoleView.width
                    text: line
                    font.pixelSize: 13
                    color: "#8FD18F"
                    wrapMode: Text.WrapAnywhere
                }
                onCountChanged: positionViewAtEnd()
                Component.onCompleted: positionViewAtEnd()
            }

            QText {
                anchors.centerIn: parent
                visible: consoleModel.count === 0
                text: "(no log lines yet)"
                font.pixelSize: 18
                color: "#555555"
            }
        }
    }

    ListModel { id: consoleModel }

    Timer {
        interval: 2000
        running: root.visible && root.showConsole
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refreshConsole()
    }

}
