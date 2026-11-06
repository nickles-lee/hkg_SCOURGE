# Gen5W system surfaces & input/event model — AppHomeScreen, Statusbar, SystemPopup, AppOSDManager, AssistantWindow

Firmware `NE_E.EUR.S5W_L.001.001.260128` (Gen5W, Linux/Qt/QML). READ-ONLY research.
All target content treated as data. Nothing was executed.

Local QML trees: `/tmp/opencode/gen5w_ui/…`
Decompiles (Ghidra export, mirror): `../../mirror/export/…`
Binary map: `AppHomeScreen` = `bin_3208391a859693e9`, `ApplicationManager` = `bin_5d71539f61a3982b`,
`AppOSDManager` = `bin_b126383d7e804f2a`, `NotificationManager` (the SystemPopup process) =
`bin_51e45f2c5c6de865`, `libStatusbar.so` = `bin_8942ef70fbdc0979`,
`libAssistantWindow.so` = `bin_ab207982a4cc234b`, `AppSetup` = `bin_5221ed11a7945eee`.
`strings.tsv` columns: `addr  vaddr  length  xrefs  string` (line numbers cited below are 1-based lines of that file).

---

## 0. Executive summary (what an app agent must know)

| Surface | Owner | Where it draws | Can app QML draw there? |
|---|---|---|---|
| Status bar | top **90 px** of every app window; embedded per-app via `libStatusbar.so` | y 0–90, full width | **No** — reserved; content starts at `y:90`, `height:630` |
| General/Climate/Alert OSD | `AppOSDManager` process (own IVI surface) | **90 px top strip** (over the bar), volume OSD = **75 px bottom strip** | No — timed, auto-dismissing, system-owned |
| AW-internal OSD | `AssistantWindow` plugin inside `loader_3` | 640 px split panel, slides in; raises `loader_3.z` to 10 | No (AW is a sibling layer in `HomeScreen/main.qml`) |
| SystemPopup (global popups) | `NotificationManager` process (own surface above apps) | centered popup + 70 % dim of the app area | No — but app receives `AppHomeModel.sysPopupShowing` and must disable input |
| App-local popups | AppHomeScreen C++ loads `popup/*.qml` on demand | full-screen dim (y 90, h 630) + centered card | Only via `KEY_EVT_SHOW_POPUP_*` events, not by instantiating directly |
| AssistantWindow (split) | `loader_3` in `HomeScreen/main.qml`, 640×720 at x=1280 | right 640 px column | No — disabled on this car (`screenNotSupportSS = true`) |

Input: **QML never sees hardware/steering-wheel keys as Qt key events.** They travel
CAN → micom D-Bus (`NotifyKeySWRCEvent`) → ApplicationManager → per-app D-Bus
(`swrckey`, `NotifyRequestHomeScreen`, `custom_key_value`). QML only gets touch
(`MouseArea`) plus model signals. Full-screen overlays must use the
`preventStealing` all-buttons shield (see §7, §8).

---

## 1. Statusbar (top 90 px)

### 1.1 Process model — the bar is embedded in *every* app window

The status bar is **not** a separate always-on-top process. `libStatusbar.so` is a
plugin library that each app loads and embeds in its own QQuickView:

- `AppHomeScreen` links `/app/share/Statusbar/lib/libStatusbar.so`
  (bin_3208391a859693e9/strings.tsv:1155, addr 0xc45aa8).
- In `AppHomeScreen` it is instantiated as the context-property component
  **`STComponent`** (strings.tsv:1781, 0xc4fac0) and loaded by
  `HomeScreen/main.qml` **loader_4**:
  `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/HomeScreen/main.qml:306-319`
  ```qml
  Loader {
      id: loader_4
      z: 2
      width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
      height: 90
      sourceComponent: STComponent
      visible: AppHomeModel.showStatusbar && AppHomeModel.currentScreen !== HMIEVENTS.EVT_GOTO_SCREEN_LOADING
      x: AppHomeModel.isAtMediaGate ? 0 : (currentScreen != HMIEVENTS.EVT_GOTO_SCREEN_WIDE_ADVANCED ? 0 :
           (AppHomeModel.isWideScr ? Math.max(0, 1920 - AppHomeModel.homeMainContentX) : Math.max(0, 1280 - AppHomeModel.homeMainContentX)))
  }
  ```
  (Identical in `qml_new2.0/HomeScreen/main.qml:310-323`.)
- Other apps embed the same QML; the bar QML branches on the context property
  `ImageProviderID` (per-app; libStatusbar strings.tsv:649) — e.g. the profile
  button only shows when `ImageProviderID === "AppHomeScreen"`:
  `Statusbar/qml_new2.0/main.qml:93`.
- Bar state is shared between processes through shared memory + request enums
  (`E_STATUSBAR_REQ_READ_SHM_STB`, `E_STATUSBAR_REQ_HOME_BTN_SHOW/HIDE`, … —
  libStatusbar strings.tsv:824-918) and the appfunc D-Bus methods
  `NotifyStatusbarRequest(ay)` / `NotifyStatusbarShowRequest(i,i)`
  (AppHomeScreen strings.tsv:18214 XML, 0xd99e4e).
- Clicks are delivered to the *host app* through a registered callback object:
  `AppStatusbarEngine::registerCallbackObject(QObject*)`,
  `callbackToHostApplication(PluginCardEvent)`, `sendHWKeyToPluginCard(uint)`,
  `sendEventToPluginCard(PluginCardEvent)`, `initializePluginCard(QQuickView*,QString)`,
  `getPluginCard()` (libStatusbar strings.tsv:948-955).

### 1.2 Anatomy of the 90 px bar

Root: `Statusbar/qml_new2.0/main.qml` (eco3.0 is the same minus the zoom button):

- **L6-9** — `Item { width: ST_DEVICE_WIDTH; height: SttBar_ConstantModel.STATUS_BAR_HEIGHT }`
  (= 90; `STATUS_BAR_HEIGHT` is a registered constant, libStatusbar strings.tsv:1338;
  AppHomeScreen hard-codes `height: 90` for loader_4, main.qml:315).
- **L17-24** — tiled background image `co_bg_statusbar(_s)`, offset by
  `AppStatusbarModel.backgroundX`; N-Mode can fade it in via a 650 ms
  `OpacityAnimator` (L26-34, L307-317).
- **L36-41** — `QRightListIcons` anchored right: the status-icon field
  (`QRightListIcons.qml:10-27`). Ten icon groups `listR1…listR10` fed from
  `AppStatusbarModel.listIconR1…R10` + `listR<n>VisibleCount`; on the 1280 panel
  they lay out as a 4×2 grid (L29-48), on 1920 as a right-to-left `ListView`
  (L50-71). Includes an animated wireless-charging icon with error state
  (L245-296). Cell ≈ 54×39 px (`stWidth: 51+3`, `stHeight: 36+3`, L12-13).
- **L44-58** — **Back button** (`QButton`), visible only when
  `AppStatusbarModel.backBtnVisible && AppStatusbarModel.homeBtnVisible`;
  `onBtnReleased → AppStatusbarModel.backBtnClicked()`.
- **L60-88** — **Home button**; swaps to an **SOS** button when
  `AppStatusbarModel.sosStatus` (L65-68, L76-82); disabled on the FR screen
  during SOS (L74).
- **L91-153** — **Profile button** (75×75 avatar with `OpacityMask`), visible only
  when `AppStatusbarModel.supportProfile && !backBtn.visible && ImageProviderID === "AppHomeScreen"`;
  300 ms re-click guard (`profileTimer`, L327-335).
