# Gen5W (Hyundai Ioniq 5, EU 260128) — Interaction & Presentation Conventions in Official QML

Scope: read-only survey of stock QML trees under `/tmp/opencode/gen5w_ui/`
(AppHomeScreen, AppSetup, AppMediaPlayer, AppOSDManager, SystemPopup,
Statusbar, AppStandbyClock, AssistantWindow, Keyboard, plugins).
Target theme variant: **qml_eco3.0** (Ioniq 5 EU). All paths below are relative
to `/tmp/opencode/gen5w_ui/` unless absolute. Line numbers refer to the current
file contents.

---

## 0. Theme variants — what each app actually uses

Every app ships **all theme trees side by side**; the C++ binary picks one at
runtime from the theme variant enum `guiThemeType`:

| guiThemeType | Theme | QML dir | Evidence |
|---|---|---|---|
| 0 | base/KMC legacy | (built-in) | `AssistantWindow/qml/components/QLoadingComponent.qml:9-13` (commented switch, `/app/share/images/kmc/...`) |
| 1 | new2.0 | `qml_new2.0` | `plugins/CPCallWidget/qml/QHMI_CPCallWidget_ScreenMain_Home_New.qml:12` `isNewGUI: guiThemeType == 1` |
| 2 | genesis | `qml_genesis` | same file `:13` `isGenesisGUI: guiThemeType == 2` |
| 3 | **eco3.0** | `qml_eco3.0` | same file `:14` `isEco3GUI: guiThemeType == 3` |

Decoded asset path (from the commented switch in
`AssistantWindow/qml/components/QLoadingComponent.qml:9-13`):
`/app/share/images/new_gui/eco3.0/hmc/{day|night}/general/...` — eco3.0 lives
under `new_gui/eco3.0/hmc/` with `day`/`night` sub-trees (there is also a `kmc`
buyer sub-tree for new2.0). **Ioniq 5 EU = guiThemeType 3 / qml_eco3.0** for
AppHomeScreen, AppOSDManager, Statusbar, SystemPopup, AppSetup (all of them
contain a complete `qml_eco3.0/`), and `AppMediaPlayer/qml_new_gui/qml_eco3.0/`.
AppStandbyClock ships `qml_eco3.0` + `new_qml`; plugins ship a single `qml/`
dir (some have `qml_new2.0`/`qml_genesis` variants, e.g. `NModeAwWidget`).

---

## 1. Animations — the standard vocabulary

### 1.1 Duration histogram (all `duration:` literals, whole tree)

| Duration (ms) | Count | Typical use |
|---|---|---|
| **300** | 295 | default micro-animation: opacity blink of climate/wind icons, fades (`Climate/Component/QHMI_WindDirection_new.qml:129-134`), popup element fades |
| **100** | 69 | popup fade in/out (`QPopupAnimationItem.qml`, `PopupItem.qml`) |
| **200** | 53 | split-screen slide (`HomeScreen/main.qml:23,150-155`), drag-return `x,y` snap (`component/QMenuPage.qml:336`) |
| **500** | 52 | list contentX snap (`component/QCustomizedMenuList.qml:293`) |
| **1000** | 43 | blink loop (`SystemPopup/.../BlinkAnimation.qml`: 1000 ms), long snap |
| **1500** | 25 | marquee end pause |
| **0** | 15 | animation disabled for specific popups (e.g. mediaGate notiID 11260) |
| **8000** | 11 | air-cleaner progress (`AppOSDManager/qml_eco3.0/component/OSD_AirCLeaning.qml:58`) |
| 50/2000/40/190/… | few | special cases |

### 1.2 Easing curves (whole tree)

| Curve | Count | Used for |
|---|---|---|
| `Easing.Linear` | 112 | frame-index spinners (`NumberAnimation on index`), continuous loops |
| `Easing.InCubic` | 48 | screen-transition **opacity** fade-in |
| `Easing.InOutQuad` | 43 | popup fade in/out |
| `Easing.OutCubic` | 40 | (secondary) |
| `Easing.OutQuad` | 28 | screen-transition **x slide**, split-screen slide |
| `Easing.InOutCubic` | 4 | blink |
| `OutQuart/InQuart/InOutElastic` | 7 | rare, climate gauges |

### 1.3 Screen transition (THE convention)

`AppHomeScreen/qml_eco3.0/component/QMainScreenAnimation.qml` (whole file, 76 lines):

- `property int aniDuration: 150` (line 10)
- `OpacityAnimator { from: 0; to: 1; duration: aniDuration; easing.type: Easing.InCubic }` (lines 37-44)
- `XAnimator { from: Math.round(positionTarget.width/2); to: 0; duration: aniDuration; easing.type: Easing.OutQuad }` (lines 46-52)
- Input disabled during the run: `positionTarget.enabled = false` until `scrTransitionComplete` (lines 27, 57)
- Skipped entirely when `AppHomeModel.preventScreenAnimation` (lines 13-18) or `isStopAni`; force-reset to `opacity 1 / x 0` on `onSwitchToOtherApp` (lines 62-70).

