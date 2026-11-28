# Gen5W `qml_eco3.0` visual pattern catalog (for the Projection settings app)

Read-only research deliverable. No OEM files were copied. Every value below is cited as
`alias/path:line`; aliases expand against repo root:

- `UI/` = `docs/ui/`  ·  `REP/` = `docs/ui/reports/`
- `TW/` = `packages/tweaks/artifacts/qml/AppHomeScreen/qml_eco3.0/`
- `WF/` = `packages/wifi/artifacts/qml/AppHomeScreen/qml_eco3.0/`

"Stock" values are decoded from firmware QML/binaries by the in-repo research reports
(whose own citations point at the extracted tree `/tmp/opencode/gen5w_ui/…`, **not present in
this workspace**). "Custom" values come from TW/WF and are *approximations* unless the note says
they match stock. `UNSOURCED` = no evidence found in the workspace.

## 0. Workspace search for extracted stock assets (file search, read-only)

- No extracted stock QML tree, `AppSetting`/`AppHomeScreen` stock source, or stock button/toggle
  image exists in the workspace. Only 152 `.qml` files, all custom app/test copies:
  `packages/{battery,tweaks,wifi,wifi1b}/artifacts/qml/AppHomeScreen/qml_eco3.0/`,
  USB stick payload copies,
  and local scratch copies outside the repo.
- Firmware is present only as **archives**, not extracted assets:
  the firmware update package,
  the build tree inputs and decrypted stock archive.
- 26 compiled stock translations (`.qm`) in `work/payload/`; 77 `.so` are venv/build
  artefacts, not firmware libraries.
- 81 `.png`: 4 custom icon sets per package under `packages/*/artifacts/assets/{tweaks,wifi,battery,hello}/`;
  43 mock-up screenshots in `.work/shots/` (e.g. `t3_g0.png`, `w2_wide_d.png`, `b_n_night.png`) —
  rendered custom pages, **not** real head-unit captures, not stock assets.
- Conclusion: all stock-facing values below are second-hand via `docs/ui/*`; do not invent stock
  screenshots or extract/copy OEM files into the app.

## 1. Page frame + title bar / back button

| Item | Geometry / value | Colour / font / state | Source |
|---|---|---|---|
| Page root | `width: isWideScr ? DEVICE_WIDTH : 1280`; `height: DEVICE_HEIGHT`; content `y: ConstantModel.MENU_BAR_Y` (90), `height: 630` | — | `UI/UI_CONTEXT.md:6-40`; `UI/tokens.md:28-31`; `UI/reports/interaction-i18n.md:268-279` |
| Overlay root | `QRootScreen`, `isEnable: !sysPopupShowing`, z 2000, beep shield z 1000 | — | `UI/styleguide.md:198-223,176-186`; `UI/components.md:28`; `UI/reports/components-home.md:47` |
| Custom implementation | `Item`, `width` ternary, `height: DEVICE_HEIGHT - 90`, `y: 90`, `z: 100` (TW) / `visible: requested && !atOtherApp` (WF) | shield: all buttons accepted, `preventStealing` | `TW/HomeScreen/TweaksPage.qml:8-14,256-266`; `WF/HomeScreen/WifiPage.qml:10-45,488-497` |
| Stock title bar | h 90, tiled bg `co_bg_title_bar`; title **42 bold**, centred; left `Q_TitleButtonExtend` at `anchors.leftMargin: 12`, icon h 90, label 39 | title colour `HomeThemeHelper.normalTextColor`; `forceDisFocus` while popup | `UI/reports/components-apps.md:119-130`; `UI/reports/components-home.md:134` |
| Home submenu bar | `QMenuBarSubmenu` h 90 at y 90; bg tiled, shadow line at y 90, title centred bold 42 | `QText` theme colour | `UI/reports/components-home.md:134`; `UI/components.md:32` |
| Custom title bar | icon 60×60 at y 15; title `x: blockX+84, y: 0, h: 90`, **48 bold**; buttons 180×66 at y 12 (12 px gutter) | `QText` + `HomeThemeHelper`; icon = plain PNG `file://` | `TW/HomeScreen/TweaksPage.qml:279-321`; `WF/HomeScreen/WifiPage.qml:504-533,1159-1169` |
| Back button | Stock Back lives in the **status bar**, C++-driven via `AppStatusbarModel`; `QButton` 4-state icons, `widthRightMultiple: 1.5`. App-local back geometry/asset: **UNSOURCED** | `icons[4] = [N,F,P,D]`, beep on release | `UI/reports/components-home.md:244-247`; `UI/reports/components-apps.md:211,473-479` |
| Transition | 150 ms: `XAnimator` from `width/2→0` OutQuad + `OpacityAnimator 0→1` InCubic, input blocked | skip when `preventScreenAnimation` | `UI/reports/interaction-i18n.md:64-81`; `UI/components.md:53` |
| Custom transition | only `Behavior on opacity { NumberAnimation 150 OutQuad }` per pane — no X slide | — | `TW/HomeScreen/TweaksPage.qml:432,533,635`; `WF/HomeScreen/WifiPage.qml:650` |

