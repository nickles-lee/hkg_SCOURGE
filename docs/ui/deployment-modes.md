# Gen5W deployment modes — every surface a custom UI can live on

Firmware `260128`, theme `qml_eco3.0`. Companion to `styleguide.md` (§9 has the
two proven modes in brief; this is the full catalog with recipes, risks and
evidence). UX behavior rules (driving lockout, edit mode, foreground loss,
trio/daemon) are in `ux-patterns.md`. Raw research: `reports/` (both rounds).

## Verdict table

| # | Mode | Verdict | Reach | Effort |
|---|---|---|---|---|
| A | App: tile + full-screen overlay | **Proven on-car** | home window | small |
| B | Card: persistent swipe page | **Proven on-car** (precedent ran 5 pages) | home window | small |
| C | Official separate-process app | **Blocked** (`.sign` key) | own surface | — |
| D | Widget plugin on the widget page | Loads, but won't surface under a new name | widget page | medium |
| E | System modal popup (NotificationManager) | **Feasible, fully specified** | above ANY app | small-medium |
| F | Assistant Window re-enable | **One-line patch** + one C++ unknown | 640-px right rail | tiny (test needed) |
| G | Own Wayland process (outside AppLauncher) | **YES — high confidence** | own surface + FG | medium + experiment |
| H | Basic-home tile | **Proven** (Hello payload) | basic home | small |
| I | Option-menu entry | Proven model side, likely dispatch | status-bar Menu dropdown | small |
| J | OSD strip | **Feasible globally** | 90-px strip above any app | small |
| K | Standby-clock customization | Feasible (screen files only) | boot/idle screen | medium |
| L | Chill screen (widget page 0) customization | **Freely patchable** | the map/car/charge% screen | small |

Modes A/B/C are documented in `styleguide.md` §9 and `reports/lifecycle.md`
(C's blocker: AppLauncher RSA-verifies every spawned binary's `.sign` section
against `/tmp/app_normal.key`; the private key is not in the firmware). The
rest are below.

**Common deployment mechanics** (all file-patching modes): patch only
`qml_eco3.0`; keep a stock backup with the 260128 hash guard; copy preserving
mode/owner/SMACK labels; remount `/` rw only during install; clear QML caches
(`/root/.cache/AppHomeScreen`, `/root/.cache/qtshadercache`, `/tmp/qmlcache`,
`/tmp/qt-qml-cache`); restart the owning process via the AppLauncher D-Bus
pattern (`terminate <id>` + `launch <id> "<name>" "<path>"` with the bus
address from `/tmp/session_appmgr.env`). Firmware updates revert everything
under `/app` and `/rw_data` payloads — re-check hashes after any update.

---

## Mode D — Widget plugin (a card on the widget page)

**What**: a stock-style widget card (like ClockWidget/WeatherWidget) hosted by
`QWidgetItem` on carousel page 0, with host-owned press/beep/loading.

**Mechanics** (decompiled; citation trail in `reports/widgets.md`):
- `WidgetManager::loadWidget("/app/plugins", …)` scans **subdirectories**; per
  folder it parses the first valid `*.pluginspec` (QDomDocument; root tag must
  be `<plugin>`; keys name/version/compatVersion/vendor/…/mainqml from root
  attributes and child elements, case-insensitive). **No signature check** —
  libraries load via plain `QLibrary` (AppLauncher's `.sign` applies only to
  spawned processes).
- Validity: a spec with `mainqml` and **no `.so` stays valid**. The host even
  has a QML-only fallback (`getComponent()` → `QQmlComponent(engine,
  <folder>/qml/<mainqml>)`) — but no stock plugin uses it, and a QML-only
  plugin receives no `sendEventToWidget` pushes (self-contained only).
- With a `.so`: it must export `extern "C" WidgetInterface* getObject(int
  scrType)` (scrType 0=FRONT, 1=REAR, 2=AW) implementing
  `getComponent/initialize/sendEventToWidget/…` (`plugins/common/WidgetInterface.h`).
- Plugin QML contract: fixed-size roots (Front 627×405, Advanced 624×576, AW
  640-wide), data via the plugin's own C++ models, touch→host via
  `signal sendMouseEventToHostApp(int)` (host connects on Loader success,
  `component/QWidgetItem.qml:52-54`). Host owns whole-card press (release →
  `EVT_MAIN_LIST_ITEMCLICKED` + beep; 650 ms hold → Edit-Widget).

