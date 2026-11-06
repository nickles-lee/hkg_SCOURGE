# Gen5W design tokens — decoded values for native UI

Decoded from the AppHomeScreen binary (`bin_3208391a859693e9`, Qt metaobject
string table + `qt_metacall` jump table), cross-validated against stock QML.
Full 327-row `ConstantModel` table, the 3 901-entry `defineModel` table, and
methodology: `reports/tokens.md`. AppSetup's ~435-value model:
`reports/constant_model_table.txt`.

**Every app binary ships its own `ConstantModel`/`defineModel` with different
values.** The values below are AppHomeScreen's (home screen, widgets, popups).
`SCREEN_FULL_MODE_WIDTH`, `STATUSBAR_HEIGHT`, `CubeR`, `APP_SETTING_*` resolve
in *other* apps' binaries.

## 1. Registration mechanism

`AppHomeEngine::initEngine()` registers context properties: `ConstantModel`
(C++ `QMLConstantModel`, 327 CONSTANT props), `defineModel` (`QMLdefineModel`,
3 901 props), `HomeThemeHelper` (`ThemeHelper`, 58 runtime QString props),
`DEVICE_WIDTH` (1280/1920 runtime), `DEVICE_HEIGHT` (**720 hardcoded**),
`RESOURCE_DIR` = `/app/share`, `AppHomeModel` (162 props), `AWComponent`,
`STComponent`. Confidence: confirmed (anchors `MENU_BAR_Y=90`,
`SCREEN_WIDTH=1280`, `IMAGE_PROVIDER='image://AppHomeScreen/'` verified).

## 2. Geometry tokens (ConstantModel)

| Token | Value | Meaning |
|---|---|---|
| `MENU_BAR_Y`, `MENU_BAR_HEIGHT`, `STATUS_BAR_HEIGHT` | 90 | status bar; content starts below |
| `SCREEN_WIDTH` / `SCREEN_WIDE_WIDTH` / `SCREEN_FULL_WIDTH` / `SCREEN_NORMAL_WIDTH` | 1280 / 1920 / 1920 / 1280 | panel widths |
| `SCREEN_HEIGHT` / `SCREEN_WIDE_HEIGHT` | 720 | panel height |
| `CONTENT_WITH_MENUBAR_AREA_HEIGHT` | 630 | full-screen content band |
| `CONTENT_AREA_Y` / `CONTENT_AREA_HEIGHT` | 180 / 540 | two-pane content area (AllMenus/Setup style) |
| `SETUP_PAGE_Y`, `SETUP_MENU_WIDTH/HEIGHT` | 180, 306 / 249 | grid page top; grid cell |
| `HOME_1920_NAVI_X/Y/W/H` | 12 / 102 / 618 / 603 | widget-page navi card |
| `HOME_1920_WIDGET_X/Y/W/H` | 642 / 102 / 1278 / 405 | widget-page card strip |
| `POPUP_WIDTH_MIN/MID/MAX` | 678 / 972 / 1260 (home MAX = 1280) | popup width classes |
| `APP_HOME_POPUP_1LINE_HEIGHT` / `…TEXT_LINE_SPACING` / `…BUTTON_HEIGHT` | 297 / 45 / 78 | popup height formula |
| `MENU_LIST_HEIGHT/WIDTH` | 108 / 555 | option-menu rows |
| `STRAIGHT_SCROLL_WIDTH` / `…HEIGHT` / `STRAIGHT_STRAIGHT_SCROLL_HEIGHT` | 15 / 477 / 540 | scrollbar |
| `TITLE_TEXT_FONT_SIZE` / `TOP_BAR_TEXT_FONT_SIZE` / `TOP_BAR_TEXT_FONT_SIZE_MENU` | 40 / 34 / 45 | title roles |
| `INDICATOR_WIDTH/HEIGHT` | 1280 / 93 | page-indicator strip |

## 3. Color tokens (ConstantModel)

| Token | Value |
|---|---|
| `ECO_DAY_TEXT_COLOR` / `ECO_NIGHT_TEXT_COLOR` | `#62797f` / `#b6c1c8` |
| `GENERAL_COLOR_BLACK` / `WHITE` / `BRIGHT_GREY` | `#000000` / `#ffffff` / `#FAFAFA` |
| `GENERAL_COLOR_BUTTON_GREY` / `FOCUS_GREY` / `DISABLE_GREY` / `DIMMED_GREY` | `#2F2F2F` / `#484848` / `#5B5B5B` / `#9E9E9E` |
| `GENERAL_COLOR_PROGRESS_BLUE` / `DIMMED_BLUE` | `#0087EF` / `#447CAD` |
| `GENERAL_COLOR_SUB_TEXT_GREY` | `#D4D4D4` |
| `GENERAL_COLOR_BLACK_HMC` | `#060a10` |
| `GENERAL_COLOR_GREEN` | day `#35ffef` / night `#42a3ff` |
| `GENERAL_COLOR_FUEL_ECO` | `#4592f8` |
| `GENERAL_COLOR_DISABLE` | day `#4e4e4e` / night `#2a3342` |
| `NMODE_COLOR_NORMAL` / `SPORT` / `SPORTPLUS` | `#b8d8fe` / `#f94c3b` / `#f94c3b` |
| Press/selected/disable text variants (`GENERAL_ITEM_TEXT_COLOR_*`, `GENERAL_LIST_ITEM_LINE_2_*`) | see `reports/tokens.md` §1 rows 75-84 |

## 4. HomeThemeHelper — the runtime palette (58 roles)

Class `ThemeHelper`: 58 QString properties switched at runtime by
`setTheme(guiThemeType, dayNight, brand)` from static palette records.
Property families (state × role):