## 2. Section header / group plate

- Stock equivalent: `QMenuBarSubmenu` (§1) or left-menu delegate `Q_AppSetting_DelegateLeft`:
  h **108**, label at left margin **33**, font **39**, `lineHeight: 39`, `Text.FixedHeight`,
  `Text.WordWrap`; 3/4/5-state bg `co_btn_bg_list_t1_108/147/186/225_{s,p,sf}`; 1 px divider
  `Q_LineHorizontal` except last visible (`UI/reports/components-apps.md:89-105,351-365`;
  `UI/reports/constant_model_table.txt:212`).
- Custom: `plate: true` `TweaksRow`/`WifiRow`, h **88** (TW nav) / **84** (WF nav, zone picker),
  `radius: 18`, title **33**, subtitle **24** (WF), selected = accent tint 22 %; 8 px column
  spacing (`TW/HomeScreen/TweaksPage.qml:323-355`; `WF/HomeScreen/WifiPage.qml:535-583`;
  `TW/component/TweaksRow.qml:18-32`). Selected accent tint alpha 0.22 and pressed 0.16 are
  **custom, UNSOURCED as stock values** (`TW/component/TweaksRow.qml:45-49`).
- Right-pane plate (custom): `radius: 24`, fill 7 % ink, 1 px border 16 % ink
  (`TW/HomeScreen/TweaksPage.qml:411-421`; `WF/HomeScreen/WifiPage.qml:629-639`) — **UNSOURCED**
  as a stock surface; stock uses 3-slice `co_bg_box_*` (`UI/components.md:33`).

## 3. List row (label + helper + trailing control)

| Property | Value | Source |
|---|---|---|
| Stock row height | 108 base; +39 per extra title line; +33 per description line (108/147/186/225) | `UI/styleguide.md:83`; `UI/reports/components-apps.md:190-192,362-365` |
| Stock text | left margin 33; title 39; guide 33 with 12 px gap; `FixedHeight/lineHeight 39`, `WordWrap`, `ElideRight` | `UI/reports/components-apps.md:96-99,174-179`; `UI/tokens.md:132-133` |
| Stock states | 5-state images `[normal, focus, pressed, disable(, selected)]`; selected drawn by delegate, no ListView highlight | `UI/reports/components-apps.md:366-368,480-484`; `UI/reports/interaction-i18n.md:135-152` |
| Custom row | `height: 108`; text `x: 24`, right trailing margin 24; Column `spacing: 3`; title 39 bold (`textBold: true`) h `39*1.25`; subtitle/detail 27 h `*1.3`, `ElideRight`; 1 px separator at left/right 24, alpha 0.22 | `TW/component/TweaksRow.qml:15-114` (WF identical: `WF/component/WifiRow.qml`) |
| Custom disabled | whole row `opacity: 0.4`; MouseArea disabled → no beep | `TW/component/TweaksRow.qml:40,116-124` |
| Custom focus | none (no `_f` state) — **UNSOURCED** | `TW/component/TweaksRow.qml` (whole file) |