AppSetup implements the identical idiom inline:
`AppSetup/qml_eco3.0/main.qml:17` `property int aniDuration: 150`, `:21`
`startX: root.width/2`, XAnimator `startX→0` OutQuad (`:673-681`),
OpacityAnimator `0→1` InCubic (`:683-691`), first load skips animation
(`:657-662`).

**Rule: page change = 150 ms slide-in from half page width (OutQuad) +
synchronized fade (InCubic), input blocked until finished.**

### 1.4 Popup enter/exit

- `AppHomeScreen/qml_eco3.0/component/common/QPopupAnimationItem.qml:28-64` and
  `AppSetup/qml_eco3.0/components/common/Q_PopupAnimationItem.qml:28-72`:
  `opacity: 0` initial, `aniDuration: 100`, `OpacityAnimator 0↔1` with
  `Easing.InOutQuad`, `startOpenAnimation()/startCloseAnimation()`, signal
  `closePopupFinish` on fade-out stop, force-quit on `onSwitchToOtherApp`.
- `SystemPopup/qml_eco3.0/Component/common/PopupItem.qml:44`
  `property int aniDuration: notiID === 11260 ? 0 : 100` (mediaGate popup is instant).
- Background dim: black `Rectangle` `opacity: 0.7` over the content area
  (`AppHomeScreen/.../HomeScreen/main.qml:456-470`, `dimStatusbarBtn` at `:403-410`),
  no animation on the dim itself.
- Option menu (right-side list): `popup/QOptionMenu.qml:24`
  `readonly property int animDuration: 10` — a nearly-instant slide via
  `Behavior on x { NumberAnimation { duration: animDuration } }` (`:91-94`).

### 1.5 Behavior-on-property patterns (whole tree)

`Behavior on angle` 59 (needle/gauge rotations), `Behavior on x` 39,
`Behavior on width` 27, `Behavior on y` 20, `Behavior on scale` 19,
`Behavior on opacity` 9. Split-screen loader slide:
`HomeScreen/main.qml:150-172` `Behavior on x { NumberAnimation 200 ms OutQuad }`
guarded by `AppHomeModel.hasAnimation && isTouchSSBtn`.

### 1.6 Page indicator (home)

`HomeScreen/QHMI_HomeScreenMain_Advanced.qml:421-455`: indicator dots shown on
`onMovementStarted`, hidden by a `5000 ms` `OpacityAnimator` (`:429-436`).
Flick under 60 px is cancelled (`component/QListViewHomePage.qml:17-23`).

### 1.7 "Use these numbers" table

| Interaction | Value | Citation |
|---|---|---|
| Page/screen transition | 150 ms, x: w/2→0 OutQuad + opacity 0→1 InCubic | `QMainScreenAnimation.qml:10,37-52`; `AppSetup main.qml:17-21,673-691` |
| Popup fade in/out | 100 ms InOutQuad (0 for mediaGate) | `QPopupAnimationItem.qml:28-64`; `SystemPopup/.../PopupItem.qml:44` |
| Split-screen slide | 200 ms OutQuad | `HomeScreen/main.qml:23,150-172` |
| Option-menu slide | 10 ms | `popup/QOptionMenu.qml:24,91-94` |
| Drag snap-back | 200 ms (`x,y`) | `component/QMenuPage.qml:336` |
| List snap (contentX) | 200–500 ms | `QCustomizedMenuList.qml:293` (500), `QCustomizedMenuListAllowEmpty.qml:1014` (200) |
| Icon blink (attention) | 300 ms per step, or 1000 ms InOutCubic infinite | `QHMI_WindDirection_new.qml:129-134`; `SystemPopup/.../BlinkAnimation.qml` |
| Indicator auto-hide | 5000 ms fade | `QHMI_HomeScreenMain_Advanced.qml:421-436` |
| Loading spinner frame | 100 ms/frame (home) or 33 ms/frame (AW, 20 frames) | `component/common/QLoadingComponent.qml:19-25`; `AssistantWindow/qml/components/QLoadingComponent.qml:52-66` |
| Marquee | 1000 ms start pause, `(paintedWidth+120)/50*1000` ms scroll, 1500 ms end pause, 50 px gap, 2500 ms re-check timer | `plugins/WeatherWidget/qml/QHMI_MarqueeText.qml:136-173` |
| Long-press threshold | 650 ms | `component/QMenuPageNoDrag.qml:91-99` |
| Re-enable touch after disable | 1000 ms | `HomeScreen/main.qml:513-521` |
| OSD auto-hide | engine-supplied `delayTime*100` ms | `AppOSDManager/qml_eco3.0/mainOSD.qml:577-589` |

