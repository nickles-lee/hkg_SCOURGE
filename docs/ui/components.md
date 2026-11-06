# Gen5W component library — what to use and when

Stock components a native-looking UI must reuse. Theme `qml_eco3.0`.
Full inventories with per-component API and citations:
`reports/components-home.md` (AppHomeScreen/Statusbar/AW/Keyboard) and
`reports/components-apps.md` (AppSetup/AppMediaPlayer/SystemPopup/OSD).

## 0. Ground rules

1. **Components are per-app, not shared.** Every app ships its own copy of the
   `Q_Text`/`Q_Button`/`Q_ThemeImage` family. Never import across apps —
   copy the local one and bind to the same singletons (`defineModel`,
   `ConstantModel`, `HomeThemeHelper`/`SetupThemeHelper`, `TranslationsModel`).
2. **Everything interactive derives from `QItem`/`Q_Item`** (press-state
   machine + beep gating + slide-off cancel + long-press signals). Don't write
   a bare MouseArea for a control.
3. Names follow the `Q_*` / `QHMI_*` family. Keep new components in that
   convention (`component/<App>Tile.qml`, `HomeScreen/<App>Page.qml`).

## 1. Primitives (`AppHomeScreen/qml_eco3.0/component/common/`)

| Component | Use for | Key API |
|---|---|---|
| `Q_ThemeImage` / `QImage` | **every image** — auto day/night | `themeSource` (a `defineModel` key or `image://` URL) |
| `QText` | **every label** — themed color, HKMC fonts, default 39 px | `textBold` (HKMC_FONT_B/R), all `Text` props |
| `QItem` / `Q_Item` | base of every touch widget | `usingBeepSound`, `forceFocus`, signals `pressed/released/pressAndHold/canceled`; auto-cancels press on move |
| `QEnlargedMouseArea` | magnified touch targets (edges, small controls) | `width/height*Multiple` (1.25–1.5) |
| `QRootScreen` | **base of every screen page** | `isEnable` (bind `!AppHomeModel.sysPopupShowing`), `isOnTop`; built-in z 1000 beep shield |
| `QButton` | icon(+label) button | `icons[4]` = [N,F,P,D], `label*`, `usingBeepSound` |
| `QEnlargedButton` | QButton + fat touch area | `mouseWidth*Multiple` |
| `QTitleButton` | 90-px-tall 3-slice title-bar button (submenu headers) | `iconsl/m/r`, `label*` |
| `QVerticalButton` / `QHorizontalButton` / `QEnlargedHorizontalButton` | 3-slice popup panels / bottom button rows | `top/middle/botImages` or `left/mid/rightImages`, `label*`, `eventID` |
| `QHorizontalListButton` | option-menu list rows (5 states incl. selected) | `leftImages…`, `lineVisible` |
| `QButtonActive` | checkbox/toggle row (flips `isChecked` on release; **no beep** — parent beeps) | `isActiveType/isCheckType/isChecked` |
| `QHMI_ToggleButton` | full-width toggle bar (Edit/OK/Cancel rows) | 3-slice `co_btn_bottom_*`, signals `btnPressed/Release/Cancel` |
| `QItemMenu` | settings-box row (3-slice `co_bg_box_*`) | `isTop/Bot/OneElement`, `isChecked` |
| `QListVew` (sic) | ListView + hard-key focus helpers, snap-to-page | `focusUp/Down`, `excHK_DialPress` |
| `QPopupAnimationItem` | popup fade wrapper | `startOpen/CloseAnimation()`, `closePopupFinish`, 100 ms InOutQuad; force-quits on `onSwitchToOtherApp` |
| `QLoadingComponent` | spinner + "Preparing" | binds host ids (`isNaviWidget`, `showLoading`) |
| `Q_StraightScrollBar_Opt` | vertical scrollbar (15 px, OpacityMask thumb) | `listContentY/Height`, `heightSet` |
| `QEditLayoutButton` | Advanced/Basic mode selector card | 3-slice bg + radio icons |

## 2. Home-screen structural components (`component/`)

