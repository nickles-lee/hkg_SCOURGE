// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// PROJECTION_PAYLOAD_PHASE1
import QtQuick 2.0
import "common"

/*
 * Shared per-source settings section.  The page instantiates this twice:
 * source "aa" (Android Auto) and source "cp" (CarPlay).  Every setting in a
 * source lives here, with a `<source>_*` status key and config key.  To add a
 * future per-source setting, add ONE row below the distance/units rows here
 * and ONE key pair in ProjectionPage.applyStatus()/configObject(); both
 * sources pick it up without any layout or pane changes.
 *
 * Example for a future row (not shipped):
 *   ProjectionRow {
 *       height: 168; stacked: true; pressable: false
 *       title: "Show speed limit"; subtitle: "On the head-up display."
 *       ProjectionSegmented { labels: ["Off", "On"]; ... }
 *   }
 */
Item {
    id: root
    width: parent ? parent.width : 780
    // Wide panes have room to keep each picker beside its label; the 1280 px
    // pane needs the two-line layout so the helper text is never elided.
    readonly property bool inlinePickers: root.width >= 900
    readonly property int pickerRowHeight: root.inlinePickers ? 108 : 168
    readonly property int pickerButtonWidth: root.inlinePickers ? 128 : 150
    readonly property int pickerWidth: 3 * pickerButtonWidth + 16
    height: 56 + 108 + 2 * pickerRowHeight

    property string source: "aa"
    property bool sourceEnabled: true
    property bool controlsEnabled: true
    property int distanceSteps: 1       // 0 precise, 1 balanced, 2 calm
    property int units: 0               // 0 follow car, 1 metric, 2 imperial

    signal sourceToggled()
    signal distanceChanged(int index)
    signal unitsPicked(int index)

    readonly property string displayName: source === "aa" ? "Android Auto" : "CarPlay"
    readonly property color accent: ConstantModel.GENERAL_COLOR_PROGRESS_BLUE

    Column {
        anchors.fill: parent

        ProjectionRow {
            height: 56
            title: root.displayName
            titleSize: 30
            titleColor: root.accent
            pressable: false
            separator: true
        }

        ProjectionRow {
            height: 108
            title: "Use " + root.displayName
            subtitle: "Takes turns from " + root.displayName + "."
            trailingWidth: 96
            trailingHeight: 54
            rowEnabled: root.controlsEnabled
            onClicked: root.sourceToggled()
            ProjectionSwitch {
                anchors.verticalCenter: parent.verticalCenter
                checked: root.sourceEnabled
                interactive: false
            }
        }

        ProjectionRow {
            height: root.pickerRowHeight
            stacked: !root.inlinePickers
            pressable: false
            trailingWidth: root.pickerWidth
            trailingHeight: 62
            rowEnabled: root.controlsEnabled && root.sourceEnabled
            title: "Distance steps"
            titleSize: 36
            subtitle: "How much the distance changes near a turn."
            subtitleSize: root.inlinePickers ? 21 : 24
            ProjectionSegmented {
                buttonWidth: root.pickerButtonWidth
                buttonLabelSize: root.inlinePickers ? 23 : 25
                labels: ["Precise", "Balanced", "Calm"]
                selected: root.distanceSteps
                controlEnabled: root.controlsEnabled && root.sourceEnabled
                onPicked: root.distanceChanged(index)
            }
        }

        ProjectionRow {
            height: root.pickerRowHeight
            stacked: !root.inlinePickers
            pressable: false
            trailingWidth: root.pickerWidth
            trailingHeight: 62
            rowEnabled: root.controlsEnabled && root.sourceEnabled
            title: "Units"
            titleSize: 36
            subtitle: "Uses the unit you picked in Setup."
            subtitleSize: root.inlinePickers ? 21 : 24
            ProjectionSegmented {
                buttonWidth: root.pickerButtonWidth
                buttonLabelSize: root.inlinePickers ? 23 : 25
                labels: ["Follow car", "Metric", "Imperial"]
                selected: root.units
                controlEnabled: root.controlsEnabled && root.sourceEnabled
                onPicked: root.unitsPicked(index)
            }
        }
    }
}
