# Gen5W App Lifecycle — registration, launch, show/hide/close, and the two deployment models

Firmware: `NE_E.EUR.S5W_L.001.001.260128` (x86-64, Yocto "mango" build
`corei7-64-oe-linux/...master.hmc.wide.26Ru1st.2-r2`, build date 2026-01-28).
All addresses are Ghidra vaddrs (image base 0x100000 already subtracted in
`strings.tsv`; decompiled functions are `FUN_<vaddr>`).
Sources: Ghidra exports `bin_c8d6d56bc254ad07` (AppLauncher),
`bin_5d71539f61a3982b` (ApplicationManager), `bin_3208391a859693e9`
(AppHomeScreen), `bin_5221ed11a7945eee` (AppSetup); the extracted 260128
rootfs; stock QML in `/tmp/opencode/gen5w_ui/`; and the prior research docs
(UI_CONTEXT.md, TERMINOLOGY.md, APP_OVERLAY_CHECKLIST.md, HELLO_SIDELOAD_APP.md,
battery_app/SPEC.md).

---

## 1. The actors

| Process | Binary | Role |
|---|---|---|
| Weston (ivi-shell) | `/usr/lib/weston` + `/app/lib/weston/ivi-window-manager.so` | Wayland compositor; the HMI window manager is the weston module `ivi-window-manager.so` (`/etc/xdg/weston/weston_1920.ini`: `shell=ivi-shell.so`, `backend=ias-backend.so`, `ivi-module=ivi-window-manager.so,ivi-controller.so`; layers: base 1000, workspace-bg 2000, workspace 3000, **application 4000**, `transition-duration=300`) |
| dbus-daemon (session) | started by `Dbuslaunch.sh` | The "app manager session bus"; address in `/tmp/session_appmgr`, env file `/tmp/session_appmgr.env` |
| AppLauncher | `/app/bin/AppLauncher` (Qt, 1 114 fns) | Process spawner + preload orchestrator. Owns D-Bus name `com.lge.avn.applauncher`, object `/Request`. Verifies each app's `.sign` ELF section before spawning. |
| ApplicationManager | `/app/bin/ApplicationManager` ("AppManager", 4 740 fns) | App state machine: foreground/background transitions, app history, display planes, key routing. Owns the ivi-wm client (`libivi-window-manager-api.so`). |
| AppHomeScreen | `/app/bin/AppHomeScreen` (12 467 fns, app ID **11**) | The home screen; QML in `/app/share/AppHomeScreen/qml_eco3.0/` |
| AppSetup | `/app/bin/AppSetup` (18 525 fns, app ID **0x3c = 60**) | Example feature app |

Boot chain (systemd, from the rootfs units):

1. `Dbuslaunch.service` (`After=micom.service`, `OOMScoreAdjust=-900`) →
   `Dbuslaunch.sh`: `dbus-daemon --session --print-address --fork` →
   `/tmp/session_appmgr` + `/tmp/session_appmgr.env`.
2. `ApplicationLauncher.service` (`After=Dbuslaunch.service
   dbus-com.lge.car.micom.service`, `EnvironmentFile=/tmp/session_appmgr.env`) →
   `AppLauncher.sh`:
   ```bash
   export QV4_MM_MAX_CHUNK_SIZE=16384
   export LD_LIBRARY_PATH=/app/lib:/navi/navi/Lib
   export LD_PRELOAD=/usr/lib/libEGL.so.1
   export XDG_RUNTIME_DIR=/var/run/user/root
   export QT_QPA_PLATFORM=wayland
   export QV4_NO_SSA=1
   export QV4_FORCE_INTERPRETER=1
   cd /app/bin; /app/bin/AppLauncher 3 &
   ```
   The `3` argv is read in `FUN_0010b9b0` (main): `iVar5 = argv[1][0] - '0'` →
   `FUN_00124a90(manager, 3)` (boot/display parameter). Main then registers the
   D-Bus service `com.lge.avn.applauncher`, creates the `/Request` adaptor
   (`FUN_00126250`), and kicks the preload (`FUN_0011d0b0`).

---

## 2. Registration — how an app gets its ID, entry, and binary path

### 2.1 The hardcoded app-ID table (AppLauncher)

`FUN_0010b080` (AppLauncher, functions_0001.c:3775) builds the canonical app
table. Each entry: `id, name, executable path`:

| ID | Name | Path |
|---|---|---|
| 3 | AppLauncher | /app/bin/AppLauncher |
| 2 | ApplicationManager | /app/bin/ApplicationManager |
| 10 | AppStandbyClock | /app/bin/AppStandbyClock |
| 100 | AppMediaPlayer | /app/bin/AppMediaPlayer |
| 110 (0x6e) | AppProjection | /app/bin/AppProjection |
| 207 (0xcf) | tmsbgapp | /app/bin/tmsbgapp |
| 212 (0xd4) | TmsFgApp | /app/bin/TmsFgApp |
| 200 | TmsGuiApp | /app/bin/TmsGuiApp |
| 209 (0xd1) | TmsVcrmApp | /app/bin/TmsVcrmApp |
| 70 (0x46) | AppRadio | /app/bin/AppRadio |
| 78 (0x4e) | AppHDData | /app/bin/AppHDData |
| 73 (0x49) | AppMobileTV | /app/bin/AppMobileTV |
| 77 (0x4d) | AppEURadio | /app/bin/AppEURadio |
| 74 (0x4a) | AppXMAudio | /app/bin/AppXMAudio |
| 75 (0x4b) | AppXMData | /app/bin/AppXMData |
| **11 (0xb)** | **AppHomeScreen** | /app/bin/AppHomeScreen |
| **60 (0x3c)** | **AppSetup** | /app/bin/AppSetup |
| 17 (0x11) | NotificationManager | /app/bin/NotificationManager |
| 22 (0x16) | AppOSDManager | /app/bin/AppOSDManager |
| 12 (0xc) | AppCamera | /app/bin/AppCamera |
| 13 (0xd) | AppClimate | /app/bin/AppClimate |
| 15 (0xf) | AppAssistantWindow | /app/bin/AppAssistantWindow |
| 121 (0x79) | BluetoothHMI | /app/bin/BluetoothHMI |
| 40 (0x28) | AppVH | /app/bin/AppVH |
| 30 (0x1e) | AppNavi | `LOCFILE:/update/activeNaviPath::DEFAULT:/navi/navi::CAND1:/navi/navi_1::CAND2:/navi/navi_2` → `/navi/navi/Bin/AppNavi` |
| 31 (0x1f) | AppGPS | /app/bin/AppGPS |
| 112 (0x70) | AppMirrorLink | /app/bin/AppMirrorLink |
| 113 (0x71) | AppCarLife | /app/bin/AppCarLife |
| 250 (0xfa) | AppFCEV | /app/bin/AppFCEV |
| 251 (0xfb) | AppRSPA | /app/bin/AppRSPA |
| 310 (0x136) | AppTestAgent | /app/bin/AppTestAgent |
| 311 (0x137) | AppDMClient | /app/bin/AppDMClient |
| 312 (0x138) | LocationManager | /app/bin/LocationManager |
| 360 (0x168) | AppPHEV | /app/bin/AppPHEV |
| 400 (0x190) | UserProfileManager | /app/bin/UserProfileManager |
| 0x401 | — | (terminal sentinel) |

ApplicationManager carries the matching logical-name table (`APP_HOME`,
`APP_CAMERA`, `APP_SETUP_*`, `APP_MYMUSIC`, `APP_USB1_AUDIO`, … at
strings.tsv 596–780; e.g. `APP_NAVI_LOADING`, `APP_HOME` with
`AppHomeScreen` at 0x2fcf28). App IDs are therefore **compile-time
constants** in both binaries: there is no on-car "register a new ID"
config for the grid.

### 2.2 The preload group list — `/rw_data/applaunch-config.json`

`ApplicationLoader::makeAppGroupLaunchList()` (`FUN_00139b60`, AppLauncher
functions_0001.c:26098) reads `/rw_data/applaunch-config.json` (string at
0x52478). Format (from the built-in default, string 0x4b1f0, 2 076 bytes):

```json
{"0": [{"appName": "ApplicationManager"},{"appName": "UserProfileManager"}],
 "1": [{"appName": "AppCamera"},{"appName": "AppStandbyClock", "dependAppName": "ApplicationManager"}],
 "2": [{"appName": "AppOSDManager", "dependAppName": "ApplicationManager"},
       {"appName": "AppHomeScreen", "dependAppName": "ApplicationManager"},
       {"appName": "AppDMClient", "dependAppName": "ApplicationManager"}],
 "3": [{"appName": "AppNavi", ...},{"appName": "AppGPS", ...},{"appName": "AppProjection", ...},{"appName": "NotificationManager", ...}],
 "4": [{"appName": "AppMediaPlayer", ...},{"appName": "AppRadio", ...},{"appName": "AppMobileTV", ...},
       {"appName": "AppEURadio", ...},{"appName": "AppXMAudio", ...},{"appName": "BluetoothHMI", ...},
       {"appName": "AppMirrorLink", ...},{"appName": "AppCarLife", ...}],
 "5": [{"appName": "tmsbgapp", ...},{"appName": "TmsFgApp", ...},{"appName": "AppAssistantWindow", ...},
       {"appName": "AppVH", ...},{"appName": "TmsGuiApp", ...}],
 "6": [{"appName": "AppXMData", ...},{"appName": "AppHDData", ...},{"appName": "AppSetup", ...},
       {"appName": "AppFCEV", ...},{"appName": "AppRSPA", ...},{"appName": "AppPHEV", "dependAppName": "AppHomeScreen"}],
 "7": [{"appName": "AppTestAgent", ...},{"appName": "LocationManager", ...},{"appName": "TmsVcrmApp"}]}
```

Keys are group indices ("0".."7"), each an array of objects with
`appName` and optional `dependAppName`. The parser walks
`QJsonObject[QString::number(i)]` per group, resolves each `appName` against
the ID table, and logs `groupIdx[%1] cnt[%2]`; a "LastAVApp %1 move to Group
%2" special-case moves the last AV app between groups. If the file is
missing/invalid the built-in default above is used
("makeAppGroupLaunchList Config file is not valid. error[%1]"; the default
JSON string is embedded at 0x4b1f0). Groups launch sequentially at boot
("======================================== All Apps launch complete"),
each app optionally waiting on `dependAppName`.

**What the config contains: only app names and dependency names** — no IDs,
no icons, no QML paths. IDs and executable paths come from the hardcoded
table (2.1); icons and grid placement come from AppHomeScreen's own item
models (2.4); QML dir comes from the `-gui:N` argument (§5.2).

### 2.3 The car-option table — `/app/bin/CarOption.csv`

