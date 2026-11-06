# Gen5W — UI patterns of official separate-process apps (AppSetup / AppMediaPlayer / SystemPopup / AppOSDManager)

Target: Hyundai Ioniq 5 Gen5W, firmware `NE_E.EUR.S5W_L.001.001.260128` (EU, HMC eco3 theme).
Scope: READ-ONLY extraction of the QML UI conventions that official apps follow, so new apps can
look and behave like built-ins. All claims carry `file:line` citations. Local QML trees:

- AppSetup: `/tmp/opencode/gen5w_ui/AppSetup/qml_new2.0/` (796 QML; `qml_eco3.0/` 791, `qml_genesis/` 759)
- AppMediaPlayer: `/tmp/opencode/gen5w_ui/AppMediaPlayer/qml_new_gui/qml_new2.0/` (216 QML)
- SystemPopup: `/tmp/opencode/gen5w_ui/SystemPopup/qml_new2.0/`
- AppOSDManager: `/tmp/opencode/gen5w_ui/AppOSDManager/qml_new2.0/`

## 0. Which theme variant does the Ioniq 5 EU use?

- The AppSetup binary (`app/bin/AppSetup`, Ghidra export `bin_5221ed11a7945eee`) contains **both**
  base-path strings `new_gui/new2.0/` and `new_gui/eco3.0/`
  (`../../mirror/export/bin_5221ed11a7945eee/strings.tsv`, strings near the QML file list).
  The variant is chosen at runtime by buyer/theme; the Ioniq 5 EU is the HMC "eco3" family
  (AppSetup ships color tables `ECO3_HMC_COLOR_TABLE`/`ECO3_KMC_COLOR_TABLE`/`GENESIS_COLOR_TABLE`,
  `/tmp/opencode/gen5w_ui/AppSetup/Js/Constant.js:8-12`).
- `qml_eco3.0` and `qml_new2.0` are near-identical: 443 differing files out of ~790, and the diffs
  in the components cited here are cosmetic (e.g. `Q_HorizontalListButton.qml` whitespace/parens;
  `Q_STSliderBar.qml` `sliderHeight: 135` in eco3.0 vs `126` in new2.0 —
  `diff qml_eco3.0/components/subComponents/Q_STSliderBar.qml qml_new2.0/...`).
  SystemPopup: 61 differing files; AppOSDManager: 41 (spot-check `osdGeneral.qml` = whitespace only).
- **Conclusion**: patterns below are documented from `qml_new2.0` and hold for eco3.0; where the
  variants differ materially it is noted. Theme *colors* never appear as literals in screens — they
  come from `SetupThemeHelper.*` / `defineModel.*` singletons (see §11).

## 1. Global frame — what every app's `main.qml` looks like

AppSetup `main.qml` (`/tmp/opencode/gen5w_ui/AppSetup/qml_new2.0/main.qml`):

```
Rectangle root  width = isWideScreen ? 1920 : 1280, height = DEVICE_HEIGHT(720)   (main.qml:10-11)
├── coverScreen MouseArea z:-9999 (touch logging)                                 (603-610)
├── topbar Loader  y:90  z:1  → components/mainComponents/Q_Appsetting_TopBar_New.qml (626-636)
├── loader_1 / loader_2   — alternating screen Loader, x/opacity cross-fade       (656-739)
├── loader_3 (Assistant Window, 640×720, x=1280/1920 when hidden)                 (750-791)
├── loader_4 (Status bar, 1920×90, z:5)                                          (839-851)
├── coverMouse MouseArea z:1000  visible: disableTouch || animators running       (619-624)
├── dimStatusBar / dimBG (black 0.7 dim under system popup)                      (893-931)
├── onsDataList ListModel ×4 {osdSource, idFading, zIndex, isOptionmenu}          (937-944)
├── dimOSDLocal Rectangle (black 0.7, y:90, z=zDimOSDLocal)                       (1024-1042)
└── ons_Creator Repeater of 4 Loaders (app-local popups/OSDs)                     (1044-1082)
```

- **Screen swap**: C++ sets a `screenNext` context Component; QML calls
  `screen_Transition()` → `current_loader.sourceComponent = screenNext` (main.qml:158-164).
  Two loaders ping-pong via `switch_current_loader()` (166-180).
- **Transition animation**: `XAnimator` from `startX = root.width/2` to 0, `Easing.OutQuad`,
  `duration: aniDuration` = **150 ms**; `OpacityAnimator` 0→1 `Easing.InCubic`, same duration
  (main.qml:22-23, 671-696). During the animation a full-screen `coverMouse` blocks all touch
  (main.qml:619-624) and `AppSettingModel.disableTouch` is set by popup animations
  (`components/common/Q_PopupAnimationItem.qml:5-8`).
- **Top 90 px** is the system status bar, loaded by `loader_4`; app content starts at y=90
  (topbar Loader `y:90`, main.qml:628; status bar Loader height 90, main.qml:842-843).
  This matches `docs/ui/UI_CONTEXT.md:6-27` (MENU_BAR_Y=90, usable height 630).
  AppSetup's own layout actually reserves more: left menu and right pane start at **y=180**
  (§2.1), because rows 90–180 are the "Setup" title band drawn by the top bar.

## 2. App page anatomy (4 representative AppSetup screens)

### 2.1 Main two-pane settings screen — `Screen_AppSetting_Main.qml`

```
y=0    ┌────────────────────────────────────────────────────────┐ 1280 (or 1920 full mode)
       │ [search][beep/help icons]      TITLE (42px bold, centered) │ top bar, h=90
y=90   │ (tabline image co_bg_list_tabline at x=373)                │
y=180  ├──────────┬───────────────────────────────────────────────┤
       │ LEFT     │ RIGHT PANE (Loader)                            │
       │ 375px   │  x=375, w=1280-375=905, h=540                  │
       │ list    │  loads e.g. sound/Q_AppSettingSoundGuidance.qml│
       │ 540px   │                                                │
y=720  └──────────┴───────────────────────────────────────────────┘
```

Citations (`Screen_AppSetting_Main.qml`):
- Root `Q_BoderMain` width `fullMode ? 1920:1280` (line 19).
- Left pane `Q_AppSetting_ContainLeft`: `anchors.left: parent.left; width: 375; y: 180`
  (22-26); list height 540 (44-47); scrollbar on the right edge, rightMargin 2, visible when
  `listCount > 5 || heightListView > 540` (50-61).
- Right pane `Q_AppSetting_ContainRight`: `x: containLeft.width`, width
  `isWideScreen ? (fullMode ? (1920-375) : (1280-375)) : (1280-375)`, `height: 540`, `y: 180`
  (64-71).