**The blocker**: placement and picker are **name-keyed**. Slot assignment
persists in the `WidgetSettingFront` table of
`/app/data/AppHomeScreen/CustomLayoutDB/CustomLayoutDB` (SQLite; a new widget
gets a slot only by user drag in Edit-Widget), and the picker's
`STR_HOME_*` labels + icon dictionary are hardcoded per known name
(`"could not found widget ID of %1"`, `"Widget is not supported: "` in the
binary). A plugin under a **new name loads but very likely never surfaces**.

**Practical routes**:
1. Ship a real `libMyWidget.so.1.0.0` (copy the ClockWidget pattern:
   `getObject` → singleton implementing `WidgetInterface`; its `getComponent()`
   may point at any QML path you want) **and reuse an existing plugin name**
   (e.g. replace `ClockPlugin`) to inherit its picker entry, STR label and slot.
2. Or drop a QML-only spec under a new name and accept it stays invisible
   unless the picker tables are patched.

**Live-car test**: drop `/app/plugins/MyWidget/` (spec + qml/main.qml, no .so),
restart AppHomeScreen, check `Exist widget:`/`number plug-in was loaded:` in the
journal — loads ≠ appears; expect (a) yes, (b) no.

---

## Mode E — System modal popup (global, above any app)

**What**: a centered card + 70 % dim on the NotificationManager surface — the
only mode that can interrupt *other vendors' apps*. Triggered by any daemon
on the session bus.

**Recipe (Route B — recommended, no binary struct needed)**:
1. Add one entry to `/app/share/SystemPopup/popup/noti_popup.xml`:
   ```xml
   <key keyID="30001"
        keyNotiPopupType="ENUMS::E_Type_B"
        keyTitleStr="My Title"
        keyBodyStr="My message body"
        keyNormalPriority="200000"
        keyDispTimeout="-1"
        keyBtnA="1" keyBtnB="2"/>
   ```
   - **keyID**: pick from the free ranges (existing ids occupy 1001-1019,
     1050, 2001-2005, 2101-2111, 2201, 2301, 2401, 2501-2502, 3000-3011,
     5001-5005, 9999, 10009-10012, 10020, 11010-11260, 12205-12215;
     **free: 4000-4999, 6000-9998, 11230-11244, 11261-12204, 12216-29999, 30000+** —
     30000+ is safest against future firmware).
   - `keyNotiPopupType`: `E_Type_A` (1 button), `B` (2), `C` (3).
   - **Literal `keyTitleStr`/`keyBodyStr` work** — the engine calls
     `QCoreApplication::translate("QMLTranslationModel", …)` which falls back
     to the source string when the key is absent from the .qm files.
   - `keyNormalPriority` 200000 = high (shows above most; **outside-press
     dismisses when shown priority > 89999**; lower values ignore outside press).
   - `keyDispTimeout`: `-1` = stays until a button; `3000` = 3 s auto-close.
   - `keyBtnA/B/C`: style-enum ids resolved by `getBtnString()` → `STR_HOME_OK/
     YES/NO/CANCEL` etc. (1=OK, 2=…; 0 = no button).
   - **No `PopupGenerator.qml` change needed**: unmapped types fall through to
     `typeA.qml` (`PopupGenerator.qml:307-311`); a custom type QML is optional.
   - Dead fields in 260128 (parsed-but-unused or not parsed at all — don't
     bother): `keyStatusErr` (inert), `keyImmediateNoti` (inert),
     `keyLongBodyType`, `keyListPopupType`, `keySymbolID`, `keyUserLockTimeout`,
     `keyMinDispTimeout`, `keyAudioChime` (not parsed at all). Duplicate keyIDs
     tolerated (first wins — stock has a 5001 dupe).
2. Restart NotificationManager (the XML is read **once, in the engine
   constructor**): `terminate 17` + `launch 17 "NotificationManager"
   "/app/bin/NotificationManager"` via the AppLauncher D-Bus pattern.
3. Trigger from the daemon (session bus, service
   `com.lge.hkmc.avn.appfunc.NotificationManager`, path
   `/com/lge/hkmc/avn/appfunc/appInterface`, interface
   `com.lge.hkmc.avn.appfunc.appInterface`):
   ```
   NotifyIndexSystemPopupRequest(i appId, i keyID, i plane, i show)
   ```
   `appId` is self-declared (used for response routing); `plane` = **1**
   (bitmask: 1=first display); `show` = 1.
