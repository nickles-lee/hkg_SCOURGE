// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0

/* Three-way picker used for distance steps and units.  It is a local copy of
   the Wi-Fi mode-button pattern: equal-width themed buttons, one selected. */
Item {
    id: root
    width: childrenRect.width
    height: childrenRect.height

    property var labels: []
    property int selected: 0
    property bool controlEnabled: true
    property int buttonWidth: 150
    property int buttonHeight: 62
    property int buttonLabelSize: 25
    signal picked(int index)

    Row {
        spacing: 8

        Repeater {
            model: root.labels
            delegate: ProjectionButton {
                width: root.buttonWidth
                height: root.buttonHeight
                label: modelData
                labelSize: root.buttonLabelSize
                selected: root.selected === index
                buttonEnabled: root.controlEnabled
                onClicked: root.picked(index)
            }
        }
    }
}