`setAppLaunchArguments` (AppLauncher, functions_0001.c ~23780–24100)
assembles the per-car CLI arguments passed to every app. It reads
`/app/bin/CarOption.csv` (string 0x526f8, key column "Vehicle Code"); if the
car type is absent it logs `setAppLaunchArguments file not contain this
cartype, recovery default` and uses an embedded fallback table
(0x4b1f0–0x52248, entries like `SG2(EV) 25MY`, `CN7 PE 26MY`, `US4 HEV`,
`NX4-NX4e`, `PDe PE2`, … each ending in `,END`). Typical row tail:

```
-et:0,-by:0,-gui:1,-sc:1,-vc:67,-kt:0,-dn:2,-nm:2,-p2s2:1,-ke:new,-pf:1,
-pvv:0x12,-os:1,-osr:8,-un:1,-er:1,-is:0
```

Known argument keys (`-cv:%1` vehicle code, `-vv`, `-sv`, `-dt`, `-l`, `-ck`,
`-pp`, `-usm`, `-et`, `-pt`, `-by`, `-pi`, `-pf`, `-cbp`, `-ecpt`, `-emmt`,
`-sc`, `-dn`, `-ke`, `-nm`, `-os`, `-osr`, `-un`, `-er`, `-is`, `-isa`,
`-isrcv`, `-pitch:WxH`, `-evm`, `-mp`, `-asvr`, and **`-gui:%1`** the GUI
theme variant). Test hooks: `/app/bin/disable_gui` → `-gui:0`,
`/app/bin/enable_gui`, `/app/bin/disable_sc`/`enable_sc`, `/home/root/test_sc`.
`CarOption.csv` is **not present in the extracted rootfs** (see §8 unknowns).

### 2.4 The home grid entry

The home grid is *not* config-driven. `AppHomeModel.prepareAdvancedDisplayList(nbrItemsPerPage)`
(C++, called from `QHMI_HomeScreenMain_Advanced.qml:89` via
`constructMenuModel()`) builds the page models (`AllMenuPage1..3Display`) with
per-item `itemID`s; the QListView/QMenuPageNoDrag delegates render them and
emit `itemClicked(itemID)`. The widget layout is persisted by AppHomeScreen
itself in `/app/data/AppHomeScreen/CustomLayoutDB/CustomLayoutDB`
(string 0xc49848) and `/app/data/AppHomeScreen/SetupDB/`. A new official app
therefore needs a new `itemID` in AppHomeScreen's C++ item table (icon name,
string ID, app ID) — i.e. a firmware rebuild, not a config edit.

### 2.5 Runtime self-registration

Two registration protocols run on the session bus after spawn:

- **AppLauncher**: each spawned app calls back;
  `FUN_0011e970` logs `[REGISTERED] registeredFromNativeApp AppId[%1][%2]
  onDemand[%3] groupNum[%4]` and rejects unknown IDs
  (`[REGISTERED ERROR] WrongAppID`). This advances the preload group state
  machine and emits `preloadcomplete`.
- **ApplicationManager**: apps call `registration` on
  `com.lge.avn.appmanager.registration` (path `/registration`, args
  `appid`, `appname` — strings 0x33e9e2/0x33e9e8/0x33e9f0 in
  ApplicationManager; AppSetup has the matching client at 0x9bd240 plus
  failure strings `REGISTRATION_FAILED_NOT_REGISTERED!!!`,
  `REGISTRATION_FAILED_DUPLICATE!!!`, `REGISTRATION_FAILED_WRONG_DISPLAY!!!`).

ApplicationManager also exposes query methods the HMI uses:
`ask_appmanager_application`, `ask_application_status`, `ask_binary_name`,
`ask_preload_complete_status`, signals `appmanager_front_application`,
`preload_complete_status` (strings 0x33b18b–0x33b7c4), plus
`com.lge.avn.appmanager.screentransition`, `com.lge.avn.appmanager.broadcast`
(`broadcast_event`), `com.lge.avn.appmanager.info` (`custom_key_value`,
per prior docs), `com.lge.avn.appmanager.appbase`, and
`com.lge.hyundai.avn.appmanager.Factory`.

---

## 3. Launch — from tile click to foreground

### 3.1 QML → C++ event (AppHomeScreen)

`QHMI_HomeScreenMain_Advanced.qml:225/266/307` (and AllMenus:139):

```qml
onItemClicked: QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)
```

The C++ handler exists (pretty-function strings
`void MAIN_LIST_ITEMCLICKED_HANDLE(QVariant)` at 0xc5d060,
`void HOME_MENU_LIST_ITEMCLICKED_HANDLE(QVariant)` at 0xc5d020,
referenced at AppHomeScreen file offsets 0x80871c / 0x807c5c); its Ghidra
decompile body was not recovered cleanly (the export contains many
`syscall(0xba)` stubs in this region), but the same binary contains the
generated D-Bus client `ComLgeAvnApplauncherRequestInterface`
(`FUN_00cfde80` constructs `QDBusAbstractInterface(...,
"com.lge.avn.applauncher.request", ...)`; `FUN_00d09d90` invokes method
`launch(appid, appname, applocation)`; `FUN_00d09e20` invokes
`terminate(appid)`), plus `ask_appmanager_application` client strings
("ask_appmanager_application response : ", 0xc54170/0xab3870 in
AppHomeScreen/AppSetup) and the `com.lge.avn.appmanager.info`
`appmanager_front_application` subscription
("Can not Connect info iface [[ appmanager_front_application ]] <-> Slot").