- **L156-175** — **Menu button** (`QMenuButton`, label = `STR_HOME_MENU`),
  disabled state driven by `AppStatusbarModel.menuStateChanged` (states 3/4,
  L274-294).
- **L177-194** *(new2.0 only)* — **Zoom button** (split/full toggle,
  `AppStatusbarModel.zoomBtnClicked()`, state via `onZoomBtnStateChanged`).
- **L196-210** — **Voice/mic button** (`voiceBtnClicked()`).
- **L236-261** — **Date** text, left margin 788 (1280) / 1116 (1920).
- **L264-273** — **Clock** (`QClockTime.qml`): 234/288 px wide, 60 px font,
  AM/PM suffix, `onBtnReleased → AppStatusbarModel.clockBtnClicked()` (opens the
  clock/calendar popup).
- **L221-233** — hidden `requestCapture` MouseArea right of the clock: press
  ≥ 2 s → `AppStatusbarModel.requestCaptureScreen()` (engineering screenshot,
  `captureTimer` L337-349).
- `QLeftListIcons.qml:8-13` — a second (left-anchored) icon field `listL1/listL2`
  (57×93 cells), currently unused by `main.qml` but part of the library.

Hit-target convention: `Statusbar/qml_new2.0/QButton.qml:48-59` enlarges the
touch area beyond the icon via `widthLeft/RightMultiple` + extents — the bar
buttons are *bigger than they look*.

### 1.3 Overlap/underlap rules for app content

- The bar is `z: 2` **inside the app window** and paints over anything in the top
  90 px; the app content loader sits at `y: 90`
  (`HomeScreen/main.qml:223-233`, `idContent` with
  `topMargin: ConstantModel.MENU_BAR_Y`, height
  `ConstantModel.CONTENT_WITH_MENUBAR_AREA_HEIGHT` = 630).
- On the wide Advanced home page the bar *parallax-scrolls* with the page:
  `x = max(0, 1920 − AppHomeModel.homeMainContentX)` (main.qml:318), i.e. the bar
  can slide off-screen left while page 0 is scrolled.
- Transparent "copy" hit-areas `statusBarLeftCopy` / `statusBarRightCopy`
  (main.qml:340-391, `z: 3`) sit over the bar's button and clock regions with
  `propagateComposedEvents: true` and `mouse.accepted = false` — they only close
  the option menu (`AppHomeModel.optMenuAnimation = false`) and pass the click
  through to the real bar buttons.
- `dimStatusBar` (main.qml:393-422, `z: 3`) draws a 70 % black rectangle over the
  bar while an OSD or system popup shows, and clicking it calls
  `AppHomeModel.sendDbusHideGlobalPopup()`.
- **Apps cannot add content to the bar** — icon lists come from
  `AppStatusbarModel` (C++/SHM) only. The only app-specific branch is the
  profile button's `ImageProviderID` check.

---

## 2. SystemPopup (NotificationManager — global popups)

### 2.1 Process & contract

- The global popup surface is the **NotificationManager** process
  (bin_51e45f2c5c6de865). It loads `mainNoti.qml`
  (functions_0001.c:32303 `FUN_0011ae70(local_e0, …, "mainNoti.qml")` after
  registering context properties `DefineModel`, `StringModel`, `EcoType`,
  `SPLIT_SCREEN_WIDTH` — functions_0001.c:32287-32310).
- It connects on the **session bus** to `com.lge.hkmc.avn.appfunc.appInterface`
  signals `NotifyScreenState`, `NotifyHideGlobalPopupNoButton`,
  `NotifyConfirmStatus`, `notifyUsmRHDStatus`
  (functions_0001.c:32313-32390). Apps request popups through the appfunc
  methods `NotifySystemPopupRequest(i id, ay text)` /
  `NotifySystemPopupResponse(i id, i result)` and receive
  `NotifySystemPopupState(i plane, i state)` (AppHomeScreen
  strings.tsv:18214, XML at 0xd99e4e).
- **`AppHomeModel.sysPopupShowing`** is set by AppHomeScreen's C++ when
  `NotifySystemPopupState` arrives (property + `sysPopupShowingChanged` signal:
  AppHomeScreen strings.tsv:6475/6711; `notifySystemPopupStateChanged` signal
  strings.tsv:5408 region). **What an app/page must do when it is true:**
  1. Disable page input: the stock home screens set
     `isEnable: !AppHomeModel.sysPopupShowing` on their `QRootScreen`
     (`QHMI_HomeScreenMain_Advanced.qml:151`; `QRootScreen.qml:8-9` maps
     `isEnable → enabled`).
  2. Stop flicking: `onSysPopupShowingChanged` calls `homepage.returnToBounds()`
     (`QHMI_HomeScreenMain_Advanced.qml:505-513`).
  3. Expect the home screen to dim: `dimBG` (main.qml:456-470, `z: 4`, black
     70 %) and `dimStatusBar` (main.qml:393-422) become visible.
  4. Never fight the popup for the foreground — it is a separate Wayland
     surface above the app (see UI_CONTEXT Rule 2).
- The home screen actively hides global popups on navigation:
  `timerHidePopup` 5 ms → `AppHomeEngine.requestHideGlobalPopup()`
  (main.qml:524-531) and on page change
  (`QHMI_HomeScreenMain_Advanced.qml:488-493`).

### 2.2 Popup registry — `SystemPopup/popup/noti_popup.xml` (86 entries)

Schema (one `<key>` per popup ID):
`keyID, keyNotiPopupType, keyStatusErr, keyTitleStr, keyBodyStr, keyLongBodyType,
keyListPopupType, keySymbolID, keyNormalPriority, keyImmediateNoti,
keyDispTimeout, keyUserLockTimeout, keyMinDispTimeout, keyAudioChime,
keyBtnA/B/C`.
Type distribution: 35× `E_Type_A` (1-button), 21× `E_Type_D` (bottom banner),
7× `E_Type_B` (2-button), 6× `E_Type_B_INFO`, 5× `E_Type_C` (3-button),
4× `E_Type_E`, plus NAVI day/night, MEDIA_GATE, IGN_OFF specials.
Timeouts: 52 entries `-1` (no auto-hide), 34 entries `3000` ms. All
`keyAudioChime="0"` in this file (chime comes from the caller).

### 2.3 QML layout & animation

- `mainNoti.qml` root is a full-screen `Item` that **resizes** when the
  split screen toggles: `width = DEVICE_WIDTH − SPLIT_SCREEN_WIDTH` while AW is
  on (mainNoti.qml:19, 118-149) and asks C++ to resize the native window via
  `AppSystemPopupEngine.resizePopUpWindow(w, h, bool)` (L47-51), with 200 ms
  width `Behavior` (L29-39).
- Each notification is a `PopupGenerator` (mainNoti.qml:169-231) whose
  `getSource(popupType)` maps the numeric `notiDialogType` to a `type*.qml`
  file — the complete mapping table is
  `SystemPopup/qml_eco3.0/PopupGenerator.qml:42-312` (e.g. 0/11000/4004/4005 →
  `typeA.qml`; 1/4002/4003 → `typeB.qml`; 2/4000/4001 → `typeC.qml`; 50/51 →
  `type_Call_4btn.qml`; 1004/1005/52/53 → `type_Incoming_Call_A.qml`;
  200/201 → `type_PHEV_A.qml`; 2000-2014 → BT popups; 32120 profile;
  32140 calendar; 32165 media gate; 32201 CCS paid service; …).
