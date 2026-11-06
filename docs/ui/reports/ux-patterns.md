# UX-behavior patterns — deep-dive evidence (round 2)

## 1. Driving-mode lockout

**State sources** (C++ model properties, not QML computations):
- AppSetup: `AppSettingModel.drivingRestriction` (bool Q_PROPERTY,
  drivingRestrictionChanged, bin_5221ed11a7945eee strings.tsv:4841 @0x9e205f;
  logs ">>>> Driving Restriction" 0x9b10f9 / "<<<< Release" 0x9b4308; variant
  `drivingRestrictionTypeB` 0x9e2ac8). Ignition = `modelUsm.ignOn`;
  BT call = `AppSettingModel.stateCall`.
- AppHomeScreen: **`AppHomeModel.drivingRestriction`** (bin_3208391a859693e9
  strings.tsv:1675 @0xc4e520; signal :6487; TypeB :1674/:6488), fed by
  `NotifyDrivingRestrictionFromUSM` → slotNotifyDrivingRestrictionFromUSM
  (:2938-2940) + notifyEnableAccessUsmWhenDriving (:2448-2450); speed tracked
  C++-side ("C_VehicleSpeed:" 0xc6ed70). Stock QML use:
  Screen_HomeScreen_Manual.qml:21.
- Guard events: `KEY_EVT_POPUP_CAR_DRIVING` (Wi-Fi),
  `KEY_EVT_POPUP_CAR_DRIVING_PROFILE` (reset/profile), `KEY_USM_UNIT_IGN_OFF`,
  `KEY_EVT_BTCALLING_ON` — senders Q_AppSetting_ContainLeft.qml:241-360;
  C++ handlers ON_CARDRIVING 0x9c29b0, POPUP_USM_CAR_DRIVING 0x9ac32c0,
  "REQUEST SHOW SYSTEM POPUP for Driving LockOut" 0x9bf378; popup path
  popup/QHMI_AppSettingPopupCarDriving.qml 0x9ac08c8.

**Stock UX examples**:
1. Popup on attempted action: reset → KEY_EVT_POPUP_CAR_DRIVING_PROFILE when
   drivingRestriction (Q_AppSettingGeneralDefault.qml:147-165); Wi-Fi add →
   KEY_EVT_POPUP_CAR_DRIVING (Q_AppSettingWifi.qml:99). The popup: full-screen
   press = CANCEL, Q_PopupAnimationItem verticalCenterOffset 405, info icon,
   body STR_SETUP_POP_TRY_AGAIN_PARKING_VEHICLE 39 px, OK.
2. Description-pane swap: Q_RightMenuDescription visible: drivingRestriction
   (Screen_AppSettingUsmMainLevel_2.qml:113-124; profile screens
   S_Profile_Setting_008.qml:26,128).
3. Mid-flow kick-out: Connections onDrivingRestrictionChanged → close all +
   driving popup (Screen_AppSettingWifiAddNetwork.qml:476-486).
4. Degrade/silent: Manual screen swaps to STR_HOME_POP_TRY_AGAIN_DRIVING_
   VEHICLE (:56,73); visual-only isNeedStroke: !drivingRestriction
   (Q_AppSettingDisplayAdjust.qml:62,80,98); tonedown items = no beep + no
   click (QMenuPageNoDrag.qml:64-67).
**No `enabled: drivingRestriction` row-disables exist in stock** — popups or
pane swaps only. forceDisFocus is only bound to popupIsShowing
(Q_Appsetting_TopBar_New.qml:181).

Custom UI: bind `AppHomeModel.drivingRestriction` directly (typeof-guarded);
daemon adds gear granularity (battery daemon decodes CAN 0x036 byte4&0x0F:
0=P, 5=D, 6=N, 7=R). Custom popup = in-QML copy of QHMI_AppHomePopup_OK.qml
geometry (the C++ popup tables load by path via KEY_EVT_SHOW_POPUP_* and are
unreachable).

## 2. Edit mode

Three screens: EditHomeLayout (option menu; toggles isAdvancedMode,
Submenu_EditHomeLayout.qml:62-108); AllMenus_Rearrange (entry = 650 ms
long-press QMenuPageNoDrag.qml:91-99 → Advanced.qml:227-231
qmlChangeTopMsg(EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED), or option menu
E_OPT_EDIT_HOME_MENU QOptionMenu.qml:213-223; edits via
AppHomeModel.editAllMenus/updateAllMenus; exit = click-outside :167-178);
EditWidget(+Advanced) (long-press on widget Screen_HomeScreen_Widget.qml:
121-145, or option menu gated currentHomeAdvancedPageIdx===0 :195-212;
edits editHomeWidgetPos / editSettingWidget(slotID1, slotID2, E_HOME_ADVANCED)
→ SaveWidetSettingAdvDB).

Stock behavior while customizeMode: guide text verticalCenterOffset 217, 33 px
(Rearrange_Adv.qml:204-212); delegates switch to home_bg_icon_edit_n/_p
(QCustomizedMenuListAllowEmpty.qml:58,72); empty cells via visibleForEdit
(QMenuPageTotal.qml:92,154); drag ghost scale 1.2 (QMenuItem.qml:71,80);
negative page spacing + inter-page arrows (AllowEmpty:245,358,440-514).