4. Receive the answer: listen for the signal
   `NotifySystemPopupResponse(i appid, i popupid, i result)` and filter on
   your appId. **Button codes: 1 = A/left/OK, 2 = B/right, 3 = C, 4 = D,
   0 = dismissed** (outside press / timeout — outside-press on OK-type btnA
   responds 1). The method `NotifySystemPopupResponse(i id, i result)` can
   force-close your popup. Queries: `AskPopupShowingStatus()`,
   `AskSystemPopupIDExist(appid, popupid)`, `AskLastSystemPopupIDOfApp(appid)`.

**Risks**: firmware update reverts `/app/share/SystemPopup` (ships in the
rootfs image); `/app` is mounted ro (use the rw-remount install pattern);
popup suppressed by `isPopUpProhibited` states (camera/standby/SOS/valet/TMS);
a popup shown while the front app is changing is stashed and replayed.

**Route A (no XML edit)** exists — `NotifySystemPopupRequest(appId, ay text)`
with a custom id + inline title/body in a binary struct — unknown ids display
with default priority 209999, but you must reverse the `ay` header layout
(12 LE u32 + 4 UTF-8 strings family; partially recovered). Prefer Route B.

---

## Mode F — Assistant Window (the 640-px split rail)

**What**: an **in-process plugin** (`libAssistantWindow.so`, hosted by 6
binaries incl. AppHomeScreen and AppSetup) rendering a 640×720 right panel:
a vertical ring-list (`QHMI_RingList`, a PathView) of full-size plugin cards
(navi/media/radio/BT/call/climate/clock/…), flick-rotated, with its own OSD
layer (SOS/USM native OSDs). The separate `AppAssistantWindow` process
(AppLauncher ID 15) **does not exist in this rootfs** — don't try to launch it.
Disabled on this car by **one hardcoded QML line**, identically in all three
theme variants (`QHMI_HomeScreenMain_Advanced.qml:533` eco3.0/new2.0, `:535`
genesis), plus AppSetup's own `isSupportedAW: false` (AppSetup main.qml:35).
`-sc` is NOT the flag (only AppProjection/AppVH parse it). No C++ gate exists.

**Re-enable recipe**:
1. `QHMI_HomeScreenMain_Advanced.qml:533`:
   `AppHomeModel.screenNotSupportSS = true` → **`= false`**. Set false, don't
   delete — Manual/Information/Submenu_Setup still write `true` on navigation
   ( DataService and Media_Gate popups write `false`).
2. Nothing else in `main.qml` — the split button appears automatically
   (`visible: isWideScr && !screenNotSupportSS && …`, main.qml:237-247),
   `loader_3` unparks to x=1280, and the AW plugin is already instantiated
   (sourceComponent is unconditional).
3. Optional: AppSetup `main.qml:35` `isSupportedAW: false → true` for the AW
   inside Setup.
4. **The one unknown**: `loader_3.visible` needs C++ to set
   `AppHomeModel.visibleSS` (persisted `ssOnOff` restored by
   `SplitScreenWorker::applyInfo`, or the split-button handler). If the AW
   doesn't slide in on the first split press, that's the C++ gate to trace.
5. Pre-patch probe: EV screens already expose an option-menu "Split Screen"
   row (`Screen_EV_Main.qml:780`) — pressing it today toggles `isFullMode`
   while `loader_3` stays parked.

**Requires the wide panel** (x=1280..1920; on 1280 the AW is fully off-screen).
This car is very likely 1920×720 (live weston config is `weston_1920.ini`;
confirm via the `LCD TYPE:` log or the split button appearing after step 1).

**On-car test checklist**: ① pre-patch EV-screen Split row → ② patch :533,
restart AppHomeScreen → ③ split button visible? → ④ press: AW slides in
(200 ms), cards populate, flick rotates ring? → ⑤ navi card buttons work? →
⑥ popup/dim widths narrow to 1280 while SS shows; SystemPopup resizes →
⑦ option menu narrows (`_isSSShowing`) → ⑧ reboot: SS state persists? →
⑨ journal: `SplitScreenWorker` logs. Note: eco3.0 Statusbar has **no zoom
button** (new2.0-only) — the split handle + option-menu row are the only
toggles. RHD mirrors the rail to the left (x=−640).

---

## Mode G — Own Wayland process (outside AppLauncher)