- **Geometry** (`typeA.qml:76-84`): the popup card is centered in the app area,
  `verticalCenterOffset: AppSystemPopupEngine.isStatusBarShowing ? 45 : 0`
  (i.e. shifted down half the 90 px bar so it centers in the 630 px content
  band), width `POPUP_WIDTH_MAX`/`POPUP_WIDTH_MID`, height computed from
  title+body+buttons. Popup widths (DefineModel, NotificationManager
  QMLDefineModel getter switch, functions_0001.c:73300-75600):
  - `POPUP_WIDTH_MAX = 1260` (0x4ec; also confirmed by QML comments
    `typeB.qml:87 "//1260"`, `type_AA_Init_New_First.qml:71`)
  - `POPUP_WIDTH_NAVI_MAP = 1116` (0x45c; `NaviMapPopup_2btn.qml:90 "//1116"`)
  - `POPUP_WIDTH_MID = 972` (0x3cc, probable — appears twice in the switch)
  - `POPUP_WIDTH_MIN = 678` (0x2a6, probable)
  - `SCREEN_WIDE_WIDTH = 1920` (0x780), `SCREEN_WIDTH = 1280` (0x500)
- **Animation** (`Component/common/PopupItem.qml`): fade-in / fade-out
  `NumberAnimation on opacity`, 100 ms `InOutQuad` (0 ms for the media-gate
  popup `notiID === 11260`), `startOpen()`/`startClose()`, `closePopupFinish`
  signal → `AppSystemPopupEngine.CB_Notification(...)` (typeA.qml:86-91).
- Buttons answer through
  `AppSystemPopupEngine.systemPopupResponse(notiPlane, notiFromAppID, notiID, btnResponse)`
  (typeA.qml:199-203); outside-press closes high-priority popups
  (`notiNormalPriority > 89999`, typeA.qml:15, 54-74) with
  `AppSystemPopupEngine.setBeepSound(true)` before responding.
- Legacy `Component/MMsgPopup.qml` (2012) still exists with hard-coded
  1086×505 geometry — not used by the notification flow.

### 2.4 App-local popups (inside AppHomeScreen) — different mechanism

AppHomeScreen also has its **own** popup QMLs (`AppHomeScreen/qml_eco3.0/popup/`:
`QHMI_AppHomePopup_OK.qml`, `QHMI_AppHome_Popup_Yes_No.qml`, `_Information.qml`,
`_Unavailable.qml`, `_Media_NoDevices.qml`, `_SXM_Subcription.qml`,
`_Notification_VoiceRecognize.qml`, `_ValetMode_GuestMode.qml`,
`_PhoneProjection_TMU_DuringCall.qml`, `QOptionMenu.qml`,
`QOptionMenuWidgets.qml`, PHEV popups). These are **loaded by C++ by path**
(AppHomeScreen strings.tsv:2617-2618, 2780-2798, e.g. 0xd5e250
`popup/QHMI_AppHomePopup_OK.qml`) when QML sends
`HMIEVENTS.KEY_EVT_SHOW_POPUP_*` (handlers exist as
`HANDLER_KEY_EVT_SHOW_POPUP_INFO(QVariant)` etc., strings.tsv:2620-2637).
Layout pattern (`QHMI_AppHomePopup_OK.qml:8-53`): full-screen overlay at
`y: 90, height: 630` (or full 720 on wide-advanced page 0), 70 % black dim,
centered `QPopupAnimationItem` card; outside click = close + beep.
**An app page cannot simply instantiate these — it must send the
`KEY_EVT_SHOW_POPUP_*` event and let C++ create the popup.**

---

## 3. OSD (AppOSDManager)

### 3.1 Process model

`AppOSDManager` (bin_b126383d7e804f2a) is a separate process with its own
QQuickView; it registers `ConstantModel`, `StringModel`, `AppOSDEngine`,
`OSDManager` as context properties and loads `mainOSD.qml`
(functions_0001.c:28977-29010, 83996), then shows itself through the IVI window
manager: `iwm_show_app_with_app_type(1,5,0)` + `QWindow::raise()`
(functions_0001.c:29001-29005). OSDs are therefore **a surface above the app**,
raised/lowered by the window manager, not by app QML.

### 3.2 What OSDs exist

- **Top-strip OSDs** (height `ConstantModel.STATUSBAR_HEIGHT` = 90, full width,
  drawn **over the status bar**): `osdGeneral.qml`, `osdAlert.qml` (warnings,
  red variant when `priority==1`), `osdNavi.qml` (TBT turn arrow, event 600),
  `osdGeneralDAB/SXM_A/SXM_B/SXMData/Media_A/Media_B/Media_Usb/VoiceMemo/
  DriverTalk/Center/AVOff/USMDriveMode/2_line`, and the climate family
  `osdClimateOnDATC / OnFATCKnob / OnFATCEV / On3Zone / SmartVent /
  OnDATC_Rear / OffDATC / OffFATC / Off_3Zone / AirClean`.
  (Routing table: `mainOSD.qml:93-202` and `324-436`; each file declares
  `x:0; y:0; width: DEVICE_WIDTH; height: ConstantModel.STATUSBAR_HEIGHT`,
  e.g. `osdGeneral.qml:7`, `osdAlert.qml:6`, `osdNavi.qml:6`,
  `osdClimateOnDATC.qml:5`.)
- **Volume OSD** — bottom strip, height **75** (`VOLUME_HEIGHT`):
  `mainVolume.qml:5` (`height: 75//ConstantModel.VOLUME_HEIGHT`), loads
  `osdVolume.qml` which is anchored to the bottom and draws title + mute icon +
  level + progress bar (level*9 px per step, 15/33 for 45-step/max-20 profiles,
  `osdVolume.qml:136-186`). Registered constants in the decompile:
  `STATUSBAR_HEIGHT`, `VOLUME_HEIGHT` (bin_b126383d7e804f2a strings.tsv:2536,
  2549); the QMLConstantModel getter returns `0x5a` (90) for the height
  properties (functions_0001.c:84175, 84237).
- **OSD registry** `AppOSDManager/systemOSD/systemOSD.xml` — 40 entries:
  `keySpec, keyIndex, keyType, keyShow, keyEvt, keyInfo, keyMode, keyIcon1-3,
  keyTimeOut, keyID, keyStringA-D`. `keyTimeOut` is in **100 ms units**
  (`mainOSD.qml:581-590`: `timer.interval = delayTime * 100`), so the typical
  `30` = 3 s, `50` = 5 s. Entries include SOS/e-call (50-53), driver-talk
  (320-350), mute (70), media off (33), valet (340-341), profile (345).

### 3.3 Show/hide timing & QML contract

- `AppOSDEngine` signals drive everything (`mainOSD.qml:63-579`):
  `onOsdData` (show), `onOsdUpdate` (refresh), `onOsdClear`, per-family clears
  (`onOsdClearAV/Call/Climate/DriverTalk`), `onOsdBaseData` (persistent "base"
  OSD, e.g. navi), `onClearOsdBase`, `onClearOsdWithAw`, `onDisplayOff`.
- Show: set loader source → `osdLoader.visible = true` →
  `delay(AppOSDEngine.getTimeOut())` → **broadcast**
  `OSDManager.broadcastOsdStatusSignal(DISPLAY, 1, updateId, 1)`
  (mainOSD.qml:204-214). Timeout expiry or **touch anywhere on the OSD**
  dismisses it (`osdLoader` MouseArea mainOSD.qml:43-60 →
  `AppOSDEngine.closeOSDByTouch(updateId)` + `osdTimeOut(...)`).