- Wide-screen "image description" Loader at x=1280, y=180, 640×555 (94-101) — the AW column.
- Divider: `Q_ThemeImage co_bg_list_tabline` at x=373 (148-156), also drawn inside the left
  container (`components/mainComponents/Q_AppSetting_ContainLeft.qml:108-112`).

**Left menu list** (`Q_AppSetting_ContainLeft.qml`):
- `ListView` width 375, `delegateHeight: 108`, `height: delegateHeight*5` (=540),
  `snapMode: ListView.SnapToItem`, `clip: true`, `cacheBuffer: delegateHeight*lsvCount()`,
  `interactive: lsv.contentHeight > 540` (114-124).
- Delegate `Q_AppSetting_DelegateLeft` (131-237): `btnWidth: 373`, height `108` per line
  (`_defaultHeight: 108 + (dlgLabelID.height - dlgLabelID.lineHeight)`,
  `Q_AppSetting_DelegateLeft.qml:106`), label at leftMargin
  `ConstantModel.APP_SETTING_USM_RIGHT_TEXT_MARGIN` (=33, binary table §11),
  vertical center, `font.pixelSize: ConstantModel.APP_SETTING_FONT_SIZE_39` (=39),
  fallback small font 33 for Hungarian overflow cases (132),
  `lineHeight: 39` (45 with glow stroke), `Text.FixedHeight`, `Text.WordWrap` (134-138).
- Selection: `isSelected: lsv.currentIndex == index`; press/selected/focus background images are
  3/4/5-state theme images `co_btn_bg_list_t1_108_{s,p,sf}` selected by delegate height
  (`Q_AppSetting_DelegateLeft.qml:33-44,101-102`).
- Row dividers: `Q_LineHorizontal` at the bottom of each delegate except the last visible one
  (`Q_HorizontalListButton.qml:54-87`; `lineVisible: !isLastVisibleElement`,
  `Q_AppSetting_ContainLeft.qml:148-150`).
- On release the delegate emits `leftIndexChanged(index)`, sets `AppSettingModel.curIndexLeft`,
  moves focus right, and sends guard events (e.g. `KEY_USM_UNIT_IGN_OFF`,
  `KEY_EVT_BTCALLING_ON`) when the destination is not allowed
  (`Q_AppSetting_ContainLeft.qml:241-420`).
- `onCurrentIndexChanged` asks the right pane for `lsv.model[currentIndex].qmlSource`
  (423-432) — **the left menu is the navigation driver; the right pane is a dumb Loader**.

**Right pane loader** (`components/mainComponents/Q_AppSetting_ContainRight.qml`):
- Plain `Loader`, height `ConstantModel.APP_SETTING_RIGHT_MENU_HEIGHT_Y` (=554), with
  `setSourcePath(_sourcePath)` mapping `AppSettingModel.settingType` → folder prefix, e.g.
  `TYPE_MODEL_SOUND → qmlBasePath + "sound/Q_AppSetting"` (10-60). Screen files are therefore
  named `Q_AppSetting<Name>.qml` per feature folder.

**Top bar** (`components/mainComponents/Q_Appsetting_TopBar_New.qml`):
- Height 90, full app width (12-15). Background `co_bg_title_bar` tiled (`Q_TopBar.qml:21-27`).
- Title: `Q_Text`, vertically centered, `font.pixelSize: ConstantModel.APP_SETTING_FONT_SIZE_42`
  (=42), bold, horizontally centered unless left buttons would overlap
  (`anchors.leftMargin: isLabelLength() ? lengthOfAllbtnLeft + 12 : (width - contentWidth)/2`)
  (142-153, 106-111).
- Left-side title buttons (search / beep / display-off / help / sound-mood): `Q_TitleButtonExtend`
  `anchors.leftMargin: 12`, `iconHeight: 90`, label `TOP_BAR_TEXT_FONT_SIZE_NEW` (=39), font
  `defineModel.FONT_R`, `forceDisFocus: AppSettingModel.popupIsShowing`, touch area extended by
  multiples (168-202, 400-425). Search button width 138 (403).
- Title table: `initTitleTable()` maps `settingType` → localized title (49-81); screens override
  via root `titleText`/`titleTextUpdated` properties (main.qml:647-653).

### 2.2 Slider screen — Display ▸ Brightness (`display/brightnessOptional/Q_AppSettingBrightness.qml`)

```
right pane 905×540 Flickable (content scrolls if > 540)
┌────────────────────────────────────────────┐ x=0 (of the 905 pane)
│ ( ) Automatic brightness   [description]  │ Q_ListViewRadioButton_General, itemHeight 108
│ (•) Manual brightness       [description]  │ btnWidth 905, textSubWidth 776
│ [  −  ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬  +  ]   │ Q_STSliderBar w=905 (SETTING_CONTAIN_RIGHT_SPLIP_W)
│            (label 33px above/below bar)     │ max 100, min 1, pointer offset ~183
│ ─────────────── 37px gap ────────────────  │
│              [ Reset ]  620×78              │ CustomButtonExtend, centered
└────────────────────────────────────────────┘  Q_StraightScrollBar on right edge (w=15)
```

Citations:
- Flickable 905×540, `interactive: flk.contentHeight > 540` (15-21).
- Radio list `Q_ListViewRadioButton_General`: `btnWidth1: 905`, `itemHeight: 108`,
  `textSubWidth2: 776`, `fiveState: true`, `interactive: false` (61-71). Selecting an item jumps
  the Flickable so the item stays visible (96-101).
- Slider `Q_STSliderBar` width `ConstantModel.SETTING_CONTAIN_RIGHT_SPLIP_W` (=905),
  `maximumValue: 100, minimumValue: 1`, `imgBg/imgBar` 3-slice theme images, drag disables the
  parent Flickable (`onMouseDraged: flk.interactive = false`, 122-165).
  Slider internals (`components/subComponents/Q_STSliderBar.qml`, `Q_SlidebarStyle.qml`):
  height 126 (135 in eco3.0), label font 33 (`Q_STSliderBar.qml:63,125`), − / + buttons
  `Q_ButtonExtend` at leftMargin 21 with icons `set_btn_minus_{n,f,p,d}` and extended touch
  multiples 1.3/1.5 (`Q_SlidebarStyle.qml:185-210`), value quantized to steps
  (`updateValue(mouseX)`, `Q_SlidebarStyle.qml:114-125`), `isNegative` mode centers at 0.
- Reset button `CustomButtonExtend` 620×78, centered, label 39px, `usingBeepSound: true`,
  disabled when values are already default (167-205); spacing above it 37 px (174).