---

## 2. Press / touch feedback

### 2.1 Pressed state = image swap (never scale/opacity on the widget itself)

- Icon arrays indexed by state: `[normal, focused, pressed, disabled]` —
  `component/common/QButton.qml:11-16` (`_NORMAL:0 _FOCUSED:1 _PRESSED:2 _DISABLE:3`),
  selection expression `:34` `(mouse.pressed && mouse.containsMouse) | mousePressed ? icons[_PRESSED] : … : icons[_DISABLE]`.
- Home-screen tile: a *separate overlay image component* is loaded while
  pressed: `component/QMenuItemNoDragComp.qml:13,25-32`
  (`icoPressed: mouseArea.pressed && mouseArea.containsMouse && firstPress`;
  `QMenuItemNoDragImagePComp.qml` shows `themeSource: imageP` =
  `home_bg_icon_p`). Normal/dim icons via `:34-40`.
- Statusbar clock: pressed = `_p` overlay **plus text color change**
  (`Statusbar/qml_eco3.0/common/QClockTime.qml:56-58,73-76`
  `TextColorEco3DayPress/TextColorEco3NightPress`).
- Popup buttons (legacy M-style): `bgImagePress`/`firstTextPressColor`
  image+color swap (`SystemPopup/qml_eco3.0/Component/MButtonTouch.qml:22-30,60-75`).
- Disabled (`_d`) and focused (`_f`) variants are separate images, dimming is
  done with explicit opacity for text only: `QMenuItemNoDragComp.qml:60`
  (day dim 0.2, night dim 0.35).
- **Scale is used only for drag-lift in edit mode, not for press**:
  `component/QMenuItem.qml:71,80` `scale: dragging&&(selectedItemId == itemID) ? 1.2 : 1`.

### 2.2 The canonical press state machine

`component/QMenuPageNoDrag.qml` (delegate, lines 25-107):

1. `onPressed`: `atPressState = true`, start `timerLongPress` (650 ms) and `delayPressTimer` (100 ms).
2. `onPositionChanged`: finger leaves the item → `atPressState = false` (no click).
3. `onCanceled`: stop timer, `atPressState = false`.
4. `onReleased`: if still `atPressState && !tonedown` → **beep + `itemClicked(itemID)`**.
5. Long-press trigger at 650 ms: beep + `itemPressAndHold()` (open edit/rearrange).

Same pattern everywhere: `HomeScreen/QHMI_HomeScreenMain_Wide.qml:106-141`,
`HomeScreen/Screen_HomeScreen_Widget.qml:49-151` (widgets),
`component/common/QButton.qml:48-73` (QItem base signals
`pressed/released/pressAndHold/canceled`, `component/common/QItem.qml:9-42`).

### 2.3 Touch-area enlargement

Edge cells get bigger touch targets via `component/common/QEnlargedMouseArea.qml`
(multiples 1.25/1.5), applied in `QMenuPageNoDrag.qml:52-60`. Statusbar buttons
use widthLeftExtent/widthRightMultiple the same way
(`Statusbar/qml_eco3.0/main.qml:36-100`).

### 2.4 hoverEnabled — touchscreen, not mouse UX

64 uses in the whole tree, two flavors:

- `AppMediaPlayer` list delegates (`QHMI_DelegateSong.qml:90` etc.) — hover
  highlight for jog/spinner or mouse-driven debug.
- Home-screen grid/widget MouseAreas (`QHMI_HomeScreenMain_Wide.qml:100`,
  `Screen_HomeScreen_Widget.qml:52,94,167`) set `hoverEnabled: true` so
  `containsMouse` stays meaningful during touch-drag; it is **not** a hover
  affordance. New touch UI does not need hover styling.

### 2.5 Beep convention (played vs not)

- **Beep is played on release (or long-press trigger), never on press-down.**
- Event per app:
  - AppHomeScreen + widget containers: `HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY`
    (794 uses; e.g. `QMenuPageNoDrag.qml:65,79`, `Screen_HomeScreen_Widget.qml:71,124,142`).
  - AppSetup: `HMIEVENTS.KEY_EVT_SETTUP_SOUND_BEEP_PLAY` (372 uses; note the
    vendor typo "SETTUP"; `AppSetup/qml_eco3.0/components/common/Q_Button.qml:75-77`).
  - AppStandbyClock: `KEY_EVT_REQUEST_BEEP` (130 uses).
  - AppMediaPlayer: `KEY_EVT_REQUEST_CLICK_CLACK_BEEP_PLAY` (9).
  - SystemPopup: QML beep calls are **commented out**
    (`SystemPopup/qml_eco3.0/Component/common/Q_Button.qml:128`,
    `QHorizontalButton.qml:148`); the C++ engine beeps via
    `AppSystemPopupEngine.setBeepSound(true)` before `systemPopupResponse(...)`
    (`SystemPopup/qml_eco3.0/typeA.qml:46-64`).
  - Special: `EVT_HOME_SOUND_BEEP_QUITEMODE_PLAY` for quiet-mode toggles
    (`RearSeatFeatures/Screen_RearQuietMode.qml:105-108` — normal beep
    explicitly disabled with `isBeepSound: false`).
