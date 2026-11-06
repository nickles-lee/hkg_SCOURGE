# Basic home tile + option-menu entries — deep-dive (round 2)

Stock tree `S` = `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/` (staged
copy of `/app/share/AppHomeScreen/qml_eco3.0/`). `Wide.qml` =
`S/HomeScreen/QHMI_HomeScreenMain_Wide.qml` (288 lines); `OM.qml` =
`S/popup/QOptionMenu.qml` (1373 lines); `ST` = AppHomeScreen strings.tsv
(bin_3208391a859693e9).

## (A) Basic home

**Anatomy** (`Wide.qml`): root `QRootScreen`, `isEnable: !sysPopupShowing`
(:7-31); bg `home_main_bg_01/02` (:47-64). One strip `itemMainMenu`:
width `HOME_1920_MENU_W`/`HOME_1280_MENU_W`, height `HOME_1920_MENU_H`=183,
x/y `HOME_1920_MENU_X/Y` (comments: 627+627+12 / 12+618+12 / 102+405+15,
:68-73). **All-Menu button** `QHomeItem` 250(wide)/207(narrow)×183, x_icon
76/55; release → `KEY_EVT_REQUEST_GOTO_SCREEN_ALL_MENUS` + beep; 650 ms
long-press → `EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS` (:76-149). **One-row
GridView** `mainGridItemMenu`: cells 250/207×183, `model:
AppHomeModel.listMenuHomeItem` (C++ Q_PROPERTY, ST:6669; "Empty
m_listMenuHomeItem" ST:1519), `interactive: false`, `currentIndex:
homePage1CurrentIdx` (:162-172); delegates reparent `QHomeItem`s into the
grid, release → `EVT_HOME_MENU_LIST` + modelData.itemID + beep, long-press
→ Edit Menus (:178-264). `Component.onCompleted`: `reqLoadWidget()`, binds
`listOptMenu`, resets indices (:269-283).

**When/switching**: Basic is the non-carousel home for **both panel widths**
("Wide" is a misnomer — every metric is an `isWideScr` ternary). User picks
mode in EditHomeLayout → `AppHomeModel.isAdvancedMode`. Screen selection is
C++-only: `EVT_GOTO_MAIN_HOME_SCREEN` → Wide.qml (ST:2714),
`EVT_GOTO_SCREEN_WIDE_ADVANCED` → Advanced.qml (ST:2777); QML receives
`screenNext` (main.qml:40-78).

**Hello precedent** (diff stock vs `hello_payload/payload/.../Wide.qml`):
(1) marker comment; (2) All-Menu tile + cells shrunk 250→200, x_icon 76/78→50
(fits a 6th 200-px cell in the 1266-px wide strip); (3) Setup-gear icon
hijack (`indexOf("home_settings")` → file:// trollface); (4) new tile after
the GridView inside `itemMainMenu`:
```qml
QHomeItem { id: itemHello; visible: AppHomeModel.isWideScr;
  width: isWideScr?200:207; height: 183;
  x: itemAllMenu.right + mainGridItemMenu.cellWidth * 4; x_icon: isWideScr?50:55;
  iconSource: ["file:///app/share/AppHomeScreen/hello/hello_icon_n.png",
               defineModel.home_bg_icon_p, "", ".../hello_icon_d.png"];
  itemName: "Hello"; icoPressed: helloMouse.pressed
  MouseArea { id: helloMouse; anchors.fill: parent; onClicked: helloPopup.visible = true } }
```
plus root-level `HelloPopup { z: 2000 }` (760×620 panel, dim click-to-close)
and a `Connections on AppHomeModel.onButtonBackClicked` close hook. **Hello
tiled all three surfaces**: Advanced.qml payload adds `emptySlots()/
firstEmptySlot()` + 3 HelloTiles (one per listPage, `visible: slot.page===N`,
z:5) + root HelloPopup; an in-grid HelloTile in
`QCustomizedMenuListAllowEmpty.qml:622-633` wired in AllMenus.qml and
Submenu_Setup.qml (per patch_qml_v3.py:33-60, patch_empty_slot.py).

**Recipe**: shrink cells on wide (narrow has no free cell — gate
`visible: isWideScr` or place outside the strip); add the tile as a sibling
after the GridView inside `itemMainMenu`; add the beep (Hello omitted it);
the overlay component file is instantiated separately in each home file at
root z:2000; add the Advanced tile too or it vanishes on mode switch.
Deploy: SMACK-preserving copy, cache clear, backup (hello run_stuff.sh:127-136).

**Verdict: PROVEN** (Hello ran on-car in Wide + Advanced + AllMenus + Setup;
Battery tile+overlay proven in Advanced). Risks: sibling overlay ≠ grid model
entry (`listMenuHomeItem` is C++-owned); cell-shrink alters stock icon
centering; no rearrange participation.

## (B) Option menu (status-bar "Menu" dropdown)

**Population**: `AppHomeModel.listOptMenu` is a **writable** context property;
every screen rebinds it in `Component.onCompleted` via `Qt.binding`
(consumers: main.qml:484-495 → Menu-button visibility; OM.qml:117-122 builds
the ListModel via `creatModel`, rebuilt on every change :1303-1307). Entry
shape `{indexOptItem, label, isItemSelect, isEnabled}` + optional
`isVisible, isOtherCheckbox, _data, dataEvent, keyEvent` (delegate reads
OM.qml:147-149; NMode rows dispatch `qmlSendEvtKey(listOptMenu[n].
keyEvent/dataEvent)` :532-583).

Construction sites (eco3.0): Advanced home Display Off/Edit Menus/Manual
(Adv.qml:19-23, bind :538); Basic home [Edit Widgets if
listWidgetAdvVisibleCount()>3]/Edit Menus/Manual — **no Display Off**
(Wide.qml:19-26, :272); AllMenus Rearrange/Manual/Split (:306-328);
Rearrange_Adv Display Off/Manual (:18-21,269); Setup submenu Display Off/
Search/Rearrange/Manual (:328-359); Media Display Off/Split (:204);
Information Display Off/Manual (:153); DataService Subscription/Split —
dispatched by index, no indexOptItem (OM.qml:434-443); Edit screens various;
~20 PHEV/EV/HEV screens (67 `listOptMenu` hits total).

**Anatomy** (OM.qml): full-screen `QRootScreen`; outside-click below y 90
closes (:62-83); panel width `MENU_LIST_WIDTH` 555 / `MENU_LIST_WIDTH_8` 501
at y 90 (:84-88); list height `count*108+4` capped 630 (:123-129); delegate
is **`QItemMenu`** (3-slice co_bg_box row) height 112/110/108 (:140-152) —
`delegateHeight:104` (:23) is declared but unused. **Dispatch**: onReleased →
global `switch(indexOptItem)` (Display Off → `EVT_CLICK_DISP_OFF` :173-189) →
`switch(AppHomeModel.currentScreen)` → per-screen switch; the home branch
(EVT_GOTO_MAIN_HOME_SCREEN + EVT_GOTO_SCREEN_WIDE_ADVANCED share it,
:192-239) has **default = beep only** (:235-237). 10 s auto-close
(:1179-1197); 10 ms slide (:20,91-93); 100 ms fade (:1216-1241). Loaded by
C++ on `EVT_OPEN_OPION_MENU` (ST:7124; path ST:2618; Statusbar sends it),
hide → `EVT_REQUEST_HIDE_POPUP, OSD_ID 13`; renders in main.qml's 4
onsDataList OSD slots at z:4. **`QOptionMenuWidgets.qml` is the AW widget
picker** (radio list from `listAWWidget`, 550×627 at x730/y93, dispatch
`EVT_SET_AW_WIDGET`, OSD_ID 12) — not a widget-page menu variant.

**Recipe**: append rows in `creatModel()` (:1357-1372) — the single funnel
for initial build and per-screen rebuilds; append AFTER the narrow-Split
removal (its indexes only stock rows); `indexOptItem` may be a literal
(900+) or unused registered enum value (`E_OPT_*` enum lives in C++ —
ST:5884-5943 — prefer literals). Dispatch: add `case <id>:` to the home
branch — can `qmlSendEvtKey` any EVT, `qmlChangeTopMsg(EVT_GOTO_*)`, or set
a model property (NMode `keyEvent`/`dataEvent` rows are the in-tree
precedent for arbitrary EVTs). A row **cannot host the overlay itself**
(menu destroyed on close, :1336-1340) — route via a watched property on an
overlay declared in the home file.

**Verdict**: model side **PROVEN** (rebinding is stock); custom dispatch
**LIKELY** (one-file OM.qml patch; NMode precedent; no payload has yet
exercised a custom case). Risks: missing case = silent beep-only row;
per-screen rebuilds (append centrally); menu transient.