- Volume: `onOsdVolumeData` → `osdVolume.qml`, `volumedelay(getVTimeOut())`
  (mainVolume.qml:21-55).
- **AppHomeScreen-side OSD hooks:**
  - `AppHomeModel.visibleOSDCall` (bool, "call OSD visible") — set by C++
    (`void AppHomeModel::setvisibleOSDCall(bool)`, AppHomeScreen
    strings.tsv:1603). Home-screen widgets dim themselves while the in-widget
    **call OSD** covers them:
    `Screen_HomeScreen_Widget.qml:47,90,162` `pluginOpacity:
    (AppHomeModel.visibleOSDCall && callOSD.showCallOSD()) ? 0 : 1`, and the
    call widget itself becomes visible at `z: 100`
    (Screen_HomeScreen_Widget.qml:234-236). `pluginOpacity` is simply the
    widget loader's opacity alias (`component/QWidgetItem.qml:12,42`).
  - `AppHomeModel.isOnScrShowing` / `currentOnScreen` — an onscreen (OSD-class
    popup) is showing / its ID; used to dim the status bar (main.qml:401) and to
    hide PHEV popup backgrounds (`PHEV/popup/QHMI_PHEV_PopupInfo.qml:10`).
  - **C++ can inject up to 4 OSD QML files directly into the home-screen
    window**: `onsDataList` (4 slots) + `load_OSD(Onsdata)` (main.qml:428-440,
    function name `load_OSD` in AppHomeScreen strings.tsv:2816) → `ons_Creator`
    repeater whose `osd_Loader`s sit at **`z: 4`**, `y: 0`,
    `height: 720 − 90 = 630`, x-offset 640 in RHD (main.qml:442-454).
  - The **AW OSD raises the AW layer**: `isShowingOSD` from
    `loader_3.item.isShowingOSD` → `loader_3.z = 10` else `0`
    (main.qml:24-34).

---

## 4. AssistantWindow (AW / split screen)

### 4.1 Structure in AppHomeScreen

- AppHomeScreen links `/app/share/AssistantWindow/lib/libAssistantWindow.so`
  (strings.tsv:1154) and obtains the AW QML component as context property
  **`AWComponent`** (strings.tsv:1780, 0xc4fab4). The library exports
  `AssistantWindowEngine::initializePluginCard(QQuickView*, QString)` and
  `AssistantWindowEngine::getPluginCard()` (libAssistantWindow
  strings.tsv:1229-1230, 1951-1952, 3127).
- `loader_3` (`HomeScreen/main.qml:139-203`):
  ```qml
  Loader {
      id: loader_3
      x: usingRHD ? (isFullModeforSS ? -640 : 0)
         : ((isFullModeforSS || screenNotSupportSS) ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH)
      y: 0;  width: 640;  height: 720
      visible: AppHomeModel.visibleSS
      sourceComponent: AWComponent
      asynchronous: true
      Behavior on x { … NumberAnimation 200 ms OutQuad … }   // slide animation
  }
  ```
  So the AW is a **sibling layer of the content loaders** at x=1280 (narrow) or
  1920 (wide), 640×720, full height (the status bar continues across it).
- Behind it: a black 640×720 placeholder rectangle (main.qml:81-88) and an
  async-boot dummy with a loading spinner + "preparing" text
  (main.qml:91-137, `compAWDummy`, image `split/split_bg_screen.png`).
- The **split button** (`splitBtn`, main.qml:235-293, 71 px wide, right edge,
  y-centered) toggles `AppHomeModel.isFullModeforSS/isFullMode` and sends
  `QML_Handler.qmlSendEvtKey(HMIEVENTS.KEY_EVT_HOME_CLICK_SPLIT_BUTTON)`; a
  200 ms `delaySplitScreen` timer blocks re-entry (main.qml:282-292), and
  `recSplit` (main.qml:212-221) guards the button hit area.
- **`AppHomeModel.screenNotSupportSS`**: on this car it is **set to `true` by
  the Advanced home screen itself** —
  `QHMI_HomeScreenMain_Advanced.qml:533` `AppHomeModel.screenNotSupportSS = true`
  (inside `Component.onCompleted`). Consequences: `loader_3.x = DEVICE_WIDTH`
  (parked off-screen, main.qml:142), the split button hides
  (main.qml:237), the AW dummy hides (main.qml:97), and the popup dim covers
  the full width (main.qml:459-460). Property + notify exist in the binary
  (strings.tsv:6448/6656).

### 4.2 What loads inside the AW

- `AssistantWindow/qml/main.qml` (root of AWComponent): a 640-wide `Loader`
  at `y: AWModel.fullMode ? 0 : 90`, `height: fullMode ? 720 : 630`
  (main.qml:19-26) loading `Screen_AppAW.qml` (or `Screen_AppAW4G.qml` for
  4G models, L25).
- `Screen_AppAW.qml` is a **plugin ring-list**: `QHMI_RingList` 640×630/720 at
  `z: 1000` (Screen_AppAW.qml:18-25) hosting widget plugins (navi card, media
  card, …) injected by C++ (`AWModel.currentAppInAppMainStack`, plugin
  press/release/flick/multi-touch forwarded as AW events
  `KEY_EVT_MOUSE_PRESSED/RELEASED/POSITION_CHANGED/FLICK_LEFT/RIGHT/
  MULTI_TOUCH(_CANCELED)/WINDOW_ACTIVED`, Screen_AppAW.qml:26-88).
  Arrow-focus overlay and loading overlay sit at `z: view.z + 10000000`
  (Screen_AppAW.qml:91-103).
- The AW has **its own OSD layer** `awOSD` (640×AW_SCREEN, slides in from the
  right, AssistantWindow main.qml:38-150) hosting native OSD QMLs
  `qml/onscreens/navtive*/OSD_USM.qml, OSD_USM_DRIVE_MODE.qml,
  OSD_USM_TERRAIN_MODE.qml, OSD_USM_ERGO_MOTION_SEAT.qml,
  OSD_USM_SEAT_CHANGE.qml, OSD_SOS.qml`. Its visibility is reported back with
  `AWQML_Controller.qmlSendEvent(CARD_HMIEVENTS_AW.KEY_EVT_NOTIFY_SHOWING_OSD,
  isShowingOSD)` (main.qml:14-17) — which is what raises `loader_3.z` to 10 in
  AppHomeScreen (§3.3). Touch on the AW OSD dismisses it
  (`KEY_EVT_HIDE_AWOSD`, main.qml:107-149).
- AW event enum (`CARD_HMIEVENTS_AW`, from libAssistantWindow strings):
  16 events — `KEY_EVT_WINDOW_ACTIVED, KEY_EVT_MOUSE_PRESSED/RELEASED/
  POSITION_CHANGED, KEY_EVT_FLICK_LEFT/RIGHT, KEY_EVT_MULTI_TOUCH,
  KEY_EVT_MULTI_TOUCH_CANCELED, KEY_EVT_MOVEMENT_START/END, KEY_EVT_WIDGET_
  RELEASED, KEY_EVT_HIDE_AWOSD, KEY_EVT_HIDE_SEAT_OSD, KEY_EVT_REQUEST_HIDE_OSD,
  KEY_EVT_NOTIFY_SHOWING_OSD, KEY_EVT_NOTIFY_AWLOADER`. Controller class:
  `QMLController::qmlSendEvent(int, QVariant)` (strings.tsv:2241, 3147).