- Scrollbar `Q_StraightScrollBar` anchored right, `z: 10`, `heightSet: 540`, visible only when
  `contentHeight > 540` (231-244).

### 2.3 Toggle/checkbox + sub-list screen — Sound ▸ Guidance (`sound/Q_AppSettingSoundGuidance.qml`)

- Same right-pane frame: Flickable 905×540 (114-125), `Q_StraightScrollBar` right-anchored,
  visible `contentHeight > 540` (238-246).
- Top part: `Q_ListViewImageArrow` — arrow rows that navigate deeper
  (`width: ConstantModel.SETTING_CONTAIN_RIGHT_SPLIP_W`, theme images
  `co_btn_bg_list_c_108_{s,p}`, 132-180).
- Bottom part: `Q_ListCheckBoxUSM` — checkbox rows bound to a bool array
  `itemState:[...]` (182-235). Toggling flips a model property and repositions the Flickable so
  the touched row is visible (228-233).
- Checkbox/radio delegate visuals (`components/subComponents/Q_CheckboxRadio.qml`):
  height 108 (+39 per extra title line, +33 per guide line),
  checkbox `co_btn_check_{on,off}_{n,p}` at left 33 / top 23 (123-136),
  radio `co_btn_radio_{on,off}_{n,p,d}` (138-153),
  title `Q_MarqueeText` fontSize 39, guide text 33 with 12 px gap (71-93),
  `usingBeepSound: true` (32).

### 2.4 Radio + big button screen — General ▸ Reset (`general/Q_AppSettingGeneralDefault.qml`)

- `Q_ListView` 905 wide, height `186*2+2`, `interactive: false`, delegates `Q_CheckboxRadio`
  height 186 (2-line rows), `use_5state: true`, `checkAreaFocus` bound to
  `AppSettingModel.areaFocus` (86-118).
- Bottom action button `Q_HorizontalButtonExtend` 620 wide, `anchors.bottom: root.bottom;
  bottomMargin: 15`, horizontally centered on the list (132-146). On release sends
  `KEY_EVT_SETTING_SYSTEM_CURRENT_RESET` / `KEY_EVT_SETTING_SYSTEM_RESET_ALL`, or
  `KEY_EVT_POPUP_CAR_DRIVING` when driving-restricted (147-165).
- Row heights and 5-state backgrounds scale by pixel height via a table of
  `co_btn_bg_list_c_{108..312}_{s,p,sf}` images (21-74) — **pick the background slice that
  matches the row height**.

### 2.5 Text-entry screen (list-style) — Wi-Fi ▸ Add network (`wifi/Screen_AppSettingWifiAddNetwork.qml`)

- Content panel `rec_parent x:24, y:182, width:1280, height: SCREEN_HEIGHT-182` (70-77).
- Label column: `QHMI_Text` 356×91, left margin 33, font 39 (80-93).
- Input boxes: `Contrls.InputboxWifi` (profileControl) height 91, background 3-slice
  `co_bg_box_84_{n,p,f,d}_{l,m,r}`, placeholder color #b9b9b9/#4d4d4d by day/night (95-116).
- Row separators: 3-slice `co_bg_list_line_{l,m,r}` tiled (131-163).
- Security row has an embedded arrow icon `set_ic_arrow_input` right margin 21 (202-210).

## 3. Component inventory (AppSetup)

### 3.1 `components/common/` — leaf widgets

| Component | API (properties/signals) | Canonical usage | Citation |
|---|---|---|---|
| `Q_Item` | `usingBeepSound, mouseEnable, mousePressed(Long), forceFocus, forceDisFocus`; signals `pressed/released/pressAndHold/canceled/exited`; helpers `itemPressed()…` | Base of every touch widget; centralizes press state; `onXChanged/onYChanged → mousePressed=false` cancels press if item moves | `components/common/Q_Item.qml:3-69` |
| `Q_Text` | Text + `fontBold` (switches family `FONT_B`/`FONT_R`), default color `SetupThemeHelper.normalTxtListTitleColor`, default size 30 | Every label | `Q_Text.qml:5-15` |
| `Q_Button` | `icons[4]` = [normal, focused, pressed, disabled]; `state` = pressed→selected→focus→normal; `usingBeepSound` | Icon buttons (split-screen button, back) | `Q_Button.qml:4-45,58-92` |
| `Q_ButtonExtend` | + `widthLeft/RightMultiple, heightTop/BottomMultiple` (oversized MouseArea); `isNeedDelayBeep` | Small buttons needing fat touch targets | `Q_ButtonExtend.qml:36-39,93-141` |
| `Q_HorizontalButton` | 3-slice `leftImages/middleImages/rightImages[4]` + `themeHelperImages_5State[5]`; states `_NORMAL.._FOCUS` (0..4) | Text buttons on 9-slice backgrounds | `Q_HorizontalButton.qml` (see `QHMI_HorizontalButtonExtend.qml:30-90` for the popup variant) |
| `Q_HorizontalListButton` | full list-row button: `btnWidth, heightBox, lineVisible, use_5state, isSelectedRoot, checkAreaFocus`; `state` / `state_5` computed | Every list row, delegates | `Q_HorizontalListButton.qml:20-47,169-201` |
| `Q_ListView` | thin ListView wrapper with `updateItemFocused()` scroll-into-view helpers | simple non-paged lists | `components/common/Q_ListView.qml:26-60` |
| `Q_MarqueeText` | `fontSize, lHeight, isFocus` (fixed-height text; **no scrolling** in this variant) | list titles | `Q_MarqueeText.qml:4-37` |
| `Q_MarqueeTextRefact` | `animationCondition` → dual-text infinite scroll, 48px gap, speed `(paintedWidth+120)/50*1000`, 1 s lead / 1.5 s pause; elide when stopped | Wi-Fi AP names (only user: `Q_AppSetting_DelegateLeft_Wifi_List.qml:72-87`; property never bound elsewhere — effectively inert in AppSetup; real marquee lives in AppMediaPlayer, §8) | `Q_MarqueeTextRefact.qml:6-73` |
| `Q_Scrollbar` / `Q_Scrollbar_New` | 46×491 masked image scrollbar | legacy lists | `Q_Scrollbar.qml:4-72` |
| `Q_SliderBarHorizontal`, `Q_ThemeImage`, `Q_Image`, `Q_TopBar`, `Q_TitleButton(Extend/SML)`, `Q_ProgressHorizontal`, `Q_RootScreen` | see files | — | `components/common/` |

### 3.2 `components/` (QHMI_*) — older-generation widgets still used by popups

