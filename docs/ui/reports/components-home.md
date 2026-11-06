# Gen5W AppHomeScreen — Component Library Inventory & Usage Rules
Firmware: `NE.EUR.S5W_L.001.001.260128` — AppHomeScreen `qml_eco3.0` tree (local copy at `/tmp/opencode/gen5w_ui/`).
All paths below are relative to `/tmp/opencode/gen5w_ui/AppHomeScreen/qml_eco3.0/` unless a full `/tmp/...` path is given.
Every claim is cited as `file:line` from the stock sources. Line numbers refer to the files as extracted.

---

## 0. Global environment every stock QML file relies on

These are C++-registered singletons/context objects; QML never defines them:

| Name | Role | Evidence of use |
|---|---|---|
| `AppHomeModel` | the giant state model: `isWideScr`, `isFullMode`, `isAdvancedMode`, `screenNotSupportSS`, `sysPopupShowing`, `isOnScrShowing`, `visibleOSDCall`, `isAtOtherApp`, `currentScreen`, `currentHomeAdvancedPageIdx`, `currentMenuPageIdx`, `currentSetupPageIdx`, `dayLightSuffix`, `textUpdate`, `usingRHD`, `listOptMenu`, list models (`listHomePage`, `listSubmenuSetup`, `listSubmenuInfor`, `listSubmenuMedia`, `listSubmenuDataService`, `naviWidget[0].sourceInput`, `weatherWidgetDL3`, `avWidgetHome`), methods (`prepareAdvancedDisplayList`, `editAllMenus`, `updateAllMenus`, `defaultEditHomeicon`, `updateItemTonedown`, `listWidgetAdvVisibleCount`) | `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:57`, `HomeScreen/main.qml:237-247`, `HomeScreen/Screen_HomeScreen_AllMenus.qml:130` |
| `AppHomeEngine` | command bridge to C++: `request_searchForSettings` (`component/QMenuBarSubmenu.qml:79`), `slotReqInjectAdvancedWidget` (`QHMI_HomeScreenMain_Advanced.qml:191`), `requestHideGlobalPopup` (`HomeScreen/main.qml:529`), `requestMAPByTouchWidget` (`Screen_HomeScreen_Widget_Advanced.qml:128`), `request_clock` (`:268`), `onClickQuietMode/onClickDriverTalk/onClickRearClimateControl` (`component/QRearComponent.qml:24-30`), `advancedModePageChange` (`QHMI_HomeScreenMain_Advanced.qml:362`) |
| `QML_Handler` | event pump: `qmlSendEvtKey(HMIEVENTS.EVT_..., data)` and `qmlChangeTopMsg(EVT_...)` for screen changes | `component/QMenuPageNoDrag.qml:65`, `QHMI_HomeScreenMain_Advanced.qml:230` |
| `ConstantModel` | geometry/typography constants: `MENU_BAR_Y` (=90), `SCREEN_WIDTH` (1280), `SCREEN_WIDE_WIDTH` (1920), `MENU_BAR_HEIGHT`, `CONTENT_WITH_MENUBAR_AREA_HEIGHT`, `SETUP_PAGE_Y`, `LSTVIEW_SUB_Y`, `LSTVIEW_SUB_WIDTH`, `DELEGATE_SUBLIST_X/Y`, `FONT_SIZE_33/39/42/45`, `APP_HOME_POPUP_*`, `STRAIGHT_SCROLL_*`, `GENERAL_COLOR_BLACK`, `GENERAL_OPACITY_PERCENT_70` | `HomeScreen/main.qml:228-232`, `component/QMenuBarSubmenu.qml:9-10`, `popup/QHMI_AppHomePopup_OK.qml:31-32` |
| `defineModel` | image-path dictionary (`home_bg_icon_n/p`, `co_btn_*`, `img_loading[]`, `prefixImg99`, `prefixImgHome`, …) and a few colors (`ColorBrightGray`, `TextColorSelected`, `ColorSubTextGray`, `ColorDisableGrey`) | `component/QMenuPageNoDrag.qml:20-21`, `component/Q_HomeSubListDelegate.qml:14-18` |
| `HomeThemeHelper` | theme colors: `normalTextColor`, `disableTextColor`, `selectedTextColor`, `normalTextEVColor`, `firstTextPHEVColor`, `normalOptTextColor` | `component/common/QText.qml:6`, `component/QMenuItem.qml:98` |
| `TranslationsModel` | i18n: `STR_*` constants and `getDefineLocalize(label)` | `component/QMenuItemNoDragComp.qml:68` |
| `HMIEVENTS 1.0` / `AppHome_Enum 1.0` / `APPENUMS 1.0` | event IDs (`EVT_HOME_SOUND_BEEP_PLAY`, `EVT_MAIN_LIST_ITEMCLICKED`, `EVT_REQUEST_HIDE_POPUP`, `EVT_GOTO_SCREEN_*`), list types (`E_LIST_ALL_MENUS`, `E_LIST_SETUP_MAIN`, `E_LIST_MEDIA_GATE`), buyer/region enums | `component/QMenuPage.qml:301-309` |
| Image provider | `image://AppHomeScreen/<subdir>/<file>.png`; day/night suffix appended by `Q_ThemeImage` via `AppHomeModel.dayLightSuffix` (`"#GUI_DAY"`/`"#GUI_NIGHT"`) | `component/common/Q_ThemeImage.qml:6`, `component/QMenuItemNoDragComp.qml:60` |
| `DEVICE_WIDTH`, `DEVICE_HEIGHT` | unqualified globals (1280/720 or 1920/720 depending on wide mode) available in every file | `HomeScreen/main.qml:15-16` |

Shared JS: `/tmp/opencode/gen5w_ui/AppHomeScreen/Js/QChart.js` — a Chart.js-style chart library (BAR/DOUGHNUT/LINE/PIE/POLAR/RADAR, `QChart.js:1-8`) used by EV/electricity-use widgets; not part of the home-screen component set.

---

## 1. `component/common/` — primitive library (25 files)

### 1.1 Text & images

| Component | Type | Public API | Canonical usage | Depends on |
|---|---|---|---|---|
| `Q_ThemeImage.qml` | **Image with day/night theming** — the base image everywhere | `themeSource: string` (a `defineModel` path or `image://` URL); everything else is `Image` | `component/QMenuItemAllowEmpty.qml:26-36` (`themeSource: isEmptyItem ? "" : imageIconN`); `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:156-162` (status-bar bg, `fillMode: Image.TileHorizontally`) | `AppHomeModel.dayLightSuffix` (`Q_ThemeImage.qml:6`) |
| `QImage.qml` | empty alias of `Q_ThemeImage` | none added | trivial (`common/QImage.qml:3-5`) | — |
| `QText.qml` | **themed Text** — default color + HKMC fonts | `textBold: bool` (switches family `HKMC_FONT_B`/`HKMC_FONT_R`); all `Text` props | `component/QSubListDelegate.qml:41-49` (`font.pixelSize: 35; font.bold: root.forceFocus`); `component/QMenuItemNoDragComp.qml:52-76` (menu label with `lineHeightMode: Text.FixedHeight`, `lineHeight`, `wrapMode: Text.Wrap`) | `HomeThemeHelper.normalTextColor` (`QText.qml:6`), default `font.pixelSize: 39` (`QText.qml:7`) |
| `TitemDL3.qml` | N-mode graph bar image pair | `souceicon`, `widthbtn` | NMode widget only | `defineModel.nm_bg_graph_bar_01*` (`TitemDL3.qml:18`) |

### 1.2 Base interactive items

| Component | Type | Public API | Notes |
|---|---|---|---|
| `QItem.qml` | **base non-visual item** | props `groupID`, `indexFocus`, `mousePressed`, `mousePresseLong`, `forceFocus`, `usingBeepSound`; signals `itemIsFocused()`, `pressed()`, `released()`, `released_sata(var)`, `released_contentY(var)`, `pressAndHold()`, `canceled()`; functions `itemPressed()/itemReleased()/itemPressAndHold()/itemCanceled()/isPressed()` (`common/QItem.qml:6-41`) | Auto-cancels press on x/y change (`QItem.qml:42-43`) — this is how drags cancel clicks |
| `Q_Item.qml` | older sibling of QItem (used by menu pages/sliders) | adds `mouseEnable`, `mousePressedLong`, `forceDisFocus`, `customHKWheel`, `diffCanceled()` (`component/Q_Item.qml:5-21`) | `QMainScreenAnimation`, `Q_STSliderBar`, `QMenuItem*` derive from it |
| `QEnlargedMouseArea.qml` | **MouseArea with magnified touch margins** — the stock answer to small touch targets | `widthLeftMultiple/widthRightMultiple/heightTopMultiple/heightBottomMultiple: real` (1.0 = exact, 1.5 = 50% larger each side) (`common/QEnlargedMouseArea.qml:6-20`) | Used on every grid delegate: `component/QMenuPageNoDrag.qml:52-60` (edges of grid get 1.25/1.5); `HomeScreen/Screen_HomeScreen_Submenu_Information.qml:96-105` |
| `QRootScreen.qml` | **full-screen page base** | `isOnTop: bool`, `isEnable: bool`; contains a `z:1000` MouseArea that plays the beep on any release and propagates events (`common/QRootScreen.qml:11-21`) | Every screen page derives from it: `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:7`, `Screen_HomeScreen_AllMenus.qml:11`, `popup/QHMI_AppHomePopup_OK.qml:7`. Pages gate input with `isEnable: !AppHomeModel.sysPopupShowing` (`QHMI_HomeScreenMain_Advanced.qml:151`) |