- Components gate beeps on `usingBeepSound` (default **false** in
  `component/common/QItem.qml:13`; opt-in, e.g. split button
  `HomeScreen/main.qml:249 usingBeepSound: true`).
- **No beep** when: item is toned-down/disabled (`QMenuPageNoDrag.qml:64`),
  press slid off the item (`atPressState == false`), toggle/checkbox state
  flips (`component/common/QButtonActive.qml:115-118` beeps via parent only),
  and free-area taps (e.g. `main.qml:212-221` empty `onPressed`).

---

## 3. Screen geometry — narrow (1280×720) vs wide (1920×720)

### 3.1 The constants

- `DEVICE_WIDTH` / `DEVICE_HEIGHT` — C++ context properties = physical panel
  (1920 or 1280 × 720). Root items use them (`HomeScreen/main.qml:15-16`).
- `ConstantModel` (C++ singleton) — per-name constants used in QML:
  `SCREEN_WIDTH` (1280), `SCREEN_WIDE_WIDTH`/`SCREEN_FULL_WIDTH` (1920),
  `SCREEN_NORMAL_WIDTH` (1280), `SCREEN_HEIGHT` (720), `MENU_BAR_Y` (90),
  `CONTENT_WITH_MENUBAR_AREA_HEIGHT` (630), `MENU_LIST_HEIGHT` (104),
  `HOME_1280_MENU_W`, `HOME_1920_MENU_W` (=627+627+12), `HOME_1920_MENU_X`
  (=12+618+12), `HOME_1920_MENU_Y` (=102+405+15), `GENERAL_COLOR_BLACK`,
  `POPUP_WIDTH_MID` (citations: `HomeScreen/main.qml:82,132,228,232,310-318`,
  `QHMI_HomeScreenMain_Wide.qml:69-73`, `QHMI_HomeScreenMain_Advanced.qml:33-37`,
  `SystemPopup/qml_eco3.0/typeA.qml:4-5`).
- `AppHomeModel.isWideScr` — 594 uses in AppHomeScreen eco3 alone; the
  standard gate: `width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`
  (`QHMI_HomeScreenMain_Advanced.qml:152,175,204,325,344`).
- Statusbar uses its own `ST_DEVICE_WIDTH` with `!== 1920` checks
  (`Statusbar/qml_eco3.0/main.qml:6,14,19-20,31`); AppSetup uses
  `AppSettingModel.isWideScreen ? 1920 : 1280` (`AppSetup/qml_eco3.0/main.qml:8`).

### 3.2 Layout strategies observed

1. **Two-variant assets/positions** — background image swap
   (`themeSource: AppHomeModel.isWideScr ? defineModel.home_main_bg_01 : defineModel.home_main_bg_02`,
   `QHMI_HomeScreenMain_Advanced.qml:167`) and per-variant constants
   (`home_main_bg_01` wide / `_02` narrow; `_s` suffixed image keys for narrow:
   `QItemMenu.qml:115` `co_bg_box_line_menu` vs `co_bg_box_line_menu_s`,
   `QMenuPageNoDrag.qml:20` `home_bg_icon_edit_p` vs `home_bg_icon_edit_s_p`).
2. **Column-count change** — app grid is 6 columns wide / 4 narrow:
   `QHMI_HomeScreenMain_Advanced.qml:26-38`
   (`k_itemWidth: 306`, `k_itemHeight: 249`, `k_nbrOfColumns: isWideScreen ? 6 : 4`,
   `k_menuPageWidth: isWideScreen ? k_itemWidth*6 : k_itemWidth*4`,
   customize page 1636 vs 999).
3. **Fixed slots per variant** — widget page: navi 618×603 @ (12,102), widget 2
   627×405 @ (642,102), widget 3 627×405 @ (1281,102) **visible only when
   wide** (`Screen_HomeScreen_Widget.qml:14-19,86-89,160-163`).
4. **Cell size change** — bottom menu strip cells 250 wide / 207 narrow, strip
   height 183 (`QHMI_HomeScreenMain_Wide.qml:69-73,79-83,49`); advanced widget
   card 438×402 wide / 375×345 narrow (`component/WidgetItemAdvanced.qml:6-8`).
5. **Split screen (AW)** — 640 px right panel; content clamps to
   `SCREEN_WIDTH` while SS is showing (`_isSSShowing` in
   `QButtonStyleDL3.qml:54`, `QMenuBarSubmenu.qml:9`). On this car
   `AppHomeModel.screenNotSupportSS = true` disables it
   (`QHMI_HomeScreenMain_Advanced.qml:533`).