| Component | Notes | Citation |
|---|---|---|
| `QHMI_Button` / `QHMI_HorizontalButton` / `QHMI_HorizontalButtonExtend` / `QHMI_VerticalButton` | `icons[3]` = [n, p, f]; `buttonClicked()`; used for popup button rows and 9-slice popup backgrounds | `components/QHMI_Button.qml:30-61`; `QHMI_HorizontalButtonExtend.qml:30-90` |
| `QHMI_ListView` | ListView + movement signals | `components/QHMI_ListView.qml:26-60` |
| `QHMI_StraightScrollBar` (+USM/Wifi/OptionMenu variants) | image scrollbar, thumb height `numViews/numItems*height` | `components/QHMI_StraightScrollBar.qml:28-110` |
| `QHMI_MarqueeText`, `QHMI_Text`, `QHMI_QItem`, `QHMI_Glow`, `QHMI_ColorBox` | misc | `components/` |
| `QHMI_AnimationLoadingMenuBar/Update`, `QHMI_PopupAnimationLoading`, `QHMI_PopupDelegateCheckbox/Radio` | loading spinners & popup list delegates | `components/` |

### 3.3 `components/subComponents/` — composite controls (the real library)

| Component | API highlights | Citation |
|---|---|---|
| `Q_STSliderBar` (+`_NA`, `Screen`, `ScreenNegative`, `Type2`, `UsmOffset`, `Glow`) | `value, maximum/MinimumValue, isNegative, isAwaysEnable, imgBg/imgBar[3-slice], spacingValue, sliderHeight, isShowBubble`; signals `updateValue(int), requestReleased(), mouseDraged(), mouseCancelDraged(), increase/decreaseValue` | `Q_STSliderBar.qml:34-107` |
| `Q_SlidebarStyle` | step math, − / + buttons, drag MouseArea, `signalValueChanged` | `Q_SlidebarStyle.qml:31-166` |
| `Q_CheckboxRadio` | `isCheckbox/isRadioBtn, itemSelected, isCheckBoxChecked, textDisplay, guideText, enableSelect, use_5state` | `Q_CheckboxRadio.qml:7-156` |
| `Q_DelegateListViewRadioButton_General` (+`_Glow`, `_Simple`, `_USM`, `Image`) | row height `108 + lines*39 (+ lines*33 desc)`, radio at x=33, title 39/desc 33, 12 px gap | `Q_DelegateListViewRadioButton_General.qml:9-120` |
| `Q_ListViewRadioButton_General` etc. | paired ListView for the delegates, `itemSelected`, `fiveState` | `Q_ListViewRadioButton_General.qml` |
| `Q_ListCheckBoxUSM` / `Q_ListCheckBoxClimate(…)` | checkbox list with `itemState[]` array binding | `Q_ListCheckBoxUSM.qml` |
| `Q_StraightScrollBar` | `heightSet, listContentY, listContentHeight, listHeight`; width 15 (`STRAIGHT_SCROLL_WIDTH`), default height 540 | `Q_StraightScrollBar.qml:5-110` |
| `Q_ListViewMenu`, `Q_ListViewImageArrow`, `Q_ListViewAdvanced`, `Q_ListViewNavigationVolume` | arrow/list screens | dir |
| `Q_RightMenuDescription` (+`_Level2`) | "not available while driving" placeholder pane | `Q_RightMenuDescription.qml` |
| `Q_AnimationLoading`, `Q_IconAnimationLoading(_Setup/_Silder)` | 20-frame spinner, 33 ms/frame (30 fps) | `components/common/Q_IconAnimationLoading.qml:4-43` |
| `Q_BackGroundBorderHorizontalLine` etc. | 3-slice separators | dir |
| `Q_DelegateSettingKeyboard`, `Q_DelegateSettingProfile` | keyboard/profile rows | dir |

### 3.4 `components/mainComponents/` — per-app chrome

`Q_Appsetting_TopBar_New.qml` (title bar, §2.1), `Q_AppSetting_ContainLeft/Right.qml` (panes),
`Q_AppSetting_DelegateLeft(_SML/_Wifi…).qml` (menu delegates), `Q_AppSetting_USM_TopBar.qml`
(USM variant: settings icon at left 33, title at left 105, font 42 bold,
`components/mainComponents/Q_AppSetting_USM_TopBar.qml:75-95`), `Q_AppSetting_USM_*ContainLeft/Right`
(level 2–4 panes, widths `APP_SETTING_USM_LEFT_LV2_WIDTH`=639, `…RIGHT_LV2_WIDTH`=639,
`…MAIN_HEIGHT`=540, y=180 — `Screen_AppSettingUsmMainLevel_2.qml:79-111`).

### 3.5 Cross-app reuse (AppHomeScreen)

`/tmp/opencode/gen5w_ui/AppHomeScreen/qml_new2.0/component/` contains the *same family* but
**different implementations** — apps do not share QML files at runtime; each app ships its own copy:

| Concept | AppSetup | AppHomeScreen |
|---|---|---|
| touch base | `components/common/Q_Item.qml` | `component/Q_Item.qml` (same name/pattern) |
| slider | `components/subComponents/Q_STSliderBar.qml` + `Q_SlidebarStyle.qml` | `component/Q_STSliderBar.qml` + `Q_SlidebarStyle.qml` |
| text | `Q_Text`/`QHMI_Text` | `QHMI_Text` family |
| scrollbars | `Q_StraightScrollBar` (w=15) | `QOptMenuScrollBar.qml` |
| menu list | `QHMI_OptionMenu.qml` | `QCustomizedMenuList.qml`, `QMenuBarSubmenu.qml` |
| toggle | `Q_CheckboxRadio` / `Q_ListCheckBox*` | `QHMI_ToggleButton.qml` |
| loading | `Q_IconAnimationLoading` (20×33 ms) | `QHMI_Active_Loading_Btn.qml`, `Q_smallLoadingIcon.qml` |

Implication for a new app: **copy the component, don't import across apps**; keep names in the
`Q_*`/`QHMI_*` family and bind to the same singletons (`defineModel`, `SetupThemeHelper`,
`ConstantModel`, `StringModel`).

## 4. Popup & dialog patterns

### 4.1 App-local popups (AppSetup)

- Declared as full-screen QML files in `popup/` (e.g. `QHMI_AppSettingPopupOKCancel.qml`); the C++
  engine pushes their **relative path** into the 4-slot ONS list, QML loads them in the
  `ons_Creator` Repeater (main.qml:1044-1082; binary strings confirm paths like
  `popup/QHMI_AppSettingPopupOKCancel.qml`, AppSetup `strings.tsv`).