### 1.3 Buttons

All stock buttons share conventions: a 4-element `icons` array indexed `[_NORMAL, _FOCUSED, _PRESSED, _DISABLE]`, beep-on-release only when `usingBeepSound: true`, and press cancel when the finger slides out (`onPositionChanged: if(!containsMouse) mousePressed = false`).

| Component | Type | Public API (key members) | Example |
|---|---|---|---|
| `QButton.qml` | **icon+label button** (single image bg) | `icons: [n,f,p,d]`, `label/labelSize/labelColor/labelFont/txtBold`, `labelYOffset`, `sourceX/sourceY`, `iconWidth/iconHeight`, `usingBeepSound`, `forceFocus`; signals `pressed/released/pressAndHold/canceled` (via QItem). Icon state binding: `common/QButton.qml:28` | split-screen button: `HomeScreen/main.qml:235-254` (`icons: [split_btn_on_n, split_btn_on_n, split_btn_on_p, split_btn_on_d]`, `usingBeepSound: true`, `iconWidth: 71`); list delegate: `component/Q_HomeSubListDelegate.qml:5-18` |
| `QEnlargedButton.qml` | QButton + enlarged touch area | adds `mouseWidthLeftMultiple` etc. aliases (`common/QEnlargedButton.qml:24-27`) | slider +/- buttons: `component/Q_SlidebarStyle.qml:65-80` (1.5 multiples, `usingBeepSound: true`) |
| `QTitleButton.qml` | **3-slice (left/mid/right) title-bar button**, height 90 | `iconsl/iconsm/iconsr` (3 states each), `label*`, `labelYOffset`, magnified-touch props (`common/QTitleButton.qml:27-30`); implicit `width: text.width + 40; height: 90` (`:6-7`) | header buttons: `component/QMenuBarSubmenu.qml:106-138` (`displayOffbtn`, `defaultbtn` with `label: TranslationsModel.STR_HOME_REARRANGE_DEFAULT`, `labelSize: ConstantModel.FONT_SIZE_39`, `labelFont: "HKMC_FONT_R"`) |
| `QVerticalButton.qml` | 3-slice vertical (top/mid/bottom) button | `topImages/middleImages/botImages`, `label*`, `labelYOffset`, `btnWidth/btnHeight`, `sourceX/sourceY` (`common/QVerticalButton.qml:8-23`) | popup background panel: `popup/QHMI_AppHomePopup_OK.qml:42-54` |
| `QHorizontalButton.qml` | 3-slice horizontal button rooted at `Q_ThemeImage`; `themeSource: (btnPressed)?icons[1]:(btnSelected?icons[2]:icons[0])` (`common/QHorizontalButton.qml:30`) | `icons`, `label*`, `eventID`, `dataEvent`, `btnSelected`, `leftImages/middleImages/rightImages`, `mirror` | popup OK button: `popup/QHMI_AppHomePopup_OK.qml:83-98` (left/mid/right `co_btn_bottom_*` images) |
| `QEnlargedHorizontalButton.qml` | same + enlarged mouse area | `mouseWidthLeftMultiple…` | factory-mode OSD button: `HomeScreen/Screen_HomeScreen_Widget_Advanced.qml:393-421` |
| `QHorizontalListButton.qml` | list-row button with separator line, states `_NORMAL/_SELECTED/_PRESSED/_FOCUSED/_DISABLE` (`common/QHorizontalListButton.qml:26-32`) | `leftImages/middleImages/rightImages` (4 states), `lineVisible`, `isSelectedRoot` | option-menu rows (QOptionMenu) |
| `QButtonStyle.qml` | Rear-seat style button (rooted at `Q_ThemeImage`, default 100×95) with 3-slice bg `re_btn_bg_h486_*` | `icons`, `btnFocus/btnPressed/btnDisable`, `isvisbleQuietMode`, `titlecomp` (`common/QButtonStyle.qml:8-50`) | base of `component/QRearComponent.qml:12` |
| `QButtonStyleDL3.qml` | NMode variant, bg `nm_bg_select_type_*` | same API as QButtonStyle (`common/QButtonStyleDL3.qml:11-37`) | NMode screens |
| `QButtonCheckbox.qml` | **checkbox row**: `sourceimg[state]` center image + hardcoded **red Rectangle placeholder** at left 33px (`common/QButtonCheckbox.qml:63-72` — debug leftover, do not copy) | `eventID` (sent via `QML_Handler.qmlSendEvtKey(eventID)` on click, `:114-116`), `btnFocus/btnPressed/btnDisble`, `txtElide/clipText` | setup lists |
| `QButtonActive.qml` | checkbox/active row; `isChecked` flips on release (`common/QButtonActive.qml:115-121` area, release handler flips `isChecked` then `buttonClicked()`); check icon `co_btn_check_on/off_n` at left 21 (`:66-76`); **no beep** (commented out, `:35` in tail) | `isActiveType/isCheckType/isChecked` | climate/settings toggles |
| `QButtonNMode.qml` | compact NMode button, `themeSource: (btnPressed)?icons[1]:(btnFocus)?icons[2]:icons[0]` (`common/QButtonNMode.qml:44`); **always beeps on release** (`:127`) | `eventID`, `sourceimg` | NMode widget |
| `QItemMenu.qml` | settings-box row (3-slice box `co_bg_box_*`), checkbox support | `bgTop/bgMid/bgBot/bgOne`, `labelItem`, `isTopElement/isBotElement/isOneElement`, `itemSelect/isChecked/isPressed`; signals `clicked/pressed/cancel/released/pressAndHold` (`common/QItemMenu.qml:35-60`) | Setup app lists |
| `QHMI_ToggleButton.qml` *(in `component/`, not common)* | **full-width toggle bar** (left/mid/right slices `co_btn_bottom_*`), states `norm/focus/press` (`component/QHMI_ToggleButton.qml:30,152-164`); beeps on release only if press started inside (`:124-131`) | `iconsLeft/iconsMid/iconsRight`, `label*`, `btnText*`, `isActiveType/isCheckType/isChecked`; signals `btnPressed/btnCancel/btnRelease` | Media-gate buttons: `HomeScreen/Screen_HomeScreen_Submenu_Media.qml:194-209` (Edit), `:223-237` (Close), `:238-272` (OK/Cancel) |

### 1.4 Lists, scrollbars, animation

| Component | Type | API | Notes |
|---|---|---|---|
| `QListVew.qml` (sic) | ListView with hard-key focus helpers | `groupID`, `numberItemInview`, `offset`, `_stopAtBounds`; functions `focusUp/focusDown/excHK_DialPress/excHK_DialRelease/setViewUpPos/setViewDownPos` (`common/QListVew.qml:5-68`) | snap-to-page on movement end (`:73-83`); used by EditMenu right list (`HomeScreen/Screen_HomeScreen_Submenu_EditMenu.qml:183-193`) |
| `QLoadingComponent.qml` | spinner + "Preparing" label | binds to outer ids `isNaviWidget`, `showLoading`, `load.status`, `isWidgetLoaded` (context-dependent ids, `common/QLoadingComponent.qml:14-15,37-38`) | loaded inside `QWidgetItem` (`component/QWidgetItem.qml:38-44`) |
| `QPopupAnimationItem.qml` | **fade in/out wrapper for popups** | `startOpenAnimation()/startCloseAnimation()`, signal `closePopupFinish()`, `aniDuration` (100 ms default), starts at `opacity: 0` (`common/QPopupAnimationItem.qml:6-18`); force-closes on `AppHomeModel.onSwitchToOtherApp` (`:44-62`) | every popup centers one of these |
| `Q_StraightScrollBar_Opt.qml` | vertical scrollbar with OpacityMask thumb | `heightSet`, `listContentY/listContentHeight/listHeight`, `yPos` (`common/Q_StraightScrollBar_Opt.qml:6-12`) | variant of `component/MyMenuComponent/QHMI_StraightScrollBar.qml` (same API, `QHMI_StraightScrollBar.qml:6-12`), used at `HomeScreen/Screen_HomeScreen_Submenu_DataService.qml:318-327` |
| `QEditLayoutButton.qml` | Advanced/Basic mode selector card | `sourceTypeHome`, `label`, `leftImagesBG/middleImagesBG/rightImagesBG`, `radioOnImg/radioOffImg`, `btnSelected/btnPressed`, signal `releaseEvent()` (`common/QEditLayoutButton.qml:7-22`); no beep inside (caller beeps) | `HomeScreen/Screen_HomeScreen_Submenu_EditHomeLayout.qml:68-107` (two cards, caller does `qmlSendEvtKey(EVT_HOME_SOUND_BEEP_PLAY)` then sets `AppHomeModel.isAdvancedMode`) |

