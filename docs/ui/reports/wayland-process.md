# Own Wayland process outside AppLauncher — deep-dive (round 2)

Key correction to round 1: **`ivi-window-manager.so` IS in the Ghidra export
set** (`bin_e1ce4d479f1fb84c`), so the admission logic is decompiled, not
inferred. Also used: `bin_249cb3e041a8d643` (libivi-platform-interface),
`bin_0f2ba91c646ad99f` (libQt5WaylandClient 5.4.1),
`bin_081a809a3a669303` (libivi-window-manager-api), AppLauncher
(`bin_c8d6d56bc254ad07`), ApplicationManager (`bin_5d71539f61a3982b`), rootfs.

## Weston config

`rootfs/etc/xdg/weston/weston_1920.ini` (weston_1280.ini identical except
output modes): `[core] shell=ivi-shell.so backend=ias-backend.so`;
`[ivi-shell] ivi-module=ivi-window-manager.so,ivi-controller.so`;
`base-layer-id=1000 workspace-background-layer-id=2000 workspace-layer-id=3000
application-layer-id=4000 transition-duration=300`; background-id=1001 …
home-id=1007. **No input section, no surface-id requirements, no
default-surface config** — the wm module owns everything. Weston **1.10.0**;
config via `-c`/`WESTON_CONFIG_FILE`. No systemd unit starts weston in the
extract (verify on car via `/proc/$(pidof weston)/cmdline`).
**Crash safety: `weston-crash.path` watches `/tmp/weston.crash` →
`weston-crash.service` → `/etc/weston-crash.sh:15` sends
`com.lge.car.micom.ihu.SetETCCPUWDTReset` — weston death = MICOM watchdog
reset of the whole head unit.**

## Surface IDs & identity

- wm pools (SurfaceIdManager, functions_0001.c:6309-6311): type 0 → 1-999,
  **type 1 (Qt) → 1000-3999**, type 2 (gstreamer) → 4000-5999; type 3 =
  hidden-at-create.
- Static `AppSurfaceInfoTable` @0x21dec0 (30 entries): pool 1: 1030
  NotificationManager (reserved first), 2080 AppCamera, 2110 AppProjection,
  2160 AppMirrorLink, 2170 AppCarlife, 2200/2201 AppMobileTV, 3000/3001
  AppNavi; pool 2: playerengine 4000/4001/4010/4020/4040/4041/4050/4100/4101,
  AppEarlyCamera 5000; pool 3 (hidden): AppNavi 4900-4909, playerengine 9999.
- AppLauncher env table (FUN_00114e50): 0x11→1030, 0x0c→2080, 0x6e/0x6f→2110,
  0x70→2160, 0x71→2170, 0x49→2200, 0x1e→3000, **everything else → 0**;
  also sets `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`.
- `QT_WAYLAND_SET_SURFACE_ID` semantics (libivi-platform-interface
  functions_0001.c:269-330): `_FORCED` → raw; `QT_WAYLAND_USE_PID_…` → PID;
  `=N` → **N + per-process counter** (2nd window = N+1); 0/unset → "no id".