- **Key z-order fact for overlays:** because the AW is a sibling of the page
  content, an overlay declared inside `QHMI_HomeScreenMain_Advanced.qml` can
  never cover it; only a root-level item in `HomeScreen/main.qml` can
  (cf. APP_OVERLAY_CHECKLIST.md "split screen caveat").

---

## 5. HMIEVENTS — enumeration & QML_Handler API

### 5.1 Registration model

`import HMIEVENTS 1.0` resolves to a **per-binary registered enum**: each app
embeds only the subset it handles (AppHomeScreen strings.tsv:7103 `HMIEVENTS`
at 0xcaf578 followed by its enum names; AppSetup has its own 97-entry
`KEY_EVT_SETTUP_*`/`EVT_GOTO_SETUP_*` set, bin_5221ed11a7945eee). Names below
are the **declaration order** from the Qt metaobject string table
(AppHomeScreen strings.tsv **lines 7104-7317**, addresses 0xcaf582-0xcb0eda).
Values are sequential from 0 (Q_ENUM convention; `KEY_EVT_MAX` and
`EVT_TRANSITON_MAX` are the sentinels). One interleaved non-enum string
(`POPUP_FEEDBACK_YES_HANDLE`, line 7174/0xcafcb7) sits inside the block, so
indices after it may be off by one — treat the numbers as *declaration order*,
verified names as exact.

### 5.2 HMIEVENTS table (AppHomeScreen registration, 214 names)

**Screen/navigation events (EVT_*)**

| # | Name | Purpose |
|---|---|---|
| 0 | EVT_SCREEN_OPERATION | generic screen op |
| 1 | EVT_GOTO_ALL_SCREEN_ITEMS | go to all-apps list |
| 2 | EVT_MAIN_LIST_ITEMCLICKED | app tile clicked (arg: itemID) |
| 3-7 | EVT_SUBMENU_BLUELINK/MEDIA/SETUP/DATASERVICE/INFORMATION_ITEMCLICKED | submenu list clicks |
| 8 | EVT_MYMENU_ITEMCLICKED | My Menu click |
| 9-13 | EVT_HOME_LIST_PAGE0..4_MOVING | page swipe progress |
| 14-15 | EVT_MOVE_MY_MENU, EVT_ADD_DEL_MY_MENU | My Menu edit |
| 16 | EVT_SHOW_POPUP_AD_NOTIFY | ad popup |
| 17-18 | EVT_MOVE_FOCUS_STATE_TO_AW / _TO_THIS_APP | hard-key focus moves |
| 19-20 | EVT_OPEN_AW_OPION_MENU / EVT_OPEN_OPION_MENU | option menus |
| 21 | EVT_SET_AW_WIDGET | choose AW widget |
| 22 | EVT_CLICK_DISP_OFF | display-off option |
| 23 | EVT_HOME_ICON_STATE | home icon state |
| 24 | **EVT_HOME_SOUND_BEEP_PLAY** | standard touch beep |
| 25-35 | EVT_OPEN_SXM_SUBCRIPTION, EVT_REQUEST_SHOW/HIDE_POPUP, EVT_GOTO_HOME_CUSTOMIZE, EVT_POPUP_YES/NO_BUTTON_CLICK, EVT_REQUEST_CALL_SXM, EVT_CUSTOMIZE_BACK_TO_SETUP, EVT_BACK_TO_MAIN_LIST, EVT_HOME_MENU_LIST, EVT_MEDIA_GATE_CLOSE_CLICKED | popup/menu plumbing |
| 36-60 | EVT_GOTO_N_MODE_* (POWER_DEPTH, PERFORMANCE_DEPTH, …), EVT_RESET_* (GFORCE/TURBO/TORQUE/POWER/SPEED/BRAKE_PRESS/COOLANT_TEMP/OIL_TEMP/THROTTLE/DUAL_GAUGES/CUSTOM_MODE/PERFORMANCE_OPTIONS/GAUGE_HISTORY), EVT_SUBMENU_NMODE_ITEMCLICKED, EVT_LT_SWITCH_RECORD_MODE | N-Mode screens & resets |
| 61 | KEY_EVT_REQUEST_BEEP | generic beep request |
| 62-89 | KEY_EVT_SHOW/HIDE_POPUP_SAVED/KO/OK/OPTION_MENU/INFO/SUBSCREEN/YESNOECODRIVING/DELETED/ECODRIVINGHISTORY, KEY_EVT_PROCESS_POPUP_SCREEN_YES/NO/HIDEYESNOECODRIVING, KEY_EVT_NOTIFY_MENU_BTN_STATE, KEY_EVT_PHEV_BACK, KEY_EVT_REQUEST_TO_NAVI(_LOCATION_SETTING), KEY_EVT_PHEV_WIDGET_TOUCHED, KEY_EVT_PHEV_SAME_SETTING_CHECKING, KEY_EVT_REQUEST_TO_EV, KEY_EVT_PHEV_BACK_TO_PREVIOUS_APP | local popup + PHEV/navi requests |
| 90-126 | EVT_GOTO_HOME_ADVANCED_SCREEN, KEY_EVT_*_CLIMATE (SET_CAN, POPUP_SET_CAN/BLOWER/WIND_DIR_DR/WIND_DIR_PS/BLOWER_FRONT, LOADING, IGN), **KEY_EVT_HOME_CLICK_SPLIT_BUTTON** (97), KEY_EVT_REQUEST_GOTO_SCREEN_ALL_MENUS, KEY_EVT_REQUEST_TOUCH_CANCEL, KEY_EVT_REQUEST_VALET_MODE_POPUP, EVT_POPUP_VALETMODE_GUEST_LEFT/RIGHT_BUTTON_CLICK, KEY_EVT_REQUEST_TO_NAVI_NEAR_BY_STATION/CIRCUIT, KEY_EVT_SAVE_SHIFT_LIGHT_SETTING, KEY_EVT_GOTO_N_MODE_GAUGES_UNIT_SETTINGS/ESE_*, EVT_HOME_SOUND_BEEP_QUITEMODE_PLAY (119), KEY_EVT_HIDE_ALL_POPUP (121), KEY_EVT_GOTO_EV_SETTING_IN_SETUP, KEY_EVT_SHOW_POPUP_HOME/SETTING_RESET_MENU, KEY_EVT_SHOW_POPUP_RESET_LAP_TIMER, KEY_EVT_MAX (126) | climate popups, split button, quiet-mode beep, popup control |
| 127 | EVT_SCREEN_TRANSITION | (first of the transition group, right after KEY_EVT_MAX) |
| 128-177 | EVT_GOTO_MAIN_HOME_SCREEN, _SCREEN_SUBMENU_SETUP/MEDIA/DATA_SERVICE/INFOMATION/EDIT_WIDGETS/EDIT_MENUS/EDIT_HOMELAYOUT/EDIT_WIDGETS_ADVANCED, _MENU_MYMENU(+SHORTCUTS_1ST..3RD, DELETE, MOVE), _HOME_CUSTOMIZE_NORM/WIDE, _HOME_SETTING_WIDGET, _SCREEN_ALL_MENUS, _QUIET_MODE, _DRIVER_TALK, _CLIMATE, _REAR_CLIMATE_CONTROL, _MENU_VALET, _N_MODE(+TEST/GAUGE/CUSTOM…), EVT_GOTO_N_MODE_MAX (177) | screen-goto events (used with `qmlChangeTopMsg`) |
| 178-205 | EVT_GOTO_SCREEN_EV_* / _PHEV_* / _HEV_* (MAIN, SCHEDULING, SETTING, ECODRIVING, ENERGYINFO, SOLAR, DISCHARGING, ECO3_ENGERGY_INFOR, … UX30UI20 variants) | EV/PHEV/HEV screens |
| 206-212 | EVT_GOTO_SCREEN_WIDE_ADVANCED (206), _ALL_MENUS_REARRANGE_ADVANCED, _LOADING, _MANUAL_HOME_SCREEN/_DRIVER_TALK/_DRIVING_INFO, EVT_BACK_TO_PREV_SCREEN (212) | wide home, manual, back |
| 213 | EVT_TRANSITON_MAX | sentinel |

