# Adding a persistent swipe "card" page to the Gen5W home carousel (QListViewHomePage)

Firmware `NE_EUR.S5W_L.001.001.260128`, AppHomeScreen, QML variant `qml_eco3.0`
(local stock tree: `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/`, byte-identical to
`../../wifi_app/base/stock/` — verified with `diff -q`).
A **real precedent exists**: the Battery app was first deployed as an extra carousel page
(preserved at `../../wifi_app/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`
and `../../tweaks_app/base/battery|battery_wifi|hello_battery_wifi/...`), and later
converted to tile+overlay (documented in `docs/ui/TERMINOLOGY.md:36-44` "What went wrong once").
This report extracts the exact mechanics and delta so a *planned* persistent card page can be added.

---

## 1. Carousel mechanics (Q1)

### 1.1 The ListView

`component/QListViewHomePage.qml` (whole file, 25 lines) is nothing but a tuned `ListView`:

- `ListView` with swipe-gesture hardening: on `onFlickStarted` it measures `|posUpX - posDownX|`;
  flicks shorter than **60 px** are cancelled and the view snaps back to `currentIndex`
  (`QListViewHomePage.qml:8-23`). Everything else is stock `ListView`.

It is instantiated **once** in `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:320-395` as `id: homepage`:

```qml
QListViewHomePage {
    id: homepage
    objectName: "lsvHomePage"          // C++ finds it by this name (widget injection)
    orientation: ListView.Horizontal
    model: totalPagesNormalMode == 0 ? 0: totalPagesNormalMode + 1   // line 329 (stock)
    snapMode: ListView.SnapOneItem
    highlightRangeMode: ListView.StrictlyEnforceRange
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 35000
    displayMarginBeginning: homepage.width*(totalPagesNormalMode + 1) // line 337
    interactive: !isButtonWidgetActive  // line 340 — a C++ "button widget" can lock swiping
    pressDelay: currentIndex == 0 ? 0 : 100  // line 341
    delegate: Item {                     // lines 342-355
        Loader {
            id: itemLoader
            enabled: !homepage.flicking
            sourceComponent: homePageItem[index]   // <-- pages come from a JS array
        }
        visible: (!homepage.isAnimating || index == AppHomeModel.currentHomeAdvancedPageIdx)
                 && itemLoader.status == Loader.Ready
    }
}
```

Key facts:

- **The model is an integer**, not a ListModel: `model: totalPagesNormalMode + 1`
  (`QHMI_HomeScreenMain_Advanced.qml:329`). The delegate uses the implicit `index` to pick the
  Component out of the `homePageItem` array.
- **`homePageItem: getModelList()`** (`QHMI_HomeScreenMain_Advanced.qml:18`) — a plain JS array of
  `Component` objects declared in the same file. `getModelList()` (lines 75-85):

```qml
function getModelList(){
    if(totalPagesNormalMode === 3){
        return [pluginPage, listPage1, listPage2, listPage3]
    }else if(totalPagesNormalMode === 2){
        return [pluginPage, listPage1, listPage2]
    }else if(totalPagesNormalMode === 1){
        return [pluginPage, listPage1]
    }else{
        return []
    }
}
```

- **Stock page order (index = carousel position):**
  - `0` = `pluginPage` — the widget page. An empty `Item` with
    `objectName: "itemWidgetPlaceHolder"` (line 177); on `Component.onCompleted` it calls
    `AppHomeEngine.slotReqInjectAdvancedWidget()` (line 191) and C++ injects
    `Screen_HomeScreen_Widget_Advanced.qml` (clock, navi map, weather, AV widget) into it
    (see §8).
  - `1..totalPagesNormalMode` = `listPage1..3` — the app-grid pages, each an `Item` wrapping a
    `QMenuPageNoDrag` GridView fed by the C++ context-property models `AllMenuPage1Display..3Display`
    via `getPageDisplayListModel(index)` (lines 107-124, 215, 256, 297).
  - `totalPagesNormalMode` is **not** hardcoded: it comes from
    `AppHomeModel.checkTotalPageFromMD()` in `updateDisplayList()` (lines 95-105) and is written
    back as `AppHomeModel.numberPage = totalPagesNormalMode` (line 103). It is 1..3 depending on
    how many apps the user has visible (wide screen fits 12 apps/page, narrow 8/page;
    `k_nbrOfColumns` line 37, `nbrItemsPerPage` line 38).
- **Swipe direction:** the ListView is horizontal, index 0 at contentX = 0. The widget page is the
  *leftmost* page; swiping right-to-left (drag left) moves to *higher* indices (app grids), and
  the stock pages end at the right. A page appended at the end of `getModelList()` appears
  **to the right of the last app-grid page** — exactly how the Battery page appeared
  ("swipe right screen", `docs/ui/TERMINOLOGY.md:38-39`). "Left of home" would mean
  inserting at index 0 — do **not** do that (index 0 is special-cased all over the UI, see §7/§12).
- **Indexing / current page:** `AppHomeModel.currentHomeAdvancedPageIdx` is a C++ Q_PROPERTY
  (binary strings: `mirror/export/bin_3208391a859693e9/strings.tsv:1889,6407,6680` —
  `currentHomeAdvancedPageIdx`, `currentHomeAdvancedPageIdxChanged`). The ListView writes to it on
  every page change, gated by `root.isCurrentIndexActive`
  (`QHMI_HomeScreenMain_Advanced.qml:358-363`):

