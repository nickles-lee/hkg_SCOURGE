# Gen5W UI layout context

Applies to the Hyundai/Kia Gen5W head unit running the Linux/Qt `AppHomeScreen`
(260128 EU firmware). Read this before adding or moving any UI element.

## Screen geometry

| Item | Value |
|---|---|
| Panel | 1280×720 (narrow) or 1920×720 (wide) |
| Top status bar | **90 px** (profile, home, menu, time, date) |
| Usable full-screen content | **y = 90**, **height = 630** |
| Home page indicator | bottom of the home screen, overlays the content |

`HomeScreen/main.qml` reserves the bar explicitly:

```qml
height: 90                                  // status bar loader
y: 90                                       // content loader
anchors.topMargin: ConstantModel.MENU_BAR_Y // = 90
```

## Rule 1 — never draw under the top 90 px

Any full-screen page, overlay, popup, or card must start at `y: 90` and fit
within `height: 630` (or use `ConstantModel.MENU_BAR_Y` /
`DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y`).

Symptoms when this is ignored: the title is clipped by the clock/home bar, the
first row of cards is partially hidden, or the page looks vertically offset.

```qml
// Full-screen overlay inside AppHomeScreen
Item {
    x: 0
    y: ConstantModel.MENU_BAR_Y          // 90
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y   // 630
}
```

## Rule 2 — foreground ownership and the steering-wheel Home/Star key

**QML never sees the steering-wheel Home/Star key.** There is no `Keys.onPressed`
handler for it and `AppHomeModel.onButtonBackClicked` is only the home screen's
own back affordance. The key travels through the app manager over the
**session bus** (address written by `Dbuslaunch.sh` to `/tmp/session_appmgr`,
`XDG_RUNTIME_DIR=/var/run/user/root`):

| Key | Delivery | Meaning |
|---|---|---|
| Home | method call `NotifyRequestHomeScreen(ay)` on `com.lge.hkmc.avn.appfunc.appInterface`, dest `AppHomeScreen` | go to the home screen |
| Star / custom key | signal `custom_key_value(i)` on `com.lge.avn.appmanager.info` | user-configured action, often "home" |

Both go to the **currently focused app**. If another app owns the foreground,
the home screen never even receives the Home request — which is why an overlay
that "should" close on Home does not react.

`AppHomeModel` exposes the foreground state:

- `isAtOtherApp` — true when another app owns the main surface
- `currentShowingAppID` — the app currently drawing in front
- `currentScreen`, `currentHomeAdvancedPageIdx`, `requestHomeByPP` — navigation

**Consequences for any overlay/page inside `AppHomeScreen`:**

1. Gate visibility on the foreground: `visible: requested && !AppHomeModel.isAtOtherApp`.
   Never show an overlay while another app's Wayland surface is in front —
   its clicks land on that app ("click fall-through").
2. Close the overlay when `isAtOtherApp` goes true, when `currentScreen` or
   `currentHomeAdvancedPageIdx` changes, on `requestHomeByPP`, and on
   `onButtonBackClicked`.
3. To react to Home/Star, tail the session bus outside QML (the Battery
   daemon runs `dbus-monitor --address $(cat /tmp/session_appmgr)` with match
   rules for the two members above and exposes a `home_seq` counter over its
   local HTTP API; the QML closes on any change). A QML-only app cannot
   subscribe to the session bus — `XMLHttpRequest`/`QML_Handler` have no
   D-Bus transport.
4. Do not try to raise/focus the Qt window from QML: surface stacking is owned
   by the app manager / ivi window manager (`libivi-window-manager-api.so`,
   `iwm_request_fg_with_app_id`). An overlay can only close itself.

## Rule 3 — input shield for full-screen overlays

An overlay must own all input. A bare `MouseArea { onClicked: {} }` is not
enough:

```qml
MouseArea {
    anchors.fill: parent
    z: 1
    preventStealing: true          // the flickable below cannot steal the drag
    acceptedButtons: Qt.AllButtons
    onPressed: mouse.accepted = true
    onReleased: mouse.accepted = true
    onClicked: mouse.accepted = true
    onWheel: wheel.accepted = true
}
Item { id: content; z: 2; ... }   // real content above the shield
```

Symptoms when this is ignored: taps on empty areas launch the app behind the
overlay, or a swipe drags the home grid underneath.

**Split screen / Assistant Window (AW) caveat:** the AW is loaded by
`HomeScreen/main.qml` as a sibling layer (`loader_3`), not inside the Advanced
page. An overlay declared inside `QHMI_HomeScreenMain_Advanced.qml` cannot
cover it. On this car `AppHomeModel.screenNotSupportSS = true` disables the
AW, but if you ever need to cover the AW, the overlay must be declared in
`main.qml` at the root level.

## Reference geometry

For the 1180 px content column used by the Battery page:

- Header: y 0–80
- Card row 1: y 96, height 136
- Card row 2: y 244, height 136
- Toggle row: y 392, height 68
- Charge-door row: y 472, height 68
- Warnings: y 548
- Debug/footer: y 610

## One-line prompt for other agents

> **Gen5W UI rules: (1) the top 90 px are the system status bar — full-screen
> QML pages/overlays start at `y: 90` and fit within 630 px; (2) never show an
> overlay while `AppHomeModel.isAtOtherApp` is true, and close it on
> `currentScreen`/`isAtOtherApp`/back changes; (3) the steering-wheel Home/Star
> key is a session-bus event (`NotifyRequestHomeScreen` / `custom_key_value`),
> not a QML key event — tail `dbus-monitor` outside QML; (4) full-screen
> overlays need a `preventStealing` all-buttons MouseArea shield. See
> `/home/nick/ioniq/UI_CONTEXT.md` and `APP_OVERLAY_CHECKLIST.md`.**