---

## 2. `component/` — home-screen structural components (35 files + MyMenuComponent)

### 2.1 Grid pages (GridView wrappers)

| Component | What it is | API | Example |
|---|---|---|---|
| `QMenuPageNoDrag.qml` | **the home-screen app grid page** (no drag/rearrange; press + 650 ms long-press) | GridView props + `rootView`, `parentView`, `pageIndex`, `selectedItemId`, `pageModel`, `imgPath`; signals `itemClicked(int _itemID, var _tonedown)` (`:13`), `itemPressAndHold()` (`:18`); delegate wraps `QMenuItemNoDrag` in a Loader with `pageIndex/itemIndex/itemId/fontSize` context ids (`:44-51`) and a `QEnlargedMouseArea` (`:52`) | instantiated 3× in `HomeScreen/QHMI_HomeScreenMain_Advanced.qml:211-232, 252-273, 292-314` (`cellWidth: k_itemWidth` 306, `cellHeight: k_itemHeight` 249, `interactive: false`, click → `qmlSendEvtKey(EVT_MAIN_LIST_ITEMCLICKED, _itemID)`, long-press → `qmlChangeTopMsg(EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED)`) |
| `QMenuPage.qml` | grid page with in-page drag reorder (older AllMenus/Media) | signals `itemClicked(_itemID,_tonedown,_isAVSelected)`, `itemDropped(dropItemPage,dropItemIndex,dragItemIndex)`, `itemPressAndHold()`, `enableAllDropArea()`, `triggerDragEvent()` (`:13-17`); `customizeMode`, `dragging` | delegate chooses `QMenuItem` variant by `listType` (`:300-310`: `E_LIST_ALL_MENUS/E_LIST_SETUP_MAIN → idItemAllmenus`, `E_LIST_MEDIA_GATE → idItemMedia`) |
| `QMenuPageAllowEmpty.qml` | grid page supporting empty cells + cross-page drag | signals `itemClicked`, `itemDropped(dragItemPage,dragItemIndex,dropItemPage,dropItemIndex)`, `itemPressAndHold()`, `interDrop()`, `dragStarted(dragItemId, dragPageIndex)` (`:13-23`); uses `QMenuItemAllowEmpty` delegate; drag bounds `drag.minimumY: 255-180-3.3` etc. (`:135-138`); overlap detection at x>1040 (narrow) / x>1672 (wide) (`:93-114`) | used by `QCustomizedMenuListAllowEmpty` pages |
| `QMenuPageTotal.qml` | newest grid page: one big grid, page virtualization via opacity, cross-page drag with `notifyReleaseDrop/notifyDisplaced` signals (`:26-27`) | `currentPageIdx`, `itemCountPerPage: 12` (`:22`); delegate opacity gating (`:45`); `remove`/`displaced` transitions with out-of-list wipe logic (`:422-566`) | `Screen_HomeScreen_AllMenus_Rearrange_Adv.qml` (rearrange screen) |

### 2.2 Grid item delegates (the "app tiles")

The tile visual contract is a 4-image array `iconSource: [imgPath+iconN, iconP, iconF, iconD]` (Normal/Press/Focus/Dim) built in the delegate (`component/QMenuItemAllowEmpty.qml:14`, `QMenuItemNoDragComp.qml:12`, `QMenuItem.qml:19`) from model roles `iconN/iconP/iconF/iconD`, `label`, `itemID`, `tonedown`, `visible`, `visibleForEdit`, `imgPath`, `fontSize` (39 normal / 33 customize).

| Component | Variant | Notes |
|---|---|---|
| `QMenuItemNoDrag.qml` | thin wrapper: `QItem` + Loader that loads `QMenuItemNoDragComp.qml` unless `isEmptyItem` (`:12-16`); props `firstPress`, `isEmptyItem` | The home-screen tile. Press state: `icoPressed: mouseArea.pressed && mouseArea.containsMouse && firstPress` computed in the Comp (`QMenuItemNoDragComp.qml:13`) |
| `QMenuItemNoDragComp.qml` | tile contents: bg `home_bg_icon_n` (`:22`), press image loaded from `QMenuItemNoDragImagePComp.qml` (`:25-32`), icon via `QMenuItemNoDragImageNComp/DComp` loaders (`:34-40`), label Component with 2-line shrink to pixelSize 27 (`:50-77`) | Dim text opacity: day 0.2 / night 0.35 (`:60`) |
| `QMenuItemNoDragImageNComp.qml` | normal icon: `Q_ThemeImage { visible: !icoDim && !isDummy && !isEmptyItem; themeSource: iconSource[0] }` (`:6-9`) | |
| `QMenuItemNoDragImageDComp.qml` | dim icon: `visible: icoDim…; themeSource: iconSource[3]` (`:6-9`) | |
| `QMenuItemNoDragImagePComp.qml` | press bg: `themeSource: imageP; fillMode: Image.TileHorizontally` (`:6-10`) | press = full-cell image swap |
| `QMenuItemAllowEmpty.qml` | drag-aware tile with empty-cell support: props `isEmptyItem`, `isUnsupported`, `upItem`, `customizeState`, drag scale `1.2` + `Behavior on scale {NumberAnimation{duration:100}}` (`:31-35`); loads `QMenuItemAllowEmptyComp.qml` | used by QMenuPageAllowEmpty/Total |
| `QMenuItemAllowEmptyComp.qml` | press image `imageP` (`:8-20`), normal/dim icons (`:28-45`), two labels (customize vs normal, `:53-93`) with dim opacity day 0.2 / night 0.35 (`:63`) or inverted in customize (`:84`) | |
| `QMenuItem.qml` | older full tile (used for Media gate): adds `icoSelected` (AV selected, `:22`), selected image `imageS` (`:55-62`), `editBgIcoSource` for edit mode (`:110-118`, z: -1), special-case label "Bluetooth \n Audio" (`:106`) | used via `QMenuPage.qml`/`QMenuPageAllowEmpty.qml` `idItemMedia` |
| `QHomeItem.qml` | **Information-submenu tile** (grid 306×249 class): fixed `width: 254; height: 183` default (`:8-9`), N/P/F/D images at absolute `x_icon/y_icon` (`:32-33`), divider lines (`lineL/lineR`, `:41-51`), label + optional Kia second line (`:81-120`) | `HomeScreen/Screen_HomeScreen_Submenu_Information.qml:78-95` (`x_icon: 104, y_icon: 51`, `iconSource: [_prefixImg+iconN, _prefixOtherImg+iconP, _prefixImg+iconF, _prefixImg+iconD]`) |
| `QEditMenuItem.qml` | EditMenu **left-list** drag source: `slotID`, `sourceItem`, `iconLabel`, `isActiveDrag`, `isFromLefttList`, signal `swapSource(slotIDSelected, slotIDUnSelected)` (`:6-12`); `Drag.active: isActiveDrag` + hotSpot center (`:67-69`) | created dynamically: `HomeScreen/Screen_HomeScreen_Submenu_EditMenu.qml:563` (`Qt.createComponent("../component/QEditMenuItem.qml")`) |
| `QEditMenuItemRight.qml` | EditMenu **right target slot**: `slotID`, `pos`, `icoFocus`, `icoMove`; drop highlight `home_bg_highlight.png` (`:29-35`) | `Screen_HomeScreen_Submenu_EditMenu.qml:207-215` |
| `QWidgetItem.qml` | **plugin host** (627×405, `:7-8`): bg `widget_bg_widget_n.png` / `widget_bg_ad_widget_n.png` (`:29`), `sourceComponent` alias for the C++-injected plugin (`:10`), loading overlay (`:38-44`), press overlay `widget_bg_widget_media_p` at `opacity: 0.7` (`:72-78`), navi error text (`:58-70`); connects plugin signal `sendMouseEventToHostApp` on load (`:52-54`) | basic widget page: `HomeScreen/Screen_HomeScreen_Widget.qml:34-47` (navi), `:80-90` (second widget 627×405 at x 642/y 102), `:154-162` (third at x 1281/y 102) |
| `WidgetItem.qml` | widget-picker card (561×373, `:6-7`): 4-state bg `home_bg_edit_n/p/f/move` (`:12`), icon dictionary keyed by `STR_HOME_*` (`:13-30`), label at topMargin 55 (`:62-76`), icon at topMargin 143 (`:79-85`) | EditWidget (basic) list |
| `WidgetItemAdvanced.qml` | advanced widget-picker card (wide 438×402 / narrow 375×345, `:6-8`), bg `home_bg_ad_edit_8_*` (`:14`), icon dict incl. `STR_HOME_MAP`, call history, favourites (`:15-36`) | `HomeScreen/Screen_HomeScreen_Submenu_EditWidget_Advanced.qml:83-85, 278-280` |