```qml
onCurrentIndexChanged: {
    if(root.isCurrentIndexActive){
        AppHomeModel.currentHomeAdvancedPageIdx = homepage.currentIndex
    }
    AppHomeEngine.advancedModePageChange(homepage.currentIndex)
}
```

  `AppHomeEngine.advancedModePageChange(int)` is a C++ invokable notified on every page change
  (`strings.tsv:7067`); the home screen also mirrors drag position into
  `AppHomeModel.homeMainContentX = contentX` (line 382-384) which the status bar uses for its
  slide-in effect (`HomeScreen/main.qml:318`).

### 1.2 Page transitions and restore

- The whole Advanced screen is loaded by `HomeScreen/main.qml` through the `loader_1`/`loader_2`
  pair (`main.qml:40-78`); `screen_Transition()` (line 40-49) swaps `current_loader.sourceComponent
  = screenNext` and the old screen is **destroyed** in `switch_current_loader()` (lines 51-64,
  `from.sourceComponent = null`). So the carousel (and every page in it) is destroyed when the
  user navigates to any other screen *inside* AppHomeScreen (All Menus, Setup submenu, Edit
  screens) and recreated when coming back.
- On (re)creation, `goHistorypage()` (lines 556-571) restores the last page from the C++ property
  and **clamps it to the page count**:

```qml
function goHistorypage(){
    root.isCurrentIndexActive = false
    homepage.positionViewAtIndex(2,ListView.Beginning)
    if(AppHomeModel.currentHomeAdvancedPageIdx <= totalPagesNormalMode)
    { ... positionViewAtIndex(AppHomeModel.currentHomeAdvancedPageIdx, ...) }
    else
    { AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode; ... }
    root.isCurrentIndexActive = true
}
```

  This clamp is one of the exact things the precedent patch had to change (§4).
- `Component.onCompleted` (lines 532-554) also plays the right/left entry animation when
  `currentHomeAdvancedPageIdx !== 0` via `QMainScreenAnimation`
  (`component/QMainScreenAnimation.qml:12-36` — a 150 ms opacity+X slide; it also resets itself
  when another app takes foreground, `onSwitchToOtherApp`, lines 62-70).
- `Connections on AppHomeModel` (lines 457-530): `onRequestUpdateAllMenu` re-runs
  `constructMenuModel()` + `homePageItem = getModelList()` and repositions
  (`homepage.currentIndex = AppHomeModel.currentHomeAdvancedPageIdx`,
  `positionViewAtIndex(indexCurrent, ListView.Beginning)`, lines 472-473);
  `onRequestUpdatePageIndexChanged` repositions on C++ request (lines 488-493);
  `onKeepCurrentScreen` / `onSysPopupShowingChanged` snap the view back if a popup arrives
  mid-flick (lines 495-513). The `onIsAtOtherAppChanged` handler is **commented out** stock
  (lines 522-529) — the home screen does nothing special when another app takes foreground.

---

## 2. Page indicator dots (Q2)

`QHMI_HomeScreenMain_Advanced.qml:403-455`:

```qml
Row{
    id: indicatorGroup
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom
    width: ((totalPagesNormalMode + 1) * 39) + (totalPagesNormalMode * 9)   // line 408
    height: 26
    spacing: 9
    Repeater {
        model: (totalPagesNormalMode + 1)                                    // line 413
        Q_ThemeImage {
            themeSource: (index === (AppHomeModel.currentHomeAdvancedPageIdx)
                          ? defineModel.home_ic_indicator_w_s : defineModel.home_ic_indicator_w_n)
        }
    }
}
```

- A dot = one `Q_ThemeImage` (39 px wide, 9 px spacing); the selected dot is
  `home_ic_indicator_w_s`, others `home_ic_indicator_w_n` (line 416).
- **A new page registers a dot purely by increasing the Repeater model count and the Row width.**
- Show/hide: `homepage.onMovementStarted` → `reqShowIndicator()`; `onMovementEnded` →
  `startHideIndicator()` (lines 386-394). `reqShowIndicator()` (lines 445-455) shows the group only
  if `(totalPagesNormalMode + 1) > 1`, then a 5000 ms `OpacityAnimator` fades it out
  (`animationHideIndicator`, lines 429-443; `timerHideIndicator` also sets `visible: false`,
  lines 421-428). C++ can also trigger it (`onReqShowIndicator`, lines 515-520).
  `reqShowIndicator()`'s `> 1` test is *not* page-count-exact but stays true, so it needs no change.

---

## 3. The exact stock → patched delta (Q3) — quoted from the working precedent

Source of the **swipe-page precedent** (verified working on the car):
`../../wifi_app/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`
(identical logic also in `../../tweaks_app/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`,
`../../tweaks_app/base/battery_wifi/...`, `../../tweaks_app/base/hello_battery_wifi/...`).
Diff vs `../../wifi_app/base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`:

```diff
--- stock/QHMI_HomeScreenMain_Advanced.qml
+++ battery-page/QHMI_HomeScreenMain_Advanced.qml
@@ -39,6 +40,7 @@
     property alias listPosY: homepage.y           // position Y of the list
     property int totalPages: 0    // total menu pages
     property int totalPagesNormalMode: 0
+    property int totalPageCount: totalPagesNormalMode + 2
     property int listType : AppHome_Enum.E_LIST_ALL_MENUS
@@ -74,11 +76,11 @@
     function getModelList(){
         if(totalPagesNormalMode === 3){
-            return [pluginPage, listPage1, listPage2, listPage3]
+            return [pluginPage, listPage1, listPage2, listPage3, batteryPage]
         }else if(totalPagesNormalMode === 2){
-            return [pluginPage, listPage1, listPage2]
+            return [pluginPage, listPage1, listPage2, batteryPage]
         }else if(totalPagesNormalMode === 1){
-            return [pluginPage, listPage1]
+            return [pluginPage, listPage1, batteryPage]
         }else{
             return []
         }
@@ -317,6 +319,20 @@   (after the listPage3 Component, before QListViewHomePage)
+    Component {
+        id: batteryPage
+
+        Item {
+            width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
+            height: DEVICE_HEIGHT
+
+            BatteryPage {
+                anchors.fill: parent
+                onGoHome: homepage.currentIndex = 0
+            }
+        }
+    }
+
     QListViewHomePage {
         id: homepage
@@ -326,7 +342,7 @@
-        model: totalPagesNormalMode == 0 ? 0: totalPagesNormalMode + 1
+        model: totalPagesNormalMode == 0 ? 0 : totalPageCount
@@ -405,12 +421,12 @@   (indicator Row)
-        width: ((totalPagesNormalMode + 1) * 39) + (totalPagesNormalMode * 9)
+        width: (totalPageCount * 39) + ((totalPageCount - 1) * 9)
@@ -413 (indicator Repeater)
-            model: (totalPagesNormalMode + 1)
+            model: totalPageCount
@@ -556,14 +572,14 @@   (goHistorypage)
-        if(AppHomeModel.currentHomeAdvancedPageIdx <= totalPagesNormalMode)
+        if(AppHomeModel.currentHomeAdvancedPageIdx <= totalPagesNormalMode + 1)
@@ -566 (goHistorypage else-branch)
-            AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode
+            AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode + 1
```

Patched-file line numbers for citation (`../../wifi_app/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`):
`totalPageCount` :43 · `getModelList` :78-86 · `batteryPage` Component :322-333
(`BatteryPage { anchors.fill: parent; onGoHome: homepage.currentIndex = 0 }` :329-331) ·
ListView `model` :345 · indicator Row width :424 · Repeater model :429 · `goHistorypage` :572-585.

**That is the entire delta: 6 hunks in one file.** The precedent deliberately:

- keeps `totalPagesNormalMode` untouched (it is the C++-owned app-grid count, written back to
  `AppHomeModel.numberPage`, `QHMI_HomeScreenMain_Advanced.qml:103`), and introduces a separate
  `totalPageCount = totalPagesNormalMode + 2` (= widget page + N grid pages + 1 custom page);
- appends the page at the **end** of every `getModelList()` branch, so stock indices 0..N are
  unchanged and the custom page index is `totalPagesNormalMode + 1`;
- widens the restore clamp so a persisted custom-page index is legal;
- adds one `Component` whose root `Item` sizes itself to the screen
  (`isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`, `DEVICE_HEIGHT`) exactly like
  `pluginPage` (stock line 174-176).

The later tile+overlay conversion (for contrast, **not** what we want) is at
`../../battery_app/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`
(diff shows `BatteryTile`s inside `listPage1..3` + a `z: 2000` `BatteryPage` overlay at y=90) and is
documented as the "correct" shape in `docs/ui/TERMINOLOGY.md:36-44` — but that was a product
choice ("appeared as a swipe page instead of an app icon"), **not** a technical failure of the page
approach. For this deployment a swipe page is the requirement.

---

## 4. Step-by-step recipe: add a new persistent card page ("MyCard")

Files to touch (target tree `/app/share/AppHomeScreen/qml_eco3.0/`, local copy
`/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/`):

| # | File | Change |
|---|---|---|
| 1 | `HomeScreen/MyCardPage.qml` | **NEW** — the card content (a plain `Item`-rooted component; see §4.1) |
| 2 | `HomeScreen/QHMI_HomeScreenMain_Advanced.qml` | the 6-hunk delta of §3, with `batteryPage`→`myCardPage` |
| 3 | *(nothing else)* | no C++ change, no Wide-file change needed for the carousel (see §9) |

### 4.1 `HomeScreen/MyCardPage.qml` (new file)

Same directory as `QHMI_HomeScreenMain_Advanced.qml`, so the bare type name resolves through the
implicit same-directory import (this is exactly how the precedent's `BatteryPage` resolved; the
overlay-era file lives at `../../battery_app/artifacts/qml/.../HomeScreen/BatteryPage.qml`).

```qml
// MYCARD_PAYLOAD_YYMMDD
import QtQuick 2.0
import "../component"
import "../component/common"

Item {
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT

    signal goHome()          // optional: let the page programmatically return to page 0

    // The page is NOT an overlay: never gate visibility on `requested`/`isAtOtherApp`.
    // The carousel delegate already manages visibility (Advanced.qml:354).

    // Full-bleed pages start below the 90 px status bar unless the page is index 0:
    // put content at y >= ConstantModel.MENU_BAR_Y, height <= DEVICE_HEIGHT - MENU_BAR_Y
    // (UI_CONTEXT.md Rule 1). The indicator Row overlaps the bottom ~26 px (Advanced.qml:403-419).

    // Input shield so horizontal drags on empty areas still swipe the carousel but
    // drags on content don't leak (UI_CONTEXT.md Rule 3): use MouseArea with
    // preventStealing where the card has its own flickables.
}
```