## 4. Toggle switch

- Custom: **96×54**, track `radius: height/2`; ON = `GENERAL_COLOR_PROGRESS_BLUE` `#0087EF`
  (`ConstantModel.GENERAL_COLOR_PROGRESS_BLUE`, `UI/tokens.md:50`), pressed ON = `Qt.darker(accent,1.25)`;
  OFF track = ink at 14 % (pressed 28 %) with **2 px** border at 60 % ink; knob diameter `h−12` = **42**,
  `y: 6`, `x: 6` / `width−w−6`, white when ON / 90 % ink when OFF; track `ColorAnimation 100 ms`,
  knob `NumberAnimation x 150 ms OutQuad`; disabled `opacity: 0.4`; beep on release
  (`TW/component/TweaksSwitch.qml:13-61`; `WF/component/WifiSwitch.qml` identical).
- Stock equivalents: `QButtonActive` check icon `co_btn_check_on/off_n` at left **21**, flips on
  release, **no beep** (parent beeps) (`UI/reports/components-home.md:65`); `QHMI_ToggleButton`
  3-slice `co_btn_bottom_*` full-width bar (`UI/components.md:35`; `UI/reports/components-home.md:68`);
  `Q_CheckboxRadio` checkbox at x 33 / top 23 (`UI/reports/components-apps.md:174-179`).
- **UNSOURCED:** a stock 96×54 pill toggle, its knob/track hex, and a stock focus state; this
  rounded-Rectangle switch is a custom approximation, not a stock asset.

## 5. Segmented / option picker (zone picker, mode buttons)

- Custom `TweaksZonePicker`: two panes, `bodyY: 90`, `bodyH: height−90−12`, `navW` 400 wide /
  340 narrow, `gap: 36`, `paneW = min(width−96−navW−gap, 1040)`, centred block; title 48 bold at
  y 0; Back 180×66 at y 12; region `ListView` rows **84** with `spacing: 8`, title 36; zone rows
  **84**, `radius: 14`, title 33, subtitle "Current" 24, pane insets 12 v / 8 h; scroll thumb
  **6 px** wide at `parent.width−12`, min h **36**, alpha 0.5; black bg + `home_main_bg_01/02`
  (`TW/component/TweaksZonePicker.qml:24-35,124-249`).
- Custom segmented buttons (`WifiPage` Mode): `WifiButton` 100×66 "Off", 130×66 "Client",
  190×66 "CarPlay AP", `labelSize: 27`, `spacing: 8`; selected = accent 22 % fill
  (`WF/HomeScreen/WifiPage.qml:674-712`; `TW/component/TweaksButton.qml:28-37`).
- Stock: `QHorizontalListButton` (5 states) / `QHMI_OptionMenu` rows **104/108** with
  `MENU_LIST_HEIGHT=108`, `MENU_LIST_WIDTH=555`, `MENU_LIST_Y=90`
  (`UI/components.md:33,78`; `UI/reports/constant_model_table.txt:324-328`;
  `UI/reports/components-home.md:235`). **UNSOURCED:** stock timezone picker, selected-segment
  tint alpha, and a 6 px scrollbar (stock is 15 px, §10).

## 6. Stepper / slider

- Custom stepper: `−` / `+` buttons **72×72**, `labelSize: 45`, `spacing: 8`; value `QText`
  **108×72**, font **39 bold**, centred; bounds clamp 0…10000 ms in 250 ms steps
  (`TW/HomeScreen/TweaksPage.qml:553-592,154-158`).
- Stock slider: `Q_STSliderBar` **966×143** (`UI/reports/components-home.md:137`); AppSetup
  **905** wide × **126** (eco3.0 **135**), label **33**, −/+ `Q_ButtonExtend` at left margin 21,
  touch multiples **1.3–1.5**, stepped quantisation, `isNegative` centre mode
  (`UI/reports/components-apps.md:151-158,573-574`; `UI/reports/components-home.md:136`);
  `mouseDraged` disables parent Flickable (`UI/reports/components-apps.md:158-164`).