### 2.3 Menu lists (paged horizontal grid containers)

| Component | What it is | API |
|---|---|---|
| `QCustomizedMenuList.qml` | paged horizontal ListView of `QMenuPage` grids (Media gate & legacy) | grid metrics `k_itemWidth/k_itemHeight/k_nbrOfColumns/nbrItemsPerPage` (`:17-27`), `lstModel`, `customizeMode`, `listPosY`, `totalPages`, `listType`, `currentPageIndex`, `draggingIndex/draggingPage/menuDummyItemIdx` (`:33-38`); signals `menuItemClicked(_itemID,_tonedown,_isAVSelected)`, `pageIndexChanged(index)`, `swapMenuItem(drag,drop)`, `clickOutSide()`, `activeDragAndDrop()` (`:78-82`); inner `ListView menuList` horizontal, SnapOneItem, StrictlyEnforceRange (`:108-115` area, `menuList` at `y: ConstantModel.MENU_BAR_Y`) | 
| `QCustomizedMenuListAllowEmpty.qml` | the modern paged grid used by AllMenus + Setup (1351 lines) | same signal set + `updateMenuItem(dragPage,dragIndex,dropPage,dropIndex)` and `indexChange()` (`:90-96`); wide 6 cols / narrow 4 cols, 306×249 cells (`:19-27`); functions `constructMenuModel()` (`:1062`), `updateHistoryPage()` (`:159`), `lockCurrentIndexChanged()` (`:183`), `updateItemTonedown()` (`:1334`), `getPageListModel()` (`:1291`) | 
| `QListViewHomePage.qml` | the **swipe-page ViewPager**: ListView with flick-distance guard (cancels flicks < 60 px, `:15-23`) | plain ListView; used as `homepage` with `orientation: ListView.Horizontal`, `snapMode: ListView.SnapOneItem`, `highlightRangeMode: ListView.StrictlyEnforceRange` (`HomeScreen/QHMI_HomeScreenMain_Advanced.qml:320-341`) |

### 2.4 Sub-list (submenu rows)

| Component | What it is | API |
|---|---|---|
| `QSubListMenu.qml` | curved/parallax ListView container | aliases `model`, `count`, `contentY/X`, `currentIndex`, `delegate`, `interactive`; signals `itemClicked(var index)`, `movementEnded()` (`:25-26`); curve math `calc_X/calc_Y/calc_Visible` (`:28-43`); inner ListView at `anchors.leftMargin: 397`, `y: ConstantModel.LSTVIEW_SUB_Y`, height 550 (`:49-62`) |
| `QSubListDelegate.qml` | list row (QButton base, size `ConstantModel.DELEGATE_SUBLIST_X/Y`, `:7-8`): icon + text + separator line (`list_line`, z: -1, `:24-29`); colors per vehicle variant (`:15-17`); `icons: ["", list_f, list_p, ""]` (`:19`) | `HomeScreen/Screen_HomeScreen_Submenu_DataService.qml:253-277` (`themeSource: pressed? iconP : current? iconF : tonedown? iconD : iconN`, `labelItem.text: TranslationsModel.getDefineLocalize(modelData.label) + AppHomeModel.textUpdate`, click → `qmlSendEvtKey(EVT_SUBMENU_DATASERVICE_ITEMCLICKED, modelData.itemID)`) |
| `Q_HomeSubListDelegate.qml` | wider variant (1238×112, `:7-8`) used on home sub-screens | same pattern (`icons: ["", list_f, list_p, ""]`, `:18`) |

### 2.5 Screens chrome & misc

| Component | What it is | API / notes |
|---|---|---|
| `QMenuBarSubmenu.qml` | **the 90 px submenu title bar**: bg `bg_title` tiled (`:37-43`), shadow line at y 90 (`:45-53`), centered-or-left title (`QText` bold 42, `:84-92`, alignment decided by `distance_align()` `:158-184`), optional search icon button (`:64-82`, `AppHomeEngine.request_searchForSettings()`), Display-Off and Default `QTitleButton`s (`:106-138`). Widths: `SCREEN_WIDE_WIDTH` unless split-screen showing (`:9`). signals `buttonDisplayOffCicked()`, `buttonDefaultClicked()`, `buttonUpdateClicked()` (`:21-24`); aliases `displayOffEnale/defaultEnale/searchEnable/title` | `HomeScreen/Screen_HomeScreen_AllMenus.qml:79-92` (`y: ConstantModel.MENU_BAR_Y`, `defaultEnale: customizeMode`), `Screen_HomeScreen_Submenu_Setup.qml:86-111` (`searchEnable: true`, display-off → `qmlSendEvtKey(EVT_CLICK_DISP_OFF)`) |
| `QMainScreenAnimation.qml` | **page-transition animation**: fade 0→1 + slide from `width/2`→0, 150 ms, `Easing.InCubic/OutQuad` (`:37-61`); `startAnimation()` respects `AppHomeModel.preventScreenAnimation` and `isStopAni` (`:12-36`); signal `scrTransitionComplete(bool)`; restores on `onSwitchToOtherApp` (`:62-70`) | every screen page instantiates it as `right_left_animation` targeting its `contentArea` (`Screen_HomeScreen_AllMenus.qml:94-98`, `Submenu_Setup.qml:120-124`) and calls `startAnimation()` in `Component.onCompleted` (`AllMenus.qml:301`) or `onStateChanged` (`Submenu_Setup.qml:11-19`) |
| `Q_SlidebarStyle.qml` | the actual slider visuals/logic: −/+ `QEnlargedButton`s (`:65-140`), bar segments at `y: 101`, `x: offsetSpaceX (188)` (`:149-179` area), signals `signalItemSelected/signalValueChanged/signalMouseReleased/signalMouseDraged/signalMouseCancelDraged` (`:59-63`) | |
| `Q_STSliderBar.qml` | public slider wrapper (966×143, `:33-34`): aliases `maximumValue/minimumValue/value/imgBg/imgBar/mouseEnable/isNegative` (`:35-54`), signals `updateValue(int)`, `requestWheelLeft/Right()`, `requestReleased()`, `mouseCancelDraged()/mouseDraged()` (`:46-51`) | climate popups (`Climate/AppClimate_Popup_Set_Blower.qml` etc.) |
| `QMenuBarSubmenu` companions: `MyMenuComponent/QHMI_StraightScrollBar.qml` | vertical scrollbar (see §1.4) | `DataService.qml:318-327` |
| `QBatteryHomeItemEco3.qml` | EV battery glyph (capacity image 00–20 png chosen from `AppPHEVModel.batCapacity`, `:18-52`); charging icon overlay (`:63-71`) | inside `QButtonBatteryHomeItemEco3` |
| `QButtonBatteryHomeItemEco3.qml` | **EV battery home widget** (QButton base): vehicle-body-type image templating `defineModel.home_ic_battery_bg_nor.replace("@", getImgType())` (`:23-46`), SOC text with DropShadow (`:64-135`), range row (`:136-179`) | loaded on widget page 0: `HomeScreen/Screen_HomeScreen_Widget_Advanced.qml:424-440` (`evLoader` at left 520 / top 290; `onReleased: AppHomeEngine.requestShowEVApp()`) |
| `QRearComponent.qml` | rear-seat feature card (QButtonStyle base): quiet-mode LED, title, icon, description; actions routed to `AppHomeEngine.onClickQuietMode/onClickDriverTalk/onClickRearClimateControl` (`:23-31`) | `RearSeatFeatures/Screen_RearQuietMode.qml`, `Screen_DriverTalk.qml` |

---

## 3. Home-screen page structure (HomeScreen/)

### 3.1 `main.qml` — root shell (548 lines)

Root `Rectangle` `width: DEVICE_WIDTH; height: DEVICE_HEIGHT` (`:15-16`).

