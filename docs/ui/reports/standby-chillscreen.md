# Standby clock + chill screen — deep-dive (round 2)

## A. AppStandbyClock (app ID 10, preload group 1)

**Four surfaces**, loaded from `/app/share/AppStandbyClock/<gui-dir>/`
(`-gui` 2→qml_genesis, 3→qml_eco3.0, else new_qml; decompile
functions_0001.c:42229-42260):

1. **Boot clock** — from process start until boot completes, then
   `AppStandbyClockEngine::gotoHomeScreen()` ("Request to AppHomeScreen")
   + `NotifyBootingDone` D-Bus (functions_0001.c:22064) hand over to home.
   Screens: `Screen_AppStandbyClock_Digital.qml` (image-digit clock:
   `home_clock_num_01/02` PNGs, colon, AM/PM, `QHMI_Calendar` at y≈611,
   Digital.qml:96-166,193-320), `Analog.qml` (image hands), `Digital_Pixel.qml`,
   `Logo.qml` (single `/tmp/front_lcd.png`). Selection C++-driven:
   `slotNotifyScreenTypeClock` etc. fire
   `EVT_GOTO_SCREEN_{DIGITAL|ANALOG|DIGITAL_PIXEL|LOGO|SOS|VALET_MODE}`;
   C++ builds `<dir>/<file>.qml` → `screenNext` (functions_0001.c:45559-580)
   → main.qml:139-158 double-buffered loaders.