- **UNSOURCED:** stock minute-stepper value button size; reuse `Q_STSliderBar` if a true slider
  is wanted.

## 7. Info / status row (icon + text)

- Custom: 16×16 dot (`radius: 8`) + `spacing: 12` + `QText` **30**; following lines **24**,
  `elide: ElideRight`; status colour accent when OK, custom warn otherwise
  (`TW/HomeScreen/TweaksPage.qml:357-408`; `WF/HomeScreen/WifiPage.qml:585-626`).
- Stock nearest: `QHomeItem` **254×183** with icon at fixed `x_icon/y_icon` and divider lines
  (`UI/reports/components-home.md:107`); `QSubListDelegate` icon + text + `list_line`
  (`UI/reports/components-home.md:127`); `Q_RightMenuDescription` for "not available while driving"
  (`UI/reports/components-apps.md:243`). **UNSOURCED:** exact stock status-dot size and row spacing.

## 8. Primary / secondary buttons

- Stock: `QButton` `icons[4] = [normal, focused, pressed, disabled]`; `QHorizontalButton`
  3-slice `left/middle/rightImages`; popup buttons **78** high, `(w−36)/2` wide, **12** px
  gutters; Reset **620×78**; beep on release only (`UI/components.md:29-32`;
  `UI/reports/components-apps.md:159-160,302-306,473-479`).
- Custom `TweaksButton`: default **240×78**, `labelSize: 33`, `radius: 16`, fill 12 % ink /
  selected 22 % accent / pressed 32 % accent, 1 px border (35 % ink, accent when down),
  `opacity: 0.4` disabled, beep on release (`TW/component/TweaksButton.qml:12-57`). Used at
  180×66, 200×66, 300×66, 72×72, 100×66 etc. (`TW/HomeScreen/TweaksPage.qml:299-321,505-519`;
  `WF/HomeScreen/WifiPage.qml:684-711,1159-1169`).
- **UNSOURCED:** stock focus tint for these custom rectangles (stock uses the `_f` image).

## 9. Confirmation modal (stock-style Yes/No — RebootPage is the reference)

| Item | Value | Source |
|---|---|---|
| Root | width `isWideScr?DEVICE_WIDTH:1280`, height `DEVICE_HEIGHT−90`; `visible: requested && !isAtOtherApp`, `enabled: !sysPopupShowing`, `opacity: sysPopupShowing?0:1` | `TW/HomeScreen/RebootPage.qml:15-31` |
| Dim | black `GENERAL_COLOR_BLACK` `#000000` at `GENERAL_OPACITY_PERCENT_70` = 0.7 | `TW/HomeScreen/RebootPage.qml:234-238`; `UI/tokens.md:48,157` |
| Shield | fill, z 1, `preventStealing`, all buttons accepted; outside release → beep + cancel (blocked while pending) | `TW/HomeScreen/RebootPage.qml:242-256` |
| Card | `QPopupAnimationItem` centred, z 2, width `APP_HOME_POPUP_WIDTH_MID` **972**, height `max(297, 297+(lineCount−1)*45)` | `TW/HomeScreen/RebootPage.qml:258-266`; `UI/tokens.md:36-37`; `UI/components.md:88-89` |
| Panel | `QVerticalButton`, `co_bg_popup_mid_{t,m,b}` 3-slice | `TW/HomeScreen/RebootPage.qml:268-275`; `UI/components.md:90` |
| Icon | `co_ic_question` (confirm) / `co_ic_info`, centred, `y: 27` | `TW/HomeScreen/RebootPage.qml:282-287`; `UI/reports/components-apps.md:300` |
| Body | width `w−84`, font `FONT_SIZE_39` **39**, `HomeThemeHelper.normalTextColor`, `WordWrap`, centred, `FixedHeight`, `lineHeight: APP_HOME_POPUP_TEXT_LINE_SPACING` **45**, `+ AppHomeModel.textUpdate` | `TW/HomeScreen/RebootPage.qml:289-301`; `UI/styleguide.md:102-108` |
| Buttons | each `(w−36)/2` × `APP_HOME_POPUP_BUTTON_HEIGHT` **78**, bottom margin **12**, left/right margin **12**; Yes **left** (primary), No **right**; label 39 via `QHMI_ToggleButton`; one-wide OK for error/unavailable | `TW/HomeScreen/RebootPage.qml:303-364`; `UI/components.md:91-93`; `UI/reports/components-apps.md:302-306,503-505` |
| Animation | `startOpenAnimation()`/`startCloseAnimation()`, **100 ms `Easing.InOutQuad`** opacity; `onClosePopupFinish` → close or stay pending | `TW/HomeScreen/RebootPage.qml:56,69,366-371`; `UI/reports/interaction-i18n.md:83-92` |
| Close hooks | back, `isAtOtherApp`, `currentScreen`, `currentHomeAdvancedPageIdx`, `requestHomeByPP`, `onNotifyHomeMainChanged` | `TW/HomeScreen/RebootPage.qml:200-212` |
| States | Yes disabled when `!serviceOk` (`opacity: 0.4`, no beep); pending modal not dismissible; unavailable/error text with recovery step | `TW/HomeScreen/RebootPage.qml:320-321,66-70,187-198` |