| Region | Geometry | Evidence |
|---|---|---|
| Screen loaders `loader_1`/`loader_2` (double-buffered page swap) | full size; `screen_Transition()` nulls old loader and sets `current_loader.sourceComponent = screenNext` (`:40-49`); `switch_current_loader()` hides/unloads the previous (`:51-64`) | `:66-78` |
| AW black background | x = RHD? −640/0 : fullMode? DEVICE_WIDTH : SCREEN_WIDTH; 640×720; hidden when `AppHomeModel.screenNotSupportSS` | `:81-88` |
| AW dummy (boot placeholder) + its loader | x like above, **y: 90, 640×630**, spinner at topMargin 207 | `:91-137` |
| **`loader_3` — Assistant Window sibling layer** | x = same formula, **y: 0, 640×720**, `visible: AppHomeModel.visibleSS`, `sourceComponent: AWComponent`, `asynchronous: true`, slide `Behavior on x` 200 ms `Easing.OutQuad`, z toggled to 10 while `isShowingOSD` (`:27-34`) | `:139-203` |
| Split-screen grab handle `recSplit` | right edge, 71×185 at y 313 | `:212-221` |
| `splitBtn` (QButton) | right of content, vertically centered, `iconWidth: 71`; visible only on wide screens and not in edit/submenu screens (long `currentScreen !== EVT_*` chain `:237-247`); toggles `AppHomeModel.isFullModeforSS/isFullMode` and sends `KEY_EVT_HOME_CLICK_SPLIT_BUTTON` (`:255-272`) | `:235-293` |
| `idContent` placeholder | `anchors.topMargin: ConstantModel.MENU_BAR_Y` (90), `height: ConstantModel.CONTENT_WITH_MENUBAR_AREA_HEIGHT` (630) | `:223-233` |
| **`loader_4` — Status bar** | z: 2, width DEVICE_WIDTH (wide) else 1280, **height: 90**, `sourceComponent: STComponent`, `visible: AppHomeModel.showStatusbar && currentScreen !== EVT_GOTO_SCREEN_LOADING`; x shifts with `AppHomeModel.homeMainContentX` on the wide advanced home (`:307-319`) | `:306-319` |
| Status-bar click shields | `statusBarLeftCopy` (z: 3, width 348/384 or 0+180+75 on wide home) and `statusBarRightCopy`; both dismiss the option menu (`AppHomeModel.optMenuAnimation = false`) with `propagateComposedEvents: true` and `mouse.accepted = false` | `:340-391` |
| `dimStatusBar` | 70% black over the bar, z: 3, `visible: AppHomeModel.isOnScrShowing || (AppHomeModel.sysPopupShowing && (!screenNotSupportSS || currentHomeAdvancedPageIdx !== 0))`; click sends `AppHomeModel.sendDbusHideGlobalPopup()` | `:393-422` |
| OSD layer | `onsDataList` ListModel of 4 OSD sources loaded by `load_OSD(Onsdata)`; Repeater of Loaders at **z: 4**, y 0 implicit, height 720−90, width full or 1280, left offset 640 when RHD with SS | `:428-454` |
| `dimBG` popup dimmer | 70% black, z: 4, `visible: AppHomeModel.sysPopupShowing`; topMargin 0 only on widget page 0 with screenNotSupportSS, else 90; height full on page 0 else `parent.height - 90` | `:456-470` |
| Touch-kill & guards | full-screen MouseArea while `AppHomeModel.disableTouch` (auto re-enable after 1000 ms, `:498-521`); `timerHidePopup` requests `AppHomeEngine.requestHideGlobalPopup()` 5 ms after screen change (`:524-531`); `coverScreen` MouseArea at z: −9999 (`:533-540`) | |

Navigation events seen here: `HMIEVENTS.EVT_GOTO_SCREEN_SUBMENU_EDIT_WIDGETS(_ADVANCED)`, `EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS`, `EVT_GOTO_SCREEN_SUBMENU_EDIT_HOMELAYOUT`, `EVT_GOTO_SCREEN_SUBMENU_INFOMATION`, `EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED`, `EVT_GOTO_SCREEN_SUBMENU_SETUP`, `EVT_GOTO_MANUAL_HOME_SCREEN/DRIVER_TALK/DRIVING_INFO`, `EVT_GOTO_SCREEN_WIDE_ADVANCED`, `EVT_GOTO_SCREEN_LOADING`, `EVT_GOTO_MAIN_HOME_SCREEN` (`:237-247, 317, 324, 334`).

### 3.2 `QHMI_HomeScreenMain_Advanced.qml` — the Advanced home (572 lines)

- Root `QRootScreen` full size; `isEnable: !AppHomeModel.sysPopupShowing` (`:151`); statusbar bg (`co_bg_statusbar`, `:156-162`) and home bg (`home_main_bg_01/02`, `:164-169`).
- Grid metrics: `k_itemWidth: 306`, `k_itemHeight: 249`, `k_nbrOfRows: 2`, `k_nbrOfColumns: 6 wide / 4 narrow`, `nbrItemsPerPage: 12/8`, `k_menuPageWidth: 1836 wide / 1224 narrow` (`:26-38`).
- **Pages**: `getModelList()` returns `[pluginPage, listPage1, listPage2, listPage3]` depending on `totalPagesNormalMode` (1–3) (`:75-85`). `pluginPage` is an `Item` with `objectName: "itemWidgetPlaceHolder"` — the C++ widget injection point, calls `AppHomeEngine.slotReqInjectAdvancedWidget()` on completion (`:171-194`). `listPageN` items sit at `y: MENU_BAR_Y + MENU_BAR_Y/2` (=135) and hold a `QMenuPageNoDrag` (`:196-318`).
- **`homepage` QListViewHomePage**: horizontal, model `totalPagesNormalMode + 1`, SnapOneItem/StrictlyEnforceRange, `flickDeceleration: 35000`, `displayMarginBeginning: width*(pages+1)`, `pressDelay: currentIndex == 0 ? 0 : 100`, `interactive: !isButtonWidgetActive` (`:320-341`); delegate loads `homePageItem[index]` in a Loader, visible only when not animating or current (`:342-355`). `onCurrentIndexChanged` writes `AppHomeModel.currentHomeAdvancedPageIdx` and calls `AppHomeEngine.advancedModePageChange` (`:358-363`).
- **Page indicator**: `Row indicatorGroup` bottom-centered, 26 px tall, dots `home_ic_indicator_w_s/n`, auto-hide after 5 s via `OpacityAnimator` + `reqShowIndicator()/startHideIndicator()` (`:403-455`).
- `QMainScreenAnimation right_left_animation` targeting `homepage` (`:397-401`); started in `Component.onCompleted` when returning to a non-zero page (`:546-548`).
- Model wiring: `constructMenuModel()` → `AppHomeModel.prepareAdvancedDisplayList(nbrItemsPerPage)` (`:87-93`); reacts to `onRequestUpdateAllMenu`, `onMenuListUpdated`, `onAllMenuEnableUpdated`, `onRequestUpdatePageIndexChanged`, `onKeepCurrentScreen`, `onSysPopupShowingChanged` (returns list to bounds), `onReqShowIndicator` (`:457-530`).
- `Component.onCompleted` sets `AppHomeModel.screenNotSupportSS = true` (this car: AW disabled), binds `AppHomeModel.listOptMenu` to `optMenuModelData` (Display Off / Edit Menus / Manual) (`:532-544`).

### 3.3 `Screen_HomeScreen_AllMenus.qml` — All apps grid (344 lines)

- Root `QRootScreen`, width SCREEN_WIDE_WIDTH when fullMode+wide else 1280, height 720 (`:24-25`); `isEnable: !sysPopupShowing` (`:22`).
- `QMenuBarSubmenu headerMenu` at `y: MENU_BAR_Y` with title All Menu / Edit All Menus and Default button in customize mode (`:79-92`).
- `QCustomizedMenuListAllowEmpty menuList`: `listType: E_LIST_ALL_MENUS`, `lstModel: AppHomeModel.listHomePage`, `currentPageIndex: AppHomeModel.currentMenuPageIdx`, `maxNumPages: 2 wide / 3 narrow`, `listPosY: ConstantModel.SETUP_PAGE_Y` (`:125-134`). Click → `EVT_MAIN_LIST_ITEMCLICKED`; swap → `AppHomeModel.editAllMenus(1, …)`; update → `AppHomeModel.updateAllMenus(1, …)` (`:138-146`).
- Page indicator Row bottom-centered (`:160-179`); customize help text at verticalCenterOffset 217 (`:180-189`).
- Outside-click exits customize mode if movement < 20 px (`:42-77`); back button: customize → finish, else `EVT_GOTO_ALL_SCREEN_ITEMS` (`:258-266`); option menu rebuilt on customize change (`:306-328`).
- Rearrange variant `Screen_HomeScreen_AllMenus_Rearrange_Adv.qml` (271 lines) uses the same pattern with `QCustomizedMenuListAllowEmpty` at `:145` and content area 630 px at y 90 (`:51-53`).

