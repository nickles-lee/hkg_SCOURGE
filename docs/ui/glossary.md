# Gen5W UI glossary — human words → precise technical terms

Use the right-hand term in code, issues, and agent prompts. Firmware
`260128`, theme `qml_eco3.0`. Sources: stock QML trees, Ghidra decompiles, and
the reports in `docs/ui/reports/`. Expands `docs/ui/TERMINOLOGY.md`.

## Home screen anatomy

| Colloquial | Technical | Where |
|---|---|---|
| Home screen (the app) | `AppHomeScreen`, process (app ID **11**); Advanced layout = `QHMI_HomeScreenMain_Advanced.qml`, Basic layout = `QHMI_HomeScreenMain_Wide.qml` | `/app/bin/AppHomeScreen`, QML in `/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/` |
| Home screen (the visible surface) | the Advanced home: `QListViewHomePage` (`id: homepage`, `objectName: "lsvHomePage"`) carousel + page indicator | `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:320-395` |
| Swipe page / carousel page | a page `Component` in the `getModelList()` array: index 0 = `pluginPage`, 1..3 = `listPage1..3`, appended custom pages after | `QHMI_HomeScreenMain_Advanced.qml:75-85` |
| Page dots | `indicatorGroup` Row of `Q_ThemeImage` dots (`home_ic_indicator_w_s/n`), 39 px + 9 px spacing, auto-hide 5 s | `QHMI_HomeScreenMain_Advanced.qml:403-455` |
| Widget page / first page | `pluginPage`: empty `Item` with `objectName: "itemWidgetPlaceHolder"`; C++ injects `Screen_HomeScreen_Widget_Advanced.qml` (clock, navi map, weather, EV widget) | `QHMI_HomeScreenMain_Advanced.qml:171-194` |
| App icon / tile | `QMenuItemNoDrag` delegate inside `QMenuPageNoDrag` (fixed grid); draggable variants: `QMenuItemAllowEmpty` in `QMenuPageAllowEmpty`/`QMenuPageTotal`; Information-submenu tile: `QHomeItem` | `component/QMenuPageNoDrag.qml`, `component/QMenuItemNoDragComp.qml` |
| App grid page | `listPage1..3`, each an `Item` wrapping a `QMenuPageNoDrag` fed by C++ models `AllMenuPage1..3Display` (hard cap 3 pages, 12 items/page wide, 8 narrow, cells 306×249) | `QHMI_HomeScreenMain_Advanced.qml:196-318` |
| Submenu screen | `Screen_HomeScreen_Submenu_*.qml` (Setup, Media, Information, DataService, Edit*) — separate screens loaded by `loader_1/2`, **not** carousel pages | `HomeScreen/` |
| All-apps screen | `Screen_HomeScreen_AllMenus.qml` (uses `QCustomizedMenuListAllowEmpty`) | `HomeScreen/` |
| Card (this project's term) | **a persistent custom page appended to `getModelList()`** — full 1280/1920×720 canvas, always alive, reached by swiping; see styleguide §9 model B | `reports/card-page.md` |
| Overlay (this project's term) | a full-screen `Item` at `z: 2000` inside a page, with `open()/close()`, `visible: requested && !AppHomeModel.isAtOtherApp` | `docs/ui/APP_OVERLAY_CHECKLIST.md` |
| Status bar / clock bar | the top **90 px**; embedded per-app via `libStatusbar.so` as `STComponent` in `loader_4` (z 2) | `HomeScreen/main.qml:306-319` |
| Split screen / Assistant Window (AW) | `loader_3` sibling layer, 640×720, source `AWComponent` from `libAssistantWindow.so`; **disabled on this car** (`AppHomeModel.screenNotSupportSS = true`) | `HomeScreen/main.qml:139-203` |
| Option menu (status-bar "Menu" dropdown) | `popup/QOptionMenu.qml` — full-screen shield + list panel at y 90, rows 104 px | `popup/QOptionMenu.qml` |

## Apps & processes

| Colloquial | Technical | Where |
|---|---|---|
| App (official) | a separate process with a **hardcoded app ID** in AppLauncher's table (`FUN_0010b080`), launched via `com.lge.avn.applauncher.request.launch(appid, appname, applocation)`, `.sign`-verified | `reports/lifecycle.md` §2-3 |
| App (ours, model A) | tile + overlay **inside AppHomeScreen's QML** — not a process; see "Overlay" above | styleguide §9.A |
| App ID | compile-time constant, e.g. 11 = AppHomeScreen, 60 = AppSetup, 30 = AppNavi, 100 = AppMediaPlayer, 22 = AppOSDManager, 17 = NotificationManager | AppLauncher `FUN_0010b080` |
| Front app / foreground app | the app whose Wayland surface the ivi window manager shows; tracked in QML via `AppHomeModel.isAtOtherApp` / `currentShowingAppID`; FG granted via `iwm_request_fg_with_app_id` | `ApplicationManager`, `libivi-window-manager-api.so` |
| "Closing" an app | **doesn't exist for Home** — GoHome is a surface transaction + history push (`AppHistoryApppend`); the process keeps running. Real kill: `terminate(appid)` / `appkill(appname)` (SIGTERM, `appterminate` signal) | `reports/lifecycle.md` §4 |
| Preload groups | `/rw_data/applaunch-config.json` `{"0":[{appName, dependAppName}],...}` — boot launch order only (no IDs/icons) | AppLauncher `makeAppGroupLaunchList` |
| Splash screen | none per-app; boot = AppStandbyClock, home boot = `Screen_HomeScreen_Loading.qml` spinner, app switches masked by weston's 300 ms layer transition | `reports/lifecycle.md` §6 |
| Theme variant / skin | `guiThemeType`: 1 = `qml_new2.0`, 2 = `qml_genesis`, **3 = `qml_eco3.0`** (this car, via `-gui:3` from `CarOption.csv`); assets under `/app/share/images/new_gui/<variant>/{hmc\|kmc}/{day\|night}/` | `reports/lifecycle.md` §5.2 |
| Widget / home-screen widget | a plugin card on the widget page: `*.pluginspec` + `lib*.so` + QML under `/app/plugins/<Name>/qml/`; host `QWidgetItem` owns press (whole-card `_p` overlay → `EVT_MAIN_LIST_ITEMCLICKED`) | `/app/plugins`, `component/QWidgetItem.qml` |
| Plugin (QML type) | C++-registered component, e.g. `PluginKeyboardArea` (`import Keyboard 1.0`), `STComponent`, `AWComponent` | `libStatusbar.so` etc. |

## Models, events, buses (the QML environment)

| Colloquial | Technical | Where |
|---|---|---|
| The model / app state | `AppHomeModel` — 162-property C++ context object: `isWideScr`, `isAtOtherApp`, `sysPopupShowing`, `visibleOSDCall`, `currentScreen`, `currentHomeAdvancedPageIdx`, `dayLightSuffix`, `textUpdate`, `focusHKThisApp`, `disableTouch`, … | AppHomeScreen binary; `AppHomeEngine::initEngine()` |
| Constants / design tokens | `ConstantModel` (C++ `QMLConstantModel`, 327 props in AppHomeScreen) — geometry, colors, font sizes; every app has its own | `docs/ui/tokens.md` |
| Image paths | `defineModel` (C++ `QMLdefineModel`, 3 901 props) — `image://AppHomeScreen/<category>/<file>.png` keys + 15 colors | `tokens.md` §3 |
| Theme colors | `HomeThemeHelper` (C++ `ThemeHelper`, 58 runtime-switched QString roles: normal/press/selected/focus/disable × families) | `tokens.md` §2 |
| Translations | `TranslationsModel` — `STR_*` properties + `getDefineLocalize()`; append `+ AppHomeModel.textUpdate` to bindings | `localization/ts/*.ts` (26 languages) |
| Events | `HMIEVENTS 1.0` — per-binary registered enum (214 names in AppHomeScreen); `EVT_GOTO_*` navigation, `EVT_MAIN_LIST_ITEMCLICKED` app launch, `EVT_HOME_SOUND_BEEP_PLAY` beep, `EVT_REQUEST_HIDE_POPUP`, sent via `QML_Handler.qmlSendEvtKey(evt[, arg])`; screen changes via `QML_Handler.qmlChangeTopMsg(evt)` | `reports/surfaces-input.md` §5 |
| The engine | `AppHomeEngine` — QML-callable C++ bridge: `slotReqInjectAdvancedWidget`, `requestHideGlobalPopup`, `advancedModePageChange`, `request_clock`, … | AppHomeScreen binary |
| Session bus (app manager) | D-Bus session daemon whose address `Dbuslaunch.sh` writes to `/tmp/session_appmgr` (env `/tmp/session_appmgr.env`); carries appmanager/appfunc traffic. **Distinct from the system bus** (micom/CAN) | `app/bin/Dbuslaunch.sh` |
| Home request (steering-wheel Home) | session-bus method `NotifyRequestHomeScreen(ay)` on `com.lge.hkmc.avn.appfunc.appInterface`, delivered to the **focused app** — never a QML key event | `UI_CONTEXT.md` Rule 2 |
| Star / custom key | steering-wheel user-configurable button; signal `custom_key_value(i)` on `com.lge.avn.appmanager.info` — never a QML key event | same |
| Hard keys generally | CAN → micom system bus (`com.lge.car.micom.can`, `NotifyKeySWRCEvent`) → ApplicationManager → per-app session-bus calls (`swrckey(i,i)`, `faceplatekey`…). QML sees only `focusHKThisApp` | `reports/surfaces-input.md` §6.2 |
| Screen change mechanism | `qmlChangeTopMsg` → `ScrController` → `AppManager`/`ScreenQueue` → context property `screenNext` → root `screen_Transition()` swaps double-buffered `loader_1/2` | `HomeScreen/main.qml:40-78` |
| System popup | global popup from the **NotificationManager** process (86 IDs in `noti_popup.xml`, types A–E), surface above all apps; app sees `AppHomeModel.sysPopupShowing` | `reports/surfaces-input.md` §2 |
| App-local popup | `popup/*.qml` in AppHomeScreen, **loaded by C++ by path** on `KEY_EVT_SHOW_POPUP_*`; each has a unique `_OSD_ID`; close via `EVT_REQUEST_HIDE_POPUP, _OSD_ID` | `popup/QHMI_AppHomePopup_OK.qml` |
| OSD | AppOSDManager process strips: 90 px top (general/alert/navi/climate/media) or 75 px bottom (volume); timed (`systemOSD.xml`, 100 ms units), touch-dismiss | `reports/surfaces-input.md` §3 |
| Beep | not audio in QML — an event (`EVT_HOME_SOUND_BEEP_PLAY` etc.) played by C++ | `reports/surfaces-input.md` §8 |

## Styling vocabulary

| Colloquial | Technical | Value/where |
|---|---|---|
| Day/night mode | `AppHomeModel.dayLightSuffix` = `"#GUI_DAY"`/`"#GUI_NIGHT"`, appended to image URLs by `Q_ThemeImage`; provider maps `@`→`d`/`n`; also `/tmp/GUI_THEME_DAY\|NIGHT` marker files | `component/common/Q_ThemeImage.qml` |
| Pressed look | swap to `_p` asset via 4-state array `icons[_NORMAL,_FOCUSED,_PRESSED,_DISABLE]` | `component/common/QButton.qml` |
| Disabled look | `_d` asset; text dim opacity day 0.2 / night 0.35 | `QMenuItemNoDragComp.qml:60` |
| Narrow-screen asset | `_s` suffix (e.g. `co_bg_box_line_menu_s`, `home_bg_icon_edit_s_p`) | `defineModel` keys |
| The font | `HKMC_FONT_R` / `HKMC_FONT_B` (Cube family in eco3.0); sizes via `ConstantModel.FONT_SIZE_*` | `tokens.md` §4 |
| The house duration | `ConstantModel.animDuration` = 300; page transition 150 ms; popup 100 ms | `tokens.md` §5 |
| Marquee / ticker | dual-Text scroller: 1 s pause → scroll `(w+120)/50*1000` ms → 1.5 s pause (AppMediaPlayer `QHMI_ScrollText` = reference; some `Q_MarqueeText.qml` files are inert wrappers) | `reports/interaction-i18n.md` §4.4 |
| Scrollbar | `Q_StraightScrollBar` / `QHMI_StraightScrollBar`, 15 px wide, masked theme images, **visible only on overflow** | `components.md` |
| Loading spinner | `defineModel.img_loading[]` frame list, 117×117, 100 ms/frame + `STR_HOME_WIDGET_PREPARING` | `component/common/QLoadingComponent.qml` |
| Keyboard / OSK | `PluginKeyboardArea` (`import Keyboard 1.0`, native `libdkbd`); numeric keypad `QHMI_KeyBoardNumerical.qml` | `components.md` §OSK |