- **No env id → the Qt platform calls `request_id("QTAPP")`** — the wm assigns
  the next free pool-1 id (1000-3999). So AppHomeScreen/AppSetup ("default 0"
  in AppLauncher's table) get **dynamic QTAPP ids at spawn**.
- Identity = the **`appId` window property** → `ivi_hmi_client::set_app_id`
  (functions_0001.c:1389-1412, watcher :2835-2845; `displayPlane` →
  set_display_plane). With `QT_WAYLAND_SHELL_INTEGRATION=ivi` set, Qt passes
  the **binary base name** as that string (libQt5WaylandClient
  functions_0001.c:22535-22562). The ivi shell-integration plugin
  (`/usr/lib/qt5/plugins/wayland-shell-integration/libivi.so`) is
  auto-selected because weston advertises `ivi_application`.
- Safe unassigned ids: 3500-3999, or unset and take the dynamic assignment.
  wm's own surfaces: id 1 (black, layer 10), 100 (layer 2) — avoid.

## Admission & foreground

- `app_surface_created` (wm functions_0001.c:4409-4464): checks special types
  (keyboard/background/statusbar/popup/OSD), `is_hidden_surface` (static
  type-3 only), **else every unknown surface → `add_surface_of_window_to_
  layer(DID, 2, window)` — application layer. No pre-registration.**
- wm layers: L2 application (visible), L4 statusbar-OSD, L10 notification/
  popup, L11 volume-OSD (bottom 75 px: y=H−0x4b), L12 AV-OSD, L13 animation
  (hidden), L7 ameba (hidden). wm logs to **`/tmp/WindowManager.log`**.
- FG protocol `/usr/share/ivi-window-manager/ivi-window-manager.xml`:
  `request_fg(display_plane_idx, surface_id)`,
  `request_fg_with_app_id(display_plane_idx, app_id: string)`,
  `set_surface_visible`, `set_visible_for_qtwayland`,
  `set_app_name_with_app_type`, `show_app_with_app_type`,
  `create_hmi_client(display, surface_id, app_id)` — **any connected client
  can bind it; no auth**. wm handler (functions_0001.c:11129-11171): only
  camera (SID 5000) and `ameba_clear` special-cased; "normal case" →
  set_FG_info, no allowlist; fails only if the window isn't found.
  Main display plane idx = **1**, rear = 4.
- ApplicationManager's appbase D-Bus (`show_window_fg`, `hide_window_bg`)
  resolves the caller by D-Bus sender → registered app — **not usable for
  us; unnecessary anyway**: call `iwm_request_fg(1, sid)` /
  `iwm_request_fg_with_app_id(1, appId)` directly via
  `libivi-window-manager-api.so.1` (exports bin_081a809a3a669303:40-67).

## Minimal recipe

Qt on target = **5.4.1** (libQt5WaylandClient.so.5.4.1; platform plugins
libqwayland-{generic,egl}.so). **No qmlscene**, but the unit ships a
zero-compile QQuickView probe: `/usr/bin/Qt5_CinematicExperience` (4-line
script → `/usr/share/cinematicexperience-1.0/Qt5_CinematicExperience`,
QQuickView loading `/Qt5_CinematicExperience.qml`). Also
`weston-simple-egl` (reads `SIMPLE_EGL_SURFACE_ID`, **default ivi id
10000** — outside all pools), weston-image/terminal/simple-ivi-share.

Env: XDG_RUNTIME_DIR=/var/run/user/root, QT_QPA_PLATFORM=wayland,
LD_LIBRARY_PATH=/app/lib, LD_PRELOAD=/usr/lib/libEGL.so.1,
QT_WAYLAND_DISABLE_WINDOWDECORATION=1,
QT_WAYLAND_SHELL_INTEGRATION=ivi (appId = binary basename; or set the
`appId` window property in code), optional QT_WAYLAND_SET_SURFACE_ID=3500.
DBUS_SESSION_BUS_ADDRESS (from /tmp/session_appmgr.env) only needed for D-Bus.

## Verdict + experiment

**YES, high confidence.** No .sign check, no registration, no allowlist;
dynamic QTAPP ids on request; FG self-grantable via the wm protocol. Residual
unknowns: ApplicationManager's reaction to `notifyFGInfo` for an unknown app
(its state machine may fight back on the next transaction); weston restarts
are watchdog resets.

Experiment (parked car): ① read /tmp/WindowManager.log, weston cmdline,
runtime dir ② Phase 1: run CinematicExperience with the env, no FG request —
success = `ivi_hmi_request_id - PID, Name(QTAPP)` + `New ID: %d` +
`wm_window_created` + `app_surface_created → L(2)` in the log, screen
unchanged ③ Phase 2: tiny C helper `iwm_init(0); iwm_request_fg(1, <sid>);`
— expect `wm_request_fg - normal case`, home demoted to BG, demo visible
(wm may resize it) ④ Recovery: steering-wheel Home (GoHome → FG for appId 11)
or the terminate(11)/launch(11) restart. Failure: no wm_window_created =
socket/EGL issue; `reserve_id` rejected → `receive_id 0` → Qt aborts (pick
another id or unset env); a Wayland protocol error kills only our client.
**Do NOT crash weston (= whole-IVI MICOM reset); avoid garbage ids in
set_parent_child_surface; duplicate live surface ids — prefer dynamic
assignment; a wedged wm state recovers only by full reboot.** NHM
(max_failed_apps=8) only tracks registered apps — we're not one.