### 3.4 Submenu pages

| Screen | Layout | Navigation events |
|---|---|---|
| `Screen_HomeScreen_Submenu_Setup.qml` | full-screen QRootScreen; header `QMenuBarSubmenu` at y 90 with search + display-off + Default (reset popup `KEY_EVT_SHOW_POPUP_SETTING_RESET_MENU`, `:101`); `QCustomizedMenuListAllowEmpty` `listType: E_LIST_SETUP_MAIN`, `lstModel: AppHomeModel.listSubmenuSetup`, `maxNumPages: 2/3`, `isFullMode: true` (`:161-198`); indicator Row (`:199-213`); customize guide text (`:152-160`) | click → `EVT_SUBMENU_SETUP_ITEMCLICKED` (`:175`); back → `EVT_BACK_TO_PREV_SCREEN` (`:232`); swap → `AppHomeModel.editAllMenus(2, …)` (`:182`) |
| `Screen_HomeScreen_Submenu_Information.qml` | header at y 90 (`:44-51`); **plain GridView** `mainGridItemMenu` (not QCustomizedMenuList): y 180 (narrow, >4 items) or 305, cell 306×249, width 1224 or `count*306`, `QHomeItem` delegates (`:62-128`); model built manually from `AppHomeModel.listSubmenuInfor` into a local ListModel (`:131-146`) | click → `EVT_SUBMENU_INFORMATION_ITEMCLICKED` + `EVT_HOME_SOUND_BEEP_PLAY` (`:119-125`) |
| `Screen_HomeScreen_Submenu_Media.qml` | Media gate: bg `me_bg_mediagate(_c/_edit)` at `MEDIA_GATE_BG_X` (`:179-187`); title row at y 102 (`:188-221`) with `QHMI_ToggleButton bnt_edit`; close/OK/Cancel toggle buttons at `MEDIA_GATE_BNT_CLOSE_Y` (`:223-272`); `QCustomizedMenuList` with `k_itemWidth: MEDIA_GATE_GRID_1920_W/6` (wide) or `/4`, `listPosY: 198`, `custoBGVisible: false` (`:273-301`); indicator at y 601 (`:302-319`) | click → `EVT_SUBMENU_MEDIA_ITEMCLICKED` (`:295`); close → `EVT_MEDIA_GATE_CLOSE_CLICKED` (`:234`); swap → `AppHomeModel.editAllMenus(4, …)` (`:299`) |
| `Screen_HomeScreen_Submenu_DataService.qml` | curved list: `QSubListMenu` full-size, `model: AppHomeModel.listSubmenuDataService`, `QSubListDelegate`s with parallax `calc_X` on contentY change (`:243-294`); bg slides −778 px on entry (`:224-240`); `QMenuBarSubmenu` at y 90 (`:295-299`); `QHMI_StraightScrollBar` at `STRAIGHT_SCROLL_X`, y 199, visible when `count > 5` (`:318-327`); submenu icon at left 126/y 318 (`:309-317`) | click → `EVT_SUBMENU_DATASERVICE_ITEMCLICKED` (`:264`); opt menu = Subscription/Split (`:338`) |
| `Screen_HomeScreen_Manual.qml` | `Rectangle` at **y: MENU_BAR_Y, height: DEVICE_HEIGHT − MENU_BAR_Y** (`:22-25`) — the canonical "start below the bar" pattern; `QMenuBarSubmenu` at y 0 inside (z 1) (`:33-40`); driving-lockout text container 540 px (`:48-60`) | opt menu Display Off / Search-for-settings (`:27-32`) |

### 3.5 Widget pages

- `Screen_HomeScreen_Widget.qml` (basic mode, 298 lines): three `QWidgetItem`s — navi 618×603 at (12,102) (`:16-19,34`), second 627×405 at (642,102) (`:86-89`), third at (1281,102) (`:160-162`); each gates `pluginOpacity: (AppHomeModel.visibleOSDCall && callOSD.showCallOSD()) ? 0 : 1` (`:47,90,162`); long-press opens edit (`qmlChangeTopMsg(EVT_GOTO_SCREEN_SUBMENU_EDIT_WIDGETS)`, `:123`); click launches `EVT_MAIN_LIST_ITEMCLICKED` with `AppHomeModel.listWidget[n].itemID` (`:141-142,213-214`).
- `Screen_HomeScreen_Widget_Advanced.qml` (page 0 of Advanced home, 467 lines): full-size Item; no-map bg (`:106-112`); **navi map loader `content` 836×720 anchored right** (`:114-132`, click → `AppHomeEngine.requestMAPByTouchWidget()`); weather widget loader 1920×163 top-right (`:210-258`); clock: big `QText` 120 px at left 75 / verticalCenterOffset 399, AM/PM 45 px, date 34 px at offset 303 (`:272-305`), clock MouseArea → `AppHomeEngine.request_clock()` (`:260-270`); EV battery widget loader at left 520/top 290 (`:424-440`); AV widget area 617/970×720 (`:442-465`); factory OSD component (`:342-423`).

### 3.6 Edit modes

| Screen | Structure |
|---|---|
| `Screen_HomeScreen_Submenu_EditHomeLayout.qml` | full-screen; header at y 90 (`:48-54`); content 540 px at topMargin 180 (`:62-67`); two `QEditLayoutButton` cards 789×366 (wide) at left 165/top 30 and to its right (`:68-107`) selecting Advanced/Basic (`AppHomeModel.isAdvancedMode`); guide text near bottom (`:110-126`) |
| `Screen_HomeScreen_Submenu_EditMenu.qml` | full-screen; custom top bar at y 90 (`:59-92`) + `QMenuBarSubmenu` (`:94-105`); left = source list of all apps, right = `content_right` (800/1281 px, `:120-125` area) with horizontal `QListVew m_list_view_target` of `QEditMenuItemRight` slots (`:183-215`); drag with 500 ms hold timer, drop → `AppHomeModel.updateHomeMenuByPos(dragPosition, dropPostion)` (`:239-249` area); dynamic creation of `QEditMenuItem` at `:563` |
| `Screen_HomeScreen_Submenu_EditWidget.qml` | left picker `listWidget1` 561×519 of `WidgetItem` cards (`:64-81`), right drop area `listWidget2` 561/1152×363 (`:258-273`), footer hint (`:503-511`); header `QMenuBarSubmenu` at y 90 (`:540-542`); drag ghost at `startPosition + 20` (`:217-226`) |
| `Screen_HomeScreen_Submenu_EditWidget_Advanced.qml` | same shape with `WidgetItemAdvanced` cards: picker 438/375×519 (`:65-85`), drop area 1332(wide)/759×402 or 345 (`:261-278`), footer 117/174 (`:504-505`); dynamic `Qt.createComponent("../component/WidgetItemAdvanced.qml")` at `:161,375` |

### 3.7 `Screen_HomeScreen_Loading.qml`

Full-screen black + centered 117 px spinner from `defineModel.img_loading[]` (`:19-40`); 10 s watchdog timer sends `EVT_BACK_TO_PREV_SCREEN` (`:47-58`); when `AppHomeModel.isNaviBootCompleted` flips true it sends `EVT_MAIN_LIST_ITEMCLICKED` with `AppHomeModel.naviWidget[0].itemID` after 200 ms (`:60-84`).

### 3.8 `QHMI_HomeScreenMain_Wide.qml` (legacy wide home, 288 lines)

Non-advanced wide layout with `QHomeItem`-style delegates; item clicks send `EVT_HOME_MENU_LIST` (`:257-258`), long-press opens edit menus (`EVT_GOTO_SCREEN_SUBMENU_EDIT_MENUS`, `:123-124`), AllMenus button sends `KEY_EVT_REQUEST_GOTO_SCREEN_ALL_MENUS` (`:144-145`).

---

## 4. `popup/` — popup patterns (14 files)

**Universal pattern** (see `QHMI_AppHomePopup_OK.qml`):