Custom-tile facts: all edit screens are separate screens; switch_current_loader
**destroys** the previous screen (main.qml:51-64) — a custom overlaid tile is
destroyed on entry (never visible/draggable/removable during rearrange;
listHomePage holds only real itemIDs). **No customizeMode property exists on
the Advanced home** (customizeModeHK_PAGE0..4 declared-but-unused,
Advanced.qml:11-15; only Basic home uses them, Wide.qml:12-16,39-43).
battery/wifi/tweaks artifacts bind nothing customizeMode-related — they rely
on screen destruction. Post-edit: onRequestUpdateAllMenu rebuilds the grid
(Advanced.qml:459-474) — the tile's `property var slot: firstEmptySlot(n)`
must re-resolve; battery tile guards `visible: slot !== null && slot.page===0`
(battery Advanced.qml:269-279). Card + overlay destroyed with the Advanced
screen; only the page index survives (goHistorypage :556-571); overlay also
closes preemptively on onCurrentScreenChanged (BatteryPage.qml:48).

## 3. Foreground-loss etiquette

Stock evidence: Advanced.qml:522-529 onIsAtOtherAppChanged **commented out**;
QMainScreenAnimation.qml:62-70 onSwitchToOtherApp stops animators, force-resets
opacity=1/x=0; QPopupAnimationItem.qml:44-62 "Force to Close Popup" on
onSwitchToOtherApp; QHMI_AppHomePopup_OK.qml:127-136 opacity=0 while
isSysPopupShowing; main.qml:401 dimStatusBar while isOnScrShowing ||
sysPopupShowing; Rearrange_Adv.qml:232-253 defers constructMenuModel until
!isAtOtherApp && visible (isPendingConstructModel). `requestHomeByPP`:
writable bool AppHomeModel::setRequestHomeByPP (strings.tsv:1730 @0xc4f320);
"PP" never expanded in the binary; observable semantics: C++ sets it when a
Home request re-shows the home screen; Advanced.qml:546-552 consumes it in
Component.onCompleted to skip the entry animation and resets it; overlays
close on it (APP_OVERLAY_CHECKLIST.md:70,100).

Per-surface: overlay → self-close (visible: requested && !isAtOtherApp + 5
close hooks + home_seq); card → stay (never self-hide on isAtOtherApp);
tile → nothing; daemon → keep running. QML timers keep running while
obscured (battery overlay polls 500 ms regardless, BatteryPage.qml:353-359);
pause expensive work via `running: visible` or gating on
currentHomeAdvancedPageIdx === myIndex.

## 4. Trio pattern

**Battery daemon reference** (/rw_data/battery/battery_daemon, static C,
battery_app/src/battery_daemon.c): SocketCAN can0 decode (0x2FC SoC/temp,
0x326 cable, 0x152 fallback, 0x036 gear); M-CAN TX by forking dbus-send →
com.lge.car.micom /Can SetCANMicomRecv (0x0C7 preheat, 0x4ED dash 5 Hz,
0x4A2 charge door). Home/Star monitor: forks dbus-monitor --address
$(cat /tmp/session_appmgr), match rules NotifyRequestHomeScreen +
custom_key_value, bumps g_home_seq + home_source (battery_daemon.c:179-196;
retries 5 s, reports home_monitor: false). HTTP 127.0.0.1:8088
(Connection: close): GET /status /health; POST /start /stop /dash/on|off /
chargedoor/open|close|toggle[?park=1]; /status = flat JSON superset incl.
home_seq, home_source, last_app_key, door_*, batt_*. Mirrors to
/rw_data/battery/status.json every 1000 ms. Install: battery-daemon.service —
After=multi-user.target Dbuslaunch.service, ConditionPathExists on the binary,
Restart=always RestartSec=3, WantedBy=multi-user.target; installed by
run_stuff.sh (USB failsafe: manifest, backup, SMACK-preserving copy, cache
clear, service start while / rw, AppHomeScreen restart via AppLauncher 11).

**QML polling** (BatteryPage.qml): refresh() = XMLHttpRequest GET
http://127.0.0.1:8088/status, fallback file:///rw_data/battery/status.json
on non-200/parse error (:235-273); Timer 500 ms repeat triggeredOnStart
(:353-359); POST helper re-refreshes on DONE (:275-284); retry timer 10 s ×4
(:367-377); staleness watchdog 5 s (:379-388). One daemon serves both
surfaces: card ignores home_seq/home_source; overlay closes on
onHomeSeqChanged (homeSeq > 0) (:52,229-232). Wifi app precedent: 2000 ms
file-mirror polling (WifiPage.qml:213-218), port 8090. Daemon death: HTTP
fails → file mirror = last-known-good; serviceOk=false renders "Service
unavailable" (:335,442), disables controls (:666,703), greys title (:649);
Restart=always recovers in ~3 s with no QML reconnect logic.