| Component | What it is |
|---|---|
| `QMenuPageNoDrag` + `QMenuItemNoDrag`(+`Comp`, `ImageN/D/PComp`) | **the home app grid + tiles.** Delegate contract: model roles `label, itemID, iconN/P/F/D, tonedown, visible, visibleForEdit, imgPath, fontSize` re-exposed as loader context (`pageIndex, itemIndex, itemId`). Press = full-cell `home_bg_icon_p` overlay; 650 ms long-press; `QEnlargedMouseArea` at grid edges |
| `QMenuPage` / `QMenuPageAllowEmpty` / `QMenuPageTotal` | drag-reorder grid variants (AllMenus, rearrange); `QMenuItemAllowEmpty` drag-scale 1.2 |
| `QCustomizedMenuList(AllowEmpty)` | paged horizontal grid container (AllMenus, Setup submenu); wide 6 / narrow 4 cols, cells 306×249 |
| `QListViewHomePage` | **the swipe pager**: horizontal ListView, SnapOneItem, StrictlyEnforceRange, cancels flicks < 60 px |
| `QWidgetItem` | plugin host (627×405): `sourceComponent` for C++-injected widget, loading overlay, press overlay 0.7, `pluginOpacity` for call-OSD dim |
| `QMenuBarSubmenu` | the 90-px submenu title bar: tiled bg, shadow line, centered bold-42 title, search/display-off/default `QTitleButton`s |
| `QMainScreenAnimation` | page transition: 150 ms fade (InCubic) + slide from width/2 (OutQuad), input blocked, respects `preventScreenAnimation` |
| `Q_STSliderBar` + `Q_SlidebarStyle` | slider (966×143): ± `QEnlargedButton`s (1.3–1.5× touch), step quantization, `isNegative` center mode |
| `QSubListMenu` + `QSubListDelegate` | curved/parallax submenu list (DataService); rows sized `DELEGATE_SUBLIST_X/Y` |
| `QHomeItem` | Information-submenu tile (254×183, fixed icon pos, divider lines) |
| `QEditMenuItem(Right)` | EditMenu drag sources/target slots |
| `WidgetItem(Advanced)` | widget-picker cards in Edit-Widget screens |
| `QButtonBatteryHomeItemEco3` | EV battery home widget (SOC + DropShadow + range) |
| `MyMenuComponent/QHMI_StraightScrollBar` | scrollbar variant |

## 3. App-side components (AppSetup family — for process-app UI)

| Component | Use for |
|---|---|
| `Q_Appsetting_TopBar_New` / `Q_TopBar` | 90-px title bar: title 42 bold centered, left icon buttons 138 px / margin 12 |
| `Q_AppSetting_ContainLeft` / `_Right` | two-pane settings frame: 375-px left menu (108-px delegates, SnapToItem, scroll only if > 540) + 905×540 right-pane Loader at x=375, y=180 |
| `Q_AppSetting_DelegateLeft` | menu delegate: 3/4/5-state `co_btn_bg_list_t1_108_{s,p,sf}` by row height (108/147/186/225), label 39, fallback 33 |
| `Q_HorizontalListButton` | every list row: 5-state selection, `lineVisible` dividers, `use_5state` |
| `Q_STSliderBar` (+`_NA/_Screen/_Type2`…) | 905-px slider, height 126 (eco3.0: 135), label 33, drag disables parent Flickable |
| `Q_CheckboxRadio` | checkbox/radio delegate: checkbox/radio at x=33, title 39 + guide 33 (12-px gap), `use_5state` |
| `Q_ListViewRadioButton_General` etc. | paired radio lists, `itemSelected`, `fiveState` |
| `Q_StraightScrollBar` | 15 px, thumb = `listHeight/contentHeight*height`, right-anchored, z 10, **visible only on overflow** |
| `Q_MarqueeTextRefact` | marquee (only working user: Wi-Fi AP names; AppMediaPlayer `QHMI_ScrollText` is the reference) |
| `Q_IconAnimationLoading` | 20-frame spinner, 33 ms/frame |
| `Q_RightMenuDescription` | "not available while driving" placeholder pane |
| `QHMI_KeyboardStandard` | OSK wrapper (`Keyboard 1.0` plugin) at y 180, z 10; API `textInit/textOutput/minLength/isPasswordKeyboard`, signal `buttonEnterClicked(textOutput)` |
| `QHMI_OptionMenu` | option menu: full-screen shield, 100 ms slide/fade, `{label, isItemSelect, isEnabled, isFocus}` entries |