## 10. Scrolling list + scrollbar

- Stock: `Q_StraightScrollBar` width **15** (`STRAIGHT_SCROLL_WIDTH`), height **477/540**, thumb
  `listHeight/contentHeight*height`, y `listContentY/(listContentHeight−listHeight)*(h−thumb)`,
  OpacityMask theme images, right-anchored z 10, **visible only on overflow** (`UI/tokens.md:39`;
  `UI/reports/components-apps.md:161-162,369-373`; `UI/reports/constant_model_table.txt:390-392`).
- Stock behaviour: `SnapToItem` / `SnapOneItem`, `StrictlyEnforceRange`, `StopAtBounds`,
  `interactive: contentHeight > 540`; list snap **200–500 ms**; long-press **650 ms**; flick
  < 60 px cancelled (`UI/reports/components-apps.md:353-357,377-380`; `UI/tokens.md:145-149`;
  `UI/reports/interaction-i18n.md:107-111`).
- Custom: `ListView { clip: true; boundsBehavior: StopAtBounds }`, thin `Rectangle` thumb
  **6 px** (zone picker) / **4 px** (scan list) at `parent.width−8/12`, min h **36/24**, alpha
  0.45–0.5, visible when `visibleArea.heightRatio < 1` (`TW/component/TweaksZonePicker.qml:218-249`;
  `WF/HomeScreen/WifiPage.qml:842-888`). **FIX:** use the 15 px stock component.

## 11. Home-screen tile

- Cell **306×249**, 6 cols wide / 4 narrow (`UI/reports/components-home.md:88,172`;
  `UI/reports/interaction-i18n.md:247-251`).
- Stock tile: bg `home_bg_icon_n` → press `home_bg_icon_p` (full-cell swap); icon prefix
  `prefixImg99` = **99×99**, `iconTopMargin: 51`; label **39**, `FixedHeight 39`, two-line shrink to
  **27**; dim text opacity day **0.2** / night **0.35**; long-press **650 ms**; edge touch targets
  **1.25–1.5×** via `QEnlargedMouseArea` (`UI/reports/components-home.md:93-107`;
  `UI/reports/interaction-i18n.md:440-445,153-154`; `UI/styleguide.md:125`).
- Custom: `Q_ThemeImage` bg with `mouseArea.pressed ? home_bg_icon_p : home_bg_icon_n`; plain PNG
  icon 99×99 top margin 51; `QText` 39 centred at `verticalCenterOffset: 38` below icon
  (`TW/component/TweaksTile.qml:9-45`; `WF/component/WifiTile.qml:9-47`). `RebootTile` adds the
  correct slide-off cancel (`pressActive` reset in `onPositionChanged`, beep only when still
  active) (`TW/component/RebootTile.qml:14-57`) — copy that into the other tiles.

