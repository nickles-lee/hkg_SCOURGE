# Assistant Window — deep-dive (round 2)

## What it is

**In-process plugin, not a process.** `libAssistantWindow.so.1.0.0`
(`/app/share/AssistantWindow/lib/`), loaded via `QLibrary` by AppHomeScreen
(functions_0001.c:9989) and exposed as context component **`AWComponent`**
(strings.tsv:1780). Hosted by 6 binaries: AppHomeScreen, AppSetup, AppNavi,
TmsGuiApp, AppCamera, AppEngineerMode. The separate `AppAssistantWindow`
process (AppLauncher ID 15, preload group 5) **does not exist in this
rootfs** — no binary in /app/bin; the preload entry is vestigial.

- Geometry: `loader_3` in `HomeScreen/main.qml:139-203` — 640×720 at y 0,
  x = usingRHD ? (fullModeForSS? −640:0) : ((isFullModeforSS ||
  screenNotSupportSS) ? DEVICE_WIDTH : 1280); `visible: AppHomeModel.visibleSS`;
  200 ms slide; z→10 while its OSD shows (main.qml:24-34). Inner loader
  640×630 at y 90 (AssistantWindow main.qml:19-26). Constants
  AW_SCREEN_WIDTH=640, AW_SCREEN_HEIGHT=630 (aw_initEngine).
- **Ring list**: `Screen_AppAW.qml:18-89` → `QHMI_RingList.qml` — a vertical
  **PathView** (full-size 640×630 cards, SnapOneItem, wrap-around flick;
  `AWModel.handleScrollUp/Down` notifies C++). Cards are widget plugins from
  `/app/plugins` (same framework; parsed by libAssistantWindow's own
  PluginSpec). Card set is **C++-fed per user+car**
  (`AssistantWindowModel::updateListAWModel`, `insertPluginList`,
  `upateModelFromSetting`); user picks cards in AppSetup theme
  split-screen page; persisted as `advanced.arr_splitScreen[%1].mode/.onoff`
  + home_screen_table keys `splitScreen/ssOnOff/ssSet` via
  `SplitScreenWorker::applyInfo/backupInfo` (restored from SHM).
- Interaction: per-card touch forwarded via `AWQML_Controller.qmlSendEvent
  (CARD_HMIEVENTS_AW.*)` — WINDOW_ACTIVED, MOUSE_PRESSED/RELEASED/
  POSITION_CHANGED (args [touchID,x,y,pluginID,pluginName]), FLICK_L/R,
  MULTI_TOUCH(_CANCELED), MOVEMENT_START/END, WIDGET_RELEASED, HIDE_AWOSD,
  HIDE_SEAT_OSD, REQUEST_HIDE_OSD, NOTIFY_SHOWING_OSD, NOTIFY_AWLOADER
  (Screen_AppAW.qml:26-88). Navi card special zones: mapview 101×87@x4,
  auto-view 138×90@x166, charging 101×87@x536 (QHMI_RingList.qml:287-394);
  2-point touch for map pan/zoom.
- Own OSD layer: `awOSD` 640×630 sliding from ±640 (300 ms), hosts C++
  OSDPlugin + native OSDs `onscreens/navtive_eco3.0/{OSD_SOS, OSD_USM,
  OSD_USM_DRIVE_MODE, OSD_USM_TERRAIN_MODE, OSD_USM_ERGO_MOTION_SEAT,
  OSD_USM_SEAT_CHANGE}.qml`; SOS not touch-dismissable.
- 4G variant: `AWModel.memCarSize != 0` → `Screen_AppAW4G.qml` +
  `QHMI_RingList4G` (navi buttons resized; settings-driven order restore).
- Toggles: right-edge `splitBtn` (71 px, main.qml:235-293,
  `KEY_EVT_HOME_CLICK_SPLIT_BUTTON`), option-menu row `E_OPT_SPLIT_SCREEN`
  (offered on EV/HEV/DataService/DriverTalk/RearQuietMode screens),
  AppSetup's own split button (dead — `isSupportedAW: false`).