- Layering: `osd_Loader.z = isOptionMenu ? 1 : loader_4.z + 1 + zIndex*2` — popups stack **above
  the status bar** (main.qml:1055); a black `dimOSDLocal` rect (opacity 0.7, y=90) sits under them
  at `z = max(zIndex)*2 + loader_4.z` (main.qml:1024-1042, 946-956). Option menus always get
  zIndex 0 (lowest) (main.qml:1009-1013).
- Hide animation: the loader calls `item.startFadeoutAni(idx)` and connects
  `fadeoutAniFinished(int)` before unloading (main.qml:986-993); the popup contract is exactly
  those two members (`popup/QHMI_AppSettingPopupLoading.qml:49-56`).
- Popup geometry (`QHMI_AppSettingPopupOKCancel.qml`):
  - Full-screen input shield MouseArea: press anywhere outside ⇒ beep + close + treat as Cancel
    (25-32).
  - `Q_PopupAnimationItem` centered, `verticalCenterOffset: 405` on `parent.top` (i.e. popup
    center ≈ y 405) (44-48); fade in/out 100 ms `Easing.InOutQuad`; `disableTouch` while
    animating (`Q_PopupAnimationItem.qml:10-24,43-75`).
  - Width by class: MIN 678 / MID 972 / MAX 1260 (`APP_SETTING_POPUP_WIDTH_{MIN,MID,MAX}`,
    §11); 1-line height 297, +45 per extra line (`APP_SETTING_POPUP_1LINE_HEIGHT`,
    `APP_SETTING_POPUP_TEXT_LINESPACING`) (201-205).
  - Icon `co_ic_question` centered at y=27, x=(w-68)/2 (63-67,198-200); body text 39 px,
    width w−84, centered (69-85).
  - **Button row**: two buttons, each `(w-36)/2` wide, height 78
    (`APP_SETTING_POPUP_BUTTON_HEIGHT`), anchored bottom margin 12, left/right margin 12;
    **OK on the LEFT, Cancel on the RIGHT** (86-145). Button text 39 px, color
    `GENERAL_COLOR_BRIGHT_GREY`. Each button sends an HMIEVENTS id on close (16-17, 103-137,
    146).
- Loading popup: centered spinner + text, no buttons (`QHMI_AppSettingPopupLoading.qml`).
- Option menu (`popup/QHMI_OptionMenu.qml`): full-screen shield whose press-outside sets
  `AppSettingModel.optMenuShowing = false` (10-33), slide/fade animator 100 ms (48, 54-60),
  list of `{label, isItemSelect, isEnabled, isFocus}` entries built per setting type
  (main.qml:214-513).

### 4.2 System popups (SystemPopup app = NotificationManager process)

- Root `mainNoti.qml` is `DEVICE_WIDTH × DEVICE_HEIGHT`; C++ injects context props
  `notiTitle, notiBody, notiDialogType, notiBtnA/B, notiFromAppID, notiID, notiPlane` and
  instantiates `PopupGenerator` (mainNoti.qml:205-228).
- `PopupGenerator.qml` maps numeric dialog type → QML file: 0/11000/4004/4005/32130/32131/32175
  → `typeA.qml`; 1/4002/4003/4007/12000 → `typeB.qml`; 2/4000/4001/4006 → `typeC.qml`; …
  (PopupGenerator.qml:42-140). **Apps request these popups by sending an HMIEVENTS event / D-Bus
  notification to the app manager, not by loading QML**; the response comes back via
  `AppSystemPopupEngine.systemPopupResponse(notiPlane, notiFromAppID, notiID, btnResponse)`
  (typeA.qml:36-37, 200-203).
- `typeA.qml` (1-button): popup `PopupItem` centered with `verticalCenterOffset:
  AppSystemPopupEngine.isStatusBarShowing ? 45 : 0` (i.e. shifted down half the 90 px bar)
  (76-82); width MID or MAX (83); height formula `27 + 69 + 42 − lineHeight/2 + body + 78 + 40`
  (84); icon at y=27 (104-109); title 39 px elide-right, max 1 line, line height 45 (120-138);
  body wrapped, centered, 39 px, clipped to 7 lines (45 px each) in a Flickable with
  `Scrollbar` right margin 2 (139-182); OK button row: `(w-12-12)` wide, 78 high, bottom 12,
  left 12, 3-slice `co_btn_bottom_{l,m,r}_{n,p,f}` images, label 39 px elide (184-215).
- `typeB.qml` (2-button): identical chrome; buttons in a `Row` `spacing: 12`, each
  `(w − 12*2 − 12)/2` wide, 78 high; **left button = response 1 (primary/OK), right = response 2
  (secondary/Cancel)** (186-253). Button labels come from `AppSystemPopupEngine.getBtnString(notiBtnA/B)`
  (222, 250) — apps pass button-style enum ids, not strings.
- Open/close: `PopupItem.startOpen()/startClose()` = opacity 0↔1, **100 ms InOutQuad**
  (`Component/common/PopupItem.qml:5-16,36-74`); special-case 0 ms for the media-gate popup
  (line 37). Press-outside closes only when `notiNormalPriority > 89999` (typeA.qml:15, 54-74).
- Split-screen: popup window width shrinks by 640 with a 200 ms `Behavior on width`
  (mainNoti.qml:19, 29-39, 107-121).
- In-app dimming while a system popup shows: AppSetup dims the status bar and content with
  black 0.7 rects gated on `AppSettingModel.isSysPopupOn` (main.qml:893-931) and hides local
  popups (`visible: isOptionMenu ? true : !AppSettingModel.isSysPopupOn`, main.qml:1057).

### 4.3 AppMediaPlayer popups

`components/qhmi_popup/QHMI_PopupAType.qml` / `QHMI_PopupCType.qml` / `QHMI_PopupLoading.qml` /
`QHMI_PopupDeleteConfirm.qml` — same 3-slice `co_bg_popup_*` backgrounds and
`QHMI_PopupAnimationItem` (100 ms fade) as AppSetup; loaded through the app's own
`load_OSD(Onsdata)` (MP_Main.qml:41-50).

## 5. List conventions

- **Left menus / option lists**: `ListView` with fixed 108 px delegates, `SnapToItem`,
  `interactive: contentHeight > 540`, `cacheBuffer: 108*count`, custom scrollbar on the right
  edge (rightMargin 2, width 15) (`Q_AppSetting_ContainLeft.qml:114-124`,
  `Screen_AppSetting_Main.qml:50-61`).