So the chain is:

```
tile click (QML)
  → QML_Handler.qmlSendEvtKey(EVT_MAIN_LIST_ITEMCLICKED, itemID)
  → AppHomeScreen C++ MAIN_LIST_ITEMCLICKED_HANDLE(itemID)
  → ask_appmanager_application (session bus → ApplicationManager)
  → AppLauncher /Request launch(appId, appName, appLocation)   [session bus]
```

ApplicationManager issues the same request itself for programmatic launches:
`FUN_002f64d0`/`FUN_002f67f0` build `com.lge.avn.applauncher` calls;
`FUN_002f7100` calls `FUN_002892b0(mgr, 0xb)` (launch AppHomeScreen, ID 11);
`FUN_002f6ad0` requests MPAutoTest (0x13b); `FUN_002f6160` requests
EngineMode (0x12d) via `CHKeyInterface::Launcher EngineerMode`.

### 3.2 AppLauncher: spawn with signature verification

`Request::launch` (`FUN_00126340`, logs `Request::launch appId[%1]
appName[%2] appLocation[%3]`) forwards to the loader manager. On-demand
launch (`FUN_0011f2e0`-region) logs `[onDemandLaunch] appId[%1][%2] New
Instance`, creates an App record (state 2), and calls the spawn function.

**Spawn function `FUN_001163f0`** (App "m_AppProcess->start(%1)"):
1. Builds the `QProcessEnvironment`; per-app it sets
   **`QT_WAYLAND_SET_SURFACE_ID`** from the surface table
   (`FUN_00114e50`, log `AppID : %1, Set to QT_WAYLAND_SET_SURFACE_ID=%2`;
   table @0x475c0: id 0x11→1030, 0x0c→2080, 0x6e/0x6f→2110, 0x70→2160,
   0x71→2170, 0x49→2200, 0x1e→3000, default 0).
2. Special cases: appId 0x137 with `-un:1` → `/app/bin/AppScomoDM`;
   appId 0x12d → `/tmp/app/bin/AppEngineerMode` if present.
3. **Signature check** (normal apps):
   - `FUN_001158c0` sets the check triplet {binary, pubkey, sign-path};
     sign path = `"/tmp/" + basename`, pubkey = `/tmp/app_normal.key`
     (copied from embedded Qt resource `:/app_normal.key`; navi uses
     `:/navi_public.key` → `/tmp/navi_public.key`, strings 0x48c8b/0x491f8).
   - `FUN_00115b50` ("readSection") opens the app ELF with libelf, walks
     sections, and when it finds a section named **`.sign`** (every official
     app has one — verified with readelf: AppSetup `[28] .sign`,
     AppHomeScreen `[28] .sign`, AppMediaPlayer, ApplicationManager, AppVH
     all have `.sign`; AppVH additionally ships `/app/bin/AppVH.sign`)
     writes the section contents to the sign path and calls
   - `FUN_001127d0` ("signCheck first/second"): SHA256 over the binary with
     the `.sign` section region zeroed, then `RSA_verify(SHA256, sig,
     PEM_read_RSA_PUBKEY(/tmp/app_normal.key))`, logging
     `signCheck first : [...]` / `second`. Failure of any step
     (`CheckSign false 1/2/3`, `readSection ... fail`, "no exist section")
     returns false → **the process is not started**; the App record is set
     to state 10 and an error path runs (`FUN_00118550`).
   - Navi (0x1e) instead uses the file-based `FUN_00112100` `CheckSign`
     against `/tmp/navi_public.key`.
