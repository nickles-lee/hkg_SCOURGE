// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// TWEAKS_PAYLOAD_260128
import QtQuick 2.0
import "common"

Item {
    id: root
    width: parent ? parent.width : 1920
    height: parent ? parent.height : 720
    visible: false
    z: 200

    property string currentZone: "Europe/Amsterdam"
    property var zones: []
    property var regions: []
    property var filteredZones: []
    property string selectedRegion: "Europe"
    property string loadError: ""

    signal selected(string zone)
    signal closed()

    function regionOf(zone) {
        var i = zone.indexOf("/")
        return i > 0 ? zone.substring(0, i) : "Other"
    }

    function sortRegions(list) {
        var order = ["Europe", "America", "Asia", "Australia", "Africa", "Pacific", "Other"]
        list.sort(function(a, b) {
            var ia = order.indexOf(a)
            var ib = order.indexOf(b)
            if (ia < 0) ia = 99
            if (ib < 0) ib = 99
            if (ia !== ib) return ia - ib
            return a < b ? -1 : (a > b ? 1 : 0)
        })
        return list
    }

    function refreshZones() {
        var out = []
        for (var i = 0; i < zones.length; i++) {
            if (regionOf(zones[i]) === selectedRegion)
                out.push(zones[i])
        }
        filteredZones = out
    }

    function applyZones(text) {
        var lines = String(text).split(/\r?\n/)
        var list = []
        var seen = {}
        for (var i = 0; i < lines.length; i++) {
            var z = lines[i].replace(/^\s+|\s+$/g, "")
            if (!z || seen[z]) continue
            seen[z] = true
            list.push(z)
        }
        if (list.length === 0)
            list = ["Europe/Amsterdam", "Europe/London", "Europe/Dublin",
                    "America/New_York", "America/Chicago", "America/Denver",
                    "America/Los_Angeles", "Australia/Sydney", "UTC"]
        zones = list
        var rs = []
        var rseen = {}
        for (var j = 0; j < zones.length; j++) {
            var r = regionOf(zones[j])
            if (!rseen[r]) { rseen[r] = true; rs.push(r) }
        }
        regions = sortRegions(rs)
        selectedRegion = regionOf(currentZone)
        if (regions.indexOf(selectedRegion) < 0)
            selectedRegion = regions.length > 0 ? regions[0] : "Other"
        refreshZones()
    }

    function loadZones() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200 || xhr.status === 0) {
                    applyZones(xhr.responseText)
                } else {
                    loadError = "No zone list at /rw_data/tweaks/zones.txt"
                    applyZones("")
                }
            }
        }
        xhr.open("GET", "file:///rw_data/tweaks/zones.txt")
        xhr.send()
    }

    Component.onCompleted: loadZones()

    onCurrentZoneChanged: {
        selectedRegion = regionOf(currentZone)
        refreshZones()
    }

    Rectangle {
        anchors.fill: parent
        color: "#0E0E0E"
    }

    // Swallow taps outside the lists/buttons.
    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    Item {
        id: content
        width: 1180
        height: 630
        anchors.horizontalCenter: parent.horizontalCenter

        QText {
            x: 0
            y: 6
            text: "DST timezone"
            font.pixelSize: 44
            textBold: true
            color: "#FFFFFF"
        }

        QText {
            x: 0
            y: 56
            width: 900
            text: "Choose the zone whose daylight-saving rule the Tweaks daemon should follow. "
                  + "The vehicle's normal timezone is still controlled by Setup > Date and Time."
            font.pixelSize: 19
            color: "#9A9A9A"
            wrapMode: Text.WordWrap
        }

        TweaksButton {
            width: 150
            height: 46
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 6
            label: "\u2039  Close"
            onClicked: root.closed()
        }

        QText {
            x: 0
            y: 100
            text: "Current: " + root.currentZone
            font.pixelSize: 21
            color: "#4CAF50"
        }

        Rectangle {
            x: 0
            y: 136
            width: 280
            height: 440
            radius: 12
            color: "#1B1B1B"
            border.color: "#333333"
            border.width: 1

            ListView {
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                model: root.regions
                delegate: Item {
                    width: parent.width
                    height: 48

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: modelData === root.selectedRegion ? "#2E7D32" : "#202020"
                    }

                    QText {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        text: modelData
                        font.pixelSize: 22
                        color: "#FFFFFF"
                        verticalAlignment: Text.AlignVCenter
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.selectedRegion = modelData
                            root.refreshZones()
                        }
                    }
                }
            }
        }

        Rectangle {
            x: 300
            y: 136
            width: 880
            height: 440
            radius: 12
            color: "#1B1B1B"
            border.color: "#333333"
            border.width: 1

            ListView {
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                model: root.filteredZones
                delegate: Item {
                    width: parent.width
                    height: 48

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: modelData === root.currentZone ? "#2E7D32" : "#202020"
                    }

                    QText {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        text: modelData
                        font.pixelSize: 22
                        color: "#FFFFFF"
                        verticalAlignment: Text.AlignVCenter
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.selected(modelData)
                    }
                }
            }
        }

        QText {
            x: 0
            y: 592
            width: 1180
            text: root.loadError.length ? root.loadError : "Tap a zone to select it."
            font.pixelSize: 17
            color: root.loadError.length ? "#FFB300" : "#7A7A7A"
            elide: Text.ElideRight
        }
    }
}