## 12. Day vs night colours

| Role | Day | Night (kmc / hmc) | Source |
|---|---|---|---|
| Normal text | `ECO_DAY_TEXT_COLOR` `#62797f` | `ECO_NIGHT_TEXT_COLOR` `#b6c1c8` / `#62797f` | `UI/tokens.md:47,80-82` |
| Press / accent | `#07a8df` | `#07a8df` / `#17beb0` | `UI/tokens.md:80-82` |
| EV selected | `#07a8df` | `#07a8df` / `#17beb0`; focus night kmc `#c5d4dc` | `UI/tokens.md:83-85` |
| Progress blue | `GENERAL_COLOR_PROGRESS_BLUE` `#0087EF` (dim `#447CAD`) | same | `UI/tokens.md:50`; `REP/tokens.md:143,147` |
| Other greys | BLACK `#000000`, WHITE `#ffffff`, BRIGHT_GREY `#FAFAFA`, SUB_TEXT_GREY `#D4D4D4`, FOCUS `#484848`, DISABLE `#5B5B5B`, DIMMED `#9E9E9E`, BUTTON `#2F2F2F` | same | `UI/tokens.md:48-51`; `REP/tokens.md:140-151` |
| Disable surface | `GENERAL_COLOR_DISABLE` `#4e4e4e` | `#2a3342` | `UI/tokens.md:55` |
| Dim text opacity | 0.2 | 0.35 | `UI/styleguide.md:125`; `UI/reports/components-home.md:100` |
| Press text variants | `#111111` | `#000000` | `REP/tokens.md:130` |
| Disable text variants | `#b8b8b8` | `#666666` | `REP/tokens.md:131` |
| Line-2 press / disable / selected | `#888888` / `#4e4e4e` / `#888888` | `#ffffff` / `#2a3342` / `#000000` | `REP/tokens.md:137-139` |
| Popup dim | black 0.7 (`GENERAL_OPACITY_PERCENT_70`) both | same | `UI/tokens.md:157`; `UI/components.md:87` |

Mechanism: `AppHomeModel.dayLightSuffix` = `"#GUI_DAY"` / `"#GUI_NIGHT"` appended by
`Q_ThemeImage`; assets under `new_gui/eco3.0/{hmc|kmc}/{day|night}/…` (`UI/tokens.md:160-166`;
`UI/reports/interaction-i18n.md:24-30,413-418`). Test day with
`AppHomeModel.dayLightSuffix == "#GUI_DAY"` (`TW/HomeScreen/TweaksPage.qml:229`).

## 13. Disabled / unavailable look

- Stock: dedicated `_d` image; text dim opacity 0.2 day / 0.35 night; `disableTextColor` role;
  no beep when toned-down; **never** `enabled: drivingRestriction` on a row that looks enabled —
  swap a description pane instead (`UI/styleguide.md:125,332`; `UI/reports/interaction-i18n.md:150-152,207-213`;
  `UI/ux-patterns.md:19-40`).
- Custom: `opacity: 0.4` on row/switch/button + disabled MouseArea; warn colour
  `#a64b00` day / `#ffb300` night (hardcoded) (`TW/component/TweaksRow.qml:40`;
  `TW/component/TweaksSwitch.qml:25`; `TW/component/TweaksButton.qml:26`;
  `TW/HomeScreen/TweaksPage.qml:232`; `WF/HomeScreen/WifiPage.qml:446`). The two warn hexes are
  **UNSOURCED as stock** (custom-only).
- Freshness: require `updated_ms` < 8 s old before showing remembered state; otherwise show the
  unavailable pane and dim every control (`WF/HomeScreen/WifiPage.qml:187-192`; `UI/ux-patterns.md:157-170`).

