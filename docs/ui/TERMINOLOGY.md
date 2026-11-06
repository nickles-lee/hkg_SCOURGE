# Gen5W terminology — colloquial vs. technical

The home screen is easy to misread because several different things are called
an "app". Use these terms in code, issues, and agent prompts.

| Colloquial | Technical | Where |
|---|---|---|
| Home screen | `AppHomeScreen`; Advanced layout = `QListViewHomePage` | `/app/bin/AppHomeScreen` |
| Swipe page | a page in `QListViewHomePage` (`pluginPage`, `listPage1..3`) | `HomeScreen/QHMI_HomeScreenMain_Advanced.qml` |
| App icon / tile | `QMenuItemNoDrag` delegate in `QMenuPageNoDrag`, or a custom `Item` overlaid on the grid (e.g. `HelloTile`, `BatteryTile`) | `component/QMenuPageNoDrag.qml`, `component/*Tile.qml` |
| App | a separate process launched by `AppLauncher` with an app ID (e.g. `AppSetup`, `AppMediaPlayer`) | `/app/bin`, `/rw_data/applaunch-config.json` |
| Front app / foreground app | the app whose Wayland surface the app manager currently shows; tracked in QML by `AppHomeModel.isAtOtherApp` / `currentShowingAppID`, raised via the ivi window manager (`iwm_request_fg_with_app_id`) | `ApplicationManager`, `libivi-window-manager-api.so` |
| Home request | session-bus method call `NotifyRequestHomeScreen(ay)` on `com.lge.hkmc.avn.appfunc.appInterface`, delivered to the focused app | app-manager session bus |
| Star / custom key | the steering-wheel user-configurable button; arrives as signal `custom_key_value(int)` on `com.lge.avn.appmanager.info`, never as a QML key event | app-manager session bus |
| Session bus (app manager) | the D-Bus session daemon whose address `Dbuslaunch.sh` writes to `/tmp/session_appmgr` (env file `/tmp/session_appmgr.env`); carries app-manager and appfunc traffic — distinct from the system bus used for micom/CAN | `app/bin/Dbuslaunch.sh`, `ApplicationLauncher.service` |
| Assistant Window (AW) / split screen | the 640 px split panel loaded by `HomeScreen/main.qml` (`loader_3`), a sibling layer of the Advanced page | `HomeScreen/main.qml` |
| Plugin | a widget plugin: `*.pluginspec` + `lib*.so` + QML under `/app/plugins` | `pluginPage` (`itemWidgetPlaceHolder`) |
| Widget page | the first Advanced page (`pluginPage`), where C++ injects home-screen widgets | `QHMI_HomeScreenMain_Advanced.qml` |
| Card | a large custom QML panel/page, like the Battery page; can be a full-screen overlay or a popup | `HomeScreen/BatteryPage.qml` |
| Status bar / clock bar | the top **90 px** system bar | `HomeScreen/main.qml` |

## Placement rules

- **App icon/tile**: place it in the first empty cell of the target
  `QMenuPageNoDrag` grid. Use the shared `firstEmptySlot(customIndex)`
  allocator so custom tiles (Hello, WiFi, Tweaks, Battery) stack in
  installation order instead of colliding.
- **Page**: the Battery page is a full-screen overlay at `z: 2000`, not a
  `QListViewHomePage` page. Do not add it to `getModelList()`.
- **Overlay**: an overlay owns its visibility via `open()`/`close()` and must
  never be visible while `AppHomeModel.isAtOtherApp` is true. See
  `APP_OVERLAY_CHECKLIST.md` for the required pattern.
- **Plugin**: only if the UI must be injected into `pluginPage` by C++. A
  self-contained overlay does not need a plugin.

## What went wrong once

The first Battery implementation was added as an extra `QListViewHomePage`
page, so it appeared as a "swipe right" screen instead of an app icon. The
correct shape is:

1. `BatteryTile` in the page-2 app grid.
2. `BatteryPage` as a full-screen overlay, opened by the tile.
3. No change to `getModelList()` or the page count.

Two later UX bugs came from foreground/input ownership, not layout:

- **Home/Star did not close the app.** The steering-wheel key is a
  session-bus event (`NotifyRequestHomeScreen` / `custom_key_value`), not a
  QML key event, and `onButtonBackClicked` never fires for it. Fixed by
  having the daemon tail `dbus-monitor` on `/tmp/session_appmgr` and expose a
  `home_seq` counter the overlay closes on.
- **Clicks fell through to the app underneath.** The overlay could be visible
  while another app still owned the Wayland surface, and its input shield did
  not accept press/release/wheel. Fixed by gating visibility on
  `isAtOtherApp`, closing on navigation changes, and a `preventStealing`
  all-buttons shield. See `APP_OVERLAY_CHECKLIST.md`.