Notes:

- **Do not copy the overlay-era `BatteryPage.qml` gating** (`visible: requested && !atOtherApp`,
  `open()/close()`, close-on-`onCurrentHomeAdvancedPageIdxChanged` — see
  `../../battery_app/artifacts/.../BatteryPage.qml:29-52`). A carousel page that closes
  itself on page-index change would vanish on every swipe; the swipe-era page was a plain always-on
  `Item` and only emitted `goHome` (handled by `homepage.currentIndex = 0` in the wrapper Component).
- Timers/updates inside the page keep running while the page is off-screen (§10); bind anything
  expensive to `AppHomeModel.currentHomeAdvancedPageIdx === myPageIndex` if you want to pause it.

### 4.2 `QHMI_HomeScreenMain_Advanced.qml` (patch, following §3 exactly)

1. Add `property int totalPageCount: totalPagesNormalMode + 2` next to
   `totalPagesNormalMode` (stock line 41 → becomes line 42/43).
   (For each additional custom page, +1 more.)
2. `getModelList()` (stock lines 75-85): append `myCardPage` to **all three non-empty branches**:
   `[pluginPage, listPage1, listPage2, listPage3, myCardPage]` etc. The `else { return [] }`
   branch stays (no apps → no home).
3. Add the page `Component` after `listPage3` (stock line 318), mirroring the precedent:

```qml
Component {
    id: myCardPage

    Item {
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT

        MyCardPage {
            anchors.fill: parent
            onGoHome: homepage.currentIndex = 0
        }
    }
}
```

4. ListView `model` (stock line 329): `model: totalPagesNormalMode == 0 ? 0 : totalPageCount`.
5. Indicator (stock lines 408, 413):
   `width: (totalPageCount * 39) + ((totalPageCount - 1) * 9)` and `Repeater { model: totalPageCount }`.
6. `goHistorypage()` (stock lines 556-571): change both `totalPagesNormalMode` bounds to
   `totalPagesNormalMode + 1` (the index of the custom page), i.e. the clamp becomes
   `if(AppHomeModel.currentHomeAdvancedPageIdx <= totalPagesNormalMode + 1) ... else
   AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode + 1`.
7. *(recommended for an "always alive" page, beyond the precedent)*: also enlarge the instantiation
   margins so the page is never destroyed even when you swipe far away:
   `displayMarginBeginning: homepage.width * totalPageCount` and add
   `displayMarginEnd: homepage.width * totalPageCount` (or uncomment/raise `cacheBuffer`,
   stock line 336). The precedent left `displayMarginBeginning: homepage.width*(totalPagesNormalMode + 1)`
   (patched line 353) untouched and worked, but with 5 pages that margin only guarantees that pages
   at-or-left of the current one stay instantiated; the last page can be torn down when you are back
   on page 0 and recreated on the next swipe (§10).

### 4.3 Deployment (from the working installers, e.g. `../../wifi_app/scripts/run_stuff.sh`)

- Install to `/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/` (QML variant `qml_eco3.0` is the
  active one on this head unit — `../../battery_app/SPEC.md` §1).
- Preserve SMACK labels on copied files (wifi_app SPEC §8 step 10).
- Clear QML caches or the old `QHMI_HomeScreenMain_Advanced.qml` may come from cache:
  `rm -rf /root/.cache/qtshadercache /root/.cache/AppHomeScreen /tmp/qmlcache /tmp/qt-qml-cache`
  (`../../wifi_app/scripts/run_stuff.sh:388-392`).
- Keep a backup of the stock file (pattern: `backup/AppHomeScreen/...` in every payload).
- Validate syntax with the target's own `qmllint` in the VM lab
  (`/usr/bin/qt5/qmllint`, used in `../../vm/runs/1793981753380712551/result.json`).

---

## 5. C++ coupling and hard caps (Q3, C++ side)

Evidence from the Ghidra export of `/app/bin/AppHomeScreen`
(`../../mirror/export/bin_3208391a859693e9/`):