6. **RHD mirroring** — `AppHomeModel.usingRHD` flips AW x to negative
   (`HomeScreen/main.qml:82,132,142`).

### 3.3 What a new page must do to support both

1. Root: `width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH; height: DEVICE_HEIGHT`.
2. Start below the bar: `y: ConstantModel.MENU_BAR_Y` (90), content height
   `630` (`HomeScreen/main.qml:223-233`).
3. Pick background/asset variant with `isWideScr ? …bg_01… : …bg_02…` and
   `_s`-suffixed narrow assets where they exist.
4. Choose column count / cell size from `isWideScr` (never hardcode 1920-only
   geometry; if a third column exists, gate it `visible: AppHomeModel.isWideScr`).
5. Keep the status bar full width: `loader_4` pattern
   (`HomeScreen/main.qml:307-319`).
6. Handle `isFullMode` (split-screen open on wide cars) — content width drops
   to `SCREEN_WIDTH` while SS shows.

---

## 4. Text conventions

### 4.1 Base components

- `AppHomeScreen/qml_eco3.0/component/common/QText.qml`: `Text` with default
  `color: HomeThemeHelper.normalTextColor`, `font.pixelSize: 39`,
  `font.family: textBold ? "HKMC_FONT_B" : "HKMC_FONT_R"`.
- AppSetup equivalent `Q_Text.qml`: default 30 px,
  `SetupThemeHelper.normalTxtListTitleColor`, `defineModel.FONT_R/FONT_B`.
- Every app re-declares the same trio (QText/Q_Image/Q_ThemeImage) — there is
  no cross-app import; **copy the local one**.

### 4.2 font.pixelSize by role (histogram, AppHomeScreen eco3 + AppSetup/SystemPopup/Statusbar/OSD eco3)

| Role | px | Evidence |
|---|---|---|
| Big clock/standby | 90–132 | `plugins/ClockWidget/qml/QHMI_ClockWidget_ScreenMain_Front_Digital.qml:41-47` (132); statusbar time 60 (`QClockTime.qml:66`) |
| Page/card title | 45 | `WidgetItem.qml:11` (textSize 45 bold) |
| Heading / popup title / loading label | 42 | `HomeScreen/main.qml:124`; `QLoadingComponent.qml:36` |
| List/body/default | **39** | `QText.qml:5`; menu labels `QMenuPageNoDrag.qml:51` |
| Sub-list / secondary | 36, 33 | histogram (36×20, 33×26); `QEditLayoutButton.qml:92` `33` narrow |
| Caption / update-time | 30 | `Q_MarqueeText.qml` default; weather update time `QHMI_WeatherWidgetAvailableFront.qml:31` |
| Two-line label fallback | 27 | `QMenuItemNoDragComp.qml:70-74` (auto-shrink when `lineCount > 2`) |
| Small print / popup button | 24, 21, 20 | `SystemPopup/.../Q_Button.qml:80` (20, `DefineModel.FONT_CUBER` legacy) |

### 4.3 Line height & wrapping

- `lineHeightMode: Text.FixedHeight` with `lineHeight: 39` is ubiquitous
  (1779 FixedHeight uses; `QMenuItemNoDragComp.qml:63-64`, `QWidgetItem.qml:70-73`).
- `wrapMode: Text.WordWrap` (1231) > `Text.Wrap` (653); `Text.NoWrap` 37.
- `elide: Text.ElideRight` 557, `Elide.Center` 48, `ElideNone` 34.
- Two-line labels auto-shrink to 27 px rather than elide
  (`QMenuItemNoDragComp.qml:69-74`).

### 4.4 Marquee vs elide

- **Elide by default**; marquee only for dynamic long strings (city names,
  song titles) in widgets/media. Real ticker implementation:
  `plugins/WeatherWidget/qml/QHMI_MarqueeText.qml` (dual `Text` + 1000 ms
  pause + linear scroll `(paintedWidth+120)/50*1000` + 1500 ms pause, loop
  infinite, 2500 ms overflow re-check timer) — duplicated per widget
  (VRWidget, WorldCupWidget, CCSSportsWidget, MediaWidget `QHMI_ScrollingText`).
  AppSetup has `Q_MarqueeTextRefact.qml` with the same numbers.
- **Caution:** files *named* `Q_MarqueeText.qml` inside AppHomeScreen Climate /
  PHEV / AppSetup eco3 are just wrap-text wrappers with no ticker
  (`AppHomeScreen/qml_eco3.0/Climate/Component/Q_MarqueeText.qml:1-32`).

### 4.5 Color usage