## 14. Stock iconography conventions

- All images via `defineModel.*` keys → `image://AppHomeScreen/<category>/<file>.png`, themed by
  `Q_ThemeImage` (`UI/tokens.md:90-104`; `UI/reports/interaction-i18n.md:411-423`).
- State suffixes `_n` normal, `_p` press, `_f` focus, `_d` disable, `_s` narrow variant
  (`_s_p` narrow press); `@` placeholder resolved by provider to `d`/`n`; ASTC-compressed
  (`UI/tokens.md:95-103`; `UI/reports/interaction-i18n.md:424-428`).
- Sizes: tile icon 99 px (`prefixImg99`), secondary 72 px (`prefixImg72`), status-bar icons
  75×75, option-menu rows 104/108 px, loading spinner 117×117
  (`UI/reports/interaction-i18n.md:440-445`; `UI/styleguide.md:228-230`).
- 3-slice stretch with `Image.TileHorizontally` / `Tile` / `TileVertically` — **no** `border-image`
  (`UI/reports/interaction-i18n.md:435-439`; `UI/styleguide.md:96`).
- Stroke width / vector outlines: **UNSOURCED** — icons are raster/ASTC PNGs; no SVG or stroke
  metadata exists in the workspace. Colour comes from the theme image set, not QML pixel values.
- Custom: title icon 60×60 plain PNG and tile icon 99×99 plain PNG via `file://`
  (`TW/HomeScreen/TweaksPage.qml:280-287`; `TW/component/TweaksTile.qml:21-30`).

## 15. Fonts (all patterns)

- Families `HKMC_FONT_R` / `HKMC_FONT_B` (Cube-Regular / Cube-Bold); `QText { textBold: true }`
  selects the bold family (`UI/tokens.md:106-118`; `UI/reports/components-home.md:37`).
- Roles: body/list/default **39**, emphasized/title **42–48**, secondary **33–36**, caption
  **24–30**, small print **21–24** (`UI/tokens.md:119-133`; `UI/reports/interaction-i18n.md:294-305`).
- Always `ConstantModel.FONT_SIZE_*`, `lineHeightMode: Text.FixedHeight`, `lineHeight: 39`,
  `wrapMode: WordWrap`, `elide: ElideRight` (`UI/styleguide.md:102-108`; `UI/tokens.md:132-133`).
- Numeric font *weight* (e.g. Qt.DemiBold) and any per-weight asset: **UNSOURCED**; boldness is
  expressed by family swap.

## 16. Reuse vs fix — Tweaks / Wi-Fi reference pages

**Reuse as-is (well implemented):**
1. `RebootPage.qml` confirmation modal — exact stock popup geometry, 3-slice panel, 78 px buttons,
   Yes-left ordering, 100 ms fade, full close-hook set (`TW/HomeScreen/RebootPage.qml:200-371`).
2. Overlay ownership — `visible: requested && !isAtOtherApp`, six close hooks, all-button
   `preventStealing` shield, content above z 2 (`WF/HomeScreen/WifiPage.qml:21-113,488-502`).
3. `RebootTile` press state — full-cell `_p` swap plus slide-off cancel and release-only beep
   (`TW/component/RebootTile.qml:14-57`).
4. Layout skeleton — y 90 / h 630 content band, `isWideScr` width ternary, centred two-pane block
   (`TW/HomeScreen/TweaksPage.qml:8-14,222-239`; `WF/HomeScreen/WifiPage.qml:442-455`).
5. Theme plumbing — `QText`/`Q_ThemeImage`, `HomeThemeHelper`, `FONT_SIZE_*`, `textUpdate`, no
   OEM asset copying (`TW/HomeScreen/TweaksPage.qml:230-232,294`; `UI/styleguide.md:87-113`).
6. Fail-safe status handling — 8 s freshness, file fallback, disabled controls when service down
   (`WF/HomeScreen/WifiPage.qml:187-293`; `UI/ux-patterns.md:157-170`).