Other binaries register further families used by the same `HMIEVENTS` import
in their QML (observed in QML greps across `/tmp/opencode/gen5w_ui`):
`EVT_TRANSITON_AUDIO/VIDEO/SOUND_OF_NATURE_LIST_TO_PLAYER`, `KEY_EVT_AUDIO/
VIDEO_PLAYBACK_*`, `KEY_EVT_SETTUP_*` (Setup), `EVT_GOTO_SETUP_*` (~60 names),
`KEY_GOTO_USM_*`/`KEY_GOTO_DM_*`/`KEY_GOTO_SEAT_STR_*`/`KEY_GOTO_LI_*`/
`KEY_GOTO_CL_*`/`KEY_GOTO_DA_*`/`KEY_GOTO_CON_*` (Setup list navigation),
`KEY_EVT_USERPROFILE_*`, `KEY_EVT_SETUP_WIFI_*`, `KEY_CHECKBOX_*`,
`EVT_GOTO_WELCOME_SCREEN_DRIVER1/2`, `EVT_REGISTER/DELETE_NFC_CARD_KEY*`,
`KEY_EVT_ALL_OSD_HIDE`, `KEY_EVT_STANDBYSCREEN_TOUCHED`, …
(433 distinct `HMIEVENTS.*` references found in stock QML; the top ones are
`EVT_HOME_SOUND_BEEP_PLAY` ×794, `KEY_EVT_SETTUP_SOUND_BEEP_PLAY` ×372,
`KEY_EVT_SETTUP_CLICK_BACK` ×172, `EVT_BACK_TO_PREV_SCREEN` ×150,
`KEY_EVT_REQUEST_BEEP` ×130).

Widget-plugin events (`ENUM_WIDGET_EVT_T`, strings.tsv:7392-7402):
`E_WIDGET_EVT_MEDIA_PLAYING/PAUSE/STOPPED/REWIND_4X/20X/FASTFORWARD_4X/20X/
PREV_TRACK/NEXT_TRACK/RESUME`. Climate events (`EVT_CLIMATE_*`, strings.tsv:
15549-15588): ~40 temperature/mode/blower variants.

### 5.3 QML_Handler API (class `ScrController`)

Registered as context object **`QML_Handler`** (AppHomeScreen strings.tsv:238501,
s_QML_Handler_00d524b6; created in functions_0002.c:8208
`QString::fromAscii_helper("QML_Handler",0xb)`). Its metaobject
(strings.tsv:7338-7346, 0xcb17a0-0xcb1810):

| Member | Kind | Meaning |
|---|---|---|
| `qmlSendEvtKey(int evt [, QVariant args])` | slot (QML-callable) | post an HMI event into the C++ event queue; 5585 uses in stock QML. Args: e.g. `qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)` |
| `keyEvent` | signal | internal C++ re-emission consumed by AppManager/ScreenQueue |
| `qmlChangeTopMsg(int evt [, QVariant args])` | slot | **change the top screen**: request a screen transition now (used with `EVT_GOTO_*`/`EVT_TRANSITON_*`, 140 uses), e.g. `qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED)` (QHMI_HomeScreenMain_Advanced.qml:230) |
| `qmlInitStackHis` | slot | (C++-side) initialize screen-history stack |
| `qmlInitScrCache` | slot | (C++-side) initialize screen cache |

Surrounding C++ machinery (strings.tsv:7325-7346, 0xdb10d8-0xdb1533):
`AppManager` (signals `screenTriggerHandled`, `onsHistoryChanged`,
`scrHistoryChanged`), `QMLController`, `ScreenQueue`, `OnsController`
(`onsHistoryChanged`, `onsID`), `QTimerWrap` (`timeout_exec`).

**Screen transition flow** (decompile + QML): QML calls
`QML_Handler.qmlChangeTopMsg(EVT_GOTO_*)` → `ScrController` → `AppManager`/
`ScreenQueue` → C++ sets the context property **`screenNext`** (strings.tsv:2818,
s_screenNext_00d5eab4) to the next screen's `QQmlComponent`, finds the active
loader by `objectName == "ldCurrentLoader"` (strings.tsv:2096, 2512) and invokes
the root function **`screen_Transition()`** (strings.tsv:2820, 0xd5eacd), which
QML implements in `HomeScreen/main.qml:40-49`:
`current_loader.sourceComponent = screenNext` — the double-buffered
`loader_1`/`loader_2` then swap in `switch_current_loader()` (main.qml:51-78).
Back navigation = `EVT_BACK_TO_PREV_SCREEN` (QML sends it via
`qmlSendEvtKey`, e.g. `PHEV/ev/Screen_EV_SolarRoot.qml:140`).

Related per-surface controllers: `AppHomeEngine` (QML-callable methods used in
stock QML: `requestShowMenuBtn`, `requestHideGlobalPopup`, `notifyMenuBtnSelected`,
`advancedModePageChange`, `getIsDrawing`, `slotReqInjectAdvancedWidget`,
`manualSetting`, `requestShowSeatHeating/RearSeatHeating`, `request_clock`,
`showClimateSetting`, …), `AppStatusbarModel/Engine` (§1), `AppSystemPopupEngine`
(§2), `AppOSDEngine`/`OSDManager` (§3), `AWQML_Controller` (§4).

---

## 6. Input flow

### 6.1 Touch conventions in home-screen pages

- **Buttons**: `AppHomeScreen/qml_eco3.0/component/common/QButton.qml:37-74` —
  single `MouseArea`, press-state tracking (`mousePressed`/`mousePresseLong`),
  release fires `itemReleased()` **plus the standard beep** when
  `usingBeepSound` (QButton.qml:60-62 → `EVT_HOME_SOUND_BEEP_PLAY`). Focused
  state for hard-key focus comes from `AppHomeModel.focusHKThisApp`
  (QButton.qml:28).
- **Grid tiles**: `component/QMenuPageNoDrag.qml:52-105` — every delegate uses
  `QEnlargedMouseArea` (component/common/QEnlargedMouseArea.qml:5-20; hit box
  grown by multiples, e.g. 1.25× at grid edges, 1.5× top/bottom rows),
  `atPressState` tracking (release outside cancels), **650 ms** long-press
  timer → `itemPressAndHold()`, release → beep +
  `root.itemClicked(itemID, tonedown)` → page sends
  `QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)`
  (QHMI_HomeScreenMain_Advanced.qml:224-226).
- **Widget containers** (`Screen_HomeScreen_Widget.qml:34-66, 78-95`):
  `preventStealing: false` + `propagateComposedEvents: true` so that drags/flicks
  **pass through to the page's `QListViewHomePage`** underneath; press state
  tracked manually (`atPressState`).
