# Gen5W full-screen overlay checklist (for app agents)

Copy this pattern for any full-screen page/overlay declared inside
`AppHomeScreen` (e.g. `QHMI_HomeScreenMain_Advanced.qml`). It fixes two
real-world bugs seen on the car:

1. **The steering-wheel Home/Star key did not close the app.**
2. **Clicks "fell through" to the app underneath.**

Both come from foreground/input ownership, not layout. Geometry rules (the
90 px status bar) are in `UI_CONTEXT.md`.

## Why

- Home/Star is **not** a QML key event. It is delivered by the app manager
  over the session bus (`/tmp/session_appmgr`):
  - Home: method call `NotifyRequestHomeScreen` on
    `com.lge.hkmc.avn.appfunc.appInterface`
  - Star/custom key: signal `custom_key_value(int)` on
    `com.lge.avn.appmanager.info`
  QML cannot subscribe to D-Bus, so a helper outside QML must tail
  `dbus-monitor` and expose a counter (the Battery daemon exposes `home_seq`
  at `http://127.0.0.1:8088/status`).
- The app manager owns surface stacking. An overlay can be *painted* while
  another app's Wayland surface still owns the foreground — then the overlay
  looks right but clicks land on the other app. QML cannot raise the window;
  the overlay must close itself instead.

## The pattern

```qml
// Inside QHMI_HomeScreenMain_Advanced.qml (or the page that owns the grid):

MyPage {
    id: myOverlay
    x: 0
    y: ConstantModel.MENU_BAR_Y                       // 90 px status bar
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y   // 630
    z: 2000
    onGoHome: close()
}

// Tile opens it with:  onClicked: myOverlay.open()
```

```qml
// Inside the overlay page itself:

Item {
    id: root

    // -- the overlay owns its visibility, nobody sets .visible directly --
    property bool requested: false
    property bool atOtherApp: typeof AppHomeModel !== "undefined"
                               ? AppHomeModel.isAtOtherApp : false
    visible: requested && !atOtherApp
    focus: visible

    function open()  { requested = true;  refresh() }
    function close() { requested = false }

    // -- close on every navigation/foreground change --
    Connections {
        target: (typeof AppHomeModel !== "undefined") ? AppHomeModel : null
        onButtonBackClicked: root.close()
        onIsAtOtherAppChanged: if (AppHomeModel.isAtOtherApp) root.close()
        onCurrentScreenChanged: root.close()
        onCurrentHomeAdvancedPageIdxChanged: root.close()
        onRequestHomeByPPChanged: if (AppHomeModel.requestHomeByPP) root.close()
    }

    // -- close on the steering-wheel Home/Star key (via helper daemon) --
    property int homeSeq: 0            // updated from the daemon's /status
    onHomeSeqChanged: if (homeSeq > 0) close()

    // -- full-screen input shield, content above it --
    Rectangle { anchors.fill: parent; color: "#0E0E0E" }   // background
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
    Item { id: content; z: 2; /* real controls here */ }
}
```

## Checklist

- [ ] Overlay starts at `y: ConstantModel.MENU_BAR_Y`, height
      `DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y` (never under the 90 px bar).
- [ ] `visible` is bound to `requested && !AppHomeModel.isAtOtherApp`; no
      external code assigns `.visible`.
- [ ] Closes on: back button, `isAtOtherApp` → true, `currentScreen` change,
      `currentHomeAdvancedPageIdx` change, `requestHomeByPP`.
- [ ] Home/Star closes it via a session-bus watcher outside QML
      (`home_seq`-style counter; see the Battery daemon's
      `monitor_start`/`monitor_tick` in `battery_daemon.c`).
- [ ] Full-screen shield MouseArea with `preventStealing: true`,
      `acceptedButtons: Qt.AllButtons`, accepted press/release/wheel; all
      content at `z: 2` above the shield.
- [ ] If the split-screen / Assistant Window must be covered: the overlay must
      be declared in `HomeScreen/main.qml` at the root, not inside the
      Advanced page (the AW is a sibling `loader_3` layer).

## Quick test on the car

1. Open the overlay, press steering-wheel Home/Star → closes within ~0.5 s.
2. Return from another app, immediately open the overlay, tap empty/edge
   areas → nothing launches behind.
3. Swipe/drag on the overlay → the home grid underneath does not move.