- Always semantic theme-helper roles, never raw hex in pages:
  `HomeThemeHelper.normalTextColor` (233), `normalTextEVColor` (112),
  `disableTextColor` (54), `firstTextPHEVColor` (50), `secondTextPHEVColor` (45),
  `normalOptTextColor` (43), `selectedTextColor` (34), `pressTextColor` (8),
  … (full histogram in §4.5 of survey). AppSetup uses `SetupThemeHelper.*`
  (`normalTxtListTitleColor` 237, `pressTxtListTitleColor` 61, …).
- Widgets use model-provided colors: `GENERAL_COLOR_DAY_NIGHT`,
  `GENERAL_COLOR_888888` (`plugins/WeatherWidget/qml/QHMI_WeatherWidgetAvailableFront.qml:22,32`).
- Day/night text for climate uses `isDay: AppHomeModel.dayLightSuffix == "#GUI_DAY"`
  ternaries (`Climate/Component/Q_Text.qml:4-7`).

### 4.6 Case

- Translations carry their own case — English is sentence case
  ("Cancel", "OK": `AppHomeScreen/localization/ts/translation_en-gb.ts`).
- `Font.AllUppercase` only for media metadata (song/album/artist):
  `AppMediaPlayer/qml_new_gui/qml_eco3.0/components/qhmi_listview/QHMI_ListViewSong.qml:232`.
- Enum-ish labels are uppercase in the source strings themselves
  (`STR_N_MODE_CUSTOM_SET_VALUE_SPORT`).

---

## 5. Localization

### 5.1 Layout

- Per-app translation dirs: `AppHomeScreen/localization/{ts,qm}` (ts sources,
  qm compiled); `AppSetup/localization/` (flat, `AppSettings_<lang>.{ts,qm}`);
  `SystemPopup/translation/`, `AppOSDManager/translation/` (`<App>_<lang>_<REGION>.ts`,
  e.g. `AppOSDManager_de_DE.ts`); `Statusbar|AppStandbyClock|AssistantWindow/localization/{ts,qm}`.
- **26 languages** (AppHomeScreen ts set): bg, cs, da, de, el, en-au, en-gb,
  es, fi, fr-eu, hr, hu(hung), it, ko, nl, no(norwe), pl, pt, ro, ru, sk, sl,
  sv, tr, uk, zh-cmn (`ls AppHomeScreen/localization/ts`).
- ts format: Qt TS 2.0/2.1, single context `<name>QMLTranslationModel</name>`,
  `<source>STR_…</source>` keys (`AppHomeScreen/localization/ts/translation_de.ts:1-11`,
  `SystemPopup/translation/NotifivationManager_de_DE.ts`).
- ~1280 strings per language for AppHomeScreen (de: 1289 `<message>`).

### 5.2 Usage pattern in QML

- Static: `text: TranslationsModel.STR_HOME_ALL_MENU` — the key is a **C++
  property**, so re-evaluation on language change needs a manual invalidator:
  append the counter `+ AppHomeModel.textUpdate` (619 uses;
  `QWidgetItem.qml:75`, `QMenuItemNoDragComp.qml:68`).
- Dynamic keys: `TranslationsModel.getDefineLocalize(label)` (289 uses) for
  model-driven strings (`WidgetItem.qml:75`).
- Each app has its own model instance, often prefixed:
  `SttBar_TranslationModel` (Statusbar), `AWTranslationModel` (AssistantWindow),
  `WeatherWidgetScreenManagerModel.STR_…` (widgets) — widgets bind through the
  host app's ScreenManagerModel.
- Adding a string: add `STR_<APP>_<NAME>` to every ts (all 26), compile to qm
  with `lrelease`, reference as `TranslationsModel.STR_…`. There is **no
  QML-side fallback table**; a missing key returns whatever the C++ model
  returns for an unknown property (empty/undefined → blank text), so keep the
  key present in all locales. The `+ textUpdate` suffix idiom is mandatory for
  any text that must refresh on language switch.
- Placeholder/format conventions inside translated strings (QML `.replace()`):
  `#` (176), `@` (126), `%s` (79), `$s` (76), `[#]` (67), `[##]` (52),
  `##.#` (24), `[%s]` (21), `%1` (16), `[MM]` (13) — e.g.
  `STR_INFO_C_C.replace("[##]", max).replace("[##]", min)`
  (`plugins/WeatherWidget/qml/QHMI_WeatherWidgetAvailableFront.qml:76`),
  `STR_INFO_UPDATE_HH_MM_AM_PM.replace("%", time)` (`:26`); Android-style
  `%1$d` appears in ts bodies (`translation_de.ts` STR_HOME_ADD_MY_MENU_QUESTION_VARI).

### 5.3 Date/time/number formatting

- Time: 24 h zero-padded `HH:MM`; 12 h with AM/PM; **Finnish uses `.` separator**
  (`Statusbar/qml_eco3.0/common/QClockTime.qml:9-51`).