## Who has it / why it's off

Writers of `AppHomeModel.screenNotSupportSS` (all theme variants):

| Screen | eco3.0 | new2.0 | genesis |
|---|---|---|---|
| Advanced home `Component.onCompleted` | **:533 = true** | :533 = true | :535 = true |
| Wide (Basic home) | :274 = true | :274 | :274 |
| Manual / Information / Submenu_Setup | :310 / :152 / :323 = true | same | same |
| Submenu_DataService / Popup_Media_Gate | = **false** | false | false |

All three themes hard-disable on the home screens. C++ side: MEMBER-style
property (strings 0xca5d47/0xca4dd7 + Changed signal), **no C++ setter** —
written only from QML. AppSetup additionally hardcodes `isSupportedAW: false`
(main.qml:35, TopBar_New.qml:42, all variants).

**`-sc` is NOT the home SS flag**: AppLauncher formats `-sc:%1` from
CarOption.csv (+ hooks `/app/bin/disable_sc`/`enable_sc`), but only
AppProjection (0x1aa584) and AppVH (0x4af064) parse it. AppHomeScreen parses
only `-gui, -wpp, -cnt, -asvr, -evm` (switch at disasm 0x7de072-0x7de258).

Trim hints: the AW navi-card error gate applies only to AC3/BC3 family
vehicles without navi (QHMI_RingList.qml:53-55) — NE (Ioniq 5) is NOT in it.
CarOption fallback rows carry `-sc:1`; no per-trim SS gate in C++ — the
disable is **fleet-wide in this build**.

`visibleSS` (the other loader_3 gate, main.qml:146) is C++-owned (property
strings.tsv:6795, notify :6574, QML only reads); persisted `ssOnOff` restored
by `SplitScreenWorker`; the C++ writer is not recoverable from the export —
the one on-car unknown.

## Re-enable

1. `QHMI_HomeScreenMain_Advanced.qml:533` → `screenNotSupportSS = false`
   (set false, don't delete — other screens write `true` on navigation).
2. Split button + AW plugin come alive automatically (sourceComponent is
   already unconditional — the plugin instantiates invisibly today).
3. Optional: AppSetup `main.qml:35` `isSupportedAW: false → true`.
4. Unknown: whether C++ sets `visibleSS` (trace journal around split presses).
5. Pre-patch probe: EV screen option menu already has a "Split Screen" row.

## Test matrix / breakage

- Popups narrow to 1280 while SS shows (popup width ternary flips);
  `dimBG` topMargin 90 / height 630 on page 0; `dimStatusBar` dims on page 0.
- SystemPopup resizes its window to `DEVICE_WIDTH − 640` on SS toggle
  (mainNoti.qml:19,118-149) — verify NotifyScreenState reaches it.
- Wide panel required (1280 panel = AW fully off-screen; splitBtn is
  isWideScr-gated). This car very likely 1920×720 (live config
  weston_1920.ini; isWideScr derived from micom LCD-type bitfield,
  "LCD TYPE:" log 0xc51d6b).
- RHD mirrors the rail to x=−640. Navi card: NE not in the error gate →
  loads if navi binary exists. eco3.0 Statusbar has **no zoom button**
  (new2.0-only `zoomSizeBTN` main.qml:177-194) — split handle + option row
  are the only toggles.

## Verdict

1. **Proven (static)**: one-line QML patch un-parks the AW; all downstream
   (plugin lib, 24 cards, native OSDs, settings UI, persistence worker)
   ships in this firmware. No new process, no C++ rebuild, no signing.
2. **Likely**: full function (C++ handlers + SplitScreenWorker exist).
3. **Needs the car**: `visibleSS` C++ gate; `isWideScr` confirmation;
   `memCarSize` (4G variant); card population; SystemPopup resize interplay.