- **Page models are C++ context properties, exactly three of them:**
  `AllMenuPage1..3` and `AllMenuPage1Display..3Display` exist in the binary string table
  (`strings.tsv:1757-1762`) and are *not* QML ids anywhere in the QML trees (verified by grep).
  `getPageDisplayListModel()` in Advanced.qml returns `AllMenuPage1Display..3Display`
  (`QHMI_HomeScreenMain_Advanced.qml:107-124`); `resetAllPages()` calls
  `AllMenuPage*Display.resetData()` (lines 126-131); ownership is arbitrated by
  `AllMenuPage1.allMenuScreenOwner` (lines 215/256/297/463). → **The app-grid page count is
  hard-capped at 3 by C++** (also `maxNumPages: (AppHomeModel.isWideScr) ? 2 : 3` in the rearrange
  screen, `Screen_HomeScreen_AllMenus_Rearrange_Adv.qml:154`). A custom page must therefore be a
  *self-contained QML* page (like the precedent's `BatteryPage`) — it cannot join the C++ grid
  model pipeline, and it must not disturb `totalPagesNormalMode`.
- **`currentHomeAdvancedPageIdx` is a C++ Q_PROPERTY** with a `Changed` signal
  (`strings.tsv:1889,6407,6680`; C++ methods `void AppHomeModel::setCurrentHomeAdvancedPageIdx(int)`
  and `void AppHomeModel::requestChangeCurrentHomeAdvancedPageIdx(int)`,
  `strings.tsv:1691-1692`). QML writes it on every swipe and on restore; C++ can request changes
  (handled by `onRequestUpdatePageIndexChanged`, Advanced.qml:488-493). It is an `int` — no
  evidence of a range check in QML; the only clamps are the QML ones in `goHistorypage()`
  (which the patch widens) and the ListView's own bounds. **The precedent ran with index 4 on the
  car without issues.**
- **`AppHomeEngine.advancedModePageChange(int)`** (`strings.tsv:7067`) is called with every page
  index (Advanced.qml:362). Unknown C++ internals (the export's `string_refs` for these functions
  are not resolved), but the precedent proves index 4 is accepted.
- **Widget injection walks the carousel by objectName:** C++ strings
  `ERR: cannot find lsvHomePage`, `ERR: cannot find contentItem of lsvHomePage`,
  `itemWidgetPlaceHolder`, `itemWidgetPlaceHolder FOUND!!!` (`strings.tsv:2097,2513-2514`) —
  C++ finds `lsvHomePage` (Advanced.qml:322), walks its `contentItem` children and injects widgets
  into the `itemWidgetPlaceHolder` Item (Advanced.qml:177). A custom page delegate is one more
  child of `contentItem`; as long as it has **no `objectName: "itemWidgetPlaceHolder"`**, it is
  skipped by the search. Do not reuse that objectName.
- **Page-move handlers exist for indices 0..4**: `HOME_LIST_PAGE0_MOVING_HANDLE` …
  `HOME_LIST_PAGE4_MOVING_HANDLE` (`strings.tsv:2683-2686`) — these serve the *basic/Wide* home's
  `customizeModeHK_PAGE0..4` pages (`QHMI_HomeScreenMain_Wide.qml:12-16`), showing C++ already has
  a 5-page concept elsewhere; no evidence of a carousel cap in AppHomeModel.
- **`checkTotalPageFromMD()`** (`strings.tsv:6633`) computes the app-grid page count from metadata;
  **`numberPage`** (`strings.tsv:6797`) is written back by QML (Advanced.qml:103). Neither knows
  about custom pages — hence the separate `totalPageCount` property; never increment
  `totalPagesNormalMode` itself or you desync the C++ grid models and the rearrange screen.
- **No page-count reference in the QML tree beyond the ones patched:** full-tree grep for
  `page2Count`/`currentPage`-style coupling finds only `currentHomeAdvancedPageIdx` uses listed in
  §7/§12 (popups, main.qml dim/statusbar, rearrange screen) — all of them either compare `=== 0`
  (widget-page special case) or do ±1 index math in the rearrange screen (§7).

---

## 6. Widget page vs app pages vs submenus — page taxonomy (Q5)

| Thing | What it is | Carousel page? |
|---|---|---|
| `pluginPage` (Advanced.qml:171-194) | widget page: empty placeholder; C++ injects `Screen_HomeScreen_Widget_Advanced.qml` (clock/navi/weather/AV widgets, `Screen_HomeScreen_Widget_Advanced.qml:8-466`) and plugin widgets (`*.pluginspec`, `/app/plugins`) | **Yes — index 0** |
| `listPage1..3` (Advanced.qml:196-318) | app-grid pages: `QMenuPageNoDrag` GridView of app icons (`component/QMenuPageNoDrag.qml:5-108`), models from C++ (`AllMenuPageNDisplay`) | **Yes — indices 1..3** |
| custom card page (precedent `batteryPage`) | self-contained QML `Item` | **Yes — appended index** |
| `Screen_HomeScreen_AllMenus.qml` | full "All menu" list screen (own `QCustomizedMenuList`, `Screen_HomeScreen_AllMenus.qml:130`) | No — separate screen, loaded by main.qml loaders |
| `Screen_HomeScreen_AllMenus_Rearrange_Adv.qml` | the "Edit menu" / rearrange screen | No — separate screen (§7) |
| `Screen_HomeScreen_Submenu_EditWidget[_Advanced].qml`, `Submenu_EditMenu.qml`, `Submenu_EditHomeLayout.qml`, `Submenu_Setup/Media/Information/DataService.qml` | submenus | No — separate screens |
| `QHMI_HomeScreenMain_Wide.qml` | the **basic-mode** home (single screen: "All Menu" button + one-row grid `listMenuHomeItem`, `QHMI_HomeScreenMain_Wide.qml:68-267`) — *no carousel at all* | No |
| `QWidgetItem.qml` / `WidgetItemAdvanced.qml` / `WidgetItem.qml` (`component/`) | widget *picker* items for the Edit-Widget screens (icons for map/climate/weather/…, `WidgetItemAdvanced.qml:14-32`) | No |

TERMINOLOGY cross-check: `docs/ui/TERMINOLOGY.md:8-19` ("Swipe page = a page in
QListViewHomePage (`pluginPage`, `listPage1..3`)", "Widget page = the first Advanced page").

---

## 7. Edit-layout / edit-menu / edit-widget interplay (Q4)

- **Users cannot reorder or hide carousel pages.** Page order is fixed by `getModelList()`;
  only the *app icons inside the grid pages* are editable. Nothing in the edit screens writes the
  page array.
- **Edit Home Layout** (`Screen_HomeScreen_Submenu_EditHomeLayout.qml:62-108`) only toggles
  `AppHomeModel.isAdvancedMode` between the Advanced (carousel) home and the Basic
  (`QHMI_HomeScreenMain_Wide.qml`) home. **A custom page exists only in Advanced mode** — if the
  user switches to Basic, the card disappears with the whole carousel. That is stock behavior for
  app-grid pages too.
- **Edit Menu / rearrange** (`Screen_HomeScreen_AllMenus_Rearrange_Adv.qml`): entered by
  long-press on a grid item (Advanced.qml:227-231 etc.) or via the option menu
  (`popup/QOptionMenu.qml:213-223`). It reuses `AppHomeModel.listHomePage` (line 151) and maps
  carousel indices to grid pages with **±1 arithmetic**:
  `currentPageIndex: (AppHomeModel.currentHomeAdvancedPageIdx - 1)` (line 152) and
  `AppHomeModel.currentHomeAdvancedPageIdx = (index + 1)` (line 157); the dot row likewise uses
  `currentHomeAdvancedPageIdx - 1` (line 199). Because the custom page is appended **after** the
  grid pages, grid-page indices keep their meaning and the mapping is unaffected.
  *Edge case:* if the option menu's "Edit Home Menu" is invoked **while the custom page is current**
  (`currentHomeAdvancedPageIdx == totalPagesNormalMode + 1`), the rearrange screen gets
  `currentPageIndex = totalPagesNormalMode` — one past the last grid page. `QCustomizedMenuListAllowEmpty`
  clamps some invalid cases (`updateHistoryPage()`, `component/QCustomizedMenuListAllowEmpty.qml:155-171`)
  but only for `totalPagesDisplay === 1` or `2`; with 3 grid pages index 3 relies on the ListView's
  own clamping. The precedent did not address this and no breakage was observed; if paranoid, reset
  `AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode` before sending
  `EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED`, or accept the clamp.
- **Edit Widget** (`Screen_HomeScreen_Submenu_EditWidget_Advanced.qml`) edits the *widget page*
  contents only (`editHomeWidgetPos` scroll position, lines 232/550/607/618); it never touches the
  carousel structure. It is only reachable from page 0 context (`QOptionMenu.qml:195-210` gates
  `E_OPT_EDIT_HOME_WIDGET` on `currentHomeAdvancedPageIdx === 0` / `fromPage0`).
- After any menu edit, C++ emits `onRequestUpdateAllMenu` → Advanced.qml rebuilds
  `constructMenuModel()` + `homePageItem = getModelList()` and repositions to
  `currentHomeAdvancedPageIdx` (Advanced.qml:459-474). With the widened clamp this restores the
  custom page correctly. If the user hides enough apps that `totalPagesNormalMode` shrinks, the
  custom page index shrinks with it (`totalPagesNormalMode + 1`), and `goHistorypage()` re-clamps —
  same behavior as the precedent.

---

## 8. Lifecycle of a page (Q6)

- **Eager, synchronous, and effectively immortal while the screen lives.** The delegate's
  `Loader` has `sourceComponent: homePageItem[index]` with `asynchronous` **commented out**
  (Advanced.qml:346-352) — pages instantiate synchronously. `displayMarginBeginning:
  homepage.width*(totalPagesNormalMode + 1)` (line 337) means every page at or left of the current
  one is kept instantiated; `cacheBuffer` is commented out (line 336), so pages to the right are
  created when a swipe brings them into view and are subject to the default cache-buffer
  retention. In practice: after the first visit (or with the §4.2 step-7 margin fix, always) the
  page object persists **until the whole Advanced screen is destroyed** by screen navigation
  (main.qml `switch_current_loader`, lines 51-64).
- **A page is never "closed" like an app.** There is no open/close lifecycle — the page is part of
  the home scene. It is only ever `visible: false` (delegate binding, Advanced.qml:354) while other
  carousel pages show, and input-disabled during flicks (`enabled: !homepage.flicking`,
  Advanced.qml:348).
- **Visibility changes are implicit.** Pages are *not* told about visibility; the delegate's
  `visible` binding flips during the entry/exit animation (`homepage.isAnimating`, lines 364-381).
  A page that needs to know should bind to `AppHomeModel.currentHomeAdvancedPageIdx` (kept in sync
  on every swipe, lines 358-363) or to its own `visible` property (QML `onVisibleChanged` works).
- **Timers and property bindings keep running while the page is off-screen** — QML does not pause
  them. The swipe-era Battery page kept its CAN/daemon state refreshing while parked on page 4.
  Gate heavy work on `visible`/`currentHomeAdvancedPageIdx` if desired.
- **State does not survive screen changes.** Navigating to All Menus / Setup / Edit screens
  destroys the Advanced screen (and the page) — the page is recreated on return, and
  `goHistorypage()` restores the *page index* from C++ (Advanced.qml:556-571). Keep volatile page
  state in the C++ model/daemon or accept the reset.

---

## 9. Wide screen (Q7)

- The **carousel exists on the wide 1920 layout and is the same file**: `QHMI_HomeScreenMain_Advanced.qml`
  adapts via `AppHomeModel.isWideScr` ternaries throughout (root width line 152; page width lines
  175/204/245/285; grid metrics `k_menuPageWidth : isWideScreen? k_itemWidth * 6: k_itemWidth * 4`,
  lines 33-38). The precedent's `batteryPage` Component already handles wide with
  `width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`
  (patched line 327) — **no separate wide variant is needed**; the same recipe covers both.
- `QHMI_HomeScreenMain_Wide.qml` is **not** the wide-screen home — it is the *basic-mode* home
  (single page, `listMenuHomeItem` grid, `QHMI_HomeScreenMain_Wide.qml:162-265`) and has **no
  carousel**. Nothing to patch there for the card page (the Hello payload patched it only to add a
  *tile* to basic mode: `../../hello_payload/payload/.../QHMI_HomeScreenMain_Wide.qml`
  diff adds a `QHomeItem` next to the grid).
- Wide-specific couplings that stay correct with an extra page:
  - status bar slides with the carousel: `x = max(0, 1920 - AppHomeModel.homeMainContentX)` when
    `currentScreen === EVT_GOTO_SCREEN_WIDE_ADVANCED` (`HomeScreen/main.qml:318`); extra pages just
    clamp it at 0.
  - popups/dim layers treat only index 0 as full-bleed (§12) — an appended page behaves like the
    grid pages on both widths.

  (Note: the stock popups key off `EVT_GOTO_SCREEN_WIDE_ADVANCED`, which is the *event id of the
  Advanced home screen itself*, not the Wide file — see `popup/QOptionMenu.qml:192-193` handling
  `EVT_GOTO_MAIN_HOME_SCREEN` and `EVT_GOTO_SCREEN_WIDE_ADVANCED` together, and the C++ screen-name
  table `strings.tsv:2714,2777` containing both files.)

---

## 10. Persistence / foreground rules (Q8)

What "persistent" means here, concretely:

1. **The page is never destroyed by launching an app.** Launching an app from a grid tile sends
   `EVT_MAIN_LIST_ITEMCLICKED` to C++ (`QMenuPageNoDrag.qml:62-67` → Advanced.qml:225); the other
   app's Wayland surface comes to front (`AppHomeModel.isAtOtherApp`, `currentShowingAppID` —
   `docs/ui/UI_CONTEXT.md:59-63`), but AppHomeScreen keeps running with its scene —
   including all carousel pages — intact. Stock even has the `onIsAtOtherAppChanged` handler for
   the carousel commented out (Advanced.qml:522-529): the home does nothing on foreground loss.
   `QMainScreenAnimation` merely cancels the entry animation on `onSwitchToOtherApp`
   (`QMainScreenAnimation.qml:62-70`).
2. **The page must not fight the foreground.** Because the page is inside the home scene (not a
   popup), it is automatically obscured when another app owns the surface; clicks land on that app,
   not on the page — that is *desired* for a carousel page. Do **not** add the overlay-era
   `isAtOtherApp` self-hiding (`BatteryPage.qml` overlay revision lines 29-52) — it would make the
   page vanish. The relevant UI_CONTEXT rules for *overlays* (close on `isAtOtherApp`, Home/Star
   via `dbus-monitor`, `preventStealing` shield) apply only if the card later spawns its own
   full-screen popup/overlay on top (`docs/ui/UI_CONTEXT.md:65-103`).
3. **The steering-wheel Home/Star key** goes to the *focused app* over the session bus and is not
   a QML event (`UI_CONTEXT.md:42-81`); when the home screen is foreground, the key asks the home
   screen to show itself (C++ `NotifyRequestHomeScreen`), which re-shows the Advanced screen at the
   persisted `currentHomeAdvancedPageIdx` — i.e. the car returns to *your card* if it was the last
   page. That is the "persistent deployment model" behavior.
4. **Page index persists in C++** (`AppHomeModel.currentHomeAdvancedPageIdx`) across in-app screen
   changes and foreground losses; the widened `goHistorypage()` clamp (§4.2 step 6) is what makes
   index `totalPagesNormalMode + 1` restorable instead of being reset to the last grid page.

---

## 11. Recipe skeleton (copy-paste summary)

```
NEW FILE  HomeScreen/MyCardPage.qml          (Item root, full-page size, signal goHome(), no overlay gating)
EDIT      HomeScreen/QHMI_HomeScreenMain_Advanced.qml
          (1) + property int totalPageCount: totalPagesNormalMode + 2
          (2) getModelList(): append myCardPage in the 3/2/1-page branches
          (3) + Component { id: myCardPage; Item { screen-size; MyCardPage { anchors.fill: parent; onGoHome: homepage.currentIndex = 0 } } }
          (4) homepage.model: totalPagesNormalMode == 0 ? 0 : totalPageCount
          (5) indicatorGroup.width: (totalPageCount*39)+((totalPageCount-1)*9); Repeater model: totalPageCount
          (6) goHistorypage(): clamp totalPagesNormalMode -> totalPagesNormalMode + 1 (both places)
          (7) [rec] displayMarginBeginning/End: homepage.width * totalPageCount  (always-alive guarantee)
DEPLOY    copy both files to /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/ (preserve SMACK labels),
          rm -rf /root/.cache/AppHomeScreen /root/.cache/qtshadercache /tmp/qmlcache /tmp/qt-qml-cache,
          keep stock backup; qmllint in the VM before flashing the USB payload.
```

---

## 12. Risks / edge cases (all with citations)

1. **Never insert at index 0 or between stock pages.** Index 0 is special-cased as the full-bleed
   widget page in: popups (`popup/QHMI_AppHomePopup_OK.qml:9-20` and 10+ other popups:
   `y: (currentScreen === EVT_GOTO_SCREEN_WIDE_ADVANCED && currentHomeAdvancedPageIdx === 0) ? 0 : 90`),
   the dim layers (`HomeScreen/main.qml:401,459,461`), the option menu's widget-edit gating
   (`popup/QOptionMenu.qml:195`), and the rearrange screen's `idx-1` mapping
   (`Screen_HomeScreen_AllMenus_Rearrange_Adv.qml:152,157,199`). Appending at the end keeps all of
   these consistent (custom page behaves like a grid page).
2. **Don't touch `totalPagesNormalMode` / `numberPage` / `checkTotalPageFromMD()`.** They are the
   C++-owned app-grid pagination (Advanced.qml:95-105; C++ context models `AllMenuPage1..3`,
   §5). Use a separate `totalPageCount`. The precedent's `+2` is `+1` per custom page.
3. **`goHistorypage()` clamp** must be widened, else the home silently resets to the last grid page
   on every screen re-entry (stock lines 556-571; the exact failure mode the patch fixes).
4. **Edit-menu entered from the custom page** gives the rearrange screen an out-of-range
   `currentPageIndex` (§7) — clamped by the ListView in practice; optionally reset the index first.
5. **`isButtonWidgetActive`** (set from C++ via `AdvancedWidgetQMLInstance.onSetButtonWidgetActive`,
   Advanced.qml:180-187) disables carousel swiping (`interactive: !isButtonWidgetActive`,
   line 340). If a C++ button-widget is active on page 0, users can't swipe away — stock behavior,
   unrelated to the custom page, but don't set that flag from the card.
6. **Horizontal drag conflicts:** any un-handled horizontal drag on the card flicks the carousel
   (that's how users leave the page). If the card contains its own horizontal flickables/sliders,
   use `preventStealing` MouseAreas (UI_CONTEXT.md Rule 3) — otherwise drags will swipe the page
   away mid-gesture. Note `pressDelay: currentIndex == 0 ? 0 : 100` (Advanced.qml:341) adds 100 ms
   press delay on non-zero pages.
7. **Page teardown when far away** (only if step 7 of §4.2 is skipped): with stock margins the last
   page may be destroyed when the user swipes back to page 0 and recreated on the next swipe —
   state resets (§8). Set both display margins to `homepage.width * totalPageCount` for a
   guaranteed-alive page (memory cost: one extra page of scene graph, same as the widget page).
8. **Basic home has no card.** Switching Home Type to Basic (`Screen_HomeScreen_Submenu_EditHomeLayout.qml:89-107`)
   leaves the card (and the whole carousel) behind; the card exists only in Advanced mode.
9. **QML cache**: stale `.qmlc`/shader caches can resurrect the old home after patching — always
   clear the caches in the installer (`../../wifi_app/scripts/run_stuff.sh:388-392`).
10. **Don't name anything `itemWidgetPlaceHolder`** — C++ widget injection searches the carousel's
    `contentItem` children by that objectName (§5) and would inject the widget page into your card.
11. **`qml_genesis` / `qml_new2.0` variants** contain parallel copies of the same screen
    (`qml_genesis/HomeScreen/QHMI_HomeScreenMain_Advanced.qml` has the same carousel with slightly
    different indicator metrics, lines 330-470). The active variant on this unit is `qml_eco3.0`
    (battery_app SPEC §1); patch only the active tree, but stay aware the others exist.
12. **The precedent is proven on-car**: the swipe-page Battery build ran with
    `totalPagesNormalMode = 3` → 5 carousel pages, indicator with 5 dots, restore-to-page-4, and
    C++ `advancedModePageChange(4)` — no C++ breakage observed before it was deliberately converted
    to tile+overlay (`docs/ui/TERMINOLOGY.md:36-44`).

---

## 13. Source index

- Stock target tree: `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/` (== `../../wifi_app/base/stock/`)
- Swipe-page precedent: `../../wifi_app/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`
  (also `../../tweaks_app/base/{battery,battery_wifi,hello_battery_wifi}/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`)
- Tile+overlay conversion (contrast): `../../battery_app/artifacts/qml/AppHomeScreen/qml_eco3.0/`
  (`QHMI_HomeScreenMain_Advanced.qml`, `BatteryPage.qml`, `component/BatteryTile.qml`) and
  `../../hello_payload/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml`
- AppHomeScreen C++ export: `../../mirror/export/bin_3208391a859693e9/`
  (`strings.tsv`, `symbols.csv`, `functions_*.c`; binary id from
  `../../mirror/sqlite/ioniq5_260128.db`, `binaries.path = app/bin/AppHomeScreen`)
- Context docs: `docs/ui/UI_CONTEXT.md`, `docs/ui/TERMINOLOGY.md` (§"What went wrong once"),
  `../../battery_app/SPEC.md`, `../../wifi_app/SPEC.md` (§7 allocator, §8 installer)
- Installer pattern: `../../wifi_app/scripts/run_stuff.sh`, `../../battery_app/scripts/patch_advanced.py`