4. On success: `QProcess::start(program, args, env)`; records the PID
   (`m_AppProcess->start(%1) PID[%2]`), starts an **appLoadTimeout** timer
   (`appLoadTimeoutSlot()`, "[TIMEOUT] Preload complete signal is generated by
   timeout", `ClearTimeoutLoading : %1, PID : %2`).
5. AppLauncher emits `com.lge.avn.applauncher.inform.launched(appid, appname)`
   (adaptor `FUN_00d09...`-family in AppHomeScreen mirror this XML:
   `launched(i,s)`, signals `preloadcomplete`, `appterminate(s appname, i signum)`).

The full D-Bus contract (introspection XML embedded in AppLauncher and
AppHomeScreen, string 0x552e3):

```xml
<interface name="com.lge.avn.applauncher.request">
  <method name="launch"> <arg type="i" name="appid"/>
                         <arg type="s" name="appname"/>
                         <arg type="s" name="applocation"/> </method>
  <method name="launch"> + <arg type="b" name="engcapturemode"/> </method>
  <method name="launch"> + <arg type="b" name="emmctestmode"/> </method>
  <method name="terminate"> <arg type="i" name="appid"/> </method>
  <method name="appkill"> <arg type="s" name="appname"/> </method>
</interface>
<interface name="com.lge.avn.applauncher.inform">
  <method name="launched"> i appid, s appname </method>
  <signal name="preloadcomplete"/>
  <signal name="appterminate"> s appname, i signum </signal>
</interface>
<interface name="com.lge.avn.applauncher.cipher">
  <method name="getIVDHash"> ay hash_data </method>   <!-- DRM/IVD hash helper -->
</interface>
```

### 3.3 Surface creation and foreground grant

- The app connects to Wayland as a Qt/wayland client
  (`QT_QPA_PLATFORM=wayland`, `XDG_RUNTIME_DIR=/var/run/user/root`,
  `QT_WAYLAND_SET_SURFACE_ID=<n>`), so its surface appears on the ivi-shell
  **application layer (id 4000)** with a known ivi surface id.
- ApplicationManager owns the ivi-wm client: `FUN_00291bc0` does
  `iwm_set_app_id(ownId)` + `iwm_init(0)`; `FUN_00291f60` (destructor)
  disconnects the wm signals `notifyFGInfo(uint32_t surface_id, int32_t
  display, QString)`, `notifyBGInfo(...)`, `notifyStopAni()`,
  `notifyVisibleAni(bool)`, `notifyLog(QString)`; the FG-notification slot
  logs `slotNotifyFGInfo surface_id:%1 display:%2 app_id %3`
  (`FUN_00291780`). A second ctor `FUN_00291cc0` registers layer names via
  `iwm_set_app_name(5/6, ...)` and `iwm_set_surface_position_with_app_id`
  for displays 1 and 4, and `iwm_set_app_name(2, "NotificationManager")`.
- `libivi-window-manager-api.so.1.0.0` exports
  `iwm_request_fg_with_app_id` (imported only by ApplicationManager;
  PLT at 0x2b520). The concrete FG-grant decision runs inside the weston
  module `ivi-window-manager.so` (server side, not in this export set).
- ApplicationManager makes FG the app whose ID it tracked:
  `DisplayPlaneBase::setCurrentDisplayGuiInfo a_bAvMode[%2]
  a_nApplicationId[%3] a_nPosition[%4] a_bHistoryUpdate[%5] a_bVRCancel[%6]`
  (`FUN_002894f0`), `DisplayPlaneBase::getDedicatedApplication(%1)` for
  display-plane ownership (`FUN_002892b0`), and `ResponseHandler`
  ("delayTransit", "checkCameraStatus", "needToHideAppID") sequences the
  transitions. The wm's `transition-duration=300` animation plus
  `notifyVisibleAni`/`notifyStopAni` mask the switch.
- Everyone else learns the front app from the
  `appmanager_front_application` signal on `com.lge.avn.appmanager.info`;
  AppHomeScreen's QML exposes it as `AppHomeModel.isAtOtherApp` /
  `currentShowingAppID` (`AppHomeModel::setIsAtOtherApp(bool)`,
  `AppHomeModel::setCurrentAppID(int)` — pretty-strings 0xc4c5a0/0xc4c3e0).

---

## 4. Show / hide / close semantics

**There is no "close" on going home — only a surface transaction.**

- Steering-wheel Home → session-bus method `NotifyRequestHomeScreen(ay)` on
  `com.lge.hkmc.avn.appfunc.appInterface`, delivered to the focused app
  (established in UI_CONTEXT.md; AppHomeScreen registers
  `com.lge.hkmc.avn.appfunc.AppHomeScreen` (string 0xc54470) and the
  appfunc bus name pattern is `com.lge.hkmc.avn.appfunc.%1` with object path
  `/com/lge/hkmc/avn/appfunc/%1` — strings 0xc5ed60/0xc5f558/0xc8f175).
- ApplicationManager runs a **GoHome transaction** (strings
  `GoXXXHome::transit getCurrentDrawingApplicationId[%1] getAppID[%2] do not
  send requestshow`, `GoXXXHome::calcHistory plan[%1] AppId[%2]`,
  `GoHome`, `goHomeTransactionForConnectivity a_nAppId[%1] a_nPlane[%2]`):
  it requests the FG for appId 11 (AppHomeScreen) and pushes the previous
  app onto the history stack (`Application::AppHistoryApppend [%1] << [%2]
  cnt[%3]`, `Application::clearAppHistory [%1]`, `Application::~Application()
  %1`). **The previous app process keeps running** with its Wayland surface
  intact (BG), which is why AppSetup/AppMediaPlayer keep all state — the
  process was never killed; `notifyBGInfo` just demoted its surface. Back /
  last-app behavior pops the history (`istransactionapp_vr`, "hitory modify
  for Bt Music").
- **Real termination** exists only via AppLauncher:
  - `terminate(appid)` → `[onDemandTerminate] appId[%1][%2] terminate`
    (`FUN_0011f2e0`): finds the App record, sets state 10, calls the
    QProcess terminate path (`FUN_00115030`: `QProcess::terminate()` =
    SIGTERM, disconnect `finished(int,QProcess::ExitStatus)` /
    `onFinishedSlot`), removes it from the list, emits `appterminate(appname,
    signum)`. If the ID isn't running: `[onDemandTerminate] ... finally not
    found` (and `killAll AppEngineerMode` special case).
  - `appkill(appname)` → `[onDemandAppKill] appId[%1][%2] is killed by
    AppManager`.
  - The proven on-car use: `terminate(11)` + `launch(11, "AppHomeScreen",
    "/app/bin/AppHomeScreen")` restarts the whole home screen (Hello
    installer, battery installer — both verified on the car).
- **Suspend/resume**: no process-level suspend exists in AppLauncher/
  ApplicationManager strings (no "suspend"/"resume" hits). Backgrounding is
  purely "surface demoted + app keeps running"; apps that care register
  their own handlers via their appfunc interface
  (`com.lge.hkmc.avn.appfunc.<AppName>`, e.g. `...appfunc.AppSetup`,
  `...appfunc.NotificationManager`, `...appfunc.assistantwindow.common`,
  `...appfunc.appphev.phevinterface`).

---

## 5. App process structure (AppSetup as reference)

### 5.1 Bootstrap

AppSetup is a `QGuiApplication` + `QQuickView` app (imports:
`QQuickView`, `setSource`, `QQmlEngine::addImageProvider`, …). Its `main`
(FUN_001d8810, reached from `entry` at 0x283310 via `__libc_start_main`) is
one of the decompiler-lost bodies, but the observable contract is:

- env inherited from AppLauncher: `DBUS_SESSION_BUS_ADDRESS`
  (from `/tmp/session_appmgr.env`), `QT_QPA_PLATFORM=wayland`,
  `XDG_RUNTIME_DIR=/var/run/user/root`, `LD_LIBRARY_PATH=/app/lib`,
  `LD_PRELOAD=/usr/lib/libEGL.so.1`, QV4 interpreter flags.
- argv from `setAppLaunchArguments` (CarOption.csv row): `-gui:N -vc:.. -sc:..
  -dn:.. -ke:.. -pf:.. -os:.. -un:.. -er:.. -is:..` etc.
- It registers with ApplicationManager via
  `com.lge.avn.appmanager.registration` (`/registration`, appid+appname),
  advertises `com.lge.hkmc.avn.appfunc.AppSetup` on the session bus, and
  uses the ivi-wm API itself: imports `iwm_init`, `iwm_deinit`,
  `iwm_set_surface_layout`, `iwm_set_parent_child_surface`.
- It loads QML from `/app/share/AppSetup/qml_<variant>/` and images from
  `/app/share/images/new_gui/<variant>/<hmc|kmc>/<day|night>/...`
  (strings 0x9b1487 `new_gui/new2.0/`, 0x9b1497 `genesis/`,
  0x9b14a0 `new_gui/eco3.0/`, plus the loading spinner GIF
  `.../general/co_ic_loading_l.gif` in all six brand/day combos in
  AppHomeScreen).

### 5.2 Theme-variant selection — the `-gui` argument

In AppSetup the qml dir is picked by the **`-gui` argv value** (objdump of
AppSetup 0x2d7215–0x2d72d9: key `-gui` @0x9b12a4 is parsed into
`this+0xb4`, then):

| `-gui` value | QML dir |
|---|---|
| 1 | `/app/share/AppSetup/qml_new2.0/` (lea 0x9b45c0) |
| 2 | `/app/share/AppSetup/qml_genesis/` (lea 0x9b45e0) |
| 3 | `/app/share/AppSetup/qml_eco3.0/` (lea 0x9b4608) |
| other | fall-through default |

AppHomeScreen has the same trio (`/app/share/AppHomeScreen/qml_genesis/`
string 0xc54df8; `new_gui/new2.0/`, `genesis/`, `new_gui/eco3.0/` at
0xc51e55–0xc51e6e). **This car runs eco3.0**, i.e. AppLauncher passes
`-gui:3` for the NE row — but the embedded fallback table only contains
`-gui:1` rows (CN7, SG2, US4, NX4, …; no NE entry), so the live car's
`/app/bin/CarOption.csv` must carry the NE row with `-gui:3` (see §8).

---

## 6. Splash / loading

There is no per-app splash app. Startup is masked by three mechanisms:

1. **Boot**: `AppStandbyClock` (app ID 10, group 1) draws the standby clock
   while the heavy apps preload; ApplicationManager gates the sequence
   ("welcomeReady" strings 0x2fcad0/0x2fceb9, `askSysBootinfo bootType[%1]`).
2. **Home screen boot**: `Screen_HomeScreen_Loading.qml` — full-screen black
   `Rectangle` + centered 117 px spinning `co_ic_loading_l.gif`
   (`defineModel.img_loading` frame list), released when
   `AppHomeModel.isNaviBootCompleted` flips (10 s `aliveScreenTimer` fallback
   sends `EVT_BACK_TO_PREV_SCREEN`). There is also a logical
   `APP_NAVI_LOADING` app ID in ApplicationManager for the navi cold start.
3. **App switching**: the weston ivi-wm runs the 300 ms layer transition
   (`transition-duration=300`, `notifyVisibleAni`/`notifyStopAni`) so the
   previous surface stays visible until the new one is ready — no black
   gap, no per-app splash image. AppLauncher additionally watches each
   spawn with the `appLoadTimeout` timer and the `preloadcomplete` signal.

---

## 7. The two deployment models

### 7.A Official way: a new separate-process app

What the firmware says a first-class app must do/be:

1. **An app ID** — must exist in AppLauncher's hardcoded table
   (`FUN_0010b080`) and in ApplicationManager's name table; the preload
   list only references names. Without a table entry,
   `registeredFromNativeApp` rejects with `WrongAppID`. Third parties cannot
   add IDs without patching both binaries (or reusing an existing ID).
2. **A binary with a valid `.sign` ELF section** — AppLauncher extracts the
   section and RSA-verifies the SHA256 of the binary (sign region zeroed)
   against `/tmp/app_normal.key` (`FUN_001158c0` → `FUN_00115b50` →
   `FUN_001127d0`). **The private key is not in the extract**; signing a new
   binary is not possible from the firmware alone. (Workarounds: launch via
   a shell/daemon instead of AppLauncher — no sign check applies to
   processes AppLauncher doesn't spawn; or patch AppLauncher; both are
   unofficial.)
3. **A QML share dir** `/app/share/<App>/qml_<variant>/` and the
   `-gui:N`-selected variant; assets under
   `/app/share/images/new_gui/<variant>/<brand>/<day|night>/`.
4. **Session-bus services**: register with
   `com.lge.avn.appmanager.registration` (appid, appname); advertise
   `com.lge.hkmc.avn.appfunc.<AppName>` at
   `/com/lge/hkmc/avn/appfunc/<AppName>` (must at least accept
   `NotifyRequestHomeScreen`); call `launched` → AppLauncher
   (`com.lge.avn.applauncher.inform.launched`) to clear the load timeout.
5. **A Wayland surface**: Qt/wayland with the `QT_WAYLAND_SET_SURFACE_ID`
   AppLauncher assigns (or default 0), on ivi application layer 4000; use
   `libivi-window-manager-api.so` (`iwm_init`, `iwm_set_surface_layout`)
   like AppSetup does.
6. **A grid entry**: an itemID in AppHomeScreen's C++ item model (icon,
   STR_ label, app id) — again a firmware rebuild, since the grid is not
   config-driven.

Practical "official-ish" hybrid (root required, no firmware rebuild): keep
the app as a **root-owned daemon/service** (like `battery-daemon.service`)
and integrate visually via model B below; or spawn it from a patched
AppLauncher bypass path. The strict official path needs the vendor signing
key — a hard blocker.

### 7.B Proven unofficial way: tile + overlay inside AppHomeScreen

Verified on the car (Hello, WiFi, Tweaks, Battery). The pattern:

1. **Patch stock QML** in `/app/share/AppHomeScreen/qml_eco3.0/`
   (`QHMI_HomeScreenMain_Advanced.qml` and friends), installed via the
   `initial_root` USB failsafe (`run_stuff.sh`): manifest check, original-
   hash guard, backup to `/rw_data/<app>/backup/<ts>/`, remount `/` rw,
   copy with mode/owner/SMACK-label preservation, clear QML caches
   (`/root/.cache/qtcache` etc.), remount `/` ro.
2. **Tile**: a custom `component/<App>Tile.qml` (standard tile background +
   `file://` PNG icon — plain PNGs avoid re-encoding ASTC textures; a small
   `Q_ThemeImage.qml` patch skips the `#GUI_DAY/NIGHT` suffix for `file://`
   URLs), placed with the shared **`firstEmptySlot(customIndex)` allocator**
   (battery_app artifacts `QHMI_HomeScreenMain_Advanced.qml:131`): walk
   `emptySlots(getPageDisplayListModel(p))` across pages, take the
   `customIndex`-th genuinely empty cell (page models are padded, so
   `getCount()` alone lies). Apps install in order (Hello→WiFi→Tweaks→
   Battery) and stack instead of colliding. Coordinates:
   `x = menuPage.x + (slot.index % k_nbrOfColumns) * k_itemWidth`,
   `y = menuPage.y + floor(slot.index / k_nbrOfColumns) * k_itemHeight`.
3. **Overlay**: a full-screen `Item` at `y: ConstantModel.MENU_BAR_Y`
   (90 px status bar), `height: DEVICE_HEIGHT - MENU_BAR_Y` (630),
   `z: 2000`, opened by the tile (`overlay.open()`), owning its visibility:
   `visible: requested && !AppHomeModel.isAtOtherApp`, closing on
   back / `isAtOtherApp` / `currentScreen` / `currentHomeAdvancedPageIdx` /
   `requestHomeByPP`, with a `preventStealing` all-buttons wheel-accepting
   MouseArea shield at `z:1` and content at `z:2`
   (APP_OVERLAY_CHECKLIST.md pattern; battery artifacts line 604).
4. **Home/Star key**: QML cannot see it (session-bus event), so a daemon
   (systemd unit like `battery-daemon.service`, `Restart=always`) runs
   `dbus-monitor --address $(cat /tmp/session_appmgr)` with match rules for
   `NotifyRequestHomeScreen` (com.lge.hkmc.avn.appfunc.appInterface) and
   `custom_key_value` (com.lge.avn.appmanager.info) and exposes a `home_seq`
   counter over local HTTP (`127.0.0.1:8088/status`); the overlay closes on
   any bump.
5. **Restart the UI** through the vendor path so env/bus are correct:
   `dbus-send --session --dest=com.lge.avn.applauncher /Request
   com.lge.avn.applauncher.request.terminate int32:11` then
   `...request.launch int32:11 string:"AppHomeScreen"
   string:"/app/bin/AppHomeScreen"` — reading
   `DBUS_SESSION_BUS_ADDRESS`/`WAYLAND_DISPLAY` from
   `/proc/<launcher pid>/environ` (battery run_stuff.sh:250–276).

**Known limitations vs a real process** (from SPEC.md §10, HELLO §5, plus
the lifecycle facts above):

- No app ID, no AppLauncher/ApplicationManager registration: the overlay is
  *part of* AppHomeScreen's surface. It can never own the foreground, so it
  must self-close when `isAtOtherApp` becomes true and cannot use
  `NotifyRequestHomeScreen` (that would go to AppHomeScreen itself only when
  the home screen is focused).
- No independent lifecycle: dies with every AppHomeScreen restart, is
  subject to Qt QML caching (caches must be cleared), and shares one
  process crash domain with the home screen.
- No QML D-Bus: everything the page needs beyond factory models
  (`AppPHEVModel`, `AppNModeModel`, `appClimateModel`, …) must come from a
  helper daemon over HTTP/files.
- Input is faked: the shield MouseArea blocks fall-through but the overlay
  still lives under the 90 px status bar and cannot cover the Assistant
  Window unless declared in `main.qml` at root level (AW is sibling
  `loader_3`).
- Foreground masking is unavailable: no weston layer of its own, no
  transition animation, no `preloadcomplete`.
- Survives only as long as the patched QML matches the firmware (guards on
  the 260128 hashes); a firmware update reverts it.

---

## 8. Known unknowns (need the live car)

1. **Exact `/rw_data/applaunch-config.json`** on the car: the extract's
   `rw_data/` is empty (runtime data). Presumably it mirrors the built-in
   default (§2.2), but the real contents, and whether the launcher rewrites
   it (e.g. "LastAVApp move to Group" persistence), is unverified.
2. **`/app/bin/CarOption.csv`**: absent from the extracted rootfs image but
   referenced by AppLauncher; the embedded fallback table has **no NE
   (Ioniq 5) row**, so the live file must exist (install-time generated?)
   with the NE row carrying `-gui:3` (eco3.0). Need to read it on the car to
   confirm the full NE argument set (`-vc`, `-sc`, `-ke`, …).
3. **Permission model of `com.lge.avn.appmanager.registration` and
   `com.lge.avn.applauncher.request`**: any process on the session bus can
   call `launch/terminate` (the Hello/battery installers already do), but
   whether ApplicationManager accepts *new* third-party app IDs at runtime
   (vs. only known IDs), and what SMACK labels are required to talk to the
   bus (`App::micommanager`-style labels are known to matter for micom
   calls), needs live testing.
4. **Signature enforcement details**: whether the state-10 sign-failure path
   is retried, whether `checkBin` (ELF dyn-section validation,
   FUN_00111e20/FUN_001120c0) also gates every launch, whether an
   engineer/test mode disables the check (`/app/bin/enable_gui`,
   autotest.conf), and where the `.sign` private keys live (not in the
   extract).
5. **FG-grant internals**: `iwm_request_fg_with_app_id` is imported by
   ApplicationManager but its call sites are inside the weston module
   protocol; the weston-side decision logic (`ivi-window-manager.so`) is
   not in the analyzed export set.
6. **Surface ID assignment for default-0 apps** (AppHomeScreen, AppSetup):
   whether their ivi surface ids come from the app's own
   `iwm_set_app_id`/config or from the wm.
7. **The `com.lge.avn.applauncher.cipher.getIVDHash`** method's role in
   launch (DRM/IVD handshake?) — unexplored.

---

## 9. Citation index

| Fact | Where |
|---|---|
| App ID table | AppLauncher `FUN_0010b080` (functions_0001.c:3775) |
| applaunch-config.json parse | AppLauncher `FUN_00139b60` "ApplicationLoader::makeAppGroupLaunchList" (functions_0001.c:26098); default JSON @0x4b1f0 |
| CarOption.csv / `-gui:%1` args | AppLauncher strings 0x526f8/0x52605; `setAppLaunchArguments` functions_0001.c:23780+; fallback table 0x4b1f0–0x52248 |
| Request::launch | AppLauncher `FUN_00126340` (functions_0001.c:16485) |
| Spawn + env + surface id | AppLauncher `FUN_001163f0` (functions_0001.c:8289); `FUN_00114e50` surface table @0x475c0 |
| `.sign` extraction & verify | `FUN_001158c0` (paths), `FUN_00115b50` (readSection, libelf), `FUN_001127d0` (signCheck, RSA/SHA256 vs `/tmp/app_normal.key`), `FUN_00112100` (navi CheckSign) |
| registeredFromNativeApp | AppLauncher `FUN_0011e970` (functions_0001.c:12735) |
| onDemand launch/terminate | AppLauncher functions_0001.c:13420 (launch), 12956 (terminate), `[onDemandAppKill]` strings 0x47c28 |
| ivi-wm client | ApplicationManager `FUN_00291bc0`/`FUN_00291cc0`/`FUN_00291f60`/`FUN_00291780` (functions_0001.c:14541–14705) |
| GoHome/history | ApplicationManager strings 0x303e80–0x303f70, 0x2fe100–0x2fe188, `GoHome` 0x323f98 |
| appfunc/registration interfaces | AppHomeScreen strings 0xc5ed60/0xc5f558/0xc54470; ApplicationManager strings 0x33e9e2–0x33e9f0, 0x33b1a2; AppSetup strings 0x9bd240–0x9bd2a8 |
| applauncher D-Bus XML | AppLauncher/AppHomeScreen strings 0x552e3/0xdb06fb |
| `-gui` theme select | AppSetup disasm 0x2d7215–0x2d72d9 (keys `-gui`@0x9b12a4; 1→qml_new2.0@0x9b45c0, 2→qml_genesis@0x9b45e0, 3→qml_eco3.0@0x9b4608) |
| Tile click event | `QHMI_HomeScreenMain_Advanced.qml:225/266/307`; handler pretty-strings 0xc5d060/0xc5d020 |
| systemd units | rootfs `lib/systemd/system/Dbuslaunch.service`, `ApplicationLauncher.service`; `app/bin/Dbuslaunch.sh`, `AppLauncher.sh` |
| weston/ivi config | rootfs `etc/xdg/weston/weston_1920.ini`; `app/lib/weston/ivi-window-manager.so` |
| Tile+overlay pattern | battery_app artifacts `QHMI_HomeScreenMain_Advanced.qml:131,269,604`; `BatteryPage.qml`; `battery-daemon.service`; `run_stuff.sh:250–276`; APP_OVERLAY_CHECKLIST.md |
