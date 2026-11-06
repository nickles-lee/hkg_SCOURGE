# Widget plugin system — deep-dive (round 2)

Firmware 260128. Method note: AppHomeScreen's Ghidra export is badly degraded
in the loader region (`syscall(0xba)` stubs); the **same plugin framework
(WidgetPlugin/PluginSpec/WidgetManager/WidgetInterface) is compiled into
`libAssistantWindow.so`** (`bin_ab207982a4cc234b`) with names intact — mechanics
decompiled there, cross-verified against AppHomeScreen's strings
(`AHS:` = bin_3208391a859693e9, `AW:` = libAssistantWindow).

## Discovery & pluginspec parsing

- `WidgetManager::loadWidget("/app/plugins", bootInfo, ecoCarVariant, naviOK)`
  (AW 0x15c730): `QDir::entryList` **subdirectories only**, then per folder
  `new WidgetPlugin("/app/plugins/"+folder)`. AHS strings: `/app/plugins`
  0xc51edc, `*.pluginspec` 0xc61fe8, `"Exist widget: "` 0xc61ab0,
  `"number plug-in was loaded: "` 0xc61b38.
- `WidgetPlugin::initialize()` (AW 0x161190): first valid `*.pluginspec` wins;
  then **all `*.so`** in the folder collected (QLibrary suffix-independent —
  picks up `lib*.so.1.0.0`). `m_mainQMLPath = folder + "/qml/" + spec.mainqml`.
  Validity: `if (m_libraryPaths.isEmpty() && m_mainQMLPath.isEmpty())
  m_isValidPlugin = false` — **mainqml-only stays valid**.
- `PluginSpec::loadPluginSpec` (AW 0x159b30): QDomDocument("plugindocument");
  root tag must be `plugin` (0xa7ca0). Keys (case-insensitive): name, version,
  compatversion, vendor, copyright, license, category, description, url,
  mainqml — root attributes + child element text both accepted. **No signature
  check, no Qt plugin metadata — plain QLibrary.**
- `loadPlugin()` (AW 0x162240; AHS twins loadPlugin/NMode/EV, Q_FUNC_INFO
  strings 0xc62000-0xc62080): no .so → false; `QLibrary::load()` →
  `resolve("getObject")` → `((WidgetInterface*(*)(int))sym)(scrType)` — AW
  hardcodes scrType=2. scrType semantics: 0=FRONT, 1=REAR, 2=AW
  (`plugins/WeatherWidget/qml/main.qml:4-6`).
- Per-name filter in loadWidget: default branch **appends any valid plugin**;
  only hardcoded names are conditionally dropped (NaviPlugin/CompassPlugin
  when !naviOK; variant-specific: DriverTalk/Weather/CcsWeather/PHEV/
  DrvAccel/DrvSpeed/NModeAw). `loadPluginNMode/loadWidgetNMode` and
  `loadPluginEV/loadWidgetEV` = drive-mode/vehicle-dependent plugin sets
  (N-mode gauge plugins at 0xc9092b-0xc909b6).

## Models & injection

- `AppHomeModel` properties: `listWidget, listWidgetAdv, listWidgetSetting,
  listWidgetSettingAdv, naviWidget, editHomeWidgetPos, listAWWidget,
  widgetIndex` + methods `reqLoadWidget, reqInjectAdvancedWidget,
  selectWidget, editSettingWidget(Advanced), creatListWidgetSetting,
  resetWidgetChecked, updateSelectedWidget(Adv), updateLisWidgetAdv,
  lstWidgetClear, sendListWidgetToVCRM` (strings 0xca5d71-0xca6029).
- Roles (from QML): `listWidget[n]` = `{sourceInput (QQmlComponent), label
  (= spec name), itemID}`; `listWidgetSetting(Adv)[i]` = `{slotID, label
  (STR_HOME_*), isChecked, isVisible}`. Launch IDs built as
  `"ITEM_ID_PLUGIN_" + name` (0xc4821b); lookup failures log
  `"could not found widget ID of %1"` (0xc61b80);
  `"Widget is not supported: "`/`"Recreate the Widget list"` (0xc4823f/6d).
- Injection: `slotReqInjectAdvancedWidget` → find `ldCurrentLoader` →
  `lsvHomePage` → contentItem → `itemWidgetPlaceHolder` (strings 0xc51333-88,
  0xc542b0-0xc54308) → per selected widget `plugin->getComponent()` →
  `create()` + `setParentItem(placeholder)`.