- Date: per-region/per-language formatter functions in
  `plugins/ClockWidget/qml/QHMI_ClockWidget_Common.qml:27-120`:
  `formatKorea`, `formatUS__EN_SP`, `formatUS__FR`, `formatEU` (12 h: `", "`,
  24 h: `". "` separator), `formatChina_Japan`, `formatMiddleEast`,
  `formatIndia_Brazil`; month names as 3-letter caps in 12 h ("JAN"…), `1.`-style
  in 24 h.
- Numbers: plain JS string concat; units come from translation keys
  (`STR_EV_UNIT_KM/MILE`, `STR_PHEV_WIDGET_KM_UNIT` in
  `plugins/PHEVWidget/qml/QHMI_PHEVWidget_ScreenMain_Front.qml:34-40`).

---

## 6. Images / assets

- **Day/night**: `Q_ThemeImage` (`AppHomeScreen/.../component/common/Q_ThemeImage.qml:1-7`)
  appends `AppHomeModel.dayLightSuffix` — the literal strings `"#GUI_DAY"` /
  `"#GUI_NIGHT"` (236 vs 11 direct comparisons in tree) are appended to the
  `image://` URL as a fragment; the C++ image provider resolves the variant
  (`HomeScreen/main.qml:96` shows a full URL:
  `"image://AppHomeScreen/split/split_bg_screen.png"` + suffix).
- **Provider naming**: `image://<AppName>/…` — `image://AppHomeScreen/` (662),
  `AppSystemPopup` (202), `AppOSDManager` (131), `AppSetting` (120),
  `AppMediaPlayer` (18), `AppStandbyClock` (3). Widgets use raw `Image` with
  model-supplied paths + per-model `dayNightSuffix`
  (`plugins/WeatherWidget/qml/QHMI_Image.qml:1-8`).
- **State suffixes** (defineModel key names, AppHomeScreen eco3 counts):
  `_n` normal (62), `_p` pressed (20), `_f` focused (2), `_d` disabled (21),
  `_s` narrow-screen variant (29), `_s_p` narrow pressed (6). Icon arrays in
  QML are ordered `[N, P, F, D]` (`QMenuItemNoDragComp.qml:12`,
  `QButton.qml:11`).
- **Theme image maps live in C++**: `defineModel` is a context-property object
  (e.g. `defineModel.home_bg_icon_p`, `defineModel.img_loading[]`,
  `defineModel.prefixImg99/72/Home`, `defineModel.TextColorNormal`) — the
  string table is compiled into the app binary/theme plugin, not QML. Same for
  per-app clones: `DefineModel` (SystemPopup), `SttBar_ImagesModel` (Statusbar),
  `AWDefinesModel` (AssistantWindow), `WeatherWidgetScreenManagerModel` (widgets).
- **9-slice substitute**: no `border-image`; stretchy chrome uses
  `fillMode: Image.TileHorizontally` (542), `Image.Tile` (132),
  `Image.TileVertically` (119) on pre-sliced 3-part assets (left/mid/right
  images in `AssistantWindow/qml/components/QHorizontalButton.qml:78-93`,
  `SystemPopup/.../Q_Button.qml`).
- **Icon sizes**: home grid tile icon prefix `prefixImg99` (99 px icons),
  `prefixImg72` (72 px); advanced grid cell 306×249, icon centered
  (`iconTopMargin: 51`, `QMenuItemNoDragComp.qml:10`); widget card 627×405;
  navi widget 618×603; statusbar icons 75×75 (`Statusbar/main.qml:76,96`);
  option-menu rows 104 px (`QOptionMenu.qml:26 delegateHeight: 104`).

---

## 7. Widget plugin presentation (plugins/*/qml)

Structure (all widgets follow it):

1. **Root = fixed-size Item, one file per placement** —
   `QHMI_…_Front.qml` (627×405, home page 0), `…_AW.qml` (640-wide split panel),
   `…_Advanced.qml` (624×576, e.g.
   `plugins/ClockWidget/qml/QHMI_ClockWidget_ScreenMain_Front_Digital.qml:18-19`
   `width: isAdvanceMode? 624 : 627; height: isAdvanceMode? 576 : 495`),
   `…_FrontFull.qml`. A thin `…_Front.qml` shell just Loaders the variant
   (`plugins/ClockWidget/qml/QHMI_ClockWidget_ScreenMain_Front.qml:9-15`).
2. **No MouseArea inside the widget** — press is owned by the host container.
   Whole-card press shows `defineModel.widget_bg_widget_p` overlay
   (`AppHomeScreen/.../HomeScreen/Screen_HomeScreen_Widget.qml:83`,
   `component/QWidgetItem.qml:24-29`), release sends
   `EVT_MAIN_LIST_ITEMCLICKED` + beep (opens the app), 650 ms hold opens the
   edit-widgets screen (`Screen_HomeScreen_Widget.qml:121-145`). Widgets that
   need internal touch (e.g. weather arrows) emit
   `sendMouseEventToHostApp` consumed by the host
   (`component/QWidgetItem.qml:57-59`).