1. Root is `QRootScreen`; **geometry**: `y: (currentScreen === EVT_GOTO_SCREEN_WIDE_ADVANCED && currentHomeAdvancedPageIdx === 0) ? 0 : 90`; width `SCREEN_WIDE_WIDTH` when (fullMode||screenNotSupportSS)&&wideScr&&!isShowingSSOSD else `SCREEN_WIDTH`; height `DEVICE_HEIGHT` on widget page 0 else `DEVICE_HEIGHT - 90` (`QHMI_AppHomePopup_OK.qml:9-11`, same in `QHMI_AppHome_Popup_Yes_No.qml:10-11`, `QHMI_AppHome_Popup_Information.qml:10-11`).
2. Full-size MouseArea: press → `rec_parent.startCloseAnimation()` + beep (`OK.qml:21-28`); dim `Rectangle` black 70% (`:29-33`).
3. Content = `QPopupAnimationItem` centered, width/height from popup size class (`_POPUP_MIN/MID/MAX` → `ConstantModel.APP_HOME_POPUP_WIDTH_*`, `:146-150`); height grows 45 px per text line (`:170-177`).
4. Panel bg = `QVerticalButton` 3-slice (`co_bg_popup_{mini,mid,max}_{t,m,b}`, `:151-165`).
5. Buttons = `QHorizontalButton` 3-slice `co_btn_bottom_*` (`:83-98`); Yes/No = two 78 px buttons `(width-36)/2` (`QHMI_AppHome_Popup_Yes_No.qml:91-140`).
6. Lifecycle: `Component.onCompleted: rec_parent.startOpenAnimation()` (`OK.qml:112-113`); `onClosePopupFinish: qmlSendEvtKey(EVT_REQUEST_HIDE_POPUP, _OSD_ID)` (`:106-108`). Each popup owns a unique `_OSD_ID` (OK=6, Media_NoDevices=5, VoiceRecognize=4, EManual=16, PhoneProjection=19, SXM=20, Unavailable=21, Feature_Unavailable=22 — grep `popup/*:_OSD_ID`).
7. Hide while a system popup shows: `onIsSysPopupShowingChanged: root.opacity = isSysPopupShowing ? 0 : 1` (`OK.qml:127-136`).
8. `QHMI_AppHome_Popup_Information.qml` adds a second MouseArea over the status-bar strip (width `SCREEN_WIDTH - parent.width`, height 90) with per-type `forceQuitFunc()`/`forceQuitFunc_3s()` (`:40-80`).
9. `QOptionMenu.qml` (the status-bar "Menu" dropdown): full-screen `QRootScreen`, list panel at y 90, width `MENU_LIST_WIDTH`/`MENU_LIST_WIDTH_8`, height `min(count*MENU_LIST_HEIGHT + 4, 630)` (`QOptionMenu.qml:126-135`), outside-click at y≥90 closes (`:62-80`), rows are `QHorizontalListButton` 104 px (`:23`). `QOptionMenuWidgets.qml` is the widget-page variant.

Inventory: `QHMI_AppHomePopup_OK`, `_Yes_No`, `_Information`, `_Media_Gate`, `_Media_NoDevices`, `_Notification_VoiceRecognize`, `_EManual_Not_Support`, `_Feature_Unavailable`, `_PhoneProjection_TMU_DuringCall`, `_SXM_Subcription`, `_Unavailable`, `_ValetMode_GuestMode`, `QOptionMenu`, `QOptionMenuWidgets` (`ls popup/`).

---

## 5. Statusbar (`/tmp/opencode/gen5w_ui/Statusbar/qml_eco3.0/`)

- `main.qml` root: `width: ST_DEVICE_WIDTH`, **`height: SttBar_ConstantModel.STATUS_BAR_HEIGHT` (=90)** (`main.qml:8-9`); tiled bg `co_bg_statusbar(_s)` (`:16-23`).
- Left cluster: Back / Home (SOS swap on `AppStatusbarModel.sosStatus`) / Profile (75×75, OpacityMask with `home_profile_mask`, only when `"AppHomeScreen" === ImageProviderID`, `:80-149`) / Menu (`QMenuButton`, `:152-171`) / Voice (`:173-187`) — all `QButton`s with 4-icon arrays and enlarged touch (`widthRightMultiple: 1.5`).
- Center-right: Date `QText` (leftMargin 788/1116, `:213-238`) and `QClockTime` (leftMargin 528/816, click → `AppStatusbarModel.clockBtnClicked()`, `:241-250`).
- Right: `QRightListIcons` — Repeater of status icons `image://<provider>/<png>` driven by `AppStatusbarModel` icon lists (`QRightListIcons.qml:78-94`).
- Interaction model: the bar is a **separate process/app surface** loaded by HomeScreen `main.qml` `loader_4` (`HomeScreen/main.qml:306-319`); apps talk to it only through `AppStatusbarModel` (back/home/menu/voice/profile/clock buttons, `backBtnVisible`, `homeBtnVisible`, `menuBtnVisible`, `menuStateChanged`, `requestCaptureScreen`) — never by touching its QML. HomeScreen only dims it (`dimStatusBar`, `HomeScreen/main.qml:393-422`) or shields clicks over it (`statusBarLeftCopy/RightCopy`, `:340-391`).

---

## 6. AssistantWindow (`/tmp/opencode/gen5w_ui/AssistantWindow/qml/`)

- Loaded by `HomeScreen/main.qml` as **`loader_3`, a sibling of the page loaders** (`HomeScreen/main.qml:139-147`): 640×720 at y 0, x = 1280 (or −640 RHD, or off-screen when full-mode/no-SS), `sourceComponent: AWComponent`, `asynchronous: true`, z raised to 10 while its OSD shows (`:24-34`). On this car `AppHomeModel.screenNotSupportSS = true` (set in `QHMI_HomeScreenMain_Advanced.qml:533`), so the AW is parked off-screen; a dummy split bg + spinner is shown while it boots (`HomeScreen/main.qml:91-137`).
- `AssistantWindow/qml/main.qml`: `loader` 640×720 (fullMode) or 630 at y 90 (`main.qml:19-26`) loads `AWBaseScreen` (ring menu of plugin cards, `Screen_AppAW.qml:18-25` — `QHMI_RingList` 640×(720/630) with per-plugin press/release/flick/multi-touch signals forwarded to `AWQML_Controller.qmlSendEvent`) or `Screen_AppAW4G.qml` when `AWModel.memCarSize != 0`.
- OSD layer `awOSD`: 640×(720/630) sliding in from x = ±640, `Behavior on x` 300 ms, hosts `OSDPlugin` and native OSDs (`onscreens/navtive_eco3.0/OSD_SOS.qml`, `OSD_USM*.qml`); `isShowingOSD` is exported so HomeScreen can raise its z (`AssistantWindow/qml/main.qml:12-17,38-105`).
- Consequence for overlays: anything declared inside `QHMI_HomeScreenMain_Advanced.qml` **cannot** cover the AW — an overlay that must cover it has to be declared at `main.qml` root level (see `docs/ui/UI_CONTEXT.md:105-110`).

---

## 7. Keyboard (OSK)

- `/tmp/opencode/gen5w_ui/Keyboard/libdkbd/` is **not QML** — it is the native DiviNum IME: `lib/libdkbd.so`, `libdhwr*.so` (handwriting), `dict/zh_TW_*.dict`, `hdb/zh_CN.hdb`, license keys (`find Keyboard/`).
- The QML OSK is a **C++ plugin type**: `import Keyboard 1.0` → `PluginKeyboardArea { maxLength, minLength, isWideScreen, isCheckSetBtnExeptionCase, isEnableSetBtnInExceptionCase, enableAnimation }` (`/tmp/opencode/gen5w_ui/AppStandbyClock/qml_eco3.0/firstSetup/KeyBoard.qml:1-12`).
- Text fields pair an `InputBox` with `keyboardAppModel` (`currentKeyboard`, `deleteBtnEnable`, `currentShiftMode`) and `keyboardEngine.clearInputStr()/setDefaultKeyboard()` (`AppStandbyClock/qml_eco3.0/firstSetup/InputBox.qml:113-118,484`; `firstSetup/Profile_FirstRun_InputScreen.qml:87-145` — the screen embeds `KeyBoard.qml` via a Loader `source: "KeyBoard.qml"` at `:130`).
- Keypad for PIN entry: `QHMI_KeyBoardNumerical.qml` (`AppStandbyClock/qml_eco3.0/components/`): 1280/1920×378 anchored bottom, API `outputText`, `defaultValue`, `maxTextLength`, signals `btnOkPressed()`, `resultText(int keyType, string strInput)` (`components/QHMI_KeyBoardNumerical.qml:10-26`).
- Home-screen side has **no text fields**; any native-looking text input should reuse `PluginKeyboardArea` (plugin) or the numeric keypad pattern above.

---

## 8. USAGE RULES (what "native-looking" means in this codebase)

1. **Beep on press/release.** Every activation plays the system beep *before* doing work:
   `QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)` — grid release `component/QMenuPageNoDrag.qml:65`, drag start `QMenuPageTotal.qml:259`, outside-click-exits-customize `HomeScreen/Screen_HomeScreen_AllMenus.qml:72`, popup dim press `popup/QHMI_AppHomePopup_OK.qml:26`. `QButton` does it for you when `usingBeepSound: true` (`common/QButton.qml:60-62`); `QButtonActive` deliberately does **not** (commented out, `common/QButtonActive.qml:135`).