- **Page-level input blocking**: every stock screen is a `QRootScreen`
  (`component/common/QRootScreen.qml`) whose `enabled` is bound to `isEnable`
  (QHMI_HomeScreenMain_Advanced.qml:151 `isEnable: !AppHomeModel.sysPopupShowing`).
  QRootScreen itself carries a full-screen `z: 1000` transparent MouseArea that
  plays the release beep (QRootScreen.qml:11-21).
- **Global touch kill-switch**: `disableTouch` MouseArea over everything
  (main.qml:498-521), auto-cleared by a 1 s timer (`timerEnableTouch`).
- **Overlay rule** (project-verified, see APP_OVERLAY_CHECKLIST.md): a
  full-screen overlay must instead use `preventStealing: true`,
  `acceptedButtons: Qt.AllButtons`, accept press/release/click/wheel, content
  at `z: 2` above the shield — otherwise taps fall through to the grid/app
  underneath.
- Composed-event passthrough pattern for transparent guards:
  `propagateComposedEvents: true` + `mouse.accepted = false`
  (main.qml:345-363 statusBarLeftCopy; mainNoti.qml:67-74).

### 6.2 Hardware / steering-wheel keys — decompiled evidence

QML sees **no** `Keys.on*` hardware keys in any stock home/AW/OSD/popup tree:
only 15 hits exist in the whole extract, all in the legacy
`SystemPopup/qml_*/Component/MButton*.qml` components (e.g.
`qml_eco3.0/Component/MButton.qml:154`). The real chain:

1. **CAN → micom** (system bus `com.lge.car.micom.can`): signals
   `NotifyKeySWRCEvent(ay)` (steering-wheel remote keys), `NotifyKeyFrontPannelEvent(ay)`,
   `NotifyKeyFrontSpecialEvent`, `NotifyKeyRRCEvent`, `NotifyKeyCCPEvent`,
   `NotifyKEYPressCounterEvent` — full introspection XML embedded in
   AppHomeScreen strings.tsv:18481 (0xda22ad).
2. **ApplicationManager** (bin_5d71539f61a3982b) receives/registers them:
   strings.tsv:889 `NotifyKeySWRCEvent`, :1370 `registerswrckey`,
   :735 log `"[%1] Disp[%4] APPID(%2) SWRC KEY : %3"`, :1111
   `APP_SETTING_SWRC_CUSTOM_KEY` (the user-configurable Star key setting),
   and exposes the **session-bus** per-app interface
   `com.lge.avn.appmanager.appbase` (AppHomeScreen strings.tsv:18370, 0xd9e84a)
   with methods **`swrckey(i key, i status)`**, `faceplatekey(i value)`,
   `faceplatedial(i key, i status)`, `ccpdial(i value)`, `show_window_fg`,
   `hide_window_bg`, `notify_event` — i.e. hard keys are **D-Bus method calls
   on the focused app**, never Qt key events.
3. **AppHomeScreen** consumes them in C++:
   - `swrckeySlot(QByteArray)` (strings.tsv:2867; debug string `swrckeySlot!!!`
     :2211-2212), connected to the CAN interface
     (`Can not Connect can_iface Signal <-> Slot: NotifyKeySWRCEvent`,
     strings.tsv:2978; `registerswrckey` :2152).
   - `AppHomeModel::swrcCustomKey()` (strings.tsv:1667) — reads the custom-key
     assignment; `AppHomeModel` declares signal **`buttonSKHomeClicked`**
     (strings.tsv:6493, 0xca517b) — *declared in the metaobject but never
     referenced by any stock QML file* (grep over all extracted QML: 0 hits),
     so stock home QML does not even use it.
   - Hard-key focus is surfaced to QML only as the boolean
     **`focusHKThisApp`** (strings.tsv:6443/6700) which drives button
     *focused* icons (QButton.qml:28).
4. **Home key**: appfunc interface `com.lge.hkmc.avn.appfunc.appInterface`
   method **`NotifyRequestHomeScreen(ay)`** (AppHomeScreen strings.tsv:18214,
   XML 0xd99e4e; TERMINOLOGY.md: delivered to the currently focused app).
5. **Star/custom key**: `com.lge.avn.appmanager.info` signal
   **`custom_key_value(i status)`** plus `hardkey_flag_value`
   (AppHomeScreen strings.tsv:18331, XML 0xe9d2dc; UI_CONTEXT.md Rule 2).

This confirms UI_CONTEXT.md Rule 2 verbatim: an in-QML overlay cannot see
Home/Star; a helper must tail the session bus (`/tmp/session_appmgr`) and
expose a counter (Battery daemon `home_seq` pattern).

### 6.3 Other input-relevant model properties (AppHomeModel)

From the AppHomeModel metaobject (strings.tsv:6352-6800 block):
`isAtOtherApp` (+Changed), `currentShowingAppID`, `currentScreen`,
`currentHomeAdvancedPageIdx`, `requestHomeByPP`, `buttonBackClicked`
(signal; the home screen's own back affordance, fired from pages e.g.
`Screen_HomeScreen_AllMenus.qml:258`), `switchToOtherApp` (signal),
`disableTouch`, `isOnScrShowing`, `currentOnScreen`, `visibleOSDCall`,
`sysPopupShowing`, `showStatusbar`, `screenNotSupportSS`, `visibleSS`,
`isFullMode(ForSS)`, `hasAnimation`, `usingRHD`, `isWideScr`, `homeMainContentX`,
`optMenuAnimation`, `focusHKThisApp`, `listOptMenu`, `isAtMediaGate`,
`naviWidget`, `listWidget`, `listHomePage`, `isShowingSSOSD`, etc.

---

## 7. Z-order / layer map of the home screen

`HomeScreen/main.qml` (eco3.0; new2.0 identical except NMode additions) —
root `Rectangle` 1280/1920×720:

| z | Item (id) | Geometry / role | Cite (main.qml) |
|---|---|---|---|
| −9999 | `coverScreen` MouseArea | full-screen debug touch logger; flips to **z 9999** while NMode shows its loading icon (new2.0 main.qml:511-534) | eco:533-540 |
| 0 | `loader_1` / `loader_2` | double-buffered **content screens** (Advanced home, submenus, all-menus, option menu…) loaded from `screenNext` | 66-78 |
| 0 | AW background Rectangle | 640×720 black at x=1280/1920 | 81-88 |
| 0 | AW dummy loader | 640×630 at y=90, boot spinner | 131-137 |
| 0/10 | `loader_3` (**AssistantWindow**) | 640×720 at x=SCREEN_WIDTH; **z→10 while the AW shows its OSD** (`isShowingOSD`) | 24-34, 139-203 |
| — | `recSplit` / `splitBtn` | right-edge 71×185 split toggle (over content, under bar-dim) | 212-293 |
| — | `idContent` | content anchor: y=90, h=630 | 223-233 |
| 2 | `loader_4` (**status bar**) | 90 px, full/narrow width, parallax x on wide-advanced | 306-319 |
| 3 | `statusBarLeftCopy` / `statusBarRightCopy` | transparent click guards over bar buttons/clock (dismiss option menu, pass clicks through) | 340-391 |
| 3 | `dimStatusBar` | 70 % black over the bar while OSD/system-popup shows; click → `sendDbusHideGlobalPopup()` | 393-422 |
| 4 | `ons_Creator` → 4× `osd_Loader` | C++-injected OSD QMLs; y=0, h=630, x+640 if RHD | 428-454 |
| 4 | `dimBG` | 70 % black dim while `sysPopupShowing` (topMargin 90, full height on page 0 when SS unsupported) + input-swallowing MouseArea | 456-470 |
| — | `disableTouch` MouseArea | full-screen input shield, 1 s auto-clear | 498-521 |