- Text: `normal/press/selected/focus/disableTextColor`
- Content text: `normal/press/selected/disableContentTextColor`
- EV text: `normal/press/selected/focus/disableTextEVColor` + `ecoFirst…ecoFourTextEVColor`
- PHEV: `first…fifthTextPHEVColor`
- Climate: `normal/press/selected/focus/disableTextClimate( button | HeatButton)Color`
- Option menu: `normal/press/selectedOptTextColor`, `disableOptTextColor`
- Checkbox/radio: `normal/focus/press/disable/selectedTextCheckboxColor`, `…RadioBtnColor`, `…DescTextRadioBtnColor`
- `loadingGifImage`

Theme ids (plugin `Defines.qml`): NONE=0, NEW2=1, GENESIS=2, **ECO3=3**.

**eco3.0 (this car) decoded palette records:**

| Family | normal | press/accent | selected | focus | disable |
|---|---|---|---|---|---|
| Text, day (hmc/kmc) | `#62797f` | `#07a8df` | `#62797f` | `#62797f` | `#62797f` |
| Text, night kmc | `#b6c1c8` | `#07a8df` | `#b6c1c8` | `#b6c1c8` | `#b6c1c8` |
| Text, night hmc | `#62797f` | `#17beb0` | `#62797f` | `#62797f` | `#62797f` |
| EV/accent, day hmc | `#62797f` | `#62797f` | `#07a8df` | `#62797f` | `#62797f` |
| EV/accent, night kmc | `#b6c1c8` | `#b6c1c8` | `#07a8df` | `#c5d4dc` | `#b6c1c8` |
| EV/accent, night hmc | `#62797f` | `#62797f` | `#17beb0` | `#62797f` | `#62797f` |

(NEW2 and GENESIS records are in `reports/tokens.md` §2. Per-record contents
confirmed; per-property-group attribution probable.)

## 5. defineModel — images and asset conventions

3 901 properties: 3 770 image URLs (`image://AppHomeScreen/<category>/<file>.png`),
15 `Color*` values, `dayNightMode`. Conventions:

- State suffixes: `_n` normal, `_p` press, `_f` focus, `_s` selected, `_d`
  disabled; `_s` also marks narrow-screen variants (`…_s_p`).
- Day/night: `@` placeholder in filenames resolved by the provider to
  `d`/`n`, or explicit `_d/_n` pairs; new_gui themes use directory trees
  `/app/share/images/new_gui/eco3.0/{hmc|kmc}/{day|night}/…`.
- N-mode assets switch category by drive mode
  (`image://AppHomeScreen/{nmode|performance}/…`).
- Assets are ASTC-compressed; loading GIF per theme at
  `…/general/co_ic_loading_l.gif` (12-entry table in `reports/tokens.md` §2).
- Full key table: `reports/tokens.md` §3.

## 6. Fonts

Registered families (from binary registration strings — TTF name tables on
disk are scrambled; the binary is authoritative):

| Family | Skin / use |
|---|---|
| `HKMC_FONT_R` / `HKMC_FONT_B` (425 uses) | eco3.0 body / bold (Cube files: `Cube-Regular.ttf`, `Cube-Bold.ttf`, CJK variants) |
| `New HD2.1` (+ Medium, Number_S, TC) | new2.0 skin |
| `GenesisR/B Incl TC`, `Genesis` | genesis skin |
| `DFHeiW5-A` (`FONT_DFHEI5`) | CJK fallback |
| `HKMC_FONT_NMODE_NUMBER` | N-mode gauges |

Pixel-size roles (frequency across stock QML):

| Role | px |
|---|---|
| Hero numbers (N-mode) | 100–129 |
| Clocks | 60–132 |
| Popup/large title | 48 |
| Emphasized title / heading | 42 |
| **Body / list / default** | **39** |
| Secondary | 33–36 |
| Caption | 27–30 |
| Small print | 21–24 |

Always `ConstantModel.FONT_SIZE_*` (11…54, 57, 60, 63, 66, 80, 100 exist);
`lineHeightMode: Text.FixedHeight`, `lineHeight: 39`.

## 7. Durations & easing

`ConstantModel.animDuration = 300` (house default). Stock histogram and
canonical assignments:

| Interaction | Value | Easing |
|---|---|---|
| Page/screen transition | **150 ms** (slide from width/2 + fade) | OutQuad + InCubic |
| Standard micro-animation / default | **300 ms** | Linear |
| Popup fade in/out | **100 ms** | InOutQuad |
| Split-screen slide / drag snap | 200 ms | OutQuad |
| List contentX snap | 200–500 ms | — |
| Blink (attention) | 300 ms/step or 1000 ms loop | InOutCubic |
| Long-press | 650 ms | — |
| Drag-start hold | 500 ms | — |
| Indicator auto-hide | 5 000 ms fade | — |
| Spinner frame | 100 ms (33 in AW) | Linear |
| Touch re-enable after disable | 1 000 ms | — |
| OSD auto-hide | engine `delayTime×100` ms | — |
| Keypad slide (`USM_KEYPAD_DURATION`) | 1 000 ms | — |
| Marquee | 1 000 ms pause → scroll `(w+120)/50*1000` → 1 500 ms pause | Linear |

Opacity tokens: `GENERAL_OPACITY_PERCENT_60` / `_70` (dims), press dim text
day 0.2 / night 0.35, drag ghost 0.7.

## 8. Day/night mechanism

`AppHomeModel.dayLightSuffix` = `"#GUI_DAY"` / `"#GUI_NIGHT"` (mode 1 = day),
appended by `Q_ThemeImage` inside the image URL; the provider resolves the
variant. Marker files `/tmp/GUI_THEME_DAY` / `/tmp/GUI_THEME_NIGHT` also
exist. QML can test day mode with
`AppHomeModel.dayLightSuffix == "#GUI_DAY"`.