- `getComponent()` (AW 0x160c90): invalid → null; **with .so → the plugin's
  own** (ClockWidget.so hardcodes `QHMI_ClockWidget_ScreenMain_{Front,AW}.qml`);
  **without .so → `QQmlComponent(engine, m_mainQMLPath)`** (host loads
  `<folder>/qml/<mainqml>`). But `sendEventToWidget` no-ops without
  `m_widgetObj` (AHS FUN_00945cc0) — QML-only plugins get no event pushes.
- Host context: `AdvancedWidgetQMLInstance` (0xc5249c) →
  `setButtonWidgetActive` into the page (`Advanced.qml:180-187`).

## Layout persistence

SQLite `/app/data/AppHomeScreen/CustomLayoutDB/CustomLayoutDB` (0xc49848,
QSQLITE, connection `CustomLayoutDB_Connection`): tables `MenuHome`,
`CustomLayoutFront`, `CustomLayoutVersion` (per-user-profile layout versions:
`:Home_All_Menu_version,:Home_Setup_Menu_version,:Home_Media_Gate_version,
:Home_User1/2/Guest_Edit`), `WidgetSettingFront` (`:WidgetSetting_Key,
:widgetSlotID,:screenSize,:isSelected`; INSERT log 0xc46a15). Boot order via
`getListWidgetAdvSLotIDOrderFromDB()` (0xc4c280) into `_listWidgetDBOrder[` +
`_slotID: ` (0xc481e1/f5); mismatch → `"not default widget"`/`"_wrongDB: "`
(0xc481ce/c3); first boot → `"Customize Home data is not available -> create
new"` (0xc499c8). A NEW widget gets a slot **only by user drag in Edit-Widget**
(row INSERTed on first check).

## Plugin QML contract

- With .so (all 24 stock plugins): `getObject` → singleton implementing
  `WidgetInterface` (`getComponent/initialize/sendEventToWidget/
  sendHardkeyEventToWidget/release/fullSize`, `plugins/common/WidgetInterface.h`);
  host provides `ICallbackInterface::{callBackToApp, getRootObject,
  getRootContext, getImageProviderID}`. Plugins register their own models
  (e.g. `WeatherWidgetModelFront`) and may use `image://AppHomeScreen/`.
- Touch→host: `signal sendMouseEventToHostApp(int eventId)` on the plugin
  root (BTAudioWidget main_Front.qml:9; 4=pressed, 6=released); host connects
  on Loader success (`QWidgetItem.qml:52-54`). Host owns press/launch/hold.
- Sizes: Front host `QWidgetItem` 627×405; Advanced variants 624×576
  (ClockWidget Front_Digital.qml:18-19); AW 640-wide.
- Loading: host `Loader{sourceComponent: model.sourceInput}` + spinner until
  Ready (`QWidgetItem.qml:31-44`).

## Feasibility verdict

| Question | Answer |
|---|---|
| Loads? | **Yes (high confidence)** — no whitelist; spec-only stays valid; QML fallback path exists |
| Appears on widget page? | **Probably not** — placement from `WidgetSettingFront` DB; new widget has no row |
| Appears in Edit-Widget picker? | **Doubtful** — `STR_HOME_*` labels + icon dictionary hardcoded per known name |
| Placeable? | Only if the picker accepts it |
| Real blocker | The name-keyed label/icon/ID mapping in AppHomeModel — **not** the .so (no signature check) |
| QML-only precedent | None stock (all 24 ship .so; ClockWidget's spec even points at a nonexistent main.qml — ignored when .so exists) |

**Practical route**: ship a real `.so` (ClockWidget `getObject` pattern, may
point at any QML) **and reuse an existing plugin name** to inherit its picker
entry/label/slot.

## Inventory (24 plugins + common/ headers; every plugin ships lib<Dir>.so.1.0.0)

AVOff/AVOffPlugin, BTAudio/BTAudioPlugin, Calendar/CalendarPlugin, Call/CallPlugin,
CCSSports/CcsSportsPlugin, CcsWeather/CcsWeatherPlugin, Climate/ClimatePlugin,
Clock/ClockPlugin, Compass/CompassPlugin, CPCall/CPCallPlugin,
DriverTalk/DriverTalkPlugin, ElectricityUse/ElectricityUsePlugin,
Media/MediaPlugin, Navi/NaviPlugin, NModeAw/NModeAwPlugin, Olympics/OlympicsPlugin,
PHEVVehicleOSD/PHEVVehicleOSDPlugin, PHEVVehicle/PHEVVehiclePlugin, PHEV/PHEVPlugin,
Radio/RadioPlugin, VoiceMemo/VoiceMemoPlugin, VR/VRPlugin,
Weather/WeatherPlugin, WorldCup/WorldCupPlugin. `common/` has no spec →
dropped by the scanner. C++ also references absent plugins (DMB, SXMRadio,
Telematics, HomeDrv*, PhoneFav, CallHistory) — region/variant-gated.