## 4. Popup anatomy (app-local, the stock pattern)

From `popup/QHMI_AppHomePopup_OK.qml` / `QHMI_AppHome_Popup_Yes_No.qml` /
AppSetup `QHMI_AppSettingPopupOKCancel.qml`:

1. Root `QRootScreen`; geometry y 90 (0 only on widget page 0), height 630/720.
2. Full-size MouseArea: press → close + beep (outside click = dismiss/Cancel).
3. 70 % black dim (`GENERAL_OPACITY_PERCENT_70`).
4. `QPopupAnimationItem` centered: width class MIN 678 / MID 972 / MAX 1260;
   1-line height 297, **+45 px per text line**; body 39 px, clipped 7 lines.
5. Panel = `QVerticalButton` 3-slice (`co_bg_popup_*_{t,m,b}`).
6. Buttons = `QHorizontalButton` 3-slice `co_btn_bottom_*`; two-button rows:
   each `(w−36)/2` wide, 78 px high, bottom margin 12; **OK/primary LEFT,
   Cancel RIGHT** (SystemPopup response codes 1=left, 2=right).
7. Lifecycle: `Component.onCompleted → startOpenAnimation()`; close →
   `qmlSendEvtKey(EVT_REQUEST_HIDE_POPUP, _OSD_ID)`; unique `_OSD_ID` per
   popup; force-hide while a system popup shows (`opacity: isSysPopupShowing ? 0 : 1`).

App-local popups are requested by **event** (`KEY_EVT_SHOW_POPUP_*`) and
loaded by C++ by path — a page never instantiates one directly. System popups
(NotificationManager) are requested over D-Bus and are a separate surface.

## 5. Component selection guide

| You need… | Use |
|---|---|
| Themed image / label | `Q_ThemeImage` + `QText` (`defineModel`/`HomeThemeHelper` keys) |
| Icon button (beep) | `QButton` with `usingBeepSound: true` |
| Title-bar button | `QTitleButton` (home) / `Q_TitleButtonExtend` (Setup) |
| Checkbox/toggle row | `QButtonActive` (home) / `Q_CheckboxRadio` (Setup) — **not** `QButtonCheckbox` (debug rect leftover) |
| Full-width toggle bar | `QHMI_ToggleButton` |
| Slider | `Q_STSliderBar` + `Q_SlidebarStyle` |
| Home tile | `QMenuItemNoDrag` in `QMenuPageNoDrag` (delegate contract §2) |
| Draggable tile | `QMenuItemAllowEmpty` + `QMenuPageTotal` |
| Settings list row | `Q_HorizontalListButton` (5-state, dividers) |
| Curved submenu list | `QSubListMenu` + `QSubListDelegate` |
| Paged grid | `QCustomizedMenuListAllowEmpty` |
| Swipe pager | `QListViewHomePage` |
| Submenu title bar | `QMenuBarSubmenu` at `y: 90` |
| Popup | §4 pattern (`QRootScreen` + dim + `QPopupAnimationItem` + 3-slice panel + `QHorizontalButton` row, `_OSD_ID`) |
| Scrollbar | `Q_StraightScrollBar` (only when content overflows) |
| Loading | `QLoadingComponent` / `Q_IconAnimationLoading` + `STR_HOME_WIDGET_PREPARING` |
| Page transition | `QMainScreenAnimation { positionTarget; opacityTarget }` + `startAnimation()` |
| Text input | `PluginKeyboardArea` / `QHMI_KeyboardStandard`; PIN: `QHMI_KeyBoardNumerical.qml` |
| Marquee | AppMediaPlayer `QHMI_ScrollText` pattern (2 px/44 ms, 1 s delay, 1.5 s pause, 150 px gap) |

## 6. Debug leftovers — do not copy

- The red placeholder `Rectangle` in `QButtonCheckbox.qml:63-72`.
- `console.log("[3] …")` spam and commented-out blocks throughout the tree.
- `Q_MarqueeText.qml` files in AppHomeScreen Climate/PHEV and AppSetup eco3 —
  they are plain wrap-text wrappers, not tickers.
- `hoverEnabled: true` on grid/widget MouseAreas is drag plumbing, not hover
  UX — the panel is a touchscreen; new UI needs no hover styling.