2. **Press = image swap, not opacity.** Tiles swap a full-cell press image (`iconSource[1]` / `imageP`) shown only while `mouseArea.pressed && mouseArea.containsMouse && firstPress` (`component/QMenuItemNoDragComp.qml:13,25-32`; `QMenuItemAllowEmptyComp.qml:8-20`). Buttons swap `icons[_PRESSED]` (`common/QButton.qml:28`). Opacity is reserved for *disabled* states (dim text day 0.2 / night 0.35, `QMenuItemNoDragComp.qml:60`) and drag ghosts (`opacity: 0.7` while dragging, `QMenuPageTotal.qml:322`).
3. **Four-state image arrays.** `[Normal, Press, Focus, Disable]` — sometimes `[N, F, P]` or `[N, P, S]` — always via `defineModel.*` keys, never literal paths except in one-off screens. Index constants `_NORMAL/_FOCUSED/_PRESSED/_DISABLE` (`common/QButton.qml:17-20`).
4. **Slide-out cancels the click.** Every MouseArea resets its press flag when the finger leaves: `onPositionChanged: if(!containsMouse) atPressState = false` (`QMenuPageNoDrag.qml:85-89`; `common/QButton.qml:45-53`; `QItem.qml:42-43` cancels on any x/y change). Mirror this in custom controls.
5. **Long-press = 650 ms Timer** → beep + `itemPressAndHold()` (`QMenuPageNoDrag.qml:91-99`); drag-start = 500 ms Timer (`QMenuPageTotal.qml:263-278`). Use the same timings.
6. **Magnify touch targets at screen edges.** `QEnlargedMouseArea` with 1.25–1.5 multiples on grid borders (`QMenuPageNoDrag.qml:57-60`), title buttons (`common/QTitleButton.qml:117-120`), slider ± (`Q_SlidebarStyle.qml:68-71`).
7. **Never draw under the top 90 px.** Pages start at `y: ConstantModel.MENU_BAR_Y` with `height: DEVICE_HEIGHT − 90` (`Screen_HomeScreen_Manual.qml:22-25`); popups use the 0/90 conditional (`popup/QHMI_AppHomePopup_OK.qml:9-11`). Full rules in `docs/ui/UI_CONTEXT.md:23-40`.
8. **Gate input on popup state.** Screen roots use `isEnable: !AppHomeModel.sysPopupShowing` (`QHMI_HomeScreenMain_Advanced.qml:151`, `Screen_HomeScreen_AllMenus.qml:22`); drop areas hide during OSD (`visible: !AppHomeModel.isOnScrShowing`, `QMenuPageTotal.qml:333`); widget plugins fade out during call OSD (`pluginOpacity: (AppHomeModel.visibleOSDCall && callOSD.showCallOSD()) ? 0 : 1`, `Screen_HomeScreen_Widget.qml:47`).
9. **Navigation is event-driven, never Loader-hacking.** Launch apps with `QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)` (`QHMI_HomeScreenMain_Advanced.qml:225`); change screens with `QML_Handler.qmlChangeTopMsg(EVT_GOTO_SCREEN_…)` (`:230`); close popups with `EVT_REQUEST_HIDE_POPUP, _OSD_ID` (`popup/QHMI_AppHomePopup_OK.qml:107`).
10. **Localization:** every user-visible string is `TranslationsModel.STR_*` or `TranslationsModel.getDefineLocalize(label)` **plus `AppHomeModel.textUpdate`** to force re-evaluation on language change (`QMenuItemNoDragComp.qml:68`, `WidgetItemAdvanced.qml:81`).
11. **Day/night theming is automatic** if you use `Q_ThemeImage`/`QText` (`common/Q_ThemeImage.qml:6`, `common/QText.qml:6-8`) and `HomeThemeHelper` colors — never hardcode colors except deliberate constants (e.g. `#FAFAFA` customize header, `Screen_HomeScreen_AllMenus.qml:187`).
12. **Anchors inside, absolute for cells.** Children are anchored (`anchors.horizontalCenter`, `verticalCenter` + offset); top-level page regions use absolute x/y/w/h from `ConstantModel` (see §3). The stock idiom for "centered but nudged" is `anchors.verticalCenter: parent.top` + `verticalCenterOffset` (`QMenuBarSubmenu.qml:84-92`).
13. **z-ordering habits:** content defaults to document order; explicit z only for shields/chrome — `QRootScreen`'s beep shield z 1000 (`common/QRootScreen.qml:13`), statusbar loader z 2 / dimmer z 3 / OSD z 4 (`HomeScreen/main.qml:309,342,452,465`), edit bg z −1 (`QMenuItem.qml:117`), debug cover z −9999 (`main.qml:536`).
14. **Delegates read model roles as plain identifiers** (`label`, `itemID`, `iconN`, `tonedown`, `visible`, `visibleForEdit`) and re-expose them as loader context properties (`pageIndex`, `itemIndex`, `itemId`, `fontSize`, `QMenuPageNoDrag.qml:44-51`). Keep this contract when injecting custom tiles.
15. **Animation conventions:** page transitions = 150 ms fade+slide via `QMainScreenAnimation` (`component/QMainScreenAnimation.qml:37-61`); popup = 100 ms opacity (`QPopupAnimationItem.qml:17-42`); indicator auto-hide 5 s (`QHMI_HomeScreenMain_Advanced.qml:421-436`); displaced grid items 200 ms (`QMenuPageTotal.qml:429`).
16. **Overlays (from project docs, validated against this tree):** full-screen overlays must be `z: 2000`-style siblings with `open()/close()`, `visible: requested && !AppHomeModel.isAtOtherApp`, close hooks on `onButtonBackClicked/onIsAtOtherAppChanged/onCurrentScreenChanged/onCurrentHomeAdvancedPageIdxChanged/onRequestHomeByPPChanged`, and a `preventStealing` all-buttons MouseArea shield; the steering-wheel Home/Star key never reaches QML (session bus) — see `docs/ui/APP_OVERLAY_CHECKLIST.md:29-109` and `docs/ui/UI_CONTEXT.md:42-110`. The stock tree confirms `isAtOtherApp` gating exists (`Screen_HomeScreen_AllMenus.qml:272-294` reconstructs lists only when back in foreground).
17. **Placement of a custom tile:** first empty cell of the target `QMenuPageNoDrag` grid via the shared `firstEmptySlot(customIndex)` allocator; a custom page is a full-screen overlay, **never** an extra `QListViewHomePage` page / `getModelList()` entry (`docs/ui/TERMINOLOGY.md:24-34,36-45`).
18. **Debug leftovers to avoid copying:** the red placeholder Rectangle in `QButtonCheckbox.qml:63-72`, `console.log("[3] …")` spam, commented-out blocks — all over the tree.

---

## 9. Quick component-selection guide

| You need… | Use |
|---|---|
| Themed image | `Q_ThemeImage` (+ `themeSource` from `defineModel`) |
| Label | `QText` (+ `TranslationsModel… + AppHomeModel.textUpdate`) |
| Icon button | `QButton` (beep: `usingBeepSound: true`) |
| Wide bar button (title bar) | `QTitleButton` / `QHMI_ToggleButton` |
| 3-slice button (popup bottom) | `QHorizontalButton` / `QEnlargedHorizontalButton` |
| Checkbox row | `QButtonActive` (not QButtonCheckbox — has debug rect) |
| App tile in home grid | `QMenuItemNoDrag` inside `QMenuPageNoDrag` (delegate contract §8.14) |
| Draggable tile | `QMenuItemAllowEmpty` + `QMenuPageTotal`/`QMenuPageAllowEmpty` |
| Submenu row list | `QSubListMenu` + `QSubListDelegate` (+ `QHMI_StraightScrollBar`) |
| Paged grid page | `QCustomizedMenuListAllowEmpty` |
| Swipe pager | `QListViewHomePage` (SnapOneItem/StrictlyEnforceRange) |
| Title bar | `QMenuBarSubmenu` at `y: 90` |
| Popup | `QRootScreen` + dim + `QPopupAnimationItem` + `QVerticalButton` panel + `QHorizontalButton` row, `_OSD_ID` + `EVT_REQUEST_HIDE_POPUP` |
| Slider | `Q_STSliderBar` (wraps `Q_SlidebarStyle`) |
| Loading spinner | `defineModel.img_loading[]` NumberAnimation pattern (`QLoadingComponent.qml:23-26`) |
| Page transition | `QMainScreenAnimation { positionTarget; opacityTarget }` + `startAnimation()` on show |