2. **Display-off/idle** — `onscreen/QHMI_AppStandbyClock_BlankScreen.qml:29-51`:
   full-screen black (#060a10 HMC) + whole-screen MouseArea →
   `KEY_EVT_STANDBYSCREEN_TOUCHED` (touch-to-wake). Display-off is *this app
   drawing black* — the clock screens are NOT shown on display-off.
3. **Navi loading** — `Screen_AppStandbyClock_Navi_Loading.qml:15-79`
   (spinner + STR_HOME_WIDGET_PREPARING, colors via ThemeHelperSTAN).
4. **firstSetup** profile flow (+ ecall SOS, ValetMode 925 lines, boot
   agreement `Screen_AppStandbyClock_Confirm.qml`).

**Runtime context** (registered properties, str 0x13db46-0x13dbee): HMIEVENTS,
APPENUMS, AppStandbyClockEngine, AppStandbyClockModel, DEVICE_WIDTH/HEIGHT,
RESOURCE_DIR, AWComponent, ConstantModel, ImagesModel, **TranslationModel**
(no "s"), **ThemeHelperSTAN** (verified = the same `ThemeHelper` class as
`HomeThemeHelper` — both binaries carry `void ThemeHelper::setTheme(int,int,int)`),
QML_Handler, STComponent (embedded Statusbar), screenNext.
**No vehicle/EV data**: full AppStandbyClockModel property dump
(0x15c078-0x15ce50) = time/timeValid/isTimeFormat12/generalLanguage/
vehicleVariant/wideScreen/buyer/ecoType/guiVariant/dayLightSuffix/ignStatus/
accStatus/altStatus/isDisplayOn/isEcall/isFactoryMode/valetModeState/
teleVariantFlg… — **no AppPHEVModel, no SOC/range/charge**. Micom D-Bus XML
only for time (com.lge.car.timemanager, functions_0001.c:14999-15032).

**Patch surface**: time is free (`AppStandbyClockModel.time` = QList
sec/min/hour/day/month/year/ampm, Digital.qml:50-53,96-105). Charge % must
come from the battery daemon (`http://127.0.0.1:8088/status` via
XMLHttpRequest, or the 1-second `/rw_data/battery/status.json` mirror).
**Scope rule: patch screen files only, never main.qml** — a syntax error in
a screen file → that screen stays black, process survives (AppHomeScreen
group 2 launches regardless; AppLauncher's appLoadTimeout synthesizes
preloadcomplete). A broken main.qml → **no boot clock, no display-off wake,
no ecall SOS UI, no firstSetup** until USB-failsafe restore; no crash
auto-restart exists (only explicit terminate/launch).

**Standby vs home decision**: ApplicationManager `GoStandBy` vs `GoHome`
transition classes (functions_0002.c:40663, str 0x323eb0);
`MicomInterfaceM::AskSysBootinfo bootType[%1]` selects boot behavior;
`AnimationController::welcomeReady` → SetCANFixedMicomRecv tells micom the
UI is up. Runtime re-entry: `requestStandbyScreen()` → D-Bus
`set_request_standby_screen` on com.lge.hyundai.avn.appmanager
(functions_0001.c:20754-20830).

## B. Chill screen = Screen_HomeScreen_Widget_Advanced.qml (467 lines)

**The whole screen is C++-created from the on-disk file**:
`QHMI_HomeScreenMain_Advanced.qml:174-192` creates the empty placeholder
(`objectName: "itemWidgetPlaceHolder"`) and calls
`AppHomeEngine.slotReqInjectAdvancedWidget()`; C++ finds it via
`ldCurrentLoader` → `lsvHomePage` → contentItem children (objdump
0x7be2c3-0x7be2f8, "FOUND!!!" 0xc7e3ee), then
`AppHomeEngine::loadWidgetAdvancedItem()` (str 0xc594a0): guards
"already created." (0xc55d60), builds URL
`HomeScreen/Screen_HomeScreen_Widget_Advanced.qml` (0xc55d90) under
`/app/share/AppHomeScreen/qml_eco3.0/`, `QQmlComponent(engine,url)` →
create → CppOwnership → `setContextProperty("AdvancedWidgetQMLInstance",
item)` (0xc5249c) → `setParentItem(placeholder)` (0x7be40f).
**Conclusion: the file lives on disk and C++ loads it by path — patch it
freely** (C++ only requires a QQuickItem root; `setButtonWidgetActive` is
optional, `Connections … ignoreUnknownSignals: true`).

**Geometry** (root Item 1920/1280×720, `:8-12`):

| Region | Geometry | Evidence |
|---|---|---|
| no-map bg | full width, home_nomap_bg(_02) | :106-112 |
| navi map | `content` anchored right, 836×720 (wide); Loader naviWidget[0].sourceInput (live AppNavi map, surface 4903); visible gated isNaviBinExist && isNaviAvailable && isNaviBootCompleted && naviSupport && !isShowLoadingNavi; click → beep + requestMAPByTouchWidget() | :114-132 |
| left gradient | home_map_bg(_02) | :196-201 |
| EV wallpaper | home_bg_wallpaper, EV+wide | :203-209 |
| weather | 1920×163 top-right; Loader AppHomeModel.weatherWidgetDL3 (WeatherWidget plugin); empty Component when isNotSupportWeather | :210-258 |
| clock | QText 120 px left 75/vcenter offset 399; AM/PM 45 px (#6a6a6a) offset 370; date 34 px offset 303; MouseArea (clock+ampm+26)*1.1 × 170 → beep + request_clock() | :260-305 |
| navi spinner | 117×117 right margin 359, visible naviSupport && (!isNaviAvailable ‖ !isNaviBootCompleted) | :307-327 |
| factory OSD | 550×216 centered top, isFactoryMode | :328-423 |
| EV widget | evLoader left 520/top 290; in-tree QButtonBatteryHomeItemEco3; visible _isEV && _isWideScr; release → requestShowEVApp() | :424-440 |
| AV area | idAvwidget x=0, 617 (EV wide)/970 (wide)/620 (narrow) ×720; Loader AppHomeModel.avWidgetHome (MediaWidget); bottom-250-px click strip → handleAVWidgetAdvanceClicked() | :442-465 |

**Data reachable**: AppHomeModel (isWideScr, ecoType, region, navi/weather/
date/time state, isFactoryMode, dayLightSuffix, textUpdate) and
**AppPHEVModel** (registered in this process, str 0xc4faa7):
`display_Batlevel` (SOC %), `display_DTE` (range), `batCapacity` (0-20 glyph
step, QBatteryHomeItemEco3.qml:24-48), `batChgSts`/`fastChgSts`/`stdChgSts`
(:68), `lowBatteryWarnType`, `percentLowSoc`, `vcuReady/vcuEvReady`,
`utilityMode`, `heatBatteryPercent`, `unitWidget`, `dte_Calc`, `dteOffDisp`,
`currentBuyer`. Park state via AppNModeModel (0xc4f9bd).

**Recipes (QML-only, ranked)**:
1. Patch `component/QButtonBatteryHomeItemEco3.qml` — in-tree, loaded only
   here; add charging-state/heater rows; keep QButton base + beep.
2. Add an info module to the screen — free space wide: x 520-1084 × y 470-720
   and band y 163-290; narrow: x 75-444 below the clock (map starts x 444).
   Input: plain MouseAreas let the carousel steal drags (stock behavior);
   internal drags need `preventStealing: true` or
   `AdvancedWidgetQMLInstance.setButtonWidgetActive(true/false)` on
   press/release (locks the carousel, Advanced.qml:180-187,340).
3. Restyle the clock block (:272-305); keep request_clock() wiring.

**Breakage**: navi not booted → map hidden + spinner (no "not support" text
on THIS screen — that's the basic-mode QWidgetItem path); call OSD → no
pluginOpacity here (basic-mode only); day/night → Q_ThemeImage/QText/
HomeThemeHelper (stock hardcodes: AM/PM #6a6a6a, battery #ffffff/#b6c1c8/
#ff6600 with checkDayGui).