3. **Data via C++ model properties**, not timers: `ClockWidgetModelFront.time`,
   `WeatherWidgetModelFront.language/region/unit`,
   `WeatherWidget.getUpdateTime()` JS bridge
   (`plugins/WeatherWidget/qml/QHMI_WeatherWidgetAvailableFront.qml:8,26,49,65`).
   Refresh invalidators appended to bindings: `+ ModelFront.updateQML`,
   `+ ModelFront.language` (53/33 uses) — same trick as `textUpdate`.
   The only QML timers are the marquee re-check (2500 ms) and blink loops.
4. **Styling**: local `QHMI_Image` (day/night suffix from
   `ModelFront.dayNightSuffix`), local `QHMI_Text`/`QHMI_MarqueeText`, colors
   from `GENERAL_COLOR_DAY_NIGHT` / `GENERAL_COLOR_888888`, fonts
   `HKMC_FONT_R` or per-widget families (`ClockWidget_FONT_B_NORMAL`).
   Theme differences via `guiThemeType` checks
   (`plugins/WeatherWidget/qml/QHMI_WeatherWidgetAvailableAW.qml:117`).
5. **Loading**: the host shows the shared spinner while the plugin Loader
   isn't Ready (`component/QWidgetItem.qml:31-35` +
   `component/common/QLoadingComponent.qml`); widgets themselves also ship
   `QHMI_WeatherWidgetLoadingFront.qml` etc. for data-unavailable states.

---

## 8. Loading / progress

- **Spinner** = frame-list `Q_ThemeImage` + `NumberAnimation on index` (Linear,
  infinite): `AppHomeScreen/.../component/common/QLoadingComponent.qml:14-27`
  (117×117 px, `defineModel.img_loading.length * 100` ms per cycle),
  `HomeScreen/main.qml:98-115` (AW boot dummy, `* 33` ms), AW 20-frame variant
  (`AssistantWindow/qml/components/QLoadingComponent.qml:40-68`),
  `Q_IconAnimationLoading.qml` (same pattern, Climate/PHEV/SystemPopup/AppSetup).
- **Boot/loading screen**: `HomeScreen/Screen_HomeScreen_Loading.qml` — black
  bg + centered 117×117 spinner + 10 s `aliveScreenTimer` fallback and a 200 ms
  settle timer before showing navi (`:30-75`).
- **Label**: `TranslationsModel.STR_HOME_WIDGET_PREPARING` at 42 px under the
  spinner (`QLoadingComponent.qml:29-37`, `HomeScreen/main.qml:117-127`).
- **Busy/OSD**: OSDs are full-width overlays with engine-driven timeout
  (`AppOSDManager/qml_eco3.0/mainOSD.qml:16-58`, touch dismisses; timeout
  `delayTime*100` ms at `:577-589`); AW OSD slides in 300 ms
  (`AssistantWindow/qml/main.qml:7 _OSD_DURATION`). Progress bars use stepped
  images or `Image.TileHorizontally` fills; long ops (air clean) run 8000 ms
  loops (`AppOSDManager/qml_eco3.0/component/OSD_AirCLeaning.qml:58`).
- **Dim + input shield** while busy/popup: 0.7 black dim + MouseArea
  (`HomeScreen/main.qml:456-470`), plus the `disableTouch` 1 s re-enable timer
  (`:498-521`).

---

## 9. Quick checklist for a new eco3.0 page (synthesis)

1. Root `QRootScreen`/`Item`, `width: isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`, `height: DEVICE_HEIGHT`, content `y: 90 / height: 630`.
2. Use `Q_ThemeImage` + `defineModel` keys with `_n/_p/_f/_d` and `isWideScr ? bg_01 : bg_02` / `_s` variants; stretch with `Image.TileHorizontally`.
3. Buttons: `QButton`/`Q_Item` icon-array pattern; press = `_p` image; no scale/opacity press effects; `usingBeepSound: true` where a click should beep; beep event = the app's own (`EVT_HOME_SOUND_BEEP_PLAY` in AppHomeScreen).
4. Long-press 650 ms with `atPressState` guard; 100 ms `delayPressTimer` if needed.
5. Transitions: 150 ms half-width slide + fade (copy `QMainScreenAnimation.qml`); popups 100 ms InOutQuad fade with `closePopupFinish`; dim 0.7.
6. Text: `QText` (39 px default), FixedHeight 39, WordWrap, elide Right; marquee only for dynamic long strings; colors from `HomeThemeHelper.*`.
7. Localization: `TranslationsModel.STR_XXX + AppHomeModel.textUpdate`; add the key to all 26 ts files; placeholders via `.replace("#"/"@"/"[##]"/"%s", …)`.
8. Loading: shared spinner (117×117, 100 ms/frame) + `STR_HOME_WIDGET_PREPARING` 42 px.