**What**: a root Qt/QML process spawned by our own systemd service — own
surface, real foreground, survives AppHomeScreen crashes, no `.sign` check
(it only applies to AppLauncher-spawned processes).

**Why it works** (decompiled from `ivi-window-manager.so` — see
`reports/wayland-process.md`):
- Weston 1.10 ivi-shell config (`weston_1920.ini`): `shell=ivi-shell.so`,
  `ivi-module=ivi-window-manager.so,ivi-controller.so`, application layer,
  `transition-duration=300`. No surface-id requirement in config.
- The wm's ID pools: type 0 → 1-999, type 1 (Qt) → **1000-3999**, type 2 →
  4000-5999; a static 30-entry `AppSurfaceInfoTable` (NotificationManager 1030,
  AppCamera 2080, … 3000/3001 AppNavi; playerengine 4000s; pool-3 hidden
  4900-4909/9999). **Qt apps without an env ID get a dynamic "QTAPP" id**
  assigned from the free pool (`request_id("QTAPP")`).
- **Admission has no allowlist**: `app_surface_created` → every unknown surface
  goes to the wm's application layer 2. Identity is the **`appId` window
  property** (or the binary basename when `QT_WAYLAND_SHELL_INTEGRATION=ivi`).
- **FG is self-grantable**: call `iwm_request_fg(1, our_sid)` /
  `iwm_request_fg_with_app_id(1, our_appId)` directly via
  `libivi-window-manager-api.so.1` — the wm's "normal case" path does no
  app-registry check (only camera surfaces are special-cased). Main display
  plane idx = **1**. ApplicationManager's `appbase` D-Bus is NOT usable
  (it resolves senders to registered apps — we bypass it entirely).

**Minimal recipe**:
```ini
# /etc/systemd/system/myapp.service (root, battery-daemon precedent)
Environment=XDG_RUNTIME_DIR=/var/run/user/root
Environment=QT_QPA_PLATFORM=wayland
Environment=LD_LIBRARY_PATH=/app/lib
Environment=LD_PRELOAD=/usr/lib/libEGL.so.1
Environment=QT_WAYLAND_DISABLE_WINDOWDECORATION=1
Environment=QT_WAYLAND_SHELL_INTEGRATION=ivi    # appId = binary basename
# optional: Environment=QT_WAYLAND_SET_SURFACE_ID=3500   (1000-3999, unassigned)
ExecStart=/usr/bin/MyApp
```
Custom Qt app: `QQuickView` + `view->setProperty("appId", "MyCustomApp")` +
`iwm_init(0); iwm_request_fg(1, sid);`. `QT_WAYLAND_SET_SURFACE_ID=N` uses
N + a per-process counter (2nd window gets N+1); 0/unset = dynamic assignment.
`DBUS_SESSION_BUS_ADDRESS` (from `/tmp/session_appmgr.env`) only needed for
D-Bus, not Wayland. Qt on target = **5.4.1**.

**⚠ CRITICAL SAFETY**: weston death = **MICOM watchdog reset of the whole head
unit** (`weston-crash.path` → `/etc/weston-crash.sh` → `SetETCCPUWDTReset`).
Never crash weston; a Wayland protocol error kills only your client. A wedged
wm state recovers only by full reboot.