**Fix (approximate / non-native):**
1. Rounded `Rectangle` rows, plates, switches, buttons (radius 16/18/24, alpha-tinted fills) —
   stock uses 3-slice images and 4/5-state arrays; replace with the local `Q*` copies
   (`TW/component/TweaksRow.qml:42-49`; `UI/components.md:29-47`; `UI/reports/components-apps.md:480-484`).
2. No focus state at all — add `_f` images / `forceFocus` (`TW/component/TweaksSwitch.qml` whole file).
3. Hardcoded English strings in both pages — use `TranslationsModel.STR_* + textUpdate`, keys in
   all 26 ts files (`TW/HomeScreen/TweaksPage.qml:334-354`; `UI/styleguide.md:109-112`).
4. Plain `Item` root / `z: 100` in Tweaks — use `QRootScreen`, z 2000; WF already gates visibility
   correctly (`TW/HomeScreen/TweaksPage.qml:8-14`; `UI/styleguide.md:176-186`;
   `packages/projection/design/APP_SPEC.md:28-31`).
5. Opacity-only pane switch — add the stock 150 ms half-width X slide, not just a fade
   (`WF/HomeScreen/WifiPage.qml:650`; `UI/reports/interaction-i18n.md:64-81`).
6. 4/6 px custom scroll thumb — use `Q_StraightScrollBar` 15 px
   (`TW/component/TweaksZonePicker.qml:241-249`; `UI/tokens.md:39`).
7. `TweaksTile`/`WifiTile`/`BatteryTile`/`HelloTile` omit the `pressActive` slide-off guard that
   `RebootTile` has (`TW/component/TweaksTile.qml:41-45` vs `TW/component/RebootTile.qml:45-57`).
8. Plate/stepper rows 84/88 and 128/148 px vs stock 108/147/186/225 cascade; stepper square
   buttons vs `Q_STSliderBar` (`TW/HomeScreen/TweaksPage.qml:331-354,553-592`;
   `UI/reports/components-apps.md:190-192,151-158`).
9. Hardcoded warn `#a64b00`/`#ffb300` and 0.4 opacity disabled look instead of `_d` assets and
   0.2/0.35 text dim (`TW/HomeScreen/TweaksPage.qml:232`; `UI/styleguide.md:125`).
10. Title 48 + 60 px icon vs stock 42 centred + 90 px title buttons; "Close" instead of status-bar
    Back (`TW/HomeScreen/TweaksPage.qml:280-310`; `UI/reports/components-apps.md:119-128`).
11. Zone picker is an instant `visible` flip with no 100 ms popup fade; use `QPopupAnimationItem`
    (`TW/component/TweaksZonePicker.qml:11,110-117`; `UI/components.md:38`).
12. Log console font 17 px (`WF/HomeScreen/WifiPage.qml:1138-1143`) is below the smallest stock
    21–24 px role and exposes technical text on the main page (`UI/tokens.md:129-130`).

## 17. Do / Don't (staying native)

**Do:** start every page at y 90 / h 630; copy `QRootScreen` + shield + all close hooks; use
`Q_ThemeImage`/`QText` with `defineModel.*` and `HomeThemeHelper.*`; `FONT_SIZE_*` with fixed 39 line
height; 150 ms slide+fade page change, 100 ms InOutQuad popup fade, 650 ms long-press; image-state
arrays `[N,F,P,D]` and beep on release only; 108 px rows (+39/+33), 33 px text margin, 12 px
gutters; 15 px scrollbar only on overflow; `TranslationsModel` keys + `textUpdate`; copy components
per app, never import across apps; append a tile to the first truly empty grid slot.

**Don't:** hardcode colours/fonts/durations; draw under the status bar; invent back buttons or
focus/hover styles; scale or fade a pressed widget instead of swapping its image; silently disable a
control that looks enabled (swap a lockout pane); fake freshness when the daemon is down; put a
system popup in QML — request it by event; use rounded-rectangle/ripple/Material styling; add a
third technical column to the main settings page; copy any OEM file into the repo.