- **Right-pane lists**: either a `ListView` with `interactive: false` inside a `Flickable`
  (Brightness, SoundGuidance) or plain stacked `Item` columns (GeneralDefault uses `Q_ListView`
  with fixed height). The Flickable is the scroll owner; inner lists disable their own
  interaction (`interactive: false`, `Q_AppSettingBrightness.qml:71`; drag on a slider also
  sets `flk.interactive = false`, 158-164).
- **Delegate heights**: 108 px base; +39 per extra title line; +33 per description line;
  standard row heights 108/147/186/225 — matched to background image variants
  (`co_btn_bg_list_t1_108/147/186/225`, `Q_AppSetting_DelegateLeft.qml:33-44`;
  `Q_CheckboxRadio.qml:18-21`; `Q_AppSettingGeneralDefault.qml:21-74`).
- **Highlight**: no ListView highlight delegate — selection is drawn by the delegate itself via
  `isSelectedRoot` + 5-state images (`s` selected, `p` pressed, `sf` selected-focused)
  (`Q_HorizontalListButton.qml:39-47`).
- **Scrollbars**: `Q_StraightScrollBar` — 15 px wide, height 540 (or `heightSet`), thumb height
  `listHeight/listContentHeight*height`, thumb y `listContentY/(listContentHeight-listHeight)*(h-thumb)`,
  OpacityMask-composited theme images; **visible only when content overflows**
  (`Q_StraightScrollBar.qml:5-110`; usage `visible: flick.contentHeight > 540`,
  `Q_AppSettingSoundGuidance.qml:240`).
