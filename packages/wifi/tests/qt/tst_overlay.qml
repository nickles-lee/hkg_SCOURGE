// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import QtTest 1.0
import "HomeScreen"

Item {
    id: scene
    width: 1920
    height: 720
    property int underlayClicks: 0
    MouseArea { anchors.fill: parent; onClicked: scene.underlayClicks++ }
    Item {
        anchors.fill: parent
        enabled: !AppHomeModel.sysPopupShowing
        WifiPage {
            id: page
            y: 90
            width: scene.width
            height: 630
            z: 2000
            // Do not access host files or sockets. Keep the real overlay,
            // visibility, close control and all its MouseAreas unchanged.
            function refresh() {}
            function refreshHome() {}
            function refreshConsole() {}
        }
    }
    TestCase {
        name: "WifiOverlayInput"
        when: windowShown
        function init() {
            AppHomeModel.sysPopupShowing = false
            AppHomeModel.isAtOtherApp = false
            page.close()
            scene.underlayClicks = 0
        }
        function test_exit_data() {
            return [{tag: "1280", width: 1280}, {tag: "1920", width: 1920}]
        }
        function test_exit(data) {
            scene.width = data.width
            page.open()
            verify(page.visible)
            var button = findChild(page, "wifiExit")
            verify(button !== null)
            mouseClick(button, button.width / 2, button.height / 2)
            compare(page.requested, false)
            compare(scene.underlayClicks, 0)
        }
        function test_shield_data() { return test_exit_data() }
        function test_shield(data) {
            scene.width = data.width
            page.open()
            mouseClick(page, page.width - 4, page.height - 4)
            compare(scene.underlayClicks, 0)
            verify(page.requested)
        }
        function test_legacy_visible_assignment() {
            // Tweaks/Battery host variants historically did
            // `wifiPage.visible = true`; a direct assignment must not make
            // Exit ineffective by destroying the old visibility binding.
            page.visible = true
            verify(page.visible)
            verify(page.requested)
            page.close()
            compare(page.visible, false)
            compare(page.requested, false)
            page.visible = true
            verify(page.requested)
            page.close()
            compare(page.visible, false)
        }
        function test_exit_after_legacy_host() {
            page.visible = true
            verify(page.requested)
            var button = findChild(page, "wifiExit")
            verify(button !== null)
            mouseClick(button, button.width / 2, button.height / 2)
            compare(page.visible, false)
            compare(page.requested, false)
        }
        function test_direct_visible_false() {
            page.open()
            verify(page.visible)
            page.visible = false
            compare(page.requested, false)
            page.open()
            verify(page.visible)
            page.close()
        }
        function test_popup_and_foreground_loss() {
            page.open()
            AppHomeModel.sysPopupShowing = true
            compare(page.requested, false)
            AppHomeModel.sysPopupShowing = false
            page.open()
            AppHomeModel.isAtOtherApp = true
            compare(page.requested, false)
            AppHomeModel.isAtOtherApp = false
            compare(page.requested, false)
        }
    }
}