**Cheapest safe experiment (parked car)**:
1. Observe: `/tmp/WindowManager.log` (the wm's log), `ps -ef | grep weston`,
   `/proc/$(pidof weston)/cmdline`, `ls /var/run/user/root`.
2. **Phase 1 — surface only**: run the shipped zero-compile probe
   `/usr/bin/Qt5_CinematicExperience` (a `QQuickView` loading
   `/Qt5_CinematicExperience.qml`) with the env above, **no FG request**.
   Success = `wm_window_created`/`app_surface_created → add_surface_of_window_
   to_layer - DID, L(2)` in the log, screen unchanged (surface under home).
   (`weston-simple-egl` also works — default ivi id 10000, outside all pools.)
3. **Phase 2 — FG**: tiny C helper: `iwm_init(0); iwm_request_fg(1, <sid from
   log>);`. Expect `wm_request_fg - normal case`, home demoted to BG, demo
   full-screen (the wm may resize it).
4. **Recovery**: steering-wheel Home (GoHome requests FG for appId 11) or the
   proven `terminate(11)/launch(11)` restart.

**Residual unknowns**: ApplicationManager's reaction to `notifyFGInfo` for an
unknown app (its state machine may fight back on the next transaction);
whether weston restarts are clean (they're not — watchdog).

---

## Mode H — Basic-home tile

**What**: a tile on the Basic home (`QHMI_HomeScreenMain_Wide.qml` — despite
the name it is the *Basic* home for **both** panel widths; every metric is an
`isWideScr` ternary). Proven by the Hello payload on-car.

**Anatomy**: one strip `itemMainMenu` (width `HOME_1920_MENU_W`/`HOME_1280_MENU_W`,
height 183, y `HOME_1920_MENU_Y`), an All-Menu `QHomeItem` (250 wide / 207
narrow), and a one-row GridView (`model: AppHomeModel.listMenuHomeItem` — a
C++ Q_PROPERTY; delegates are `QHomeItem`s reparented into the grid). Screen
selection is C++-only: `EVT_GOTO_MAIN_HOME_SCREEN` → `Wide.qml`,
`EVT_GOTO_SCREEN_WIDE_ADVANCED` → `Advanced.qml`. User switches modes in
Edit-Home-Layout (`AppHomeModel.isAdvancedMode`).

**Recipe** (Hello precedent, `hello_payload/payload/.../Wide.qml`):
1. On wide, free a cell: shrink `cellWidth`/`itemAllMenu.width` 250→200 and
   `x_icon` 76/78→50 (fits a 6th cell in the 1266-px strip). On narrow there
   is no free cell — gate `visible: AppHomeModel.isWideScr` or place the tile
   outside the strip.
2. Add a `QHomeItem`/custom tile as a **sibling after the GridView, inside
   `itemMainMenu`**, `x: itemAllMenu.right + cellWidth * N` (the grid is
   anchored left of `itemAllMenu.right`, so this lands in the row).
3. Click handling: plain MouseArea (precedent) — but add the beep
   (`EVT_HOME_SOUND_BEEP_PLAY`; Hello omitted it, every stock tile beeps).
   The tile can open the same overlay pattern (the overlay component file is
   instantiated **separately in each home file** at root level, `z: 2000`),
   or send events like its neighbors
   (`qmlSendEvtKey(EVT_MAIN_LIST_ITEMCLICKED, itemID)` /
   `qmlChangeTopMsg(EVT_GOTO_*)`).
4. Add the Advanced-home tile too (as Hello did), or the app vanishes when
   the user switches home type.

**Risks**: the tile is a sibling overlay, not a grid model entry — it can't
participate in rearrange; cell-shrink alters stock icon centering; mode
switches and Edit screens destroy the home (see `ux-patterns.md` §2).

---

## Mode I — Option-menu entry (status-bar Menu dropdown)

**What**: a custom row in the "Menu" dropdown — a tile-less launcher for
low-frequency tools. **Proven model-side** (rebinding `listOptMenu` is itself
the stock pattern); dispatch is a one-file patch.

**Mechanics**: `AppHomeModel.listOptMenu` is a **writable** context property;
every screen rebinds it in `Component.onCompleted`
(`AppHomeModel.listOptMenu = Qt.binding(function(){return …})`). The menu
(`popup/QOptionMenu.qml`, loaded by C++ on `EVT_OPEN_OPION_MENU`, OSD_ID 13,
z 4 in the home window) rebuilds its ListModel through **`creatModel()`**
(`QOptionMenu.qml:1357-1372`) on every change — the single funnel for initial
build and per-screen rebuilds. Entry shape:
`{indexOptItem, label, isItemSelect, isEnabled}` (+ `keyEvent`/`dataEvent` for
data-driven dispatch, per the NMode timer rows). Panel: 555/501 wide at y 90,
rows `QItemMenu` 108-112 px, 10 ms slide, 100 ms fade, **10 s auto-close**,
outside-click close.

**Recipe**:
1. Patch `creatModel()` to append custom rows **after** the stock copy and
   after its narrow-Split removal (its removal indexes only stock rows).
   `indexOptItem` is a plain int — use a literal (e.g. 900) or an unused
   registered enum value (the `E_OPT_*` enum lives in C++; prefer literals).
2. Patch the home-screen dispatch: `QOptionMenu.qml`'s release handler is
   `switch(indexOptItem)` → `switch(AppHomeModel.currentScreen)` → per-screen
   switch. The home branch (`EVT_GOTO_MAIN_HOME_SCREEN` +
   `EVT_GOTO_SCREEN_WIDE_ADVANCED` share one branch, `:192-239`) has
   **default = beep only** — add a `case <id>:` that sends any EVT
   (`qmlSendEvtKey`), changes screens (`qmlChangeTopMsg`), or sets a model
   property. One case block covers both home types.
3. A row **cannot host the overlay itself** (the menu is destroyed on close) —
   route the click to an overlay declared in the home file via a watched
   property.
4. Note: `QOptionMenuWidgets.qml` is the **AW widget picker**, not a widget-page
   menu variant — don't patch it for this.

**Caveats**: the Menu button auto-hides when the list is empty; per-screen
menus differ (Advanced: Display Off/Edit Menus/Manual; Basic adds Edit
Widgets; submenus add Search/Rearrange/Subscription/Split) — the `creatModel`
funnel covers them all.

---

## Mode J — OSD strip (global toast)

**What**: a transient 90-px strip at the top (over the status bar) or the
75-px volume strip at the bottom — **above any app**, auto-timed,
touch-dismissible. AppOSDManager is an **unauthenticated D-Bus server** on
the session bus: `com.lge.hkmc.avn.appfunc.AppOSDManager`, path
`/com/lge/hkmc/avn/appfunc/appInterface`.

**Recipe**:
1. Add an entry to `/app/share/AppOSDManager/systemOSD/systemOSD.xml`
   (runtime-parsed with QXmlStreamReader, **once at process start**, lookup by
   `keyIndex`, no signature):
   ```xml
   <key keySpec="1" keyIndex="900" keyType="3" keyShow="1" keyEvt="1"
        keyInfo="0" keyMode="251" keyIcon1="0" keyIcon2="0" keyIcon3="0"
        keyTimeOut="50" keyID="0"
        keyStringA="" keyStringB="My toast text" keyStringC="" keyStringD=""/>
   ```
   - `keyIndex` = the request lookup id (existing active ids: 10-54, 70-71,
     100-101, 120, 140, 200, 300-350 — pick fresh, e.g. 900+).
   - `keyType` 3 + `keyEvt` 1 route to **`osdGeneral.qml`** — a plain text
     strip, **no QML patch needed**. `keyStringB` goes through
     `translate("QMLTranslationModel", …)` → plain text renders as-is.
   - `keyTimeOut` is in **100 ms units** (30 = 3 s, 50 = 5 s).
   - `keyInfo` → icon via `OSDIconFile()` (0 = none); priority-1 turns the
     strip red (`co_bg_osd_title_red`).
2. Restart AppOSDManager (`terminate 22` + `launch 22 "AppOSDManager"
   "/app/bin/AppOSDManager"`).
3. Trigger from the daemon:
   `NotifySystemOSDRequest(i appId, i index, i plane, i show [, i priority])`
   — plane 1. Hide early: `NotifyOSDRequestHide(ay)`.
4. Optional custom layout: drop `osdMyToast.qml` in `qml_eco3.0/` and add one
   `case` to `mainOSD.qml`'s inner `switch(AppOSDEngine.getEvent())`
   (mirrored in `onOsdUpdate`), then set `keyEvt` to that value.

**Caveats**: suppressed while display-off/camera-active/SOS-active/valet
(`ProcessOSD` block rules); preempted by higher-priority OSDs; the home
window learns via the `NotifyOsdStatus` broadcast (sets `isOnScrShowing` →
bar dim). The home-window `onsDataList` slots are **full-screen "onscreens"**
(DriverTalk/QuietMode/Climate), not strips — all real strips live in the
OSDManager process. Fallbacks: home-only notices → own overlay (Mode A
machinery); richer card with buttons → SystemPopup type-D (Mode E).

---

## Mode K — Standby-clock customization

**What**: AppStandbyClock (app ID 10, preload group 1) owns **four surfaces**:
the **boot clock** (digital/analog/pixel variants + logo, selected C++-side
via `EVT_GOTO_SCREEN_{DIGITAL|ANALOG|DIGITAL_PIXEL|LOGO|SOS|VALET_MODE}` into
the same `screenNext` double-buffered loader), the **display-off black screen**
(`onscreen/QHMI_AppStandbyClock_BlankScreen.qml` — full-screen black +
touch→`KEY_EVT_STANDBYSCREEN_TOUCHED` wake; the clock is NOT shown on
display-off), the **navi-loading spinner**, and the **firstSetup profile
flow** (+ ecall SOS, valet, boot agreement).

**Data**: time is free (`AppStandbyClockModel.time` = sec/min/hour/day/…
list, `Digital.qml:50-105`). **No EV/vehicle data in-process** (full model
dump = time/format/language/variant/ign/acc/display flags only) — charge %
must come from the battery daemon (`http://127.0.0.1:8088/status` or the
1-second `/rw_data/battery/status.json` mirror).

**Recipe**: patch **screen files only** (e.g.
`Screen_AppStandbyClock_Digital.qml` for a custom parked/charging display;
`BlankScreen.qml` if you want charge info on the display-off surface —
**keep the touch-wake MouseArea**). Theme colors via `ThemeHelperSTAN`
(the same `ThemeHelper` class as `HomeThemeHelper`).

**⚠ Scope rule — never patch `main.qml`**: a broken screen file → that screen
stays black but the process survives (boot continues; AppLauncher's
`appLoadTimeout` synthesizes `preloadcomplete`). A broken `main.qml` → **no
boot clock, no display-off wake screen, no ecall SOS UI, no firstSetup**, and
no auto-restart exists — recoverable only via the USB failsafe.

---

## Mode L — Chill screen (widget page 0: map + car + charge %)

**What**: `Screen_HomeScreen_Widget_Advanced.qml` — the first Advanced-home
page. C++ loads this file **by path from disk** (`slotReqInjectAdvancedWidget`
→ `loadWidgetAdvancedItem` → `QQmlComponent(engine, "HomeScreen/
Screen_HomeScreen_Widget_Advanced.qml")` → parented into the carousel's
`itemWidgetPlaceHolder`, exposed back as context property
`AdvancedWidgetQMLInstance`). **Patch it freely** — worst case is a blank
page 0; the app grid pages are unaffected.

**Layout** (wide 1920; root is `Item` 1920/1280×720):

| Region | Geometry | Notes |
|---|---|---|
| no-map bg | full width, `home_nomap_bg` | shown when navi map hidden |
| navi map | `content` anchored right, **836×720** | Loader `naviWidget[0].sourceInput` (live AppNavi map, surface 4903); click → beep + `AppHomeEngine.requestMAPByTouchWidget()` |
| weather | **1920×163** top-right | Loader `AppHomeModel.weatherWidgetDL3` |
| clock | 120 px time at left 75 / vcenter offset 399; AM/PM 45 px; date 34 px | MouseArea → beep + `request_clock()` |
| EV widget | `evLoader` at **left 520 / top 290** | in-tree `QButtonBatteryHomeItemEco3`; release → `requestShowEVApp()`; EV + wide only |
| AV area | x=0, **617 (EV wide) / 970 (wide) / 620 (narrow)** ×720 | Loader `AppHomeModel.avWidgetHome` (MediaWidget); bottom-250-px click strip |
| factory OSD | 550×216 centered top | `isFactoryMode` only |

**Data for free** (models registered in this process): `AppPHEVModel.
display_Batlevel` (SOC %), `display_DTE` (range), `batCapacity` (0-20 glyph),
`batChgSts`/`fastChgSts`/`stdChgSts` (charging state), `heatBatteryPercent`,
`lowBatteryWarnType`, plus `AppHomeModel` navi/weather/date/time state.

**Recipes (ranked, QML-only)**:
1. **Patch `component/QButtonBatteryHomeItemEco3.qml`** — smallest,
   self-contained, all EV data in scope (add charging state / heater rows);
   keep the `QButton` base + beep conventions.
2. **Add an info module** to the screen — free space on wide: x 520-1084 ×
   y 470-720 (between EV widget / AV edge / map) and the band y 163-290.
   Input: default MouseAreas let the carousel steal drags (stock behavior —
   swiping still pages); for internal drag controls set `preventStealing:
   true` or emit `AdvancedWidgetQMLInstance.setButtonWidgetActive(true/false)`
   on press/release (locks the carousel, `Advanced.qml:180-187,340`).
3. **Restyle the clock block** — trivial; keep the `request_clock()` wiring.

**Breakage cases**: navi not booted → map hidden, 117×117 spinner (right
margin 359) — no "not support" text on *this* screen; call OSD → no
`pluginOpacity` here (that's the basic-mode widget page); day/night → use
`Q_ThemeImage`/`QText`/`HomeThemeHelper` (stock hardcodes: AM/PM `#6a6a6a`,
battery text `#ffffff`/`#b6c1c8`/`#ff6600`).