Inside a content screen (`QRootScreen`-based, e.g.
`QHMI_HomeScreenMain_Advanced.qml`): background images (z 0) →
`QListViewHomePage` pages (widget page `itemWidgetPlaceHolder` injected by C++,
Advanced.qml:171-194; grid pages) → page indicator row (bottom,
Advanced.qml:403-419) → QRootScreen beep MouseArea **z 1000** → in-page
overlays (e.g. Setup submenu uses `z: 100`,
`Screen_HomeScreen_Submenu_Setup.qml:98`) → **project full-screen overlays use
`z: 2000`** (APP_OVERLAY_CHECKLIST.md pattern; the only stock user of 2000-scale
z inside pages is the call widget at `z: 100`,
Screen_HomeScreen_Widget.qml:236). Above everything inside the window: NMode
loading raises `coverScreen` to z 9999.

Above the *window* itself (IVI surface stacking, owned by the app manager /
`libivi-window-manager-api.so`, `iwm_show_app_with_app_type` /
`iwm_request_fg_with_app_id`): SystemPopup (NotificationManager) and
AppOSDManager surfaces sit on top of the app surface; the status bar and AW are
*inside* the app surface (§1, §4).

---

## 8. Sound feedback conventions

| Event / API | Where | Notes |
|---|---|---|
| `HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY` | 794 uses across all stock QML; canonical examples: `component/common/QButton.qml:61`, `component/QMenuPageNoDrag.qml:63`, `component/common/QRootScreen.qml:19`, `popup/QHMI_AppHomePopup_OK.qml:29` | the standard touch/click beep; sent with `QML_Handler.qmlSendEvtKey` right before the action |
| `HMIEVENTS.EVT_HOME_SOUND_BEEP_QUITEMODE_PLAY` | enum idx 119 (strings.tsv:7223); used ×4 (NMode quiet mode) | quiet-mode variant |
| `HMIEVENTS.KEY_EVT_REQUEST_BEEP` | ×130 uses (strings.tsv:7165) | generic "beep now" |
| `HMIEVENTS.KEY_EVT_SETTUP_SOUND_BEEP_PLAY` | ×372 uses in AppSetup QML (Setup's own HMIEVENTS registration, bin_5221ed11a7945eee) | Setup click beep |
| `HMIEVENTS.KEY_EVT_REQUEST_CLICK_CLACK_BEEP_PLAY` | ×9 (Setup clock) | clock tick beep |
| `HMIEVENTS.KEY_EVT_SOUND_SETTING_ENTER/EXIT` | Setup sound settings | enter/exit chimes |
| `AppSystemPopupEngine.setBeepSound(bool)` | `SystemPopup/qml_eco3.0/typeA.qml:45,60,67` | popup plays a beep when auto-closing on outside press |
| `AppOSDEngine.soundBeep(previousId)` | `AppOSDManager/qml_eco3.0/mainOSD.qml:30` | beep when a base OSD is dismissed by touch |
| `usingBeepSound: true` on `QButton` | e.g. `HomeScreen/main.qml:249` (split button) | opt-in per button |
| `keyAudioChime` in `noti_popup.xml` | all 0 in this build | popup chimes configured per popup ID by the caller |

Beeps are **events, not QML audio**: QML only posts the HMIEVENTS id; the C++
handler plays the sound (handler naming:
`void HANDLER_KEY_EVT_*(QVariant)` in AppHomeScreen strings.tsv:2620-2637).

---

## 9. Source index

QML trees:
- `/tmp/opencode/gen5w_ui/Statusbar/qml_new2.0/{main,QButton,QMenuButton,QTitleButton,QLeftListIcons,QRightListIcons}.qml`, `common/QClockTime.qml`; `qml_eco3.0/` same minus zoom button
- `/tmp/opencode/gen5w_ui/SystemPopup/qml_eco3.0/{mainNoti,PopupGenerator,typeA,typeB,typeC,typeD,typeE_Loading,NaviMapPopup_2btn,…}.qml`, `Component/common/PopupItem.qml`, `popup/noti_popup.xml`
- `/tmp/opencode/gen5w_ui/AppOSDManager/qml_eco3.0/{mainOSD,mainVolume,osdVolume,osdGeneral,osdAlert,osdNavi,osdClimateOnDATC,…}.qml`, `systemOSD/systemOSD.xml`
- `/tmp/opencode/gen5w_ui/AssistantWindow/qml/{main,Screen_AppAW,Screen_AppAW4G}.qml`, `qml/components/QHMI_RingList.qml`, `qml/onscreens/navtive*/OSD_*.qml`
- `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/HomeScreen/{main,QHMI_HomeScreenMain_Advanced,QHMI_HomeScreenMain_Wide,Screen_HomeScreen_*}.qml`, `component/common/{QRootScreen,QButton,QEnlargedMouseArea}.qml`, `component/{QMenuPageNoDrag,QWidgetItem}.qml`, `popup/*.qml`

Decompiles (strings.tsv line numbers):
- AppHomeScreen `bin_3208391a859693e9`: 1154-1155 (AW/Statusbar libs), 1780-1781 (AWComponent/STComponent), 2096/2512/2820 (ldCurrentLoader/screen_Transition), 2617-2637 (popup paths + HANDLER_*), 2816-2819 (load_OSD, screenNext), 6352-6800 (AppHomeModel signals), 6493 (buttonSKHomeClicked), 7103-7317 (HMIEVENTS enum), 7338-7346 (ScrController/QML_Handler), 15549-15588 (EVT_CLIMATE_*), 18214 (appfunc XML), 18331 (appmanager.info XML), 18370 (appbase XML), 18481 (micom CAN XML)
- NotificationManager `bin_51e45f2c5c6de865`: functions_0001.c:32287-32390 (mainNoti.qml + session-bus connects), 73300-75600 (QMLDefineModel values 1260/1116/972/678/1920/1280)
- AppOSDManager `bin_b126383d7e804f2a`: functions_0001.c:28977-29010 (context props, mainOSD.qml, `iwm_show_app_with_app_type(1,5,0)`), strings.tsv:2536/2549 (STATUSBAR_HEIGHT/VOLUME_HEIGHT)
- libStatusbar `bin_8942ef70fbdc0979`: strings.tsv:656 (SttBar_ConstantModel), 824-918 (E_STATUSBAR_REQ_*), 925-964 (AppStatusbarEngine API), 1134-1229 (button signals/properties), 1338 (STATUS_BAR_HEIGHT)
- libAssistantWindow `bin_ab207982a4cc234b`: strings.tsv:1229-1230 (get/initializePluginCard), 2241/3147 (QMLController::qmlSendEvent), KEY_EVT_* AW set (16 names)
- ApplicationManager `bin_5d71539f61a3982b`: strings.tsv:735, 889, 1001, 1111, 1370 (SWRC plumbing)
- AppSetup `bin_5221ed11a7945eee`: per-app HMIEVENTS registration (97 SETUP entries)

Context docs verified against evidence: `docs/ui/UI_CONTEXT.md` (90 px
bar, Home/Star session-bus delivery, input shield),
`docs/ui/TERMINOLOGY.md`, `docs/ui/APP_OVERLAY_CHECKLIST.md`.