- **Marquee**: AppSetup titles use fixed-height `Q_MarqueeText` (no scroll). The working marquee
  is AppMediaPlayer's `QHMI_ScrollText`: elide-right normally; when `isAllowScroll &&
  truncated && viewer visible`, two copies scroll left **2 px every 44 ms**, after a 1 s start
  delay, 1.5 s pause per cycle, gap 150 px (`QHMI_ScrollText.qml:13-17,35-47,115-141`).
- **Paged media lists**: AppMediaPlayer pages model data (`pageSize`, `updatePage`,
  `get_top_index_view()`), shows a scroll "handler" only during flick
  (`onMovementStarted → idScrollbar.showHandler()`, `QHMI_ListViewCommon.qml:63-89`).

## 6. Keyboard / OSK usage (AppSetup)

- The keyboard is **not** drawn per-screen: screens instantiate
  `KeyboardQML/QHMI_KeyboardStandard.qml` (a wrapper around the shared `Keyboard 1.0` plugin,
  `QHMI_KeyboardStandard.qml:2,49-50`), positioned `y: 180, z: 10`, full 1280 width
  (`Screen_AppSettingWifiKeyboard.qml:136-139`).
- API used by screens: `textInit, textOutput, textSuggest, minLength, isPasswordKeyboard,
  keyboardAreaInFoccus, requestResetShiftKeyState()`; signals `buttonEnterClicked(textOutput),
  keyboardOutFocus(data), onRequestSettupKeyboardType, checkIsUmlautChanged`
  (`QHMI_KeyboardStandard.qml:21-52`; usage `Screen_AppSettingWifiKeyboard.qml:136-182`).
- Entering text mode: input box release → `keyboardEngine.setDefaultKeyboard()` +
  `QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_GOTO_SETUP_WIFI_ENTER_STRING)`
  (`Screen_AppSettingWifiAddNetwork.qml:118-125`); the screen sets
  `AppSettingModel.keyboardVisible = true` so the top bar hides its split button
  (`Screen_AppSettingWifiKeyboard.qml:257`; main.qml:805).
- Leaving: `EVT_BACK_TO_PREV_SCREEN` after storing the typed string in a JS singleton
  (`Listwifi.tempPasswd`) so the value survives screen reloads
  (`Screen_AppSettingWifiKeyboard.qml:172-178, 246-258`).
- Shift-state cache: `keyboardAppModel.keyboardLayoutShiftStateCache = []` resets the layout
  when re-entering (`Screen_AppSettingWifiAddNetwork.qml:123`;
  `Screen_AppSettingWifiKeyboard.qml:255-260`).
- Umlaut popup: tapping outside closes it via
  `keyboard.plugin_Keyboard.requestCloseUmlautFromApp(0)` + beep
  (`Screen_AppSettingWifiKeyboard.qml:222-230`).

## 7. Navigation model

- **No StackView.** Two alternating `Loader`s + a C++-owned `screenNext` Component
  (main.qml:12-13, 158-180). AppMediaPlayer uses the identical pattern
  (`MP_Main.qml:12-39`) — this is the platform-standard app skeleton.
- Screen id: `AppSettingModel.currentScreen` compared against `HMIEVENTS.EVT_GOTO_*`
  (main.qml:32-70). All requests funnel through
  `QML_Handler.qmlSendEvtKey(HMIEVENTS.<event>, payload?)` — QML never navigates directly,
  the C++ engine decides and swaps `screenNext`.
- Back: C++ engine drives it; QML hooks:
  - `AppSettingEngine.reqDirectBack()` for "leave now" cases (main.qml:1102;
    `Q_AppSettingGeneralDefault.qml:222`).
  - `EVT_BACK_TO_PREV_SCREEN` / `KEY_EVT_SETTUP_CLICK_BACK` events
    (`Screen_AppSettingWifiKeyboard.qml:175`; `Screen_AppSettingWifiAddNetwork.qml:463`).
  - `Connections { target: AppSettingEngine; onTouchBackButton: {...} }` for screens needing
    to save/restore before going back (`Q_AppSettingDisplayDateTime.qml:19-27`).
- Within-screen navigation: left-menu index (`AppSettingModel.curIndexLeft`) + right-pane
  Loader (`Q_AppSetting_ContainLeft.qml:423-432`); USM screens add
  `curIndexLeft2/3/4` levels (`Screen_AppSettingUsmMainLevel_2.qml`).
- Focus model for jog/wheel: `AppSettingModel.areaFocus` (LEFT/RIGHT/NONE) +
  `forceFocus`/`forceDisFocus` on widgets, `forceDisFocus: AppSettingModel.popupIsShowing`
  on all top-bar buttons (`Q_Appsetting_TopBar_New.qml:181,217,266,300`).
- The steering-wheel Home/Star key is **not** a QML event — see `docs/ui/UI_CONTEXT.md:42-81`.

## 8. AppMediaPlayer specifics (brief)

- `MP_Main.qml` mirrors AppSetup: loader_1/2 + `screen_Transition()`, `load_OSD()`,
  status-bar loader z:3, and a `disableTouch` MouseArea per loader that auto-clears after
  1000 ms (`MP_Main.qml:24-110`).
- Browser list: left tab strip + `QHMI_ListViewCommon` (paged); delegate
  `QHMI_DelegateSong` is a 108 px row with press/select/focus backgrounds
  `me_btn_bg_list_c_108_{p,s,f}.png`, equalizer animation icon for the playing track, marquee
  title (`QHMI_DelegateSong.qml:7-84`), MouseArea sets `bgPress` on press, plays beep via
  `MMUSController.mmusSetBeepPlay(true)` on click, then transitions with
  `QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_TRANSITON_AUDIO_LIST_TO_PLAYER, [...])`
  (87-110). Beep here is a **controller call**, showing the alternative to
  `KEY_EVT_SETTUP_SOUND_BEEP_PLAY`.
- Now-playing: full-bleed background (cover art or `home_main_bg_01/02` + day/night suffix),
  `QHMI_MainHeader` with right text button (`btnRightText: STR_MEDIA_BUTTON_LIST`), left
  mini-list `QHMI_AudioListViewLeft` in wide mode, `QHMI_ScreenAnimationItem` for transitions
  (`Screen_AudioPlayer.qml:29-100`).

## 9. OSD patterns (AppOSDManager)

- Two roots: `mainVolume.qml` (volume strip, height **75**, bottom-anchored loader;
  `mainVolume.qml:3-19`) and `mainOSD.qml` (status-bar-height strip
  `ConstantModel.STATUSBAR_HEIGHT` = 90; `mainOSD.qml:4-6`).
- **Show**: C++ emits `onOsdVolumeData`/`onOsdData` → QML sets `loader.source = "osdX.qml"`
  (mainVolume.qml:21-39; mainOSD.qml:83+). **Hide**: one-shot `Timer` with
  `interval = delayTime * 100` ms (engine-supplied), then `AppOSDEngine.osdTimeOut(DISPLAY, …)`
  (mainVolume.qml:63-78; mainOSD.qml:581-604). No fade animation — instant show/hide.
- **Tap-to-dismiss**: a MouseArea over the loader closes the OSD, plays
  `AppOSDEngine.soundBeep(id)`, and reports `osdTimeOut(...,2)` (touch)
  (mainVolume.qml:13-18; mainOSD.qml:23-60).
- Volume OSD layout (`osdVolume.qml`): background image `co_bg_osd_volume(_8)` anchored
  bottom; info block left margin 14 (wide: 619, narrow: 371), title text right-aligned
  32/36 px, mode icon, value text 33 px in a 54 px box, progress bar 675 px wide built from
  `co_progress_bg_{l,m,r}` slices (5-150). RTL mirrored via `getMidEast()`.
- General OSD (`osdGeneral.qml`): full-width strip, tiled `co_bg_osd_title` background —
  **red variant `co_bg_osd_title_red` when `priority == 1`**, warning icon override at
  priority 2 (`osdGeneral.qml:34-57`).
- Event → QML file switch is a big C++-fed `switch(AppOSDEngine.getEvent())` in
  `mainOSD.qml:92-130` (FM/DAB → `osdGeneralDAB.qml`, SXM → `osdGeneralSXM_A/B.qml`, …).

## 10. USAGE RULES (do / don't)

1. **DO play the press beep on every release** — `if(usingBeepSound)
   QML_Handler.qmlSendEvtKey(HMIEVENTS.KEY_EVT_SETTUP_SOUND_BEEP_PLAY)` inside `onReleased`
   (`Q_Button.qml:78-82`, `Q_HorizontalListButton.qml:185-191`,
   `Q_ButtonExtend.qml:120-128`); AppMediaPlayer equivalent
   `MMUSController.mmusSetBeepPlay(true)` (`QHMI_DelegateSong.qml:103`). Delayed variant for
   sliders: `qmlSendEvtKey(KEY_EVT_SETTUP_SOUND_BEEP_PLAY, isNeedDelayBeep)`
   (`Q_ButtonExtend.qml:125`). DON'T beep on press.
2. **DO use 4/5-state image arrays** `[normal, focus, pressed, disabled(, selected)]` selected
   by a computed `state` int — never recolor at runtime
   (`Q_Button.qml:32-45`, `Q_HorizontalListButton.qml:39-47`). Text color comes from parallel
   arrays indexed by the same state (`colorTab[state]`,
   `Q_AppSetting_DelegateLeft.qml:114-139`).
3. **DO extend touch targets** with `widthLeftMultiple`-style oversized MouseAreas for small
   controls (sliders use 1.3–1.5×, `Q_SlidebarStyle.qml:188-192`; reset buttons 1.1–1.5×,
   `Q_AppSettingBrightness.qml:179-182`).
4. **DO block input during transitions/animations**: full-screen `coverMouse` while animators
   run (main.qml:619-624); `AppSettingModel.disableTouch` set by popup fade
   (`Q_PopupAnimationItem.qml:5-8`); AppMediaPlayer re-enables touch after 1000 ms
   (`MP_Main.qml:103-110`).
5. **DO use 150 ms OutQuad slide + InCubic fade** for screen changes (main.qml:22,
   671-696), **100 ms InOutQuad opacity** for popups (`Q_PopupAnimationItem.qml:43-75`;
   SystemPopup `PopupItem.qml:39-74`), 100 ms for option-menu slide
   (`QHMI_OptionMenu.qml:48,54-60`), 200 ms for split-screen width changes (main.qml:760-766;
   mainNoti.qml:29-39).
6. **DO elide long text** (`elide: Text.ElideRight`, `maximumLineCount: 1` on popup titles,
   typeA.qml:132-133; button labels clip at width−24, typeA.qml:206-213) and marquee only in
   focused/selected media rows (`QHMI_ScrollText.qml:13-17`).
7. **DON'T draw above y=90** — status bar owns it (loader_4 z:5; dim rects start at y:90,
   main.qml:917-931, 1024-1042); content band is y=180..720 for AppSetup panes (540 px tall
   right pane), 630 px for generic full-screen pages (`UI_CONTEXT.md:23-39`).
8. **DO put OK/primary on the left, Cancel on the right**, 78 px tall, 12 px margins,
   3-slice `co_btn_bottom_*` images (`QHMI_AppSettingPopupOKCancel.qml:86-145`;
   SystemPopup typeB.qml:186-253 with response codes 1=left, 2=right).
9. **DO dim the background under popups** with black at opacity 0.7
   (`GENERAL_OPACITY_PERCENT_70`; main.qml:898-903, 924, 1031).
10. **DO gate interactivity on vehicle state** — send `KEY_USM_UNIT_IGN_OFF` when ignition is
    off, `KEY_EVT_POPUP_CAR_DRIVING` when driving-restricted, `KEY_EVT_BTCALLING_ON` during
    calls, before performing the action (`Q_AppSetting_ContainLeft.qml:241-378`;
    `Q_AppSettingGeneralDefault.qml:147-165`).
11. **DO route all navigation/actions through `QML_Handler.qmlSendEvtKey(HMIEVENTS.…)`** and
    keep per-screen state in C++-exposed models (`AppSettingModel.curIndexLeft`,
    `areaFocus`, `keyboardVisible`) — QML never mutates screen state directly except through
    these models (§7).
12. **DO keep spacing rhythm**: 12 px popup/button gutters, 33 px list text left margin,
    37 px gap above footer buttons, 15 px below bottom buttons, 21 px slider button margin,
    108 px rows (+39/+33 per extra line) (§2, §11).
13. **DON'T hardcode colors** — `SetupThemeHelper.<state><role>Color` /
    `defineModel.Color*` / `OSDThemeHelper.*` / `NotiThemeHelper.*` so day/night and
    buyer themes work (`Q_Text.qml:7`; `Q_AppSettingSoundGuidance.qml:14-15`;
    osdVolume.qml:44; typeA.qml:128).
14. **DO use theme (day/night/buyer) image indirection** — `Q_ThemeImage { themeSource:
    defineModel.<key> }` everywhere instead of literal paths (`Q_ThemeImage.qml`;
    usages passim). AppMediaPlayer uses `RESOURCE.<key> + SettingModel.dayNightSuffix`
    (`QHMI_DelegateSong.qml:11`).
15. **DO give every full-screen overlay an input shield** and close on outside-press when
    allowed (`QHMI_AppSettingPopupOKCancel.qml:25-32`; option menu `mouseClickOutside`,
    `QHMI_OptionMenu.qml:10-33`); also see the platform rules in
    `docs/ui/UI_CONTEXT.md:83-110`.

## 11. Extracted `ConstantModel` values (AppSetup binary)

Recovered from the Ghidra export of `app/bin/AppSetup`
(`bin_5221ed11a7945eee/functions_0009.c`, `qt_metacall` of `QMLConstantModel`, joined with the
enum string table in `strings.tsv`; full table: `/tmp/opencode/gen5w_ui/constant_model_table.txt`).
Verified anchors: `SCREEN_WIDTH=1280`, `SCREEN_HEIGHT=720`, `SCREEN_FULL_MODE_WIDTH=1920`.

| Constant | Value | Constant | Value |
|---|---|---|---|
| TIME_DURATION | 150 | SCREEN_WIDTH | 1280 |
| HEIGHT_TOPBAR | 90 | SCREEN_HEIGHT | 720 |
| SETTING_ITEM_LIST_H | 108 | SCREEN_FULL_MODE_WIDTH | 1920 |
| SETTING_CONTAIN_RIGHT_SPLIP_W | 905 | GENERAL_OPACITY_PERCENT_70 | 0.7 |
| APP_SETTING_USM_MAIN_CONTAIN_LEFT_WIDTH | 375 | APP_SETTING_POPUP_WIDTH_MIN/MID/MAX | 678/972/1260 |
| APP_SETTING_USM_MAIN_HEIGHT | 540 | APP_SETTING_POPUP_1LINE_HEIGHT | 297 |
| APP_SETTING_USM_LEFT/RIGHT_LV2_WIDTH | 639/639 | APP_SETTING_POPUP_TYPEB_1LINE_HEIGHT | 204 |
| APP_SETTING_RIGHT_MENU_HEIGHT_Y | 554 | APP_SETTING_POPUP_BUTTON_HEIGHT | 78 |
| APP_SETTING_TITLE_TEXT_X/Y/W/H | 46/0/629/40 | APP_SETTING_POPUP_TEXT_LINESPACING | 45 |
| APP_SETTING_LEFTMENU_LISTVIEW_Y | 170 | APP_SETTING_POPUP_LOADING_Y | 303 |
| APP_SETTING_USM_RIGHT_TEXT_MARGIN | 33 | TOP_BAR_TEXT_FONT_SIZE_NEW | 39 |
| APP_SETTING_FONT_SIZE_24/27/28/30/32/33/36/39/40/42 | 24/27/28/30/32/33/36/39/40/42 | STRAIGHT_SCROLL_WIDTH / STRAIGHT_STRAIGHT_SCROLL_HEIGHT | 15 / 540 |
| FONT_SIZE_33 / FONT_SIZE_39 / FONT_SIZE_42 | 33/39/42 | WIFI_STRAIGHT_SCROLL_WIDTH | 21 |
| APP_SETTING_WIFI_SUB_LIST_LEFT_MARGIN | 105 | X_COORDINATE_DEFAULT / Y_COORDINATE_DEFAULT | 0/0 |

Colors (`GENERAL_COLOR_*`) are returned as strings (`#FAFAFA`, `#929292`, `#4e4e4e`, `#5B5B5B`,
`#C1C1C1`, `#989899`, `#0087EF`, `#FE7E00`, `#D4D4D4` appear in the binary's stringdata);
fonts are the "Cube" families (`FONT_B`=CubeB-bold, `FONT_R`=CubeR —
`strings.tsv` "CubeBa1"/"CubeR"; NotificationManager `DefineModel` exposes `FONT_CUBEB/FONT_CUBER`).

## 12. Open items / discrepancies

- `ConstantModel`, `DefineModel`, `HMIEVENTS`, `SetupThemeHelper`, `defineModel`,
  `StringModel`, `QML_Handler`, `AppSettingModel/Engine` are **C++ context singletons** — a new
  app must register equivalents (or link the platform libs) before reusing this QML verbatim.
- SystemPopup `DefineModel` numeric values could not be mapped reliably from the
  NotificationManager binary (metacall ordering differs); popup geometry there is cited from
  QML arithmetic instead (§4.2).
- `Q_MarqueeTextRefact`'s `animationCondition` is never bound in AppSetup — the component is
  inert; treat AppMediaPlayer's `QHMI_ScrollText` as the reference marquee.
- AppOSDManager `mainVolume.qml` hardcodes height 75 with `ConstantModel.VOLUME_HEIGHT`
  commented out (mainVolume.qml:5) — the binary enum suggests 72; use 75 as shipped.
- AppSetup `qml_eco3.0` slider height is 135 vs 126 in `qml_new2.0`
  (`Q_STSliderBar.qml:63` in both trees) — the only functional component diff found.
