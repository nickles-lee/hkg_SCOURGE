# Gen5W design-token system — decoded (AppHomeScreen, firmware 260128)

Target: Hyundai/Kia Gen5W head unit, `AppHomeScreen` (NE_E.EUR.S5W_L.001.001.260128).
Theme variant in scope: **qml_eco3.0** (alternates: qml_new2.0, qml_genesis).
Everything below was decoded read-only from the Ghidra export
`../../mirror/export/bin_3208391a859693e9/` cross-checked against the
actual ELF `/mnt/shares/.../rootfs/app/bin/AppHomeScreen` (disassembly + Qt
metaobject parsing) and the stock QML trees `/tmp/opencode/gen5w_ui/`.

## 0. How the token system works (mechanism summary)

`AppHomeEngine::initEngine()` (ELF 0x786540‑0x786b00, Ghidra 0x886540+) registers the
QML context properties (strings at ELF 0xc4f800‑0xc4fab4, `strings.tsv` lines 1738‑1778):

| QML name | C++ class / value | Evidence |
|---|---|---|
| `ConstantModel` | `QMLConstantModel` — 327 CONSTANT properties (ints, colors, geometry) | qt_metacast strcmp "QMLConstantModel", functions_0005.c:5174‑style; metaobject @ ELF 0x10e0880 |
| `defineModel` | `QMLdefineModel` — 3901 CONSTANT properties (image URLs, 15 colors, dayNightMode) | metaobject @ ELF 0x10e0940 (stringdata 0xcfd2c0, data 0xcede80) |
| `HomeThemeHelper` | `ThemeHelper` — 58 NOTIFY QString properties, palette switched at runtime | metaobject @ ELF 0x10e2b00, stringdata 0xd80580 |
| `DEVICE_WIDTH` | int from engine+0x1c0 (1280 narrow / 1920 wide, runtime‑detected) | disasm 0x786549‑0x786580 |
| `DEVICE_HEIGHT` | **720 hardcoded** (`mov $0x2d0,%esi`) | disasm 0x786595 |
| `RESOURCE_DIR` | `"/app/share"` | string 0xc4fa31 |
| `AppHomeModel` | `AppHomeModel` — 162 properties (isWideScr, dayLightSuffix, …) | metaobject @ ELF 0x10df4a0 |
| `AWComponent` | AssistantWindow component | string 0xc4fab4 |

**Important:** every app binary ships its *own* `QMLConstantModel`/`QMLdefineModel`
with overlapping names but different values (AppSetup = `bin_5221ed11a7945eee`,
AppMediaPlayer = `bin_53ca3d15231d1da8`, AppOSDManager = `bin_b126383d7e804f2a`,
AppStandbyClock = `bin_cd488d6aead5b067`). `ConstantModel.CubeR`,
`ConstantModel.APP_SETTING_*`, `ConstantModel.STATUSBAR_HEIGHT` used in the QML
trees belong to **other apps'** models, not AppHomeScreen's. This report decodes
**AppHomeScreen's** models (the home screen + its widgets/popups).

Decoding method (100% mechanical, verified): Qt6‑style `QMetaObject`
{superdata, stringdata(QByteArrayData[]+blob), data(uint[]), static_metacall}.
Property *i* ↔ `static_metacall` ReadProperty case *i* ↔ stringdata name *i+1*.
Anchors verified: `MENU_BAR_Y=90`, `SCREEN_WIDTH=1280`, `SCREEN_HEIGHT=720`,
`STATUS_BAR_HEIGHT=90`, `IMAGE_PROVIDER='image://AppHomeScreen/'`,
`FONT_SIZE_11=11 … FONT_SIZE_54=54`. **Confidence for the whole table: confirmed**
(extracted from the binary's own jump table at ELF 0xcb4c40; every ambiguous
Ghidra `&DAT/&LAB` reference was re‑resolved by disassembling the real `lea`
targets).

---

## 1. QMLConstantModel — full table (327 properties)

Source: static_metacall switch, Ghidra `FUN_00c4b860` (functions_0005.c:3775‑4850),
jump table ELF 0xcb4c40, names from stringdata blob ELF 0xcb7fc0‑0xcb9b27.
`day:/night:` = value depends on the day/night flag (object+0x10; 1 = day).
Negative values are stored as 32‑bit two's complement.

| # | case | ConstantModel.NAME | value |
|---|---|---|---|
| 0 | 0x00 | APP_HOME_MYMENU_DELEGATE_HEIGHT | 110 |
| 1 | 0x01 | APP_HOME_MYMENU_MAIN_LIST_HEIGHT | 550 |
| 2 | 0x02 | APP_HOME_POPUP_1LINE_HEIGHT | 297 |
| 3 | 0x03 | APP_HOME_POPUP_BGR_TEXT_TYPEB_X | 18 |
| 4 | 0x04 | APP_HOME_POPUP_BGR_TEXT_TYPEB_Y | 18 |
| 5 | 0x05 | APP_HOME_POPUP_BUTTON_HEIGHT | 78 |
| 6 | 0x06 | APP_HOME_POPUP_NO_BUTTON_X | 770 |
| 7 | 0x07 | APP_HOME_POPUP_NO_BUTTON_Y | 233 |
| 8 | 0x08 | APP_HOME_POPUP_OK_BGTEXT_X | 41 |
| 9 | 0x09 | APP_HOME_POPUP_OK_BGTEXT_Y | 181 |
| 10 | 0x0a | APP_HOME_POPUP_OK_BUTTON_X | 770 |
| 11 | 0x0b | APP_HOME_POPUP_OK_BUTTON_Y | 18 |
| 12 | 0x0c | APP_HOME_POPUP_OK_WIDE_SCREEN_X | 413 |
| 13 | 0x0d | APP_HOME_POPUP_OK_WIDE_SCREEN_Y | 184 |
| 14 | 0x0e | APP_HOME_POPUP_OK_X | 93 |
| 15 | 0x0f | APP_HOME_POPUP_OK_Y | 171 |
| 16 | 0x10 | APP_HOME_POPUP_TEXT_LINE_SPACING | 45 |
| 17 | 0x11 | APP_HOME_POPUP_WIDTH_MAX | 1280 |
| 18 | 0x12 | APP_HOME_POPUP_WIDTH_MID | 972 |
| 19 | 0x13 | APP_HOME_POPUP_WIDTH_MIN | 678 |
| 20 | 0x14 | APP_HOME_POPUP_YES_BUTTON_X | 770 |
| 21 | 0x15 | APP_HOME_POPUP_YES_BUTTON_Y | 18 |
| 22 | 0x16 | APP_HOME_WIDE_POPUP_OK_X | 413 |
| 23 | 0x17 | AREA_ACTIVE_BACK_BUTTON | 2 |
| 24 | 0x18 | AREA_ACTIVE_BACK_SUBMENU | 1 |
| 25 | 0x19 | BACK_BUTTON_HEIGHT | 72 |
| 26 | 0x1a | BACK_BUTTON_WIDTH | 141 |
| 27 | 0x1b | BTNBACKHOME_X | 1120 |
| 28 | 0x1c | BTNBACKHOME_Y | 92 |
| 29 | 0x1d | BTNBACKHOME_Z | 10 |
| 30 | 0x1e | CONTENT_AREA_HEIGHT | 540 |
| 31 | 0x1f | CONTENT_AREA_Y | 180 |
| 32 | 0x20 | DELEGATE_SUB2_X | 0 |
| 33 | 0x21 | DELEGATE_SUB2_Y | 0 |
| 34 | 0x22 | DELEGATE_SUBLIST_X | 750 |
| 35 | 0x23 | DELEGATE_SUBLIST_Y | 110 |
| 36 | 0x24 | DELEGATE_SUB_HEIGHT | 2 |
| 37 | 0x25 | DELEGATE_SUB_WIDTH | 661 |
| 38 | 0x26 | DELEGATE_SUB_X | 0 |
| 39 | 0x27 | DELEGATE_SUB_Y | 97 |
| 40 | 0x28 | DELEGATE_SUB_Z | -30 |
| 41 | 0x29 | DELEGATE_X | 500 |
| 42 | 0x2a | DELEGATE_Y | 112 |
| 43 | 0x2b | FONT_DFHEI5 | DFHeiW5-A |
| 44 | 0x2c | FONT_FAMILY_HDB | CubeB |
| 45 | 0x2d | FONT_FAMILY_HDR | HDR |
| 46 | 0x2e | FONT_SIZE_11 | 11 |
| 47 | 0x2f | FONT_SIZE_13 | 13 |
| 48 | 0x30 | FONT_SIZE_14 | 14 |
| 49 | 0x31 | FONT_SIZE_15 | 15 |
| 50 | 0x32 | FONT_SIZE_16 | 16 |
| 51 | 0x33 | FONT_SIZE_18 | 18 |
| 52 | 0x34 | FONT_SIZE_19 | 19 |
| 53 | 0x35 | FONT_SIZE_21 | 21 |
| 54 | 0x36 | FONT_SIZE_23 | 23 |
| 55 | 0x37 | FONT_SIZE_24 | 24 |
| 56 | 0x38 | FONT_SIZE_27 | 27 |
| 57 | 0x39 | FONT_SIZE_29 | 29 |
| 58 | 0x3a | FONT_SIZE_30 | 30 |
| 59 | 0x3b | FONT_SIZE_31 | 31 |
| 60 | 0x3c | FONT_SIZE_33 | 33 |
| 61 | 0x3d | FONT_SIZE_34 | 34 |
| 62 | 0x3e | FONT_SIZE_36 | 36 |
| 63 | 0x3f | FONT_SIZE_38 | 38 |
| 64 | 0x40 | FONT_SIZE_39 | 39 |
| 65 | 0x41 | FONT_SIZE_40 | 40 |
| 66 | 0x42 | FONT_SIZE_42 | 42 |
| 67 | 0x43 | FONT_SIZE_44 | 44 |
| 68 | 0x44 | FONT_SIZE_45 | 45 |
| 69 | 0x45 | FONT_SIZE_50 | 50 |
| 70 | 0x46 | FONT_SIZE_54 | 54 |
| 71 | 0x47 | ECO_DAY_TEXT_COLOR | #62797f |
| 72 | 0x48 | ECO_NIGHT_TEXT_COLOR | #b6c1c8 |
| 73 | 0x49 | GENERAL_COLOR_111111 | #111111 |
| 74 | 0x4a | GENERAL_COLOR_666666 | #666666 |
| 75 | 0x4b | GENERAL_ITEM_TEXT_COLOR_PRESS_VARIANT | day:#111111 night:#000000 |
| 76 | 0x4c | GENERAL_ITEM_TEXT_COLOR_DISABLE_VARIANT | day:#b8b8b8 night:#666666 |
| 77 | 0x4d | GENERAL_DROP_ITEM_TEXT_COLOR_NORMAL_VARIANT | #111111 (constant) |
| 78 | 0x4e | GENERAL_DROP_ITEM_TEXT_COLOR_PRESS_VARIANT | day:#111111 night:#ffffff |
| 79 | 0x4f | GENERAL_DROP_ITEM_TEXT_COLOR_DISABLE_VARIANT | #666666 (constant) |
| 80 | 0x50 | GENERAL_COLOR_848484 | #848484 |
| 81 | 0x51 | GENERAL_COLOR_888888 | #888888 |
| 82 | 0x52 | GENERAL_LIST_ITEM_LINE_2_TEXT_COLOR_PRESS_VARIANT | day:#888888 night:#ffffff |
| 83 | 0x53 | GENERAL_LIST_ITEM_LINE_2_TEXT_COLOR_DISABLE_VARIANT | day:#4e4e4e night:#2a3342 |
| 84 | 0x54 | GENERAL_LIST_ITEM_LINE_2_TEXT_COLOR_SELECTED_VARIANT | day:#888888 night:#000000 |
| 85 | 0x55 | GENERAL_COLOR_BLACK | #000000 |
| 86 | 0x56 | GENERAL_COLOR_BRIGHT_GREY | #FAFAFA |
| 87 | 0x57 | GENERAL_COLOR_BUTTON_GREY | #2F2F2F |
| 88 | 0x58 | GENERAL_COLOR_DIMMED_BLUE | #447CAD |
| 89 | 0x59 | GENERAL_COLOR_DIMMED_GREY | #9E9E9E |
| 90 | 0x5a | GENERAL_COLOR_DISABLE_GREY | #5B5B5B |
| 91 | 0x5b | GENERAL_COLOR_FOCUS_GREY | #484848 |
| 92 | 0x5c | GENERAL_COLOR_PROGRESS_BLUE | #0087EF |
| 93 | 0x5d | GENERAL_COLOR_SUB_TEXT_GREAY | #D4D4D4 |
| 94 | 0x5e | GENERAL_COLOR_SUB_TEXT_GREY | #D4D4D4 |
| 95 | 0x5f | GENERAL_COLOR_WHITE | #ffffff |
| 96 | 0x60 | GENERAL_COLOR_363535 | #363535 |
| 97 | 0x61 | GENERAL_COLOR_b8b8b8 | #b8b8b8 |
| 98 | 0x62 | CONST_POSITION_ESC | 2 |
| 99 | 0x63 | GENERAL_OPACITY_PERCENT_60 | 1058642330 |
| 100 | 0x64 | GENERAL_OPACITY_PERCENT_70 | 1060320051 |
| 101 | 0x65 | HOME_1280_MENU_W | 417 |
| 102 | 0x66 | HOME_1920_MENU_H | 183 |
| 103 | 0x67 | HOME_1920_MENU_W | 1010 |
| 104 | 0x68 | HOME_1920_MENU_X | 642 |
| 105 | 0x69 | HOME_1920_MENU_Y | 522 |
| 106 | 0x6a | HOME_1920_NAVI_H | 603 |
| 107 | 0x6b | HOME_1920_NAVI_W | 618 |
| 108 | 0x6c | HOME_1920_NAVI_X | 12 |
| 109 | 0x6d | HOME_1920_NAVI_Y | 102 |
| 110 | 0x6e | HOME_1920_WIDGET_H | 405 |
| 111 | 0x6f | HOME_1920_WIDGET_W | 1278 |
| 112 | 0x70 | HOME_1920_WIDGET_X | 642 |
| 113 | 0x71 | HOME_1920_WIDGET_Y | 102 |
| 114 | 0x72 | HOME_BUTTON_HEIGHT | 93 |
| 115 | 0x73 | HOME_BUTTON_WIDTH | 149 |
| 116 | 0x74 | ICON_CUSTOMIZE_MENU_Y | 18 |
| 117 | 0x75 | ICON_MENU_HEIGHT | 99 |
| 118 | 0x76 | ICON_MENU_WIDTH | 99 |
| 119 | 0x77 | ICON_MENU_Y | 51 |
| 120 | 0x78 | ICON_TEXT_X | 0 |
| 121 | 0x79 | ICON_TEXT_Y | 30 |
| 122 | 0x7a | IDX_PRJ_AA | 550 |
| 123 | 0x7b | IDX_PRJ_CP | 552 |
| 124 | 0x7c | IMAGE_FOCUS_X | -200 |
| 125 | 0x7d | IMAGE_FOCUS_Y | -2 |
| 126 | 0x7e | IMAGE_LIST_ARROW_DOWN_X | 727 |
| 127 | 0x7f | IMAGE_LIST_ARROW_DOWN_Y | 689 |
| 128 | 0x80 | IMAGE_LIST_ARROW_DOWN_Z | 1 |
| 129 | 0x81 | IMAGE_LIST_ARROW_UP_X | 727 |
| 130 | 0x82 | IMAGE_LIST_ARROW_UP_Y | 169 |
| 131 | 0x83 | IMAGE_LIST_ARROW_UP_Z | 1 |
| 132 | 0x84 | INDICATOR_HEIGHT | 93 |
| 133 | 0x85 | INDICATOR_WIDTH | 1280 |
| 134 | 0x86 | LEADING | 45 |
| 135 | 0x87 | LISTLINE_HEIGHT | 3 |
| 136 | 0x88 | LISTLINE_WIDTH | 731 |
| 137 | 0x89 | LISTLINE_X | 0 |
| 138 | 0x8a | LISTLINE_Y | 109 |
| 139 | 0x8b | LIST_NUMBER_FONT_SIZE | 30 |
| 140 | 0x8c | LSTVIEWICONTXT__SUB_FOCUS_X | 156 |
| 141 | 0x8d | LSTVIEWICONTXT__SUB_FOCUS_Y | 30 |
| 142 | 0x8e | LSTVIEWICONTXT__SUB_HEIGHT | 35 |
| 143 | 0x8f | LSTVIEWICONTXT__SUB_WIDTH | 200 |
| 144 | 0x90 | LSTVIEWICONTXT__SUB_X | 191 |
| 145 | 0x91 | LSTVIEWICONTXT__SUB_Y | 31 |
| 146 | 0x92 | LSTVIEWICON_SUB_FOCUS_X | -15 |
| 147 | 0x93 | LSTVIEWICON_SUB_FOCUS_Y | -25 |
| 148 | 0x94 | LSTVIEWICON_SUB_X | 0 |
| 149 | 0x95 | LSTVIEWICON_SUB_Y | 0 |
| 150 | 0x96 | LSTVIEW_SUB_HEIGHT | 490 |
| 151 | 0x97 | LSTVIEW_SUB_WIDTH | 800 |
| 152 | 0x98 | LSTVIEW_SUB_X | 457 |
| 153 | 0x99 | LSTVIEW_SUB_Y | 165 |
| 154 | 0x9a | LSTVIEW_X | 360 |
| 155 | 0x9b | LSTVIEW_Y | 125 |
| 156 | 0x9c | MEDIA_GATE_BG_X | 102 |
| 157 | 0x9d | MEDIA_GATE_BNT_CLOSE_Y | 618 |
| 158 | 0x9e | MEDIA_GATE_GRID_1280_W | 1256 |
| 159 | 0x9f | MEDIA_GATE_GRID_1920_W | 1896 |
| 160 | 0xa0 | MEDIA_GATE_ITEM_W | 204 |
| 161 | 0xa1 | MENU_BAR_HEIGHT | 90 |
| 162 | 0xa2 | MENU_BAR_Y | 90 |
| 163 | 0xa3 | MENU_LIST_HEIGHT | 108 |
| 164 | 0xa4 | MENU_LIST_WIDTH | 555 |
| 165 | 0xa5 | MENU_LIST_WIDTH_8 | 501 |
| 166 | 0xa6 | MENU_LIST_X | 9 |
| 167 | 0xa7 | MENU_LIST_Y | 90 |
| 168 | 0xa8 | MYMENU_ADD1_IDX_DMB_STATIONS | 2 |
| 169 | 0xa9 | MYMENU_ADD1_IDX_NAVI_FAVORITE_PLACES | 0 |
| 170 | 0xaa | MYMENU_ADD1_IDX_RADIO_PRESET | 1 |
| 171 | 0xab | MYMENU_ADD1_IDX_SHORTCUTS | 3 |
| 172 | 0xac | MYMENU_ADD2_IDX_RADIO_AM | 3 |
| 173 | 0xad | MYMENU_ADD2_IDX_RADIO_FM | 0 |
| 174 | 0xae | MYMENU_ADD2_IDX_RADIO_FM1 | 1 |
| 175 | 0xaf | MYMENU_ADD2_IDX_RADIO_FM2 | 2 |
| 176 | 0xb0 | MYMENU_ADD2_IDX_RADIO_FM_DAB | 4 |
| 177 | 0xb1 | MYMENU_ADD2_IDX_SHORTCUTS_ANDROID_AUTO | 9 |
| 178 | 0xb2 | MYMENU_ADD2_IDX_SHORTCUTS_APPLE_CARPLAY | 10 |
| 179 | 0xb3 | MYMENU_ADD2_IDX_SHORTCUTS_MEDIA | 2 |
| 180 | 0xb4 | MYMENU_ADD2_IDX_SHORTCUTS_NAVI_ROUTE | 5 |
| 181 | 0xb5 | MYMENU_ADD2_IDX_SHORTCUTS_NAVI_SREACH | 4 |
| 182 | 0xb6 | MYMENU_ADD2_IDX_SHORTCUTS_NAVI_TPEG | 6 |
| 183 | 0xb7 | MYMENU_ADD2_IDX_SHORTCUTS_PHONE | 3 |
| 184 | 0xb8 | MYMENU_ADD2_IDX_SHORTCUTS_RADIO | 0 |
| 185 | 0xb9 | MYMENU_ADD2_IDX_SHORTCUTS_SETUP | 7 |
| 186 | 0xba | MYMENU_ADD2_IDX_SHORTCUTS_SIRIUS_XM | 1 |
| 187 | 0xbb | MYMENU_ADD2_IDX_SHORTCUTS_UVO_BL | 8 |
| 188 | 0xbc | MYMENU_ADD2_IDX_SIRIUS_XM | 5 |
| 189 | 0xbd | MYMENU_BUTTON_FUNCTION_ADD | 1 |
| 190 | 0xbe | MYMENU_BUTTON_FUNCTION_DELETE | 0 |
| 191 | 0xbf | MYMENU_MAX_NUMBER_ITEM | 24 |
| 192 | 0xc0 | MYMENU_OPTION_MENU_FOCUS_DOWN | 0 |
| 193 | 0xc1 | MYMENU_OPTION_MENU_FOCUS_UP | 1 |
| 194 | 0xc2 | MYMENU_POPUP_NOTIFY_X | 250 |
| 195 | 0xc3 | MYMENU_POPUP_NOTIFY_Y | 454 |
| 196 | 0xc4 | NEXT_ICON_HEIGHT | 81 |
| 197 | 0xc5 | NEXT_ICON_WIDTH | 90 |
| 198 | 0xc6 | NEXT_ICON_X_POS | 721 |
| 199 | 0xc7 | NEXT_ICON_Y_POS | 506 |
| 200 | 0xc8 | PLAY_ICON_HEIGHT | 157 |
| 201 | 0xc9 | PLAY_ICON_WIDTH | 148 |
| 202 | 0xca | PLAY_ICON_X_POS | 565 |
| 203 | 0xcb | PLAY_ICON_Y_POS | 465 |
| 204 | 0xcc | PREV_ICON_HEIGHT | 81 |
| 205 | 0xcd | PREV_ICON_WIDTH | 90 |
| 206 | 0xce | PREV_ICON_X_POS | 471 |
| 207 | 0xcf | PREV_ICON_Y_POS | 506 |
| 208 | 0xd0 | SCREEN_HEIGHT | 720 |
| 209 | 0xd1 | SCREEN_WIDE_HEIGHT | 720 |
| 210 | 0xd2 | SCREEN_WIDE_WIDTH | 1920 |
| 211 | 0xd3 | SCREEN_WIDTH | 1280 |
| 212 | 0xd4 | STATUS_BAR_HEIGHT | 90 |
| 213 | 0xd5 | SETUP_CONTENT_Y | 0 |
| 214 | 0xd6 | SETUP_CUSTOMIZE_CONTENT_Y | 75 |
| 215 | 0xd7 | SETUP_CUSTOMIZE_GUIDE_Y | 200 |
| 216 | 0xd8 | SETUP_CUSTOMIZE_MENU_HEIGHT | 207 |
| 217 | 0xd9 | SETUP_CUSTOMIZE_MENU_WIDE_WIDTH | 272 |
| 218 | 0xda | SETUP_CUSTOMIZE_MENU_WIDTH | 248 |
| 219 | 0xdb | SETUP_CUSTOMIZE_PAGE_Y | 255 |
| 220 | 0xdc | SETUP_MENU_HEIGHT | 249 |
| 221 | 0xdd | SETUP_MENU_WIDTH | 306 |
| 222 | 0xde | SETUP_PAGE_Y | 180 |
| 223 | 0xdf | STRAIGHT_SCROLL_HEIGHT | 477 |
| 224 | 0xe0 | STRAIGHT_SCROLL_WIDTH | 15 |
| 225 | 0xe1 | STRAIGHT_SCROLL_X | 1252 |
| 226 | 0xe2 | STRAIGHT_SCROLL_Y | 196 |
| 227 | 0xe3 | SUBMENUBG2DEPT_X | 0 |
| 228 | 0xe4 | SUBMENUBG2DEPT_Y | 429 |
| 229 | 0xe5 | SUBMENUBG2DEPT_Z | 10 |
| 230 | 0xe6 | SUBMENUICON_TEXT_HEIGHT | 44 |
| 231 | 0xe7 | SUBMENUICON_TEXT_X | 126 |
| 232 | 0xe8 | SUBMENUICON_TEXT_Y | 250 |
| 233 | 0xe9 | SUBMENUICON_X | 54 |
| 234 | 0xea | SUBMENUICON_Y | 157 |
| 235 | 0xeb | SUBMENUICON_Z | 10 |
| 236 | 0xec | TEXT_BUTTON_HEIGHT | 72 |
| 237 | 0xed | TEXT_BUTTON_WIDTH | 141 |
| 238 | 0xee | TITLE_HEIGHT | 72 |
| 239 | 0xef | TITLE_ICON_HEIGHT | 60 |
| 240 | 0xf0 | TITLE_ICON_WIDTH | 60 |
| 241 | 0xf1 | TITLE_TEXT_AREA_TOTAL_WIDTH | 770 |
| 242 | 0xf2 | TITLE_TEXT_AREA_WIDTH | 724 |
| 243 | 0xf3 | TITLE_TEXT_FONT_SIZE | 40 |
| 244 | 0xf4 | TITLE_TEXT_LEFT_MARGIN | 46 |
| 245 | 0xf5 | TITLE_WIDTH | 1280 |
| 246 | 0xf6 | TOP_BAR_TEXT_FONT_SIZE | 34 |
| 247 | 0xf7 | TOP_BAR_TEXT_FONT_SIZE_MENU | 45 |
| 248 | 0xf8 | TYPE_A_CUE_HEIGHT | 11 |
| 249 | 0xf9 | TYPE_A_CUE_WIDTH | 108 |
| 250 | 0xfa | TYPE_A_INDICATOR_REAR_X | 526 |
| 251 | 0xfb | TYPE_A_INDICATOR_REAR_Y | 691 |
| 252 | 0xfc | TYPE_A_INDICATOR_X | 885 |
| 253 | 0xfd | TYPE_A_INDICATOR_Y | 693 |
| 254 | 0xfe | TYPE_A_PAGE_HEIGHT | 590 |
| 255 | 0xff | TYPE_A_PAGE_WIDTH | 1920 |
| 256 | 0x100 | TYPE_A_PAGE_WIDTH_REAR | 1280 |
| 257 | 0x101 | TYPE_A_WIDGET_CLIMNATE_X | 1260 |
| 258 | 0x102 | TYPE_A_WIDGET_CLIMNATE_Y | 0 |
| 259 | 0x103 | TYPE_A_WIDGET_HEIGTH | 590 |
| 260 | 0x104 | TYPE_A_WIDGET_IMAGE_MAP_HEIGHT | 566 |
| 261 | 0x105 | TYPE_A_WIDGET_IMAGE_MAP_WIDTH | 604 |
| 262 | 0x106 | TYPE_A_WIDGET_IMAGE_MAP_X | 13 |
| 263 | 0x107 | TYPE_A_WIDGET_IMAGE_MAP_Y | 12 |
| 264 | 0x108 | TYPE_A_WIDGET_MAP_X | 0 |
| 265 | 0x109 | TYPE_A_WIDGET_MAP_Y | 0 |
| 266 | 0x10a | TYPE_A_WIDGET_MEDIA_X | 630 |
| 267 | 0x10b | TYPE_A_WIDGET_MEDIA_Y | 0 |
| 268 | 0x10c | TYPE_A_WIDGET_MENU_HEIGHT | 550 |
| 269 | 0x10d | TYPE_A_WIDGET_MENU_ICO_HEIGHT | 316 |
| 270 | 0x10e | TYPE_A_WIDGET_MENU_ICO_WIDTH | 316 |
| 271 | 0x10f | TYPE_A_WIDGET_MENU_ICO_X | 0 |
| 272 | 0x110 | TYPE_A_WIDGET_MENU_ICO_Y | 182 |
| 273 | 0x111 | TYPE_A_WIDGET_MENU_PAGE_GRID_HEIGHT | 550 |
| 274 | 0x112 | TYPE_A_WIDGET_MENU_PAGE_GRID_WIDTH | 1913 |
| 275 | 0x113 | TYPE_A_WIDGET_MENU_PAGE_GRID_X | 7 |
| 276 | 0x114 | TYPE_A_WIDGET_MENU_PAGE_GRID_Y | 112 |
| 277 | 0x115 | TYPE_A_WIDGET_MENU_PAGE_HEIGHT | 550 |
| 278 | 0x116 | TYPE_A_WIDGET_MENU_PAGE_WIDTH | 1920 |
| 279 | 0x117 | TYPE_A_WIDGET_MENU_TEXT_HEIGHT | 42 |
| 280 | 0x118 | TYPE_A_WIDGET_MENU_TEXT_LINE_1_1_Y | 127 |
| 281 | 0x119 | TYPE_A_WIDGET_MENU_TEXT_LINE_1_2_Y | 99 |
| 282 | 0x11a | TYPE_A_WIDGET_MENU_TEXT_LINE_2_2_Y | 151 |
| 283 | 0x11b | TYPE_A_WIDGET_MENU_TEXT_SIZE | 42 |
| 284 | 0x11c | TYPE_A_WIDGET_MENU_TEXT_WIDHT | 316 |
| 285 | 0x11d | TYPE_A_WIDGET_MENU_WIDTH | 316 |
| 286 | 0x11e | TYPE_A_WIDGET_PAGE_HEIGHT | 590 |
| 287 | 0x11f | TYPE_A_WIDGET_PAGE_WIDTH | 1920 |
| 288 | 0x120 | TYPE_A_WIDGET_PAGE_X | 0 |
| 289 | 0x121 | TYPE_A_WIDGET_PAGE_Y | 93 |
| 290 | 0x122 | TYPE_A_WIDGET_SPACE | 0 |
| 291 | 0x123 | TYPE_A_WIDGET_WIDTH | 630 |
| 292 | 0x124 | TYPE_B_CUE_WIDTH | 69 |
| 293 | 0x125 | X_COORDINATE_DEFAULT | 0 |
| 294 | 0x126 | Y_COORDINATE_DEFAULT | 0 |
| 295 | 0x127 | NMODE_COLOR_NORMAL | #b8d8fe |
| 296 | 0x128 | NMODE_COLOR_SPORT | #f94c3b |
| 297 | 0x129 | NMODE_COLOR_SPORTPLUS | #f94c3b |
| 298 | 0x12a | STRAIGHT_STRAIGHT_SCROLL_HEIGHT | 540 |
| 299 | 0x12b | FACTOR_OF_KMH_MPH | 1070464730 |
| 300 | 0x12c | WIDTH_HEIGHT_GFORCE_CHART | 444 |
| 301 | 0x12d | CONTENT_WITH_MENUBAR_AREA_HEIGHT | 630 |
| 302 | 0x12e | CONVERT_W2KWH | 1000 |
| 303 | 0x12f | FONT_SIZE_26 | 26 |
| 304 | 0x130 | FONT_SIZE_28 | 28 |
| 305 | 0x131 | FONT_SIZE_32 | 32 |
| 306 | 0x132 | FONT_SIZE_48 | 48 |
| 307 | 0x133 | FONT_SIZE_57 | 57 |
| 308 | 0x134 | FONT_SIZE_60 | 60 |
| 309 | 0x135 | FONT_SIZE_63 | 63 |
| 310 | 0x136 | FONT_SIZE_66 | 66 |
| 311 | 0x137 | FONT_SIZE_80 | 80 |
| 312 | 0x138 | FONT_SIZE_100 | 100 |
| 313 | 0x139 | GENERAL_COLOR_BLACK_HMC | #060a10 |
| 314 | 0x13a | GENERAL_COLOR_DISABLE | day:#4e4e4e night:#2a3342 |
| 315 | 0x13b | GENERAL_COLOR_GREEN | day:#35ffef night:#42a3ff |
| 316 | 0x13c | GENERAL_COLOR_GREEN_HEV | day:#01ecff night:#42a3ff |
| 317 | 0x13d | GENERAL_COLOR_GREY | #888888 (constant) |
| 318 | 0x13e | GENERAL_COLOR_808080 | #808080 |
| 319 | 0x13f | GENERAL_COLOR_FUEL_ECO | #4592f8 |
| 320 | 0x140 | IMAGE_PROVIDER | image://AppHomeScreen/ |
| 321 | 0x141 | SCREEN_FULL_WIDTH | 1920 |
| 322 | 0x142 | SCREEN_NORMAL_WIDTH | 1280 |
| 323 | 0x143 | animDuration | 300 |
| 324 | 0x144 | fontBold | 0 |
| 325 | 0x145 | nmodeBrakePressMax | 100 |
| 326 | 0x146 | nmodeBrakePressMin | 0 |

### Key tokens with QML cross‑validation

| Token | Value | QML evidence |
|---|---|---|
| MENU_BAR_Y / MENU_BAR_HEIGHT / STATUS_BAR_HEIGHT | 90 | `HomeScreen/main.qml` top bar; UI_CONTEXT.md rule 1 |
| SCREEN_WIDTH / SCREEN_HEIGHT | 1280 / 720 | narrow panel; `width: ConstantModel.SCREEN_WIDTH` |
| SCREEN_WIDE_WIDTH / SCREEN_WIDE_HEIGHT | 1920 / 720 | wide panel (`AppHomeModel.isWideScr`) |
| CONTENT_AREA_Y / CONTENT_AREA_HEIGHT | 180 / 540 | content area inside all‑menu screen |
| CONTENT_WITH_MENUBAR_AREA_HEIGHT | 630 | = 720−90 full‑screen content height |
| IMAGE_PROVIDER | `image://AppHomeScreen/` | 106 QML uses; custom ASTC image provider |
| FONT_DFHEI5 / FONT_FAMILY_HDB / FONT_FAMILY_HDR | `DFHeiW5-A` / `CubeB` / `HDR` | registered font families (§4) |
| animDuration | 300 ms | default animation duration (§5) |
| USM_KEYPAD_DURATION | 1000 ms | keypad slide, `ConstantModel.USM_KEYPAD_DURATION` (18 uses) |
| ECO_DAY / ECO_NIGHT_TEXT_COLOR | #62797f / #b6c1c8 | eco3.0 EV text pair (matches plugin Defines.qml) |
| NMODE_COLOR_NORMAL / SPORT / SPORTPLUS | #b8d8fe / #f94c3b / #f94c3b | N‑mode (track) screen accents |

Note: `SCREEN_FULL_MODE_WIDTH`, `SCREEN_BOOT_*`, `STATUSBAR_HEIGHT`, `CubeR`,
`APP_SETTING_*` are **not** properties of AppHomeScreen's ConstantModel — they
resolve in other apps' binaries.

---

## 2. HomeThemeHelper (C++ class `ThemeHelper`)

* Class `ThemeHelper`, 58 QString properties + 59 `*Changed` signals.
  Metaobject @ ELF 0x10e2b00 (stringdata 0xd80580, data 0xd7fc00,
  static_metacall ELF 0xbf2380 → Ghidra FUN_00cf2380, functions_0006.c:14270).
  Property READ dispatches to member getters FUN_00be8d00+i*0x50
  (functions_0004.c:32755+) — values are **member QStrings at object+0x10+8*i**,
  i.e. runtime state, not constants.
* Registered as context property `HomeThemeHelper` in `initEngine()`
  (disasm 0x786a6a: `lea "HomeThemeHelper"` → `QQmlContext::setContextProperty`).
  AppStandbyClock registers the same class as `ThemeHelperSTAN`.
* The palettes are static tables built by a load‑time initializer
  (ELF 0x683000‑0x687000): arrays of 56‑byte records
  `{QString c[5]; int guiThemeType; int isNight; int isKmc;}` plus a 12‑entry
  loading‑gif table `{QString gif; int t; int x; int y;}` at ELF 0x129e900.
  Theme ids confirmed from plugin `Defines.qml`:
  **mP_GUI_THEME_NONE=0, NEW2=1, GENESIS=2, ECO3=3**
  (plugins/BTAudioWidget/qml/Defines.qml:303‑306).

### Property order (declaration order = metacall order = member offset order)

 1. `normalTextColor` (offset +0x10)
 2. `pressTextColor` (offset +0x18)
 3. `selectedTextColor` (offset +0x20)
 4. `focusTextColor` (offset +0x28)
 5. `disableTextColor` (offset +0x30)
 6. `normalContentTextColor` (offset +0x38)
 7. `pressContentTextColor` (offset +0x40)
 8. `selectedContentTextColor` (offset +0x48)
 9. `disableContentTextColor` (offset +0x50)
10. `normalTextEVColor` (offset +0x58)
11. `pressTextEVColor` (offset +0x60)
12. `selectedTextEVColor` (offset +0x68)
13. `focusTextEVColor` (offset +0x70)
14. `disableTextEVColor` (offset +0x78)
15. `firstTextPHEVColor` (offset +0x80)
16. `secondTextPHEVColor` (offset +0x88)
17. `thirdTextPHEVColor` (offset +0x90)
18. `fourthTextPHEVColor` (offset +0x98)
19. `fifthTextPHEVColor` (offset +0xa0)
20. `normalTextClimateColor` (offset +0xa8)
21. `pressTextClimateColor` (offset +0xb0)
22. `selectedTextClimateColor` (offset +0xb8)
23. `focusTextClimateColor` (offset +0xc0)
24. `disableTextClimateColor` (offset +0xc8)
25. `normalTextClimateButtonColor` (offset +0xd0)
26. `pressTextClimateButtonColor` (offset +0xd8)
27. `selectedTextClimateButtonColor` (offset +0xe0)
28. `focusTextClimateButtonColor` (offset +0xe8)
29. `disableTextClimateButtonColor` (offset +0xf0)
30. `normalTextClimateHeatButtonColor` (offset +0xf8)
31. `pressTextClimateHeatButtonColor` (offset +0x100)
32. `selectedTextClimateHeatButtonColor` (offset +0x108)
33. `focusTextClimateHeatButtonColor` (offset +0x110)
34. `disableTextClimateHeatButtonColor` (offset +0x118)
35. `disableOptTextColor` (offset +0x120)
36. `normalOptTextColor` (offset +0x128)
37. `pressOptTextColor` (offset +0x130)
38. `selectedOptTextColor` (offset +0x138)
39. `ecoFirstTextEVColor` (offset +0x140)
40. `ecoSecondTextEVColor` (offset +0x148)
41. `ecoThirdTextEVColor` (offset +0x150)
42. `ecoFourTextEVColor` (offset +0x158)
43. `normalTextCheckboxColor` (offset +0x160)
44. `focusTextCheckboxColor` (offset +0x168)
45. `pressTextCheckboxColor` (offset +0x170)
46. `disableTextCheckBoxColor` (offset +0x178)
47. `selectedTextCheckBoxColor` (offset +0x180)
48. `normalTextRadioBtnColor` (offset +0x188)
49. `focusTextRadioBtnColor` (offset +0x190)
50. `pressTextRadioBtnColor` (offset +0x198)
51. `disableTextRadioBtnColor` (offset +0x1a0)
52. `selectedTextRadioBtnColor` (offset +0x1a8)
53. `normalDescTextRadioBtnColor` (offset +0x1b0)
54. `focusDescTextRadioBtnColor` (offset +0x1b8)
55. `pressDescTextRadioBtnColor` (offset +0x1c0)
56. `disableDescTextRadioBtnColor` (offset +0x1c8)
57. `selectedDescTextRadioBtnColor` (offset +0x1d0)
58. `loadingGifImage` (offset +0x1d8)

### Decoded palette records (static init, ELF 0x6835d9‑0x684400)

Record = 5 state colors in property order + ints (guiThemeType, isNight, isKmc).
Arrays found: 0x129e230 (16 recs, first 4 empty), 0x129e620 (12), 0x129c4e8,
0x129c808, 0x129cfd0‑0x129d0f8 (one array per property group).

**Text‑color group array (0x129e230):**

| theme | night | kmc | c1 (normal) | c2 (press) | c3 (selected) | c4 (focus) | c5 (disable) |
|---|---|---|---|---|---|---|---|
| NEW2(1) | 0 | 0 | #2e2d33 | #2e2d33 | #2e2d33 | #9f9fa6 | #2e2d33 |
| NEW2(1) | 1 | 1 | #ffffff | #2dc4f3 | #ffffff | #505050 | #ffffff |
| NEW2(1) | 1 | 0 | #000000 | #ff0084 | #000000 | #b8b9bc | #000000 |
| GENESIS(2) | 0 | 1 | #ffffff | #fe77c2 | #ffffff | #505050 | #ffffff |
| GENESIS(2) | 0 | 0 | #c1c1c1 | #ba544d | #fafafa | #4a4948 | #fafafa |
| GENESIS(2) | 1 | 1 | #c1c1c1 | #ba544d | #fafafa | #4a4948 | #fafafa |
| GENESIS(2) | 1 | 0 | #c1c1c1 | #ba544d | #fafafa | #4a4948 | #fafafa |
| ECO3(3) | 0 | 1 | #62797f | #07a8df | #62797f | #62797f | #62797f |
| **ECO3(3)** | **0** | **0** | **#62797f** | **#07a8df** | **#62797f** | **#62797f** | **#62797f** |
| ECO3(3) | 1 | 1 | #b6c1c8 | #07a8df | #b6c1c8 | #b6c1c8 | #b6c1c8 |
| ECO3(3) | 1 | 0 | #62797f | #17beb0 | #62797f | #62797f | #62797f |
| (3) | — | — | #b6c1c8 | #15757e | #b6c1c8 | #b6c1c8 | #b6c1c8 |

**Second group array (0x129e620)** (EV/accent text family):

| theme | night | kmc | c1 | c2 | c3 | c4 | c5 |
|---|---|---|---|---|---|---|---|
| NEW2 | day | hmc | #2e2d33 | #2e2d33 | #ce5285 | #2e2d33 | #9f9fa6 |
| NEW2 | night | kmc | #c1c1c1 | #c1c1c1 | #00c2ff | #c1c1c1 | #505050 |
| NEW2 | night | hmc | #000000 | #000000 | #ce5285 | #000000 | #b8b8b8 |
| ECO3 | day | kmc | #c1c1c1 | #c1c1c1 | #ff69bc | #c1c1c1 | #505050 |
| ECO3 | day | hmc | #62797f | #62797f | #07a8df | #62797f | #62797f |
| ECO3 | night | kmc | #b6c1c8 | #b6c1c8 | #07a8df | #c5d4dc | #b6c1c8 |
| ECO3 | night | hmc | #62797f | #62797f | #17beb0 | #62797f | #62797f |
| (2) | — | — | #b6c1c8 | #b6c1c8 | #15757e | #c5d4dc | #b6c1c8 |
| GENESIS | … | … | #c1c1c1 | #fafafa | #ba544d | #fafafa | #4a4948 (×4 day/night) |

Cross‑validation (confirmed): plugin `CallWidget/qml/Defines.qml` uses the exact
same pairs — eco3 normal text day `#62797f` / night `#b6c1c8`; new2 normal day
`#2e2d33` / night `#ffffff`; new2 accent `#ce5285`/`#ff69bc`; genesis `#fafafa`.
ConstantModel's `ECO_DAY_TEXT_COLOR=#62797f`, `ECO_NIGHT_TEXT_COLOR=#b6c1c8`
match the ECO3 records.

**Practical answer for eco3.0 UI agents:** `HomeThemeHelper.normalTextColor` ≈
**#62797f (day) / #b6c1c8 (night)** for the eco text family, with accent
**#07a8df** (press) / **#17beb0** (alt); general UI greys `#c1c1c1/#fafafa/
#4a4948/#ba544d` for genesis‑style components. Exact per‑property values are
selected at runtime by `setTheme(guiThemeType, dayNight, brand)` — the record
arrays above are the source data; per‑group attribution is *probable*, the
record contents and theme keys are *confirmed*.

### Loading gif table (ELF 0x129e900, 12 entries)

| gui theme | brand | day/night | loadingGifImage |
|---|---|---|---|
| new2.0 | hmc | day | /app/share/images/new_gui/new2.0/hmc/day/general/co_ic_loading_l.gif |
| new2.0 | hmc | night | /app/share/images/new_gui/new2.0/hmc/night/general/co_ic_loading_l.gif |
| new2.0 | kmc | day | …/new2.0/kmc/day/general/co_ic_loading_l.gif |
| new2.0 | kmc | night | …/new2.0/kmc/night/general/co_ic_loading_l.gif |
| eco3.0 | hmc | day | …/eco3.0/hmc/day/general/co_ic_loading_l.gif |
| eco3.0 | hmc | night | …/eco3.0/hmc/night/… |
| eco3.0 | kmc | day | …/eco3.0/kmc/day/… |
| eco3.0 | kmc | night | …/eco3.0/kmc/night/… |
| genesis | any | any | /app/share/images/genesis/general/co_ic_loading_l.gif |

(strings.tsv lines 1145‑1153; default registered value = new2.0 hmc day,
functions_0001.c:18632).

---

## 3. defineModel (C++ class `QMLdefineModel`)

* 3901 CONSTANT properties, all QString except `dayNightMode` (int member).
  Metaobject @ ELF 0x10e0940, stringdata 0xcfd2c0, data 0xcede80,
  static_metacall uses jump table **ELF 0xce9f60** (Ghidra `FUN_00c4d4c0`,
  functions_0005.c:5195‑5254; Ghidra could not recover the table —
  "Could not recover jumptable at 0x00c4d5ab" — it was decoded from the binary).
* Composition: **3770 image URLs** (`image://AppHomeScreen/<category>/<name>.png`),
  15 `Color*` properties, `dayNightMode`, plus ~115 misc.
* Image URL conventions (confirmed against the asset store
  `/app/share/images/{hmc,kmc,genesis}/<category>/`):
  * state suffixes: `_n` normal, `_p` press, `_f` focus, `_s` selected, `_d` disable
    (e.g. `co_btn_bottom_l_{n,p,f,s}.png`, `co_bg_box_106_l_{n,p,f,d}.png`);
  * day/night themed assets come in `_d`/`_n` pairs (e.g. `home_soundmood_list_72_d.png`
    / `_n.png`) or use the `@` placeholder (see §6);
  * assets are **ASTC‑compressed**: served as `<name>.png.astc.header` +
    `.png.astc.lz4` (or `.gz`) — the provider decompresses on the fly;
  * N‑mode images are dynamic: value =
    `image://AppHomeScreen/{nmode|performance}/<file>` — prefix chosen at runtime
    by drive mode (FUN_00c4d3a0, functions_0005.c:5136‑5156: mode 3/4 →
    `performance/`, else `nmode/`).
* Colors in defineModel (AppHomeScreen): `Color888888 #888888`,
  `Colorff4f2e #ff4f2e`, `Color6aabed #6aabed`, `Colorffffff #ffffff`,
  `Color4ab3ef #4ab3ef`, `Color8f9192 #8f9192`, `ColorBlack #000000`,
  `ColorBrightGray/BrightGrey #FAFAFA`, … and day/night text variants:
  `TextColorNormal` `#ffffff`, `TextColorDisableVariant` day `#4e4e4e` night
  `#ffffff`, `TextColorSelectedVariant` day `#ffffff` night `#000000`,
  `TextColorClimate4e4e4e` day `#4e4e4e` night `#2a3342`
  (helpers FUN_00a54990/00a54a00/00a54a70, functions_0003.c:34859‑34902).
* NOTE: `DefineModel.FONT_CUBER`, `defineModel.FONT_R`, `DefineModel.FONT_SIZE_39`
  etc. in the QML trees belong to **SystemPopup / AppSetup / plugins** models
  (each registers its own). AppHomeScreen's defineModel has no FONT_* properties.

### defineModel full decoded table (name = value)

`DYN` = `image://AppHomeScreen/{nmode|performance}/` prefix (runtime drive‑mode
dependent). `?` = day/night branch too complex to capture mechanically (21).

```
dayNightMode = <int member: dayNightMode>
nmodeImageSource = ?
Color888888 = #888888
Colorff4f2e = #ff4f2e
Color6aabed = #6aabed
Colorffffff = #ffffff
Color4ab3ef = #4ab3ef
Color8f9192 = #8f9192
ColorBlack = #000000
ColorBrightGray = #FAFAFA
ColorBrightGrey = #FAFAFA
ColorBtnDisableGrey = #5a5a5a
ColorDisableGrey = #414141
ColorGey = #999999
ColorSelectRed = #fe7e00
ColorSubTextGray = #D4D4D4
ColorSubTextGrey = #CCCCCC
FOCUS_OUT_BY_WL = 4
FOCUS_OUT_BY_WR = 5
FOCUS_TO_BOT = 1
FOCUS_TO_LEFT = 3
FOCUS_TO_RIGHT = 2
FOCUS_TO_TOP = 0
TextColorBlue = #2c8ccf
TextColorDisable = #4e4e4e
TextColorDisableVariant = day:#4e4e4e night:#ffffff
TextColorFocus = #3F4D5C
TextColorGray = #888888
TextColorGreen = #7dce42
TextColorInput = black
TextColorNormal = #ffffff
TextColorPassYellow = #d7b633
TextColorSelected = #FE7E00
TextColorSelectedVariant = day:#ffffff night:#000000
TextColorClimate4e4e4e = day:#4e4e4e night:#2a3342
TextColorSub = #999999
TextColorTitle = #FAFAFA
TextColorDisableHMC = #576275
prefixImg72 = image://AppHomeScreen/home/72x72/
prefixImg99 = image://AppHomeScreen/home/99x99/
prefixImgHome = image://AppHomeScreen/home/
bg_main = image://AppHomeScreen/general/bg_main.png
bg_main_03 = image://AppHomeScreen/general/bg_main_03.png
bg_menu_l = image://AppHomeScreen/general/bg_menu_l.png
bg_menu_l_mask = image://AppHomeScreen/general/bg_menu_l_mask.png
bg_menu_l_s = image://AppHomeScreen/general/bg_menu_l_s.png
bg_menu_r = image://AppHomeScreen/general/bg_menu_r.png
bg_menu_r_mask = image://AppHomeScreen/general/bg_menu_r_mask.png
bg_menu_r_s = image://AppHomeScreen/general/bg_menu_r_s.png
bg_menu_tab_l_f = image://AppHomeScreen/general/bg_menu_tab_l_f.png
bg_menu_tab_l_p = image://AppHomeScreen/general/bg_menu_tab_l_p.png
bg_menu_tab_r_f = image://AppHomeScreen/general/bg_menu_tab_r_f.png
bg_menu_tab_r_p = image://AppHomeScreen/general/bg_menu_tab_r_p.png
bg_optionmenu = image://AppHomeScreen/general/bg_optionmenu.png
bg_optionmenu_list_f = image://AppHomeScreen/general/bg_optionmenu_list_f.png
bg_optionmenu_list_p = image://AppHomeScreen/general/bg_optionmenu_list_p.png
bg_title = image://AppHomeScreen/general/co_bg_title_bar.png
btn_title_back_d = image://AppHomeScreen/general/co_btn_back_d.png
btn_title_back_f = image://AppHomeScreen/general/co_btn_back_f.png
btn_title_back_n = image://AppHomeScreen/general/co_btn_back_n.png
btn_title_back_p = image://AppHomeScreen/general/co_btn_back_p.png
btn_title_sub_d = image://AppHomeScreen/general/btn_title_sub_d.png
btn_title_sub_f = image://AppHomeScreen/general/co_btn_status_m_f.png
btn_title_sub_fp = image://AppHomeScreen/general/btn_title_sub_fp.png
btn_title_sub_n = image://AppHomeScreen/general/co_btn_status_m_n.png
btn_title_sub_p = image://AppHomeScreen/general/co_btn_status_m_p.png
co_bg_box_l = image://AppHomeScreen/general/co_bg_box_l.png
co_bg_box_m = image://AppHomeScreen/general/co_bg_box_m.png
co_bg_box_r = image://AppHomeScreen/general/co_bg_box_r.png
co_bg_box_l_f = image://AppHomeScreen/general/co_bg_box_l_f.png
co_bg_box_l_n = image://AppHomeScreen/general/co_bg_box_l_n.png
co_bg_box_l_p = image://AppHomeScreen/general/co_bg_box_l_p.png
co_bg_box_l_d = image://AppHomeScreen/general/co_bg_box_l_d.png
co_bg_box_m_f = image://AppHomeScreen/general/co_bg_box_m_f.png
co_bg_box_m_n = image://AppHomeScreen/general/co_bg_box_m_n.png
co_bg_box_m_p = image://AppHomeScreen/general/co_bg_box_m_p.png
co_bg_box_m_d = image://AppHomeScreen/general/co_bg_box_m_d.png
co_bg_box_r_f = image://AppHomeScreen/general/co_bg_box_r_f.png
co_bg_box_r_n = image://AppHomeScreen/general/co_bg_box_r_n.png
co_bg_box_r_p = image://AppHomeScreen/general/co_bg_box_r_p.png
co_bg_box_r_d = image://AppHomeScreen/general/co_bg_box_r_d.png
co_bg_box_t_l = image://AppHomeScreen/general/co_bg_box_t_l.png
co_bg_box_t_m = image://AppHomeScreen/general/co_bg_box_t_m.png
co_bg_box_t_r = image://AppHomeScreen/general/co_bg_box_t_r.png
co_bg_box_m_l = image://AppHomeScreen/general/co_bg_box_m_l.png
co_bg_box_m_m = image://AppHomeScreen/general/co_bg_box_m_m.png
co_bg_box_m_r = image://AppHomeScreen/general/co_bg_box_m_r.png
co_bg_box_b_l = image://AppHomeScreen/general/co_bg_box_b_l.png
co_bg_box_b_m = image://AppHomeScreen/general/co_bg_box_b_m.png
co_bg_box_b_r = image://AppHomeScreen/general/co_bg_box_b_r.png
co_bg_box_t_l_n = image://AppHomeScreen/general/co_bg_box_t_l_n.png
co_bg_box_t_l_f = image://AppHomeScreen/general/co_bg_box_t_l_f.png
co_bg_box_t_l_p = image://AppHomeScreen/general/co_bg_box_t_l_p.png
co_bg_box_t_l_d = image://AppHomeScreen/general/co_bg_box_t_l_d.png
co_bg_box_t_m_n = image://AppHomeScreen/general/co_bg_box_t_m_n.png
co_bg_box_t_m_f = image://AppHomeScreen/general/co_bg_box_t_m_f.png
co_bg_box_t_m_p = image://AppHomeScreen/general/co_bg_box_t_m_p.png
co_bg_box_t_m_d = image://AppHomeScreen/general/co_bg_box_t_m_d.png
co_bg_box_t_r_n = image://AppHomeScreen/general/co_bg_box_t_r_n.png
co_bg_box_t_r_f = image://AppHomeScreen/general/co_bg_box_t_r_f.png
co_bg_box_t_r_p = image://AppHomeScreen/general/co_bg_box_t_r_p.png
co_bg_box_t_r_d = image://AppHomeScreen/general/co_bg_box_t_r_d.png
co_bg_box_m_l_n = image://AppHomeScreen/general/co_bg_box_m_l_n.png
co_bg_box_m_l_f = image://AppHomeScreen/general/co_bg_box_m_l_f.png
co_bg_box_m_l_p = image://AppHomeScreen/general/co_bg_box_m_l_p.png
co_bg_box_m_l_d = image://AppHomeScreen/general/co_bg_box_m_l_d.png
co_bg_box_m_m_n = image://AppHomeScreen/general/co_bg_box_m_m_n.png
co_bg_box_m_m_f = image://AppHomeScreen/general/co_bg_box_m_m_f.png
co_bg_box_m_m_p = image://AppHomeScreen/general/co_bg_box_m_m_p.png
co_bg_box_m_m_d = image://AppHomeScreen/general/co_bg_box_m_m_d.png
co_bg_box_m_r_n = image://AppHomeScreen/general/co_bg_box_m_r_n.png
co_bg_box_m_r_f = image://AppHomeScreen/general/co_bg_box_m_r_f.png
co_bg_box_m_r_p = image://AppHomeScreen/general/co_bg_box_m_r_p.png
co_bg_box_m_r_d = image://AppHomeScreen/general/co_bg_box_m_r_d.png
co_bg_box_b_l_n = image://AppHomeScreen/general/co_bg_box_b_l_n.png
co_bg_box_b_l_f = image://AppHomeScreen/general/co_bg_box_b_l_f.png
co_bg_box_b_l_p = image://AppHomeScreen/general/co_bg_box_b_l_p.png
co_bg_box_b_l_d = image://AppHomeScreen/general/co_bg_box_b_l_d.png
co_bg_box_b_m_n = image://AppHomeScreen/general/co_bg_box_b_m_n.png
co_bg_box_b_m_f = image://AppHomeScreen/general/co_bg_box_b_m_f.png
co_bg_box_b_m_p = image://AppHomeScreen/general/co_bg_box_b_m_p.png
co_bg_box_b_m_d = image://AppHomeScreen/general/co_bg_box_b_m_d.png
co_bg_box_b_r_n = image://AppHomeScreen/general/co_bg_box_b_r_n.png
co_bg_box_b_r_f = image://AppHomeScreen/general/co_bg_box_b_r_f.png
co_bg_box_b_r_p = image://AppHomeScreen/general/co_bg_box_b_r_p.png
co_bg_box_b_r_d = image://AppHomeScreen/general/co_bg_box_b_r_d.png
co_bg_box_line_l = image://AppHomeScreen/general/co_bg_box_line_l.png
co_bg_box_line_m = image://AppHomeScreen/general/co_bg_box_line_m.png
co_bg_box_line_r = image://AppHomeScreen/general/co_bg_box_line_r.png
co_bg_box_line_menu = image://AppHomeScreen/general/co_bg_box_line_menu.png
co_bg_box_line_menu_s = image://AppHomeScreen/general/co_bg_box_line_menu_s.png
co_bg_box_list_f = image://AppHomeScreen/general/co_bg_box_list_f.png
co_bg_box_list_p = image://AppHomeScreen/general/co_bg_box_list_p.png
co_bg_box_103_l_n = image://AppHomeScreen/general/co_bg_box_103_l_n.png
co_bg_box_103_l_f = image://AppHomeScreen/general/co_bg_box_103_l_f.png
co_bg_box_103_l_p = image://AppHomeScreen/general/co_bg_box_103_l_p.png
co_bg_box_103_l_d = image://AppHomeScreen/general/co_bg_box_103_l_d.png
co_bg_box_103_m_n = image://AppHomeScreen/general/co_bg_box_103_m_n.png
co_bg_box_103_m_f = image://AppHomeScreen/general/co_bg_box_103_m_f.png
co_bg_box_103_m_p = image://AppHomeScreen/general/co_bg_box_103_m_p.png
co_bg_box_103_m_d = image://AppHomeScreen/general/co_bg_box_103_m_d.png
co_bg_box_103_r_n = image://AppHomeScreen/general/co_bg_box_103_r_n.png
co_bg_box_103_r_f = image://AppHomeScreen/general/co_bg_box_103_r_f.png
co_bg_box_103_r_p = image://AppHomeScreen/general/co_bg_box_103_r_p.png
co_bg_box_103_r_d = image://AppHomeScreen/general/co_bg_box_103_r_d.png
co_bg_box_menu_t = image://AppHomeScreen/general/co_bg_box_menu_t.png
co_bg_box_menu_one = image://AppHomeScreen/general/co_bg_box_menu_one.png
co_bg_box_menu_m = image://AppHomeScreen/general/co_bg_box_menu_m.png
co_bg_box_menu_b = image://AppHomeScreen/general/co_bg_box_menu_b.png
co_bg_box_menu_f = image://AppHomeScreen/general/co_bg_box_menu_f.png
co_bg_box_menu_p = image://AppHomeScreen/general/co_bg_box_menu_p.png
co_bg_list_line_l = image://AppHomeScreen/general/co_bg_list_line_l.png
co_bg_list_line_m = image://AppHomeScreen/general/co_bg_list_line_m.png
co_bg_list_line_r = image://AppHomeScreen/general/co_bg_list_line_r.png
co_bg_list_tabline = image://AppHomeScreen/general/co_bg_list_tabline.png
co_bg_popup_max_b = image://AppHomeScreen/general/co_bg_popup_max_b.png
co_bg_popup_max_m = image://AppHomeScreen/general/co_bg_popup_max_m.png
co_bg_popup_max_t = image://AppHomeScreen/general/co_bg_popup_max_t.png
co_bg_popup_mid_b = image://AppHomeScreen/general/co_bg_popup_mid_b.png
co_bg_popup_mid_m = image://AppHomeScreen/general/co_bg_popup_mid_m.png
co_bg_popup_mid_t = image://AppHomeScreen/general/co_bg_popup_mid_t.png
co_bg_popup_mini_b = image://AppHomeScreen/general/co_bg_popup_mini_b.png
co_bg_popup_mini_m = image://AppHomeScreen/general/co_bg_popup_mini_m.png
co_bg_popup_mini_t = image://AppHomeScreen/general/co_bg_popup_mini_t.png
co_btn_bg_list_l_f = image://AppHomeScreen/general/co_btn_bg_list_l_f.png
co_btn_bg_list_l_p = image://AppHomeScreen/general/co_btn_bg_list_l_p.png
co_btn_bg_list_l_s = image://AppHomeScreen/general/co_btn_bg_list_l_s.png
co_btn_bg_list_m_f = image://AppHomeScreen/general/co_btn_bg_list_m_f.png
co_btn_bg_list_m_p = image://AppHomeScreen/general/co_btn_bg_list_m_p.png
co_btn_bg_list_m_s = image://AppHomeScreen/general/co_btn_bg_list_m_s.png
co_btn_bg_list_r_f = image://AppHomeScreen/general/co_btn_bg_list_r_f.png
co_btn_bg_list_r_p = image://AppHomeScreen/general/co_btn_bg_list_r_p.png
co_btn_bg_list_r_s = image://AppHomeScreen/general/co_btn_bg_list_r_s.png
co_bg_popup_title = image://AppHomeScreen/general/co_bg_popup_title.png
co_bg_list_line_t1 = image://AppHomeScreen/general/co_bg_list_line_t1.png
co_bg_list_line = image://AppHomeScreen/general/co_bg_list_line.png
co_bg_list_s_line = image://AppHomeScreen/general/co_bg_list_s_line.png
co_bg_list_line_c = image://AppHomeScreen/general/co_bg_list_line_c.png
co_btn_bg_list_t1_108_p = image://AppHomeScreen/general/co_btn_bg_list_t1_108_p.png
co_btn_bg_list_t1_108_s = image://AppHomeScreen/general/co_btn_bg_list_t1_108_s.png
co_btn_bg_list_t1_108_sf = image://AppHomeScreen/general/co_btn_bg_list_t1_108_sf.png
co_btn_bg_list_t1_147_p = image://AppHomeScreen/general/co_btn_bg_list_t1_147_p.png
co_btn_bg_list_t1_147_s = image://AppHomeScreen/general/co_btn_bg_list_t1_147_s.png
co_btn_bg_list_t1_147_sf = image://AppHomeScreen/general/co_btn_bg_list_t1_147_sf.png
co_btn_bg_list_t1_186_p = image://AppHomeScreen/general/co_btn_bg_list_t1_186_p.png
co_btn_bg_list_t1_186_s = image://AppHomeScreen/general/co_btn_bg_list_t1_186_s.png
co_btn_bg_list_t1_186_sf = image://AppHomeScreen/general/co_btn_bg_list_t1_186_sf.png
clm_btn_led_02_on = image://AppHomeScreen/climate/clm_btn_led_02_on.png
clm_btn_led_02_off = image://AppHomeScreen/climate/clm_btn_led_02_off.png
clm_btn_bottom_ = image://AppHomeScreen/climate/clm_btn_bottom_
co_btn_bottom_l_f = image://AppHomeScreen/general/co_btn_bottom_l_f.png
co_btn_bottom_l_n = image://AppHomeScreen/general/co_btn_bottom_l_n.png
co_btn_bottom_l_p = image://AppHomeScreen/general/co_btn_bottom_l_p.png
co_btn_bottom_l_s = image://AppHomeScreen/general/co_btn_bottom_l_s.png
co_btn_bottom_l_d = image://AppHomeScreen/general/co_btn_bottom_l_d.png
co_btn_bottom_m_f = image://AppHomeScreen/general/co_btn_bottom_m_f.png
co_btn_bottom_m_n = image://AppHomeScreen/general/co_btn_bottom_m_n.png
co_btn_bottom_m_p = image://AppHomeScreen/general/co_btn_bottom_m_p.png
co_btn_bottom_m_s = image://AppHomeScreen/general/co_btn_bottom_m_s.png
co_btn_bottom_m_d = image://AppHomeScreen/general/co_btn_bottom_m_d.png
co_btn_bottom_r_f = image://AppHomeScreen/general/co_btn_bottom_r_f.png
co_btn_bottom_r_n = image://AppHomeScreen/general/co_btn_bottom_r_n.png
co_btn_bottom_r_p = image://AppHomeScreen/general/co_btn_bottom_r_p.png
co_btn_bottom_r_s = image://AppHomeScreen/general/co_btn_bottom_r_s.png
co_btn_bottom_r_d = image://AppHomeScreen/general/co_btn_bottom_r_d.png
clm_btn_top_l_n = image://AppHomeScreen/climate/clm_btn_top_l_n.png
clm_btn_top_l_p = image://AppHomeScreen/climate/clm_btn_top_l_p.png
clm_btn_top_l_s = image://AppHomeScreen/climate/clm_btn_top_l_s.png
clm_btn_top_l_d = image://AppHomeScreen/climate/clm_btn_top_l_d.png
clm_btn_top_m_n = image://AppHomeScreen/climate/clm_btn_top_m_n.png
clm_btn_top_m_p = image://AppHomeScreen/climate/clm_btn_top_m_p.png
clm_btn_top_m_s = image://AppHomeScreen/climate/clm_btn_top_m_s.png
clm_btn_top_m_d = image://AppHomeScreen/climate/clm_btn_top_m_d.png
clm_btn_top_r_n = image://AppHomeScreen/climate/clm_btn_top_r_n.png
clm_btn_top_r_p = image://AppHomeScreen/climate/clm_btn_top_r_p.png
clm_btn_top_r_s = image://AppHomeScreen/climate/clm_btn_top_r_s.png
clm_btn_top_r_d = image://AppHomeScreen/climate/clm_btn_top_r_d.png
clm_btn_top_105_n = image://AppHomeScreen/climate/clm_btn_top_105_n.png
clm_btn_top_105_f = image://AppHomeScreen/climate/clm_btn_top_105_f.png
clm_btn_top_105_p = image://AppHomeScreen/climate/clm_btn_top_105_p.png
clm_btn_top_105_d = image://AppHomeScreen/climate/clm_btn_top_105_d.png
clm_btn_top_105_s = image://AppHomeScreen/climate/clm_btn_top_105_s.png
co_btn_check_off_d = image://AppHomeScreen/general/co_btn_check_off_d.png
co_btn_check_off_p = image://AppHomeScreen/general/co_btn_check_off_p.png
co_btn_check_off_n = image://AppHomeScreen/general/co_btn_check_off_n.png
co_btn_check_on_p = image://AppHomeScreen/general/co_btn_check_on_p.png
co_btn_check_on_n = image://AppHomeScreen/general/co_btn_check_on_n.png
co_btn_title_l_f = image://AppHomeScreen/general/co_btn_title_l_f.png
co_btn_title_l_n = image://AppHomeScreen/general/co_btn_title_l_n.png
co_btn_title_l_p = image://AppHomeScreen/general/co_btn_title_l_p.png
co_btn_title_l_d = image://AppHomeScreen/general/co_btn_title_l_d.png
co_btn_title_m_f = image://AppHomeScreen/general/co_btn_title_m_f.png
co_btn_title_m_n = image://AppHomeScreen/general/co_btn_title_m_n.png
co_btn_title_m_p = image://AppHomeScreen/general/co_btn_title_m_p.png
co_btn_title_m_d = image://AppHomeScreen/general/co_btn_title_m_d.png
co_btn_title_r_f = image://AppHomeScreen/general/co_btn_title_r_f.png
co_btn_title_r_n = image://AppHomeScreen/general/co_btn_title_r_n.png
co_btn_title_r_p = image://AppHomeScreen/general/co_btn_title_r_p.png
co_btn_title_r_d = image://AppHomeScreen/general/co_btn_title_r_d.png
co_btn_bottom_283_n = image://AppHomeScreen/general/co_btn_bottom_283_n.png
co_btn_bottom_283_p = image://AppHomeScreen/general/co_btn_bottom_283_p.png
co_btn_bottom_283_d = image://AppHomeScreen/general/co_btn_bottom_283_d.png
co_btn_bottom_347_n = image://AppHomeScreen/general/co_btn_bottom_347_n.png
co_btn_bottom_347_p = image://AppHomeScreen/general/co_btn_bottom_347_p.png
co_btn_bottom_347_d = image://AppHomeScreen/general/co_btn_bottom_347_d.png
co_btn_bottom_386_n = image://AppHomeScreen/general/co_btn_bottom_386_n.png
co_btn_bottom_386_p = image://AppHomeScreen/general/co_btn_bottom_386_p.png
co_btn_bottom_386_s = image://AppHomeScreen/general/co_btn_bottom_386_s.png
co_btn_bottom_404_n = image://AppHomeScreen/general/co_btn_bottom_404_n.png
co_btn_bottom_404_p = image://AppHomeScreen/general/co_btn_bottom_404_p.png
co_btn_bottom_404_s = image://AppHomeScreen/general/co_btn_bottom_404_s.png
co_btn_bottom_430_n = image://AppHomeScreen/general/co_btn_bottom_430_n.png
co_btn_bottom_430_p = image://AppHomeScreen/general/co_btn_bottom_430_p.png
co_btn_bottom_430_d = image://AppHomeScreen/general/co_btn_bottom_430_d.png
co_btn_bottom_527_n = image://AppHomeScreen/general/co_btn_bottom_527_n.png
co_btn_bottom_527_p = image://AppHomeScreen/general/co_btn_bottom_527_p.png
co_btn_bottom_527_d = image://AppHomeScreen/general/co_btn_bottom_527_d.png
co_btn_bottom_612_n = image://AppHomeScreen/general/co_btn_bottom_612_n.png
co_btn_bottom_612_p = image://AppHomeScreen/general/co_btn_bottom_612_p.png
co_btn_bottom_612_s = image://AppHomeScreen/general/co_btn_bottom_612_s.png
co_btn_bottom_612_d = image://AppHomeScreen/general/co_btn_bottom_612_d.png
co_btn_bottom_620_n = image://AppHomeScreen/general/co_btn_bottom_620_n.png
co_btn_bottom_620_p = image://AppHomeScreen/general/co_btn_bottom_620_p.png
co_btn_bottom_948_n = image://AppHomeScreen/general/co_btn_bottom_948_n.png
co_btn_bottom_948_p = image://AppHomeScreen/general/co_btn_bottom_948_p.png
co_btn_bottom_948_s = image://AppHomeScreen/general/co_btn_bottom_948_s.png
co_btn_bottom_1236_n = image://AppHomeScreen/general/co_btn_bottom_1236_n.png
co_btn_bottom_1236_p = image://AppHomeScreen/general/co_btn_bottom_1236_p.png
co_btn_bottom_1236_s = image://AppHomeScreen/general/co_btn_bottom_1236_s.png
home_factorymode_btn_n = image://AppHomeScreen/general/home_factorymode_btn_n.png
home_factorymode_btn_p = image://AppHomeScreen/general/home_factorymode_btn_p.png
clm_btn_bottom_218_ = image://AppHomeScreen/climate/clm_btn_bottom_218_
co_menu_check_off_n = image://AppHomeScreen/general/co_menu_check_off_n.png
co_menu_check_on_n = image://AppHomeScreen/general/co_menu_check_on_n.png
co_ic_info = image://AppHomeScreen/general/co_ic_info.png
co_ic_question = image://AppHomeScreen/general/co_ic_question.png
ic_loading_01 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_01.png
ic_loading_02 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_02.png
ic_loading_03 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_03.png
ic_loading_04 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_04.png
ic_loading_05 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_05.png
ic_loading_06 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_06.png
ic_loading_07 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_07.png
ic_loading_08 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_08.png
ic_loading_09 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_09.png
ic_loading_10 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_10.png
ic_loading_11 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_11.png
ic_loading_12 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_12.png
ic_loading_13 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_13.png
ic_loading_14 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_14.png
ic_loading_15 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_15.png
ic_loading_16 = image://AppHomeScreen/setup/loading_s/set_ic_loading_s_16.png
enum_appIdBluelink = 2
enum_appIdMedia = 11
enum_appIdSetting = 4
enum_subMeneBlueLinkACN = 8
enum_subMeneBlueLinkApps = 3
enum_subMeneBlueLinkDrivingInfo = 0
enum_subMeneBlueLinkMaintenance = 6
enum_subMeneBlueLinkSOS = 7
enum_subMeneBlueLinkSendToCar = 1
enum_subMeneBlueLinkSetting = 4
enum_subMeneBlueLinkVehicleDiagnos = 2
enum_subMeneUvo_ACN = 8
enum_subMeneUvo_Concierge = 3
enum_subMeneUvo_DrivingInfo = 0
enum_subMeneUvo_Maintenance = 6
enum_subMeneUvo_SOS = 7
enum_subMeneUvo_SendToCar = 1
enum_subMeneUvo_SendToCar_Na = 5
enum_subMeneUvo_Setup = 4
enum_subMeneUvo_VehicleDiagnos = 2
enum_subMenuConcierge = 3
enum_subMenuDrivingInfo = 0
enum_subMenuMaintenance = 6
enum_subMenuMediaAUX = 8
enum_subMenuMediaAndroidAuto = 17
enum_subMenuMediaAppleCarplay = 16
enum_subMenuMediaBTAudio = 15
enum_subMenuMediaBTMusic = 7
enum_subMenuMediaDiscCD = 3
enum_subMenuMediaDiscDVD = 4
enum_subMenuMediaDiscMp3 = 18
enum_subMenuMediaIpod1 = 5
enum_subMenuMediaIpod2 = 13
enum_subMenuMediaMyMusic = 0
enum_subMenuMediaPandora = 6
enum_subMenuMediaPandoraBT = 14
enum_subMenuMediaUsbMusic = 1
enum_subMenuMediaUsbMusic2 = 10
enum_subMenuMediaUsbPhoto = 9
enum_subMenuMediaUsbPhoto2 = 12
enum_subMenuMediaUsbVideo = 2
enum_subMenuMediaUsbVideo2 = 11
enum_subMenuSendToCar = 1
enum_subMenuSettingBluetooth = 29
enum_subMenuSettingClock = 31
enum_subMenuSettingDisplay = 27
enum_subMenuSettingEmanual = 41
enum_subMenuSettingGeneral = 33
enum_subMenuSettingNavi = 28
enum_subMenuSettingSound = 26
enum_subMenuSettingSysInfo = 100
enum_subMenuSettingVr = 30
enum_subMenuSetup = 4
enum_subMenuSetupBluetooth = 3
enum_subMenuSetupClock = 5
enum_subMenuSetupConnectivity = 7
enum_subMenuSetupDisplay = 2
enum_subMenuSetupEManual = 8
enum_subMenuSetupGeneral = 6
enum_subMenuSetupHomeCustomize = 11
enum_subMenuSetupNavi = 0
enum_subMenuSetupSound = 1
enum_subMenuSetupSytemInfo = 9
enum_subMenuSetupVehicle = 12
enum_subMenuSetupVoice = 4
enum_subMenuSetupWifi = 10
enum_subMenuVehicleDiagnos = 2
fontBold = ?
fontColor = White
homeScr_2dep_list_mask = image://AppHomeScreen/homescreen/2dep_list_mask.png
homeScr_background = image://AppHomeScreen/general/bg_main_02.png
homeScr_subMenuBg2Dept = image://AppHomeScreen/homescreen/bg_2dep_glow.png
homeScr_subMenuListLine = image://AppHomeScreen/homescreen/2dep_line.png
home_allmenu_99_d = image://AppHomeScreen/home/99x99/home_allmenu_99_d.png
home_allmenu_99_n = image://AppHomeScreen/home/99x99/home_allmenu_99_n.png
home_bg_edit_f = image://AppHomeScreen/home/home_bg_edit_f.png
home_bg_edit_move = image://AppHomeScreen/home/home_bg_edit_move.png
home_bg_edit_n = image://AppHomeScreen/home/home_bg_edit_n.png
home_bg_edit_p = image://AppHomeScreen/home/home_bg_edit_p.png
home_bg_ad_edit_p = image://AppHomeScreen/home/home_bg_ad_edit_p.png
home_bg_ad_edit_n = image://AppHomeScreen/home/home_bg_ad_edit_n.png
home_bg_ad_edit_f = image://AppHomeScreen/home/home_bg_ad_edit_f.png
home_bg_ad_edit_move = image://AppHomeScreen/home/home_bg_ad_edit_move.png
home_bg_ad_edit_drag_f = image://AppHomeScreen/home/home_bg_ad_edit_drag_f.png
home_bg_ad_edit_8_p = image://AppHomeScreen/home/home_bg_ad_edit_8_p.png
home_bg_ad_edit_8_n = image://AppHomeScreen/home/home_bg_ad_edit_8_n.png
home_bg_ad_edit_8_f = image://AppHomeScreen/home/home_bg_ad_edit_8_f.png
home_bg_ad_edit_move_8 = image://AppHomeScreen/home/home_bg_ad_edit_move_8.png
home_bg_horizontal_arrow_l = image://AppHomeScreen/home/home_bg_horizontal_arrow_l.png
home_bg_horizontal_arrow_r = image://AppHomeScreen/home/home_bg_horizontal_arrow_r.png
home_bg_horizontal = image://AppHomeScreen/home/home_bg_horizontal.png
home_bg_horizontal_c = image://AppHomeScreen/home/home_bg_horizontal_c.png
home_bg_horizontal_2_highlight = image://AppHomeScreen/home/home_bg_horizontal_2_highlight.png
home_bg_horizontal_3_highlight_1280 = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_1280.png
home_bg_icon_edit_p = image://AppHomeScreen/home/home_bg_icon_edit_p.png
home_bg_icon_edit_n = image://AppHomeScreen/home/home_bg_icon_edit_n.png
home_bg_icon_edit_s_p = image://AppHomeScreen/home/home_bg_icon_edit_s_p.png
home_bg_icon_edit_s_n = image://AppHomeScreen/home/home_bg_icon_edit_s_n.png
home_bg_icon_main_p = image://AppHomeScreen/media/me_bg_m_selected.png
home_bg_horizontal_3_highlight_l_1280 = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_l_1280.png
home_bg_horizontal_3_highlight_l_s = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_l_s.png
home_bg_horizontal_3_highlight_l = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_l.png
home_bg_horizontal_3_highlight_r_s = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_r_s.png
home_bg_horizontal_3_highlight_r_1280 = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_r_1280.png
home_bg_horizontal_3_highlight_r = image://AppHomeScreen/home/home_bg_horizontal_3_highlight_r.png
home_bg_horizontal_highlight_r = image://AppHomeScreen/home/home_bg_horizontal_highlight_r.png
home_bg_horizontal_highlight_r_1280 = image://AppHomeScreen/home/home_bg_horizontal_highlight_r_1280.png
home_bg_horizontal_highlight_l = image://AppHomeScreen/home/home_bg_horizontal_highlight_l.png
home_bg_horizontal_highlight_l_1280 = image://AppHomeScreen/home/home_bg_horizontal_highlight_l_1280.png
me_bg_m_n = image://AppHomeScreen/media/me_bg_m_n.png
me_bg_m_p = image://AppHomeScreen/media/me_bg_m_p.png
me_bg_m_n_s = image://AppHomeScreen/media/me_bg_m_n_s.png
me_bg_m_p_s = image://AppHomeScreen/media/me_bg_m_p_s.png
home_bg_icon_p = image://AppHomeScreen/home/home_bg_icon_p.png
home_bg_icon_n = image://AppHomeScreen/home/home_bg_icon_n.png
home_bg_map = image://AppHomeScreen/home/home_bg_map.png
home_bg = image://AppHomeScreen/home/home_bg.png
home_profile_p = image://AppHomeScreen/home/home_profile_p.png
home_profile_mask = image://AppHomeScreen/home/home_profile_mask.png
home_map_bg = image://AppHomeScreen/home/home_map_bg.png
home_map_bg_02 = image://AppHomeScreen/home/home_map_bg_02.png
home_nomap_bg = image://AppHomeScreen/home/home_nomap_bg.png
home_nomap_bg_02 = image://AppHomeScreen/home/home_nomap_bg_02.png
me_bg_player_default_s = image://AppHomeScreen/media/me_bg_player_default_s.png
me_bg_player_default_w = image://AppHomeScreen/media/me_bg_player_default_w.png
home_main_bg_01 = image://AppHomeScreen/general/home_main_bg_01.png
home_main_bg_02 = image://AppHomeScreen/general/home_main_bg_02.png
co_bg_statusbar = image://AppHomeScreen/general/co_bg_statusbar.png
home_ic_ad_edit_arrow = image://AppHomeScreen/home/home_ic_ad_edit_arrow.png
home_ic_edit_map = image://AppHomeScreen/home/home_ic_edit_map.png
home_ic_edit_climate = image://AppHomeScreen/home/home_ic_edit_climate.png
home_ic_edit_clock = image://AppHomeScreen/home/home_ic_edit_clock.png
home_ic_edit_analogue = image://AppHomeScreen/home/home_ic_edit_analogue.png
home_ic_edit_analogue_01 = image://AppHomeScreen/home/home_ic_edit_analogue_01.png
home_ic_edit_analogue_02 = image://AppHomeScreen/home/home_ic_edit_analogue_02.png
home_ic_edit_analogue_03 = image://AppHomeScreen/home/home_ic_edit_analogue_03.png
home_ic_edit_analogue_04 = image://AppHomeScreen/home/home_ic_edit_analogue_04.png
home_ic_edit_analogue_05 = image://AppHomeScreen/home/home_ic_edit_analogue_05.png
home_ic_edit_analogue_06 = image://AppHomeScreen/home/home_ic_edit_analogue_06.png
home_ic_edit_analogue_07 = image://AppHomeScreen/home/home_ic_edit_analogue_07.png
home_ic_edit_analogue_08 = image://AppHomeScreen/home/home_ic_edit_analogue_08.png
home_ic_edit_driving = image://AppHomeScreen/home/home_ic_edit_driving.png
home_ic_edit_ev = image://AppHomeScreen/home/home_ic_edit_ev.png
home_ic_edit_music = image://AppHomeScreen/home/home_ic_edit_music.png
home_ic_edit_nmode = image://AppHomeScreen/home/home_ic_edit_nmode.png
home_ic_edit_speed = image://AppHomeScreen/home/home_ic_edit_speed.png
home_ic_edit_rear = image://AppHomeScreen/home/home_ic_edit_rear.png
home_ic_edit_sports = image://AppHomeScreen/home/home_ic_edit_sports.png
home_ic_edit_trip = image://AppHomeScreen/home/home_ic_edit_trip.png
home_ic_edit_weather = image://AppHomeScreen/home/home_ic_edit_weather.png
home_ic_indicator_n = image://AppHomeScreen/home/home_ic_indicator_n.png
home_ic_indicator_s = image://AppHomeScreen/home/home_ic_indicator_s.png
home_ic_indicator_w_n = image://AppHomeScreen/home/home_ic_indicator_w_n.png
home_ic_indicator_w_s = image://AppHomeScreen/home/home_ic_indicator_w_s.png
home_ic_settings = image://AppHomeScreen/home/home_ic_settings.png
home_ic_search = image://AppHomeScreen/phone/bt_ic_search_n.png
home_ic_information = image://AppHomeScreen/home/home_ic_information.png
home_ic_live = image://AppHomeScreen/home/home_ic_live.png
home_ic_drivinfo = image://AppHomeScreen/home/home_ic_drivinfo.png
home_99_shadow = image://AppHomeScreen/home/home_99_shadow.png
tmu_ic_update_n = image://AppHomeScreen/tmu/tmu_ic_update_n.png
tmu_ic_update_d = image://AppHomeScreen/tmu/tmu_ic_update_d.png
tmu_ic_drivinginfo = image://AppHomeScreen/tmu/tmu_ic_drivinginfo.png
tmu_bg_driving_01 = image://AppHomeScreen/tmu/tmu_bg_driving_01.png
tmu_bg_driving_02 = image://AppHomeScreen/tmu/tmu_bg_driving_02.png
tmu_etc_progress_b_l = image://AppHomeScreen/tmu/tmu_etc_progress_01_l.png
tmu_etc_progress_b_m = image://AppHomeScreen/tmu/tmu_etc_progress_01_m.png
tmu_etc_progress_b_r = image://AppHomeScreen/tmu/tmu_etc_progress_01_r.png
tmu_etc_progress_b_r2 = image://AppHomeScreen/tmu/tmu_etc_progress_01_r2.png
tmu_etc_progress_r_l = image://AppHomeScreen/tmu/tmu_etc_progress_02_l.png
tmu_etc_progress_r_l2 = image://AppHomeScreen/tmu/tmu_etc_progress_02_l2.png
tmu_etc_progress_r_m = image://AppHomeScreen/tmu/tmu_etc_progress_02_m.png
tmu_etc_progress_r_r = image://AppHomeScreen/tmu/tmu_etc_progress_02_r.png
tmu_bg_graph_01 = image://AppHomeScreen/tmu/tmu_bg_graph_01.png
tmu_bg_graph_02 = image://AppHomeScreen/tmu/tmu_bg_graph_02.png
tmu_etc_graph_01_t = image://AppHomeScreen/tmu/tmu_etc_graph_01_t.png
tmu_etc_graph_01_m = image://AppHomeScreen/tmu/tmu_etc_graph_01_m.png
tmu_etc_graph_01_b = image://AppHomeScreen/tmu/tmu_etc_graph_01_b.png
tmu_etc_graph_02_t = image://AppHomeScreen/tmu/tmu_etc_graph_02_t.png
tmu_etc_graph_02_m = image://AppHomeScreen/tmu/tmu_etc_graph_02_m.png
tmu_etc_graph_02_b = image://AppHomeScreen/tmu/tmu_etc_graph_02_b.png
tmu_etc_graph_03_t = image://AppHomeScreen/tmu/tmu_etc_graph_03_t.png
tmu_etc_graph_03_m = image://AppHomeScreen/tmu/tmu_etc_graph_03_m.png
tmu_etc_graph_03_b = image://AppHomeScreen/tmu/tmu_etc_graph_03_b.png
tmu_etc_graph_04_t = image://AppHomeScreen/tmu/tmu_etc_graph_04_t.png
tmu_etc_graph_04_m = image://AppHomeScreen/tmu/tmu_etc_graph_04_m.png
tmu_etc_graph_04_b = image://AppHomeScreen/tmu/tmu_etc_graph_04_b.png
tmu_etc_graph_05_t = image://AppHomeScreen/tmu/tmu_etc_graph_05_t.png
tmu_etc_graph_05_m = image://AppHomeScreen/tmu/tmu_etc_graph_05_m.png
tmu_etc_graph_05_b = image://AppHomeScreen/tmu/tmu_etc_graph_05_b.png
tmu_etc_graph_06_t = image://AppHomeScreen/tmu/tmu_etc_graph_06_t.png
tmu_etc_graph_06_m = image://AppHomeScreen/tmu/tmu_etc_graph_06_m.png
tmu_etc_graph_06_b = image://AppHomeScreen/tmu/tmu_etc_graph_06_b.png
icoPrefix = image://AppHomeScreen/home/
icoPrefix72 = image://AppHomeScreen/home/72x72/
icoPrefix99 = image://AppHomeScreen/home/99x99/
ico_check_d = image://AppHomeScreen/general/co_btn_check_off_d.png
ico_check_n = image://AppHomeScreen/general/co_btn_check_off_n.png
ico_check_s = image://AppHomeScreen/general/co_btn_check_on_n.png
ico_check_s_d = image://AppHomeScreen/general/co_btn_check_on_d.png
ico_home_data_service_s = image://AppHomeScreen/homescreen/ico_home_data_service_s.png
ico_radio_n = image://AppHomeScreen/general/ico_radio_n.png
ico_radio_s = image://AppHomeScreen/general/ico_radio_s.png
iconD = _d.png
iconF = _f.png
iconN = _n.png
iconP = _p.png
set_bg_web_img = image://AppHomeScreen/setup/set_bg_web_img.png
set_bg_delete_img = image://AppHomeScreen/phone/set_bg_delete_img.png
bt_img_bg = image://AppHomeScreen/general/bt_img_bg.png
co_bg_osd_org_t = image://AppHomeScreen/general/co_bg_osd_org_t.png
co_bg_osd_org_m = image://AppHomeScreen/general/co_bg_osd_org_m.png
co_bg_osd_org_b = image://AppHomeScreen/general/co_bg_osd_org_b.png
nm_etc_edit_normal = image://AppHomeScreen/{nmode|performance}/nm_etc_edit_normal.png
nm_etc_edit_racing = image://AppHomeScreen/{nmode|performance}/nm_etc_edit_racing.png
nm_etc_edit_normal_c = image://AppHomeScreen/{nmode|performance}/nm_etc_edit_normal_c.png
nm_bg_select_type_n_l = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_n_l.png
nm_bg_select_type_n_m = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_n_m.png
nm_bg_select_type_n_r = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_n_r.png
nm_bg_select_type_p_l = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_p_l.png
nm_bg_select_type_p_m = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_p_m.png
nm_bg_select_type_p_r = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_p_r.png
nm_bg_select_type_s_l = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_s_l.png
nm_bg_select_type_s_m = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_s_m.png
nm_bg_select_type_s_r = image://AppHomeScreen/{nmode|performance}/nm_bg_select_type_s_r.png
line_menu_list = image://AppHomeScreen/general/line_menu_list.png
line_optionmenu = image://AppHomeScreen/general/line_optionmenu.png
list_f = image://AppHomeScreen/general/list_f.png
list_line = image://AppHomeScreen/general/co_bg_list_line_m.png
list_p = image://AppHomeScreen/general/list_p.png
me_bg_mediagate = image://AppHomeScreen/media/me_bg_mediagate.png
me_bg_mediagate_c = image://AppHomeScreen/media/me_bg_mediagate_c.png
me_bg_mediagate_edit = image://AppHomeScreen/media/me_bg_mediagate_edit.png
me_bg_mediagate_edit_c = image://AppHomeScreen/media/me_bg_mediagate_edit_c.png
qui_btn_menu02_l_n = image://AppHomeScreen/quick/qui_btn_menu02_l_n.png
qui_btn_menu02_l_s = image://AppHomeScreen/quick/qui_btn_menu02_l_s.png
qui_btn_menu02_m_n = image://AppHomeScreen/quick/qui_btn_menu02_m_n.png
qui_btn_menu02_m_s = image://AppHomeScreen/quick/qui_btn_menu02_m_s.png
qui_btn_menu02_r_n = image://AppHomeScreen/quick/qui_btn_menu02_r_n.png
qui_btn_menu02_r_s = image://AppHomeScreen/quick/qui_btn_menu02_r_s.png
qui_btn_menu_l_n = image://AppHomeScreen/quick/qui_btn_menu_l_n.png
qui_btn_menu_l_s = image://AppHomeScreen/quick/qui_btn_menu_l_s.png
qui_btn_menu_m_n = image://AppHomeScreen/quick/qui_btn_menu_m_n.png
qui_btn_menu_m_s = image://AppHomeScreen/quick/qui_btn_menu_m_s.png
qui_btn_menu_r_n = image://AppHomeScreen/quick/qui_btn_menu_r_n.png
qui_btn_menu_r_s = image://AppHomeScreen/quick/qui_btn_menu_r_s.png
re_btn_bar_d = image://AppHomeScreen/rear/re_btn_bar_d.png
re_btn_bar_n = image://AppHomeScreen/rear/re_btn_bar_n.png
re_btn_bar_p = image://AppHomeScreen/rear/re_btn_bar_p.png
re_btn_bg_h486_l_f = image://AppHomeScreen/rear/re_btn_bg_h486_l_f.png
re_btn_bg_h486_l_n = image://AppHomeScreen/rear/re_btn_bg_h486_l_n.png
re_btn_bg_h486_l_p = image://AppHomeScreen/rear/re_btn_bg_h486_l_p.png
re_btn_bg_h486_m_f = image://AppHomeScreen/rear/re_btn_bg_h486_m_f.png
re_btn_bg_h486_m_n = image://AppHomeScreen/rear/re_btn_bg_h486_m_n.png
re_btn_bg_h486_m_p = image://AppHomeScreen/rear/re_btn_bg_h486_m_p.png
re_btn_bg_h486_r_f = image://AppHomeScreen/rear/re_btn_bg_h486_r_f.png
re_btn_bg_h486_r_n = image://AppHomeScreen/rear/re_btn_bg_h486_r_n.png
re_btn_bg_h486_r_p = image://AppHomeScreen/rear/re_btn_bg_h486_r_p.png
re_btn_line_split_2btn = image://AppHomeScreen/rear/re_btn_line_split_2btn.png
re_btn_line_split_3btn = image://AppHomeScreen/rear/re_btn_line_split_3btn.png
re_btn_line_wide_2btn = image://AppHomeScreen/rear/re_btn_line_wide_2btn.png
re_btn_line_wide_3btn = image://AppHomeScreen/rear/re_btn_line_wide_3btn.png
re_btn_minus_d = image://AppHomeScreen/rear/re_btn_minus_d.png
re_btn_minus_f = image://AppHomeScreen/rear/re_btn_minus_f.png
re_btn_minus_n = image://AppHomeScreen/rear/re_btn_minus_n.png
re_btn_minus_p = image://AppHomeScreen/rear/re_btn_minus_p.png
re_btn_plus_d = image://AppHomeScreen/rear/re_btn_plus_d.png
re_btn_plus_f = image://AppHomeScreen/rear/re_btn_plus_f.png
re_btn_plus_n = image://AppHomeScreen/rear/re_btn_plus_n.png
re_btn_plus_p = image://AppHomeScreen/rear/re_btn_plus_p.png
re_etc_bar_bubble = image://AppHomeScreen/rear/re_etc_bar_bubble.png
re_etc_progress_bar = image://AppHomeScreen/rear/re_etc_progress_bar.png
re_etc_progress_bg = image://AppHomeScreen/rear/re_etc_progress_bg.png
re_ic_climate = image://AppHomeScreen/rear/re_ic_climate.png
re_ic_quiet = image://AppHomeScreen/rear/re_ic_quiet.png
re_ic_talk = image://AppHomeScreen/rear/re_ic_talk.png
re_ic_title_rear = image://AppHomeScreen/rear/re_ic_title_rear.png
re_ic_title_reartalk = image://AppHomeScreen/rear/re_ic_title_reartalk.png
re_ic_talk_default = image://AppHomeScreen/rear/re_ic_talk_default.png
split_ic_message = image://AppHomeScreen/split/split_ic_message.png
rear_ic_message = image://AppHomeScreen/rear/rear_ic_message.png
re_ic_title_quietmode = image://AppHomeScreen/rear/re_ic_title_quietmode.png
re_ic_quiet_mode = image://AppHomeScreen/rear/re_ic_quiet.png
re_ic_voice = image://AppHomeScreen/rear/re_ic_voice.png
re_led_off = image://AppHomeScreen/rear/re_led_off.png
re_led_on = image://AppHomeScreen/rear/re_led_on.png
bubble02_ic_img = image://AppHomeScreen/rear/bubble02_ic_img.png
bubble01_ic_img = image://AppHomeScreen/home/bubble01_ic_img.png
bubble01_sq_01 = image://AppHomeScreen/home/bubble01/bubble01_sq_01.png
bubble01_sq_02 = image://AppHomeScreen/home/bubble01/bubble01_sq_02.png
bubble01_sq_03 = image://AppHomeScreen/home/bubble01/bubble01_sq_03.png
bubble01_sq_04 = image://AppHomeScreen/home/bubble01/bubble01_sq_04.png
bubble01_sq_05 = image://AppHomeScreen/home/bubble01/bubble01_sq_05.png
bubble01_sq_06 = image://AppHomeScreen/home/bubble01/bubble01_sq_06.png
bubble01_sq_07 = image://AppHomeScreen/home/bubble01/bubble01_sq_07.png
bubble01_sq_08 = image://AppHomeScreen/home/bubble01/bubble01_sq_08.png
bubble01_sq_09 = image://AppHomeScreen/home/bubble01/bubble01_sq_09.png
bubble01_sq_10 = image://AppHomeScreen/home/bubble01/bubble01_sq_10.png
bubble01_sq_11 = image://AppHomeScreen/home/bubble01/bubble01_sq_11.png
bubble01_sq_12 = image://AppHomeScreen/home/bubble01/bubble01_sq_12.png
bubble01_sq_13 = image://AppHomeScreen/home/bubble01/bubble01_sq_13.png
bubble01_sq_14 = image://AppHomeScreen/home/bubble01/bubble01_sq_14.png
bubble01_sq_15 = image://AppHomeScreen/home/bubble01/bubble01_sq_15.png
bubble01_sq_16 = image://AppHomeScreen/home/bubble01/bubble01_sq_16.png
bubble01_sq_17 = image://AppHomeScreen/home/bubble01/bubble01_sq_17.png
bubble01_sq_18 = image://AppHomeScreen/home/bubble01/bubble01_sq_18.png
bubble01_sq_19 = image://AppHomeScreen/home/bubble01/bubble01_sq_19.png
bubble01_sq_20 = image://AppHomeScreen/home/bubble01/bubble01_sq_20.png
bubble01_sq_21 = image://AppHomeScreen/home/bubble01/bubble01_sq_21.png
bubble01_sq_22 = image://AppHomeScreen/home/bubble01/bubble01_sq_22.png
bubble01_sq_23 = image://AppHomeScreen/home/bubble01/bubble01_sq_23.png
bubble01_sq_24 = image://AppHomeScreen/home/bubble01/bubble01_sq_24.png
bubble01_sq_25 = image://AppHomeScreen/home/bubble01/bubble01_sq_25.png
bubble01_sq_26 = image://AppHomeScreen/home/bubble01/bubble01_sq_26.png
bubble01_sq_27 = image://AppHomeScreen/home/bubble01/bubble01_sq_27.png
bubble01_sq_28 = image://AppHomeScreen/home/bubble01/bubble01_sq_28.png
bubble01_sq_29 = image://AppHomeScreen/home/bubble01/bubble01_sq_29.png
bubble01_sq_30 = image://AppHomeScreen/home/bubble01/bubble01_sq_30.png
bubble01_sq_31 = image://AppHomeScreen/home/bubble01/bubble01_sq_31.png
bubble01_sq_32 = image://AppHomeScreen/home/bubble01/bubble01_sq_32.png
bubble01_sq_33 = image://AppHomeScreen/home/bubble01/bubble01_sq_33.png
bubble01_sq_34 = image://AppHomeScreen/home/bubble01/bubble01_sq_34.png
bubble01_sq_35 = image://AppHomeScreen/home/bubble01/bubble01_sq_35.png
bubble01_sq_36 = image://AppHomeScreen/home/bubble01/bubble01_sq_36.png
bubble01_sq_37 = image://AppHomeScreen/home/bubble01/bubble01_sq_37.png
bubble01_sq_38 = image://AppHomeScreen/home/bubble01/bubble01_sq_38.png
bubble01_sq_39 = image://AppHomeScreen/home/bubble01/bubble01_sq_39.png
bubble01_sq_40 = image://AppHomeScreen/home/bubble01/bubble01_sq_40.png
bubble01_sq = ?
ic_loading = ?
ic_loading_l = ?
scroll_b = image://AppHomeScreen/general/co_bg_drop_scroll_bar_b.png
scroll_edit_bg = image://AppHomeScreen/general/scroll_edit_bg.png
scroll_m = image://AppHomeScreen/general/co_bg_drop_scroll_bar_m.png
scroll_menu = image://AppHomeScreen/general/scroll_menu.png
scroll_menu_bg = image://AppHomeScreen/general/scroll_menu_bg.png
scroll_menu_list_bg = image://AppHomeScreen/general/co_bg_drop_scroll.png
scroll_option_bg = image://AppHomeScreen/general/scroll_option_bg.png
scroll_t = image://AppHomeScreen/general/co_bg_drop_scroll_bar_t.png
second_dep_line = image://AppHomeScreen/homescreen/2dep_line.png
second_dep_list_arrow_down = image://AppHomeScreen/homescreen/2dep_list_arrow_down.png
second_dep_list_arrow_up = image://AppHomeScreen/homescreen/2dep_list_arrow_up.png
second_dep_list_f = image://AppHomeScreen/homescreen/2dep_list_f.png
second_dep_top_mask = image://AppHomeScreen/homescreen/2dep_top_mask.png
set_ic_menu_d = image://AppHomeScreen/setup/set_ic_menu_d.png
set_ic_menu_n = image://AppHomeScreen/setup/set_ic_menu_n.png
split_btn_on_n = image://AppHomeScreen/split/split_btn_on_n.png
split_btn_on_p = image://AppHomeScreen/split/split_btn_on_p.png
split_btn_on_d = image://AppHomeScreen/split/split_btn_on_d.png
split_btn_off_n = image://AppHomeScreen/split/split_btn_off_n.png
split_btn_off_p = image://AppHomeScreen/split/split_btn_off_p.png
split_btn_off_d = image://AppHomeScreen/split/split_btn_off_d.png
tab_list_f = image://AppHomeScreen/general/tab_list_f.png
tab_list_p = image://AppHomeScreen/general/tab_list_p.png
title = image://AppHomeScreen/general/bg_title.png
widget_bg_widget_p = image://AppHomeScreen/home/widget_bg_widget_p.png
widget_bg_widget_n = image://AppHomeScreen/home/widget_bg_widget_n.png
widget_bg_widget_media_p = image://AppHomeScreen/home/widget_bg_widget_media_p.png
widget_bg_map_widget_p = image://AppHomeScreen/home/widget_bg_map_widget_p.png
widget_bg_ad_widget_n = image://AppHomeScreen/home/widget_bg_ad_widget_n.png
widget_bg_ad_widget_p = image://AppHomeScreen/home/widget_bg_ad_widget_p.png
n_mode_bg = ?
n_mode_bg_000 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_000.png
n_mode_bg_001 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_001.png
n_mode_bg_002 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_002.png
n_mode_bg_003 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_003.png
n_mode_bg_004 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_004.png
n_mode_bg_005 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_005.png
n_mode_bg_006 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_006.png
n_mode_bg_007 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_007.png
n_mode_bg_008 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_008.png
n_mode_bg_009 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_009.png
n_mode_bg_010 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_010.png
n_mode_bg_011 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_011.png
n_mode_bg_012 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_012.png
n_mode_bg_013 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_013.png
n_mode_bg_014 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_014.png
n_mode_bg_015 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_015.png
n_mode_bg_016 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_016.png
n_mode_bg_017 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_017.png
n_mode_bg_018 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_018.png
n_mode_bg_019 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_019.png
n_mode_bg_020 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_020.png
n_mode_bg_021 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_021.png
n_mode_bg_022 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_022.png
n_mode_bg_023 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_023.png
n_mode_bg_024 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_024.png
n_mode_bg_025 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_025.png
n_mode_bg_026 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_026.png
n_mode_bg_027 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_027.png
n_mode_bg_028 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_028.png
n_mode_bg_029 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_029.png
n_mode_bg_030 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_030.png
n_mode_bg_031 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_031.png
n_mode_bg_032 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_032.png
n_mode_bg_033 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_033.png
n_mode_bg_034 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_034.png
n_mode_bg_035 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_035.png
n_mode_bg_036 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_036.png
n_mode_bg_037 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_037.png
n_mode_bg_038 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_038.png
n_mode_bg_039 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_039.png
n_mode_bg_040 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_040.png
n_mode_bg_041 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_041.png
n_mode_bg_042 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_042.png
n_mode_bg_043 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_043.png
n_mode_bg_044 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_044.png
n_mode_bg_045 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_045.png
n_mode_bg_046 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_046.png
n_mode_bg_047 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_047.png
n_mode_bg_048 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_048.png
n_mode_bg_049 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_049.png
n_mode_bg_050 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_050.png
n_mode_bg_051 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_051.png
n_mode_bg_052 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_052.png
n_mode_bg_053 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_053.png
n_mode_bg_054 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_054.png
n_mode_bg_055 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_055.png
n_mode_bg_056 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_056.png
n_mode_bg_057 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_057.png
n_mode_bg_058 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_058.png
n_mode_bg_059 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_059.png
n_mode_bg_060 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_060.png
n_mode_bg_061 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_061.png
n_mode_bg_062 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_062.png
n_mode_bg_063 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_063.png
n_mode_bg_064 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_064.png
n_mode_bg_065 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_065.png
n_mode_bg_066 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_066.png
n_mode_bg_067 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_067.png
n_mode_bg_068 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_068.png
n_mode_bg_069 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_069.png
n_mode_bg_070 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_070.png
n_mode_bg_071 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_071.png
n_mode_bg_072 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_072.png
n_mode_bg_073 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_073.png
n_mode_bg_074 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_074.png
n_mode_bg_075 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_075.png
n_mode_bg_076 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_076.png
n_mode_bg_077 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_077.png
n_mode_bg_078 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_078.png
n_mode_bg_079 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_079.png
n_mode_bg_080 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_080.png
n_mode_bg_081 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_081.png
n_mode_bg_082 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_082.png
n_mode_bg_083 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_083.png
n_mode_bg_084 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_084.png
n_mode_bg_085 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_085.png
n_mode_bg_086 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_086.png
n_mode_bg_087 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_087.png
n_mode_bg_088 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_088.png
n_mode_bg_089 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_089.png
n_mode_bg_090 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_090.png
n_mode_bg_091 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_091.png
n_mode_bg_092 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_092.png
n_mode_bg_093 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_093.png
n_mode_bg_094 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_094.png
n_mode_bg_095 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_095.png
n_mode_bg_096 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_096.png
n_mode_bg_097 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_097.png
n_mode_bg_098 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_098.png
n_mode_bg_099 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_099.png
n_mode_bg_100 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_100.png
n_mode_bg_101 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_101.png
n_mode_bg_102 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_102.png
n_mode_bg_103 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_103.png
n_mode_bg_104 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_104.png
n_mode_bg_105 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_105.png
n_mode_bg_106 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_106.png
n_mode_bg_107 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_107.png
n_mode_bg_108 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_108.png
n_mode_bg_109 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_109.png
n_mode_bg_110 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_110.png
n_mode_bg_111 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_111.png
n_mode_bg_112 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_112.png
n_mode_bg_113 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_113.png
n_mode_bg_114 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_114.png
n_mode_bg_115 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_115.png
n_mode_bg_116 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_116.png
n_mode_bg_117 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_117.png
n_mode_bg_118 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_118.png
n_mode_bg_119 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_119.png
n_mode_bg_120 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_120.png
n_mode_bg_121 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_121.png
n_mode_bg_122 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_122.png
n_mode_bg_123 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_123.png
n_mode_bg_124 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_124.png
n_mode_bg_125 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_125.png
n_mode_bg_126 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_126.png
n_mode_bg_127 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_127.png
n_mode_bg_128 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_128.png
n_mode_bg_129 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_129.png
n_mode_bg_130 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_130.png
n_mode_bg_131 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_131.png
n_mode_bg_132 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_132.png
n_mode_bg_133 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_133.png
n_mode_bg_134 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_134.png
n_mode_bg_135 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_135.png
n_mode_bg_136 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_136.png
n_mode_bg_137 = image://AppHomeScreen/{nmode|performance}/n_mode_bg_137.png
nm_bg_road_wide = image://AppHomeScreen/{nmode|performance}/nm_bg_road_wide.png
nm_bg_road_split = image://AppHomeScreen/{nmode|performance}/nm_bg_road_split.png
nm_bg_widget_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_01.png
nm_bg_widget_02 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_02.png
nm_bg_widget_03 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_03.png
nm_bg_widget_04 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_04.png
nm_bg_widget_05 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_05.png
nm_bg_widget_06 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_06.png
nm_bg_widget_07 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_07.png
nm_bg_widget_01_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_01_n.png
nm_bg_widget_01_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_01_p.png
nm_bg_widget_01_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_01_s.png
nm_bg_widget_02_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_02_n.png
nm_bg_widget_02_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_02_p.png
nm_bg_widget_02_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_02_s.png
nm_bg_widget_03_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_03_n.png
nm_bg_widget_03_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_03_p.png
nm_bg_widget_03_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_03_s.png
nm_bg_widget_04_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_04_n.png
nm_bg_widget_04_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_04_p.png
nm_bg_widget_04_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_04_s.png
nm_bg_widget_05_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_05_n.png
nm_bg_widget_05_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_05_p.png
nm_bg_widget_05_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_05_s.png
nm_bg_widget_06_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_06_n.png
nm_bg_widget_06_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_06_p.png
nm_bg_widget_06_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_06_s.png
nm_bg_widget_07_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_07_n.png
nm_bg_widget_07_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_07_p.png
nm_bg_widget_07_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_07_s.png
nm_bg_widget_08_n = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_08_n.png
nm_bg_widget_08_p = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_08_p.png
nm_bg_widget_08_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_08_s.png
nm_bg_widget_press_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_01.png
nm_bg_widget_press_02 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_02.png
nm_bg_widget_press_03 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_03.png
nm_bg_widget_press_04 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_04.png
nm_bg_widget_press_05 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_05.png
nm_bg_widget_press_06 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_06.png
nm_bg_widget_press_07 = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_press_07.png
nm_edit_widget_bg_test = image://AppHomeScreen/{nmode|performance}/nm_edit_widget_bg_test.png
nm_bg_edit_widget_brake_pressure = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_brake_pressure.png
nm_bg_edit_widget_engine_temp = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_engine_temp.png
nm_bg_edit_widget_oil_temp = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_oil_temp.png
nm_bg_edit_widget_power = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_power.png
nm_bg_edit_widget_speed = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_speed.png
nm_bg_edit_widget_throttle = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_throttle.png
nm_bg_edit_widget_torque = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_torque.png
nm_bg_edit_widget_turbo = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_widget_turbo.png
nm_ic_shiftlight_w = image://AppHomeScreen/{nmode|performance}/nm_ic_shiftlight_w.png
nm_ic_shiftlight_y = image://AppHomeScreen/{nmode|performance}/nm_ic_shiftlight_y.png
nm_ic_shiftlight_r = image://AppHomeScreen/{nmode|performance}/nm_ic_shiftlight_r.png
nm_ic_shiftlight_off = image://AppHomeScreen/{nmode|performance}/nm_ic_shiftlight_off.png
nm_widget_ic_shiftlight_s_w = image://AppHomeScreen/{nmode|performance}/nm_widget_ic_shiftlight_s_w.png
nm_widget_ic_shiftlight_s_y = image://AppHomeScreen/{nmode|performance}/nm_widget_ic_shiftlight_s_y.png
nm_widget_ic_shiftlight_s_r = image://AppHomeScreen/{nmode|performance}/nm_widget_ic_shiftlight_s_r.png
nm_widget_ic_shiftlight_s_off = image://AppHomeScreen/{nmode|performance}/nm_widget_ic_shiftlight_s_off.png
nm_widget_indicator_m_on = image://AppHomeScreen/{nmode|performance}/nm_widget_indicator_m_on.png
nm_widget_indicator_m_off = image://AppHomeScreen/{nmode|performance}/nm_widget_indicator_m_off.png
nm_widget_progress_bg = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bg.png
nm_widget_progress_line = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_line.png
nm_widget_progress_pointer = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_pointer.png
nm_widget_progress_seg = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_seg.png
nm_widget_progress_bar_ = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_
nm_widget_progress_bar_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_01.png
nm_widget_progress_bar_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_02.png
nm_widget_progress_bar_03 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_03.png
nm_widget_progress_bar_04 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_04.png
nm_widget_progress_bar_05 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_05.png
nm_widget_progress_bar_06 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_06.png
nm_widget_progress_bar_07 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_07.png
nm_widget_progress_bar_08 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_08.png
nm_widget_progress_bar_09 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_09.png
nm_widget_progress_bar_10 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_10.png
nm_widget_progress_bar_11 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_11.png
nm_widget_progress_bar_12 = image://AppHomeScreen/{nmode|performance}/nm_widget_progress_bar_12.png
nm_widget_etc_num_n = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_num_n
nm_widget_etc_num_w = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_num_w
nm_widget_etc_num_y = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_num_y
nm_widget_etc_num_r = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_num_r
nm_widget_btn_track_n = image://AppHomeScreen/{nmode|performance}/nm_widget_btn_track_n.png
nm_widget_btn_track_p = image://AppHomeScreen/{nmode|performance}/nm_widget_btn_track_p.png
nm_edit_etc_num_n = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_num_n
nm_edit_etc_num_w = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_num_w
nm_edit_etc_num_y = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_num_y
nm_edit_etc_num_r = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_num_r
nm_map_circuit_01 = image://AppHomeScreen/{nmode|performance}/nm_map_circuit_01.png
nm_map_circuit_02 = image://AppHomeScreen/{nmode|performance}/nm_map_circuit_02.png
nm_map_circuit_04 = image://AppHomeScreen/{nmode|performance}/nm_map_circuit_04.png
nm_ic_beep_01_n = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_01_n.png
nm_ic_beep_01_s = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_01_s.png
nm_ic_beep_01_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_01_off.png
nm_ic_beep_02_n = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_02_n.png
nm_ic_beep_02_s = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_02_s.png
nm_ic_beep_02_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_02_off.png
nm_ic_beep_03_n = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_03_n.png
nm_ic_beep_03_s = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_03_s.png
nm_ic_beep_03_on = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_03_on.png
nm_ic_beep_03_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_03_off.png
nm_ic_beep_04_n = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_04_n.png
nm_ic_beep_04_s = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_04_s.png
nm_ic_beep_05_on = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_05_on.png
nm_ic_beep_05_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_05_off.png
nm_etc_circuit_s_elsd = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_elsd.png
nm_etc_circuit_s_engine = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_engine.png
nm_etc_circuit_s_esc = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_esc.png
nm_etc_circuit_s_exhaustsound = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_exhaustsound.png
nm_etc_circuit_s_hud = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_hud.png
nm_etc_circuit_s_revmatching = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_revmatching.png
nm_etc_circuit_s_steering = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_steering.png
nm_etc_circuit_s_supension = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_supension.png
nm_etc_circuit_s_transmission = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_s_transmission.png
nm_etc_circuit_l_elsd = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_elsd.png
nm_etc_circuit_l_engine = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_engine.png
nm_etc_circuit_l_esc = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_esc.png
nm_etc_circuit_l_exhaustsound = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_exhaustsound.png
nm_etc_circuit_l_hud = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_hud.png
nm_etc_circuit_l_revmatching = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_revmatching.png
nm_etc_circuit_l_steering = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_steering.png
nm_etc_circuit_l_supension = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_supension.png
nm_etc_circuit_l_transmission = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_l_transmission.png
nm_etc_circuit_xl_supension_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_supension_01.png
nm_etc_circuit_xl_supension_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_supension_02.png
nm_etc_circuit_xl_supension_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_supension_03.png
nm_etc_circuit_xl_esc_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_esc_01.png
nm_etc_circuit_xl_esc_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_esc_02.png
nm_etc_circuit_xl_esc_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_esc_03.png
nm_etc_circuit_xl_steering_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_steering_01.png
nm_etc_circuit_xl_steering_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_steering_02.png
nm_etc_circuit_xl_steering_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_steering_03.png
nm_etc_circuit_xl_elsd_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_elsd_01.png
nm_etc_circuit_xl_elsd_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_elsd_02.png
nm_etc_circuit_xl_elsd_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_elsd_03.png
nm_etc_circuit_xl_revmatching_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_revmatching_01.png
nm_etc_circuit_xl_revmatching_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_revmatching_02.png
nm_etc_circuit_xl_revmatching_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_revmatching_03.png
nm_etc_circuit_xl_exhaustsound_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_exhaustsound_01.png
nm_etc_circuit_xl_exhaustsound_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_exhaustsound_02.png
nm_etc_circuit_xl_exhaustsound_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_exhaustsound_03.png
nm_etc_circuit_xl_transmission_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_transmission_01.png
nm_etc_circuit_xl_transmission_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_transmission_02.png
nm_etc_circuit_xl_transmission_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_transmission_03.png
nm_etc_circuit_xl_engine_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_engine_01.png
nm_etc_circuit_xl_engine_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_engine_02.png
nm_etc_circuit_xl_engine_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_engine_03.png
nm_etc_circuit_xl_hud_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_hud_01.png
nm_etc_circuit_xl_hud_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xl_hud_02.png
nm_etc_circuit_xxl_engine_01 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_01.png
nm_etc_circuit_xxl_engine_02 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_02.png
nm_etc_circuit_xxl_engine_03 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_03.png
nm_etc_circuit_xxl_engine_03_new = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_03_new.png
nm_etc_circuit_xxl_engine_04 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_04.png
nm_etc_circuit_xxl_engine_04_new = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_04_new.png
nm_etc_circuit_xxl_engine_05 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_05.png
nm_etc_circuit_xxl_engine_06 = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_xxl_engine_06.png
nm_etc_circuit_list_elsd = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_elsd.png
nm_etc_circuit_list_engine = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_engine.png
nm_etc_circuit_list_esc = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_esc.png
nm_etc_circuit_list_exhaustsound = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_exhaustsound.png
nm_etc_circuit_list_hud = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_hud.png
nm_etc_circuit_list_revmatching = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_revmatching.png
nm_etc_circuit_list_steering = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_steering.png
nm_etc_circuit_list_supension = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_supension.png
nm_etc_circuit_list_transmission = image://AppHomeScreen/{nmode|performance}/nm_etc_circuit_list_transmission.png
nm_edit_etc_progress_bg = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_progress_bg.png
nm_edit_etc_progress_line = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_progress_line.png
nm_edit_etc_progress_pointer = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_progress_pointer.png
nm_edit_etc_progress_seg = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_progress_seg.png
nm_edit_etc_progress_bar_ = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_progress_bar_
nm_widget_etc_progress_line_s_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_s_01.png
nm_widget_etc_progress_line_m_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_m_01.png
nm_widget_etc_progress_line_l_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_l_01.png
nm_widget_etc_progress_line_s_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_s_02.png
nm_widget_etc_progress_line_m_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_m_02.png
nm_widget_etc_progress_line_l_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_line_l_02.png
nm_widget_etc_progress_seg_s_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_s_01.png
nm_widget_etc_progress_seg_m_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_m_01.png
nm_widget_etc_progress_seg_l_01 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_l_01.png
nm_widget_etc_progress_seg_s_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_s_02.png
nm_widget_etc_progress_seg_m_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_m_02.png
nm_widget_etc_progress_seg_l_02 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_l_02.png
nm_widget_etc_progress_seg_s_03 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_s_03.png
nm_widget_etc_progress_seg_m_03 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_m_03.png
nm_widget_etc_progress_seg_l_03 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_seg_l_03.png
nm_widget_etc_progress_bar_s_type_01_10 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_bar_s_type_01_10.png
nm_widget_etc_progress_bar_m_type_01_10 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_bar_m_type_01_10.png
nm_widget_etc_progress_bar_m_type_03_08 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_bar_m_type_03_08.png
nm_widget_etc_progress_bar_l_type_01_10 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_bar_l_type_01_10.png
nm_widget_etc_progress_bar_l_type_02_10 = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_bar_l_type_02_10.png
nm_widget_etc_progress_s_bg = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_s_bg.png
nm_widget_etc_progress_m_bg = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_m_bg.png
nm_widget_etc_progress_l_bg = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_l_bg.png
nm_widget_etc_progress_pointer_s = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_pointer_s.png
nm_widget_etc_progress_pointer_m = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_pointer_m.png
nm_widget_etc_progress_pointer_l = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_progress_pointer_l.png
nm_edit_indicator_on = image://AppHomeScreen/{nmode|performance}/nm_edit_indicator_on.png
nm_edit_indicator_off = image://AppHomeScreen/{nmode|performance}/nm_edit_indicator_off.png
nm_edit_indicator_s_off = image://AppHomeScreen/{nmode|performance}/nm_edit_indicator_s_off.png
nm_etc_indicator_on = image://AppHomeScreen/{nmode|performance}/nm_etc_indicator_on.png
nm_etc_indicator_off = image://AppHomeScreen/{nmode|performance}/nm_etc_indicator_off.png
nm_etc_progress_bg = image://AppHomeScreen/{nmode|performance}/nm_etc_progress_bg.png
nm_etc_progress_line = image://AppHomeScreen/{nmode|performance}/nm_etc_progress_line.png
nm_etc_progress_pointer = image://AppHomeScreen/{nmode|performance}/nm_etc_progress_pointer.png
nm_etc_progress_seg = image://AppHomeScreen/{nmode|performance}/nm_etc_progress_seg.png
nm_etc_progress_bar_ = image://AppHomeScreen/{nmode|performance}/nm_etc_progress_bar_
nm_etc_num_n = image://AppHomeScreen/{nmode|performance}/nm_etc_num_n
nm_etc_num_w = image://AppHomeScreen/{nmode|performance}/nm_etc_num_w
nm_etc_num_y = image://AppHomeScreen/{nmode|performance}/nm_etc_num_y
nm_etc_num_r = image://AppHomeScreen/{nmode|performance}/nm_etc_num_r
nm_etc_Alphabet_n = image://AppHomeScreen/{nmode|performance}/nm_etc_Alphabet_n
nm_etc_Alphabet_w = image://AppHomeScreen/{nmode|performance}/nm_etc_Alphabet_w
nm_etc_Alphabet_y = image://AppHomeScreen/{nmode|performance}/nm_etc_Alphabet_y
nm_etc_Alphabet_r = image://AppHomeScreen/{nmode|performance}/nm_etc_Alphabet_r
nm_edit_etc_Alphabet_n = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_Alphabet_n
nm_edit_etc_Alphabet_w = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_Alphabet_w
nm_edit_etc_Alphabet_y = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_Alphabet_y
nm_edit_etc_Alphabet_r = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_Alphabet_r
nm_widget_etc_Alphabet_n = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_Alphabet_n
nm_widget_etc_Alphabet_w = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_Alphabet_w
nm_widget_etc_Alphabet_y = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_Alphabet_y
nm_widget_etc_Alphabet_r = image://AppHomeScreen/{nmode|performance}/nm_widget_etc_Alphabet_r
nm_widget_shiftlight_l = image://AppHomeScreen/{nmode|performance}/nm_widget_shiftlight_l
nm_widget_shiftlight_m = image://AppHomeScreen/{nmode|performance}/nm_widget_shiftlight_m
nm_widget_shiftlight_s = image://AppHomeScreen/{nmode|performance}/nm_widget_shiftlight_s
nm_btn_info_01_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_01_n.png
nm_btn_info_01_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_01_p.png
nm_btn_info_02_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_02_n.png
nm_btn_info_02_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_02_p.png
nm_btn_info_03_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_03_n.png
nm_btn_info_03_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_03_p.png
nm_btn_info_04_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_04_n.png
nm_btn_info_04_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_04_p.png
nm_btn_info_05_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_05_n.png
nm_btn_info_05_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_05_p.png
nm_btn_info_06_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_06_n.png
nm_btn_info_06_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_06_p.png
nm_btn_info_07_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_07_n.png
nm_btn_info_07_n_new = image://AppHomeScreen/{nmode|performance}/nm_btn_info_07_n_new.png
nm_btn_info_07_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_07_p.png
nm_btn_info_07_p_new = image://AppHomeScreen/{nmode|performance}/nm_btn_info_07_p_new.png
nm_btn_info_08_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_08_n.png
nm_btn_info_08_n_new = image://AppHomeScreen/{nmode|performance}/nm_btn_info_08_n_new.png
nm_btn_info_08_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_08_p.png
nm_btn_info_08_p_new = image://AppHomeScreen/{nmode|performance}/nm_btn_info_08_p_new.png
nm_btn_info_09_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_09_n.png
nm_btn_info_09_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_09_p.png
nm_btn_info_10_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_10_n.png
nm_btn_info_10_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_10_p.png
nm_btn_info_11_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_11_n.png
nm_btn_info_11_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_11_p.png
nm_btn_info_12_n = image://AppHomeScreen/{nmode|performance}/nm_btn_info_12_n.png
nm_btn_info_12_p = image://AppHomeScreen/{nmode|performance}/nm_btn_info_12_p.png
nm_btn_custom_03_n = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_03_n.png
nm_btn_custom_03_p = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_03_p.png
nm_ic_beep_04_on = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_04_on.png
nm_ic_beep_04_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_04_off.png
nm_ic_beep_06_on = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_06_on.png
nm_ic_beep_06_off = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_06_off.png
nm_ic_beep_07_on = image://AppHomeScreen/{nmode|performance}/nm_ic_beep_07_on.png
nm_bg_edit_gforce_graph = image://AppHomeScreen/{nmode|performance}/nm_bg_edit_gforce_graph.png
nm_bg_widget_gforce_graph_s = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_gforce_graph_s.png
nm_bg_widget_gforce_graph_l = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_gforce_graph_l.png
nm_ic_btn_off = image://AppHomeScreen/{nmode|performance}/nm_ic_btn_off.png
nm_ic_btn_on = image://AppHomeScreen/{nmode|performance}/nm_ic_btn_on.png
nm_btn_control_d_ = image://AppHomeScreen/{nmode|performance}/nm_btn_control_d_
nm_btn_control_u_ = image://AppHomeScreen/{nmode|performance}/nm_btn_control_u_
nm_btn_control_s_d_ = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_d_
nm_btn_control_s_u_ = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_u_
nm_btn_control_left_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_left_d.png
nm_btn_control_left_f = image://AppHomeScreen/{nmode|performance}/nm_btn_control_left_f.png
nm_btn_control_left_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_left_p.png
nm_btn_control_left_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_left_n.png
nm_btn_control_left_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_left_s.png
nm_btn_control_right_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_right_d.png
nm_btn_control_right_f = image://AppHomeScreen/{nmode|performance}/nm_btn_control_right_f.png
nm_btn_control_right_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_right_p.png
nm_btn_control_right_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_right_n.png
nm_btn_control_right_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_right_s.png
nm_ic_title_nmode = image://AppHomeScreen/{nmode|performance}/nm_ic_title_nmode.png
ev_ic_title_setting = image://AppHomeScreen/ev/ev_ic_title_setting.png
nm_ic_lab_trophy_list = image://AppHomeScreen/{nmode|performance}/nm_ic_lab_trophy_list.png
me_ic_list = image://AppHomeScreen/media/me_ic_list.png
nm_btn_custom_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_l_n.png
nm_btn_custom_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_m_n.png
nm_btn_custom_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_r_n.png
nm_btn_custom_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_l_p.png
nm_btn_custom_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_m_p.png
nm_btn_custom_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_custom_r_p.png
nm_btn_01_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_01_l_n.png
nm_btn_01_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_01_m_n.png
nm_btn_01_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_01_r_n.png
nm_btn_01_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_01_l_p.png
nm_btn_01_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_01_m_p.png
nm_btn_01_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_01_r_p.png
nm_btn_02_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_02_l_n.png
nm_btn_02_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_02_m_n.png
nm_btn_02_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_02_r_n.png
nm_btn_02_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_02_l_p.png
nm_btn_02_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_02_m_p.png
nm_btn_02_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_02_r_p.png
nm_ic_car = image://AppHomeScreen/{nmode|performance}/nm_ic_car.png
nm_ic_split_car = image://AppHomeScreen/{nmode|performance}/nm_ic_split_car.png
nm_btn_timer_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_l_n.png
nm_btn_timer_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_m_n.png
nm_btn_timer_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_r_n.png
nm_btn_timer_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_l_p.png
nm_btn_timer_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_m_p.png
nm_btn_timer_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_timer_r_p.png
nm_btn_gforce_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_l_n.png
nm_btn_gforce_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_m_n.png
nm_btn_gforce_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_r_n.png
nm_btn_gforce_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_l_p.png
nm_btn_gforce_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_m_p.png
nm_btn_gforce_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_gforce_r_p.png
nm_ic_gforce = image://AppHomeScreen/{nmode|performance}/nm_ic_gforce.png
nm_btn_turbo_n = image://AppHomeScreen/{nmode|performance}/nm_btn_turbo_n.png
nm_btn_turbo_p = image://AppHomeScreen/{nmode|performance}/nm_btn_turbo_p.png
nm_etc_value_bar = image://AppHomeScreen/{nmode|performance}/nm_etc_value_bar.png
nm_etc_value_split_bar = image://AppHomeScreen/{nmode|performance}/nm_etc_value_split_bar.png
nm_etc_value_marking = image://AppHomeScreen/{nmode|performance}/nm_etc_value_marking.png
nm_btn_torque_n = image://AppHomeScreen/{nmode|performance}/nm_btn_torque_n.png
nm_btn_torque_p = image://AppHomeScreen/{nmode|performance}/nm_btn_torque_p.png
nm_btn_power_n = image://AppHomeScreen/{nmode|performance}/nm_btn_power_n.png
nm_btn_power_p = image://AppHomeScreen/{nmode|performance}/nm_btn_power_p.png
nm_btn_split_torque_n = image://AppHomeScreen/{nmode|performance}/nm_btn_split_torque_n.png
nm_btn_split_torque_p = image://AppHomeScreen/{nmode|performance}/nm_btn_split_torque_p.png
nm_btn_split_turbo_n = image://AppHomeScreen/{nmode|performance}/nm_btn_split_turbo_n.png
nm_btn_split_turbo_p = image://AppHomeScreen/{nmode|performance}/nm_btn_split_turbo_p.png
nm_btn_split_power_n = image://AppHomeScreen/{nmode|performance}/nm_btn_split_power_n.png
nm_btn_split_power_p = image://AppHomeScreen/{nmode|performance}/nm_btn_split_power_p.png
nm_btn_dim_l = image://AppHomeScreen/{nmode|performance}/nm_btn_dim_l.png
nm_btn_dim_r = image://AppHomeScreen/{nmode|performance}/nm_btn_dim_r.png
nm_btn_line = image://AppHomeScreen/{nmode|performance}/nm_btn_line.png
nm_btn_activate_01_n = image://AppHomeScreen/{nmode|performance}/nm_btn_activate_01_n.png
nm_btn_activate_01_p = image://AppHomeScreen/{nmode|performance}/nm_btn_activate_01_p.png
nm_btn_activate_02_n = image://AppHomeScreen/{nmode|performance}/nm_btn_activate_02_n.png
nm_btn_activate_02_p = image://AppHomeScreen/{nmode|performance}/nm_btn_activate_02_p.png
nm_home_etc_gauge_green_bg = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg.png
nm_home_etc_gauge_green_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_l.png
nm_home_etc_gauge_green_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_m.png
nm_home_etc_gauge_green_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_r.png
nm_home_etc_gauge_red_bg = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg.png
nm_home_etc_gauge_red_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_l.png
nm_home_etc_gauge_red_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_m.png
nm_home_etc_gauge_red_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_r.png
nm_home_etc_gauge_green_bg_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_s.png
nm_home_etc_gauge_green_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_l_s.png
nm_home_etc_gauge_green_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_m_s.png
nm_home_etc_gauge_green_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_r_s.png
nm_home_etc_gauge_red_bg_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_s.png
nm_home_etc_gauge_red_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_l_s.png
nm_home_etc_gauge_red_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_m_s.png
nm_home_etc_gauge_red_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_r_s.png
nm_home_etc_gauge_green_bg_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_l.png
nm_home_etc_gauge_green_bg_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_m.png
nm_home_etc_gauge_green_bg_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_r.png
nm_home_etc_gauge_red_bg_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_l.png
nm_home_etc_gauge_red_bg_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_m.png
nm_home_etc_gauge_red_bg_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_r.png
nm_home_etc_gauge_green_bg_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_l_s.png
nm_home_etc_gauge_green_bg_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_m_s.png
nm_home_etc_gauge_green_bg_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_green_bg_r_s.png
nm_home_etc_gauge_red_bg_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_l_s.png
nm_home_etc_gauge_red_bg_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_m_s.png
nm_home_etc_gauge_red_bg_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_red_bg_r_s.png
nm_home_etc_gauge_blue_bg = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg.png
nm_home_etc_gauge_blue_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_l.png
nm_home_etc_gauge_blue_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_m.png
nm_home_etc_gauge_blue_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_r.png
nm_home_etc_gauge_blue_bg_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_s.png
nm_home_etc_gauge_blue_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_l_s.png
nm_home_etc_gauge_blue_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_m_s.png
nm_home_etc_gauge_blue_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_r_s.png
nm_home_etc_gauge_blue_bg_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_l.png
nm_home_etc_gauge_blue_bg_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_m.png
nm_home_etc_gauge_blue_bg_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_r.png
nm_home_etc_gauge_blue_bg_l_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_l_s.png
nm_home_etc_gauge_blue_bg_m_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_m_s.png
nm_home_etc_gauge_blue_bg_r_s = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_blue_bg_r_s.png
nm_home_etc_gauge_amber_bg_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_bg_l.png
nm_home_etc_gauge_amber_bg_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_bg_m.png
nm_home_etc_gauge_amber_bg_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_bg_r.png
nm_home_etc_gauge_amber_l = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_l.png
nm_home_etc_gauge_amber_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_m.png
nm_home_etc_gauge_amber_r = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_amber_r.png
nm_home_etc_gauge_divider_left_t = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_left_t.png
nm_home_etc_gauge_divider_left_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_left_m.png
nm_home_etc_gauge_divider_left_b = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_left_b.png
nm_home_etc_gauge_divider_center_t = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_center_t.png
nm_home_etc_gauge_divider_center_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_center_m.png
nm_home_etc_gauge_divider_center_b = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_center_b.png
nm_home_etc_gauge_divider_center = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_center.png
nm_home_etc_gauge_divider_right_t = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_right_t.png
nm_home_etc_gauge_divider_right_m = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_right_m.png
nm_home_etc_gauge_divider_right_b = image://AppHomeScreen/{nmode|performance}/nm_home_etc_gauge_divider_right_b.png
nm_edit_etc_gauge_divider_center = image://AppHomeScreen/{nmode|performance}/nm_edit_etc_gauge_divider_center.png
nm_ic_engine = image://AppHomeScreen/{nmode|performance}/nm_ic_engine.png
nm_ic_rev = image://AppHomeScreen/{nmode|performance}/nm_ic_rev.png
nm_ic_lsd = image://AppHomeScreen/{nmode|performance}/nm_ic_lsd.png
nm_ic_exhaust = image://AppHomeScreen/{nmode|performance}/nm_ic_exhaust.png
nm_bg_lab = image://AppHomeScreen/{nmode|performance}/nm_bg_lab.png
nm_bg_lab_list = image://AppHomeScreen/{nmode|performance}/nm_bg_lab_list.png
nm_bg_lab_split = image://AppHomeScreen/{nmode|performance}/nm_bg_lab_split.png
nm_ic_rec = image://AppHomeScreen/{nmode|performance}/nm_ic_rec.png
nm_ic_lab_trophy_l = image://AppHomeScreen/{nmode|performance}/nm_ic_lab_trophy_l.png
nm_ic_lab_trophy_s = image://AppHomeScreen/{nmode|performance}/nm_ic_lab_trophy_s.png
nm_ic_lap_plus_edit = image://AppHomeScreen/{nmode|performance}/nm_ic_lap_plus_edit.png
nm_ic_lap_minus_edit = image://AppHomeScreen/{nmode|performance}/nm_ic_lap_minus_edit.png
nm_ic_lap_plus_l = image://AppHomeScreen/{nmode|performance}/nm_ic_lap_plus_l.png
nm_ic_lap_minus_l = image://AppHomeScreen/{nmode|performance}/nm_ic_lap_minus_l.png
co_bg_scroll_t = image://AppHomeScreen/general/co_bg_scroll_t.png
co_bg_scroll_m = image://AppHomeScreen/general/co_bg_scroll_m.png
co_bg_scroll_b = image://AppHomeScreen/general/co_bg_scroll_b.png
co_bg_scroll_bar_b = image://AppHomeScreen/general/co_bg_scroll_bar_b.png
co_bg_scroll_bar_m = image://AppHomeScreen/general/co_bg_scroll_bar_m.png
co_bg_scroll_bar_t = image://AppHomeScreen/general/co_bg_scroll_bar_t.png
nm_bg_lab_line_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_lab_line_01.png
nm_bg_lab_line_02 = image://AppHomeScreen/{nmode|performance}/nm_bg_lab_line_02.png
set_btn_date_u_n = image://AppHomeScreen/setup/set_btn_date_u_n.png
set_btn_date_u_p = image://AppHomeScreen/setup/set_btn_date_u_p.png
set_btn_date_u_f = image://AppHomeScreen/setup/set_btn_date_u_f.png
set_btn_date_u_d = image://AppHomeScreen/setup/set_btn_date_u_d.png
set_btn_date_d_n = image://AppHomeScreen/setup/set_btn_date_d_n.png
set_btn_date_d_p = image://AppHomeScreen/setup/set_btn_date_d_p.png
set_btn_date_d_f = image://AppHomeScreen/setup/set_btn_date_d_f.png
set_btn_date_d_d = image://AppHomeScreen/setup/set_btn_date_d_d.png
nm_bg_acceleration = image://AppHomeScreen/{nmode|performance}/nm_bg_acceleration.png
nm_btn_h126_n = image://AppHomeScreen/{nmode|performance}/nm_btn_h126_n.png
nm_btn_h126_p = image://AppHomeScreen/{nmode|performance}/nm_btn_h126_p.png
nm_btn_h126_f = image://AppHomeScreen/{nmode|performance}/nm_btn_h126_f.png
nm_ic_title_gforce = image://AppHomeScreen/{nmode|performance}/nm_ic_title_gforce.png
nm_bg_gforce = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce.png
nm_ic_gforce_point = image://AppHomeScreen/{nmode|performance}/nm_ic_gforce_point.png
nm_btn_minus_n = image://AppHomeScreen/{nmode|performance}/nm_btn_minus_n.png
nm_btn_minus_p = image://AppHomeScreen/{nmode|performance}/nm_btn_minus_p.png
nm_btn_minus_f = image://AppHomeScreen/{nmode|performance}/nm_btn_minus_f.png
nm_btn_minus_d = image://AppHomeScreen/{nmode|performance}/nm_btn_minus_d.png
nm_btn_plus_n = image://AppHomeScreen/{nmode|performance}/nm_btn_plus_n.png
nm_btn_plus_p = image://AppHomeScreen/{nmode|performance}/nm_btn_plus_p.png
nm_btn_plus_f = image://AppHomeScreen/{nmode|performance}/nm_btn_plus_f.png
nm_btn_plus_d = image://AppHomeScreen/{nmode|performance}/nm_btn_plus_d.png
nm_ic_title_turbo = image://AppHomeScreen/{nmode|performance}/nm_ic_title_turbo.png
nm_ic_title_torque = image://AppHomeScreen/{nmode|performance}/nm_ic_title_torque.png
nm_ic_title_power = image://AppHomeScreen/{nmode|performance}/nm_ic_title_power.png
nm_bg_graph = image://AppHomeScreen/{nmode|performance}/nm_bg_graph.png
nm_bg_graph_m = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_m.png
nm_bg_graph_split = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_split.png
nm_bg_graph_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_c.png
nm_ic_noti_time = image://AppHomeScreen/{nmode|performance}/nm_ic_noti_time.png
nm_ic_noti_vehicle = image://AppHomeScreen/{nmode|performance}/nm_ic_noti_vehicle.png
nm_ic_noti_turbo = image://AppHomeScreen/{nmode|performance}/nm_ic_noti_turbo.png
nm_btn_arrow_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_l_n.png
nm_btn_arrow_l_d = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_l_d.png
nm_btn_arrow_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_l_p.png
nm_btn_arrow_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_r_n.png
nm_btn_arrow_r_d = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_r_d.png
nm_btn_arrow_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_arrow_r_p.png
nm_btn_ready_l_n = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_l_n.png
nm_btn_ready_l_p = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_l_p.png
nm_btn_ready_m_n = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_m_n.png
nm_btn_ready_m_p = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_m_p.png
nm_btn_ready_r_n = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_r_n.png
nm_btn_ready_r_p = image://AppHomeScreen/{nmode|performance}/nm_btn_ready_r_p.png
nm_etc_bubble_time = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_time.png
nm_etc_time_marker = image://AppHomeScreen/{nmode|performance}/nm_etc_time_marker.png
nm_etc_bubble_turbo_l = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_turbo_l.png
nm_etc_bubble_turbo_m = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_turbo_m.png
nm_etc_bubble_turbo_r = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_turbo_r.png
nm_etc_bubble_vehicle_l = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_vehicle_l.png
nm_etc_bubble_vehicle_m = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_vehicle_m.png
nm_etc_bubble_vehicle_r = image://AppHomeScreen/{nmode|performance}/nm_etc_bubble_vehicle_r.png
nm_ic_set_engine_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_engine_normal.png
nm_ic_set_rev_off = image://AppHomeScreen/{nmode|performance}/nm_ic_set_rev_off.png
nm_ic_set_rev_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_rev_normal.png
nm_ic_set_rev_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_rev_sport.png
nm_ic_set_lsd_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_lsd_normal.png
nm_ic_set_lsd_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_lsd_sport.png
nm_ic_set_exhaust_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_exhaust_normal.png
nm_ic_set_exhaust_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_exhaust_sport.png
nm_ic_suspension = image://AppHomeScreen/{nmode|performance}/nm_ic_suspension.png
nm_ic_steering = image://AppHomeScreen/{nmode|performance}/nm_ic_steering.png
nm_ic_esc = image://AppHomeScreen/{nmode|performance}/nm_ic_esc.png
nm_ic_set_suspen_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_suspen_normal.png
nm_ic_set_suspen_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_suspen_sport.png
nm_ic_set_steering_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_steering_normal.png
nm_ic_set_steering_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_steering_sport.png
nm_ic_set_esc_normal = image://AppHomeScreen/{nmode|performance}/nm_ic_set_esc_normal.png
nm_ic_set_esc_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_esc_sport.png
nm_ic_set_esc_off = image://AppHomeScreen/{nmode|performance}/nm_ic_set_esc_off.png
nm_ic_set_engine_sport = image://AppHomeScreen/{nmode|performance}/nm_ic_set_engine_sport.png
nm_ic_set_engine_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_ic_set_engine_sport_plus.png
co_btn_radio_off_d = image://AppHomeScreen/general/co_btn_radio_off_d.png
co_btn_radio_off_f = image://AppHomeScreen/general/co_btn_radio_off_f.png
co_btn_radio_off_n = image://AppHomeScreen/general/co_btn_radio_off_n.png
co_btn_radio_off_p = image://AppHomeScreen/general/co_btn_radio_off_p.png
co_btn_radio_on_d = image://AppHomeScreen/general/co_btn_radio_on_d.png
co_btn_radio_on_f = image://AppHomeScreen/general/co_btn_radio_on_f.png
co_btn_radio_on_n = image://AppHomeScreen/general/co_btn_radio_on_n.png
co_btn_radio_on_p = image://AppHomeScreen/general/co_btn_radio_on_p.png
nm_ic_set_list_off_n = image://AppHomeScreen/{nmode|performance}/nm_ic_set_list_off_n.png
nm_ic_set_list_off_s = image://AppHomeScreen/{nmode|performance}/nm_ic_set_list_off_s.png
nm_ic_point_turbo = image://AppHomeScreen/{nmode|performance}/nm_ic_point_turbo.png
nm_ic_point_vehicle = image://AppHomeScreen/{nmode|performance}/nm_ic_point_vehicle.png
home_ic_edit_gforce = image://AppHomeScreen/home/home_ic_edit_gforce.png
home_ic_edit_turbo = image://AppHomeScreen/home/home_ic_edit_turbo.png
home_ic_edit_torque = image://AppHomeScreen/home/home_ic_edit_torque.png
home_ic_edit_power = image://AppHomeScreen/home/home_ic_edit_power.png
home_ic_edit_call = image://AppHomeScreen/home/home_ic_edit_call.png
home_ic_edit_favorite = image://AppHomeScreen/home/home_ic_edit_favorite.png
home_bg_adv_edit_l_s = image://AppHomeScreen/home/home_bg_adv_edit_l_s.png
home_bg_adv_edit_l_n = image://AppHomeScreen/home/home_bg_adv_edit_l_n.png
home_bg_adv_edit_l_p = image://AppHomeScreen/home/home_bg_adv_edit_l_p.png
home_bg_adv_edit_m_s = image://AppHomeScreen/home/home_bg_adv_edit_m_s.png
home_bg_adv_edit_m_n = image://AppHomeScreen/home/home_bg_adv_edit_m_n.png
home_bg_adv_edit_m_p = image://AppHomeScreen/home/home_bg_adv_edit_m_p.png
home_bg_adv_edit_r_s = image://AppHomeScreen/home/home_bg_adv_edit_r_s.png
home_bg_adv_edit_r_n = image://AppHomeScreen/home/home_bg_adv_edit_r_n.png
home_bg_adv_edit_r_p = image://AppHomeScreen/home/home_bg_adv_edit_r_p.png
home_bg_adv_edit_line = image://AppHomeScreen/home/home_bg_adv_edit_line.png
home_bg_adv_edit_a = image://AppHomeScreen/home/home_bg_adv_edit_a.png
home_bg_adv_edit_b = image://AppHomeScreen/home/home_bg_adv_edit_b.png
home_bg_adv_edit_c = image://AppHomeScreen/home/home_bg_adv_edit_c.png
home_bg_adv_edit_d = image://AppHomeScreen/home/home_bg_adv_edit_d.png
nm_icon_arrow_left_d = image://AppHomeScreen/{nmode|performance}/nm_icon_arrow_left_d.png
nm_icon_arrow_left_n = image://AppHomeScreen/{nmode|performance}/nm_icon_arrow_left_n.png
nm_icon_arrow_right_d = image://AppHomeScreen/{nmode|performance}/nm_icon_arrow_right_d.png
nm_icon_arrow_right_n = image://AppHomeScreen/{nmode|performance}/nm_icon_arrow_right_n.png
nm_widget_icon_arrow_left_n = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_left_n.png
nm_widget_icon_arrow_left_d = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_left_d.png
nm_widget_icon_arrow_right_n = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_right_n.png
nm_widget_icon_arrow_right_d = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_right_d.png
nm_widget_icon_arrow_left_p = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_left_p.png
nm_widget_icon_arrow_right_p = image://AppHomeScreen/{nmode|performance}/nm_widget_icon_arrow_right_p.png
set_bg_n_gage = image://AppHomeScreen/setup/set_bg_n_gage.png
set_ic_n_gage_dot = image://AppHomeScreen/setup/set_ic_n_gage_dot.png
set_btn_f_n = image://AppHomeScreen/setup/set_btn_f_n.png
set_btn_f_p = image://AppHomeScreen/setup/set_btn_f_p.png
set_btn_f_d = image://AppHomeScreen/setup/set_btn_f_d.png
set_btn_minus_n = image://AppHomeScreen/setup/set_btn_minus_n.png
set_btn_minus_f = image://AppHomeScreen/setup/set_btn_minus_f.png
set_btn_minus_p = image://AppHomeScreen/setup/set_btn_minus_p.png
set_btn_minus_d = image://AppHomeScreen/setup/set_btn_minus_d.png
set_btn_plus_n = image://AppHomeScreen/setup/set_btn_plus_n.png
set_btn_plus_f = image://AppHomeScreen/setup/set_btn_plus_f.png
set_btn_plus_p = image://AppHomeScreen/setup/set_btn_plus_p.png
set_btn_plus_d = image://AppHomeScreen/setup/set_btn_plus_d.png
set_ic_pointer = image://AppHomeScreen/setup/set_ic_pointer.png
set_btn_vol_box_d = image://AppHomeScreen/setup/set_btn_vol_box_d.png
set_btn_vol_box_n = image://AppHomeScreen/setup/set_btn_vol_box_n.png
set_btn_vol_box_p = image://AppHomeScreen/setup/set_btn_vol_box_p.png
set_btn_vol_bubble_01 = image://AppHomeScreen/setup/set_btn_vol_bubble_01.png
set_etc_bar_10_bg = image://AppHomeScreen/setup/set_etc_bar_10_bg.png
set_etc_bar_10 = image://AppHomeScreen/setup/set_etc_bar_10.png
ev_btn_set_d = image://AppHomeScreen/ev/ev_btn_set_d.png
nm_icon_bubblepop_blue_l_l = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_l.png
nm_icon_bubblepop_blue_l_m = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_m.png
nm_icon_bubblepop_blue_l_r = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_r.png
nm_icon_bubblepop_red_l_l = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_l.png
nm_icon_bubblepop_red_l_m = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_m.png
nm_icon_bubblepop_red_l_r = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_r.png
nm_icon_bubblepop_blue_l_l_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_l_t.png
nm_icon_bubblepop_blue_l_m_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_m_t.png
nm_icon_bubblepop_blue_l_r_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l_r_t.png
nm_icon_bubblepop_red_l_l_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_l_t.png
nm_icon_bubblepop_red_l_m_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_m_t.png
nm_icon_bubblepop_red_l_r_t = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l_r_t.png
clm_car_bg = image://AppHomeScreen/climate/clm_car_bg.png
clm_car_bg_rhd = image://AppHomeScreen/climate/clm_car_bg_rhd.png
clm_top_temp_c_n = image://AppHomeScreen/climate/clm_top_temp_c_n.png
clm_top_temp_f_n = image://AppHomeScreen/climate/clm_top_temp_f_n.png
clm_bottom_line = image://AppHomeScreen/climate/clm_bottom_line.png
clm_btn_fan_n = image://AppHomeScreen/climate/clm_btn_fan_n.png
clm_btn_fan_s = image://AppHomeScreen/climate/clm_btn_fan_s.png
clm_btn_fan_d = image://AppHomeScreen/climate/clm_btn_fan_d.png
clm_btn_heat_n = image://AppHomeScreen/climate/clm_btn_heat_n.png
clm_btn_heat_s = image://AppHomeScreen/climate/clm_btn_heat_s.png
clm_btn_heat_d = image://AppHomeScreen/climate/clm_btn_heat_d.png
clm_bg_divider = image://AppHomeScreen/climate/clm_bg_divider.png
clm_bg_osd_divider = image://AppHomeScreen/climate/clm_bg_osd_divider.png
clm_ic_climate = image://AppHomeScreen/climate/clm_ic_climate.png
clm_ic_co2_d = image://AppHomeScreen/climate/clm_ic_co2_d.png
clm_ic_co2_n = image://AppHomeScreen/climate/clm_ic_co2_n.png
clm_ic_co2 = image://AppHomeScreen/climate/clm_ic_co2.png
clm_ic_fan = image://AppHomeScreen/climate/clm_ic_fan.png
clm_ic_fan_osd = image://AppHomeScreen/climate/clm_ic_fan_osd.png
clm_ic_fan_osd_s = image://AppHomeScreen/climate/clm_ic_fan_osd_s.png
clm_ic_fan_s = image://AppHomeScreen/climate/clm_ic_fan_s.png
clm_ic_heat_osd_n = image://AppHomeScreen/climate/clm_ic_heat_osd_n.png
clm_ic_heat_osd_s = image://AppHomeScreen/climate/clm_ic_heat_osd_s.png
clm_ic_osd_message_airfresh = image://AppHomeScreen/climate/clm_ic_osd_message_airfresh.png
clm_ic_outtemp_c = image://AppHomeScreen/climate/clm_ic_outtemp_c.png
clm_ic_outtemp_f = image://AppHomeScreen/climate/clm_ic_outtemp_f.png
clm_ic_step_ani_00 = image://AppHomeScreen/climate/clm_ic_step_ani_00.png
clm_ic_step_ani_01 = image://AppHomeScreen/climate/clm_ic_step_ani_01.png
clm_ic_step_ani_02 = image://AppHomeScreen/climate/clm_ic_step_ani_02.png
clm_ic_step_ani_03 = image://AppHomeScreen/climate/clm_ic_step_ani_03.png
clm_ic_step_ani_04 = image://AppHomeScreen/climate/clm_ic_step_ani_04.png
clm_ic_step_ani_05 = image://AppHomeScreen/climate/clm_ic_step_ani_05.png
clm_ic_step_ani_06 = image://AppHomeScreen/climate/clm_ic_step_ani_06.png
clm_ic_step_ani_07 = image://AppHomeScreen/climate/clm_ic_step_ani_07.png
clm_ic_step_ani_08 = image://AppHomeScreen/climate/clm_ic_step_ani_08.png
clm_ic_step_ani_osd_00 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_00
clm_ic_step_ani_osd_01 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_01
clm_ic_step_ani_osd_02 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_02
clm_ic_step_ani_osd_03 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_03
clm_ic_step_ani_osd_04 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_04
clm_ic_step_ani_osd_05 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_05
clm_ic_step_ani_osd_06 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_06
clm_ic_step_ani_osd_07 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_07
clm_ic_step_ani_osd_08 = image://AppHomeScreen/climate/clm_ic_step_ani_osd_08
clm_ic_temp_c = image://AppHomeScreen/climate/clm_ic_temp_c.png
clm_ic_temp_c_s = image://AppHomeScreen/climate/clm_ic_temp_c_s.png
clm_ic_temp_f = image://AppHomeScreen/climate/clm_ic_temp_f.png
clm_ic_temp_f_s = image://AppHomeScreen/climate/clm_ic_temp_f_s.png
clm_ic_temp_osd_c = image://AppHomeScreen/climate/clm_ic_temp_osd_c.png
clm_ic_temp_osd_f = image://AppHomeScreen/climate/clm_ic_temp_osd_f.png
clm_ic_wind_osd_01 = image://AppHomeScreen/climate/clm_ic_wind_osd_01.png
clm_ic_wind_osd_02 = image://AppHomeScreen/climate/clm_ic_wind_osd_02.png
clm_ic_wind_osd_03 = image://AppHomeScreen/climate/clm_ic_wind_osd_03.png
clm_ic_wind_osd_04 = image://AppHomeScreen/climate/clm_ic_wind_osd_04.png
clm_ic_wind_osd_05 = image://AppHomeScreen/climate/clm_ic_wind_osd_05.png
clm_ic_wind_osd_06 = image://AppHomeScreen/climate/clm_ic_wind_osd_06.png
clm_ic_wind_osd_07 = image://AppHomeScreen/climate/clm_ic_wind_osd_07.png
clm_ic_wind_osd_08 = image://AppHomeScreen/climate/clm_ic_wind_osd_08.png
clm_ic_wind_osd_09 = image://AppHomeScreen/climate/clm_ic_wind_osd_09.png
clm_ic_wind_osd_10 = image://AppHomeScreen/climate/clm_ic_wind_osd_10.png
clm_ic_wind_osd_11 = image://AppHomeScreen/climate/clm_ic_wind_osd_11.png
clm_ic_wind_osd_12 = image://AppHomeScreen/climate/clm_ic_wind_osd_12.png
clm_ic_wind_osd_13 = image://AppHomeScreen/climate/clm_ic_wind_osd_13.png
co_bg_title_bar = image://AppHomeScreen/general/co_bg_title_bar.png
co_bg_title_bar_shadow = image://AppHomeScreen/general/co_bg_title_bar_shadow.png
clm_bottom_ico_fan01_n = image://AppHomeScreen/climate/clm_bottom_ico_fan01_n.png
clm_bottom_ico_fan01_d = image://AppHomeScreen/climate/clm_bottom_ico_fan01_d.png
clm_bottom_ico_co2_n = image://AppHomeScreen/climate/clm_bottom_ico_co2_n.png
clm_bottom_ico_co2_d = image://AppHomeScreen/climate/clm_bottom_ico_co2_d.png
split_ic_step_ani_00 = image://AppHomeScreen/split/split_ic_step_ani_00.png
split_ic_step_ani_01 = image://AppHomeScreen/split/split_ic_step_ani_01.png
split_ic_step_ani_02 = image://AppHomeScreen/split/split_ic_step_ani_02.png
split_ic_step_ani_03 = image://AppHomeScreen/split/split_ic_step_ani_03.png
split_ic_step_ani_04 = image://AppHomeScreen/split/split_ic_step_ani_04.png
split_ic_step_ani_05 = image://AppHomeScreen/split/split_ic_step_ani_05.png
split_ic_step_ani_06 = image://AppHomeScreen/split/split_ic_step_ani_06.png
split_ic_step_ani_07 = image://AppHomeScreen/split/split_ic_step_ani_07.png
split_ic_step_ani_08 = image://AppHomeScreen/split/split_ic_step_ani_08.png
split_ic_wind = image://AppHomeScreen/split/split_ic_wind.png
widget_ic_clim = image://AppHomeScreen/home/widget_ic_clim.png
widget_ic_step_ani_s_00 = image://AppHomeScreen/home/widget_ic_step_ani_s_00.png
widget_ic_step_ani_s_01 = image://AppHomeScreen/home/widget_ic_step_ani_s_01.png
widget_ic_step_ani_s_02 = image://AppHomeScreen/home/widget_ic_step_ani_s_02.png
widget_ic_step_ani_s_03 = image://AppHomeScreen/home/widget_ic_step_ani_s_03.png
widget_ic_step_ani_s_04 = image://AppHomeScreen/home/widget_ic_step_ani_s_04.png
widget_ic_step_ani_s_05 = image://AppHomeScreen/home/widget_ic_step_ani_s_05.png
widget_ic_step_ani_s_06 = image://AppHomeScreen/home/widget_ic_step_ani_s_06.png
widget_ic_step_ani_s_07 = image://AppHomeScreen/home/widget_ic_step_ani_s_07.png
widget_ic_step_ani_s_08 = image://AppHomeScreen/home/widget_ic_step_ani_s_08.png
widget_ic_wind_s = image://AppHomeScreen/home/widget_ic_wind_s.png
set_ic_beep_on = image://AppHomeScreen/setup/set_ic_beep_on.png
set_ic_beep_off = image://AppHomeScreen/setup/set_ic_beep_off.png
clm_bg_rear_control_n = image://AppHomeScreen/climate/clm_bg_rear_control_n.png
clm_bg_rear_control_f = image://AppHomeScreen/climate/clm_bg_rear_control_f.png
clm_btn_rear_top_n = image://AppHomeScreen/climate/clm_btn_rear_top_n.png
clm_btn_rear_top_f = image://AppHomeScreen/climate/clm_btn_rear_top_f.png
clm_btn_rear_top_p = image://AppHomeScreen/climate/clm_btn_rear_top_p.png
clm_btn_rear_top_s = image://AppHomeScreen/climate/clm_btn_rear_top_s.png
clm_btn_rear_top_d = image://AppHomeScreen/climate/clm_btn_rear_top_d.png
clm_btn_rear_middle_n = image://AppHomeScreen/climate/clm_btn_rear_middle_n.png
clm_btn_rear_middle_f = image://AppHomeScreen/climate/clm_btn_rear_middle_f.png
clm_btn_rear_middle_p = image://AppHomeScreen/climate/clm_btn_rear_middle_p.png
clm_btn_rear_middle_s = image://AppHomeScreen/climate/clm_btn_rear_middle_s.png
clm_btn_rear_middle_d = image://AppHomeScreen/climate/clm_btn_rear_middle_d.png
clm_btn_rear_bottom_n = image://AppHomeScreen/climate/clm_btn_rear_bottom_n.png
clm_btn_rear_bottom_f = image://AppHomeScreen/climate/clm_btn_rear_bottom_f.png
clm_btn_rear_bottom_p = image://AppHomeScreen/climate/clm_btn_rear_bottom_p.png
clm_btn_rear_bottom_s = image://AppHomeScreen/climate/clm_btn_rear_bottom_s.png
clm_btn_rear_bottom_d = image://AppHomeScreen/climate/clm_btn_rear_bottom_d.png
clm_ic_rear_man_n = image://AppHomeScreen/climate/clm_ic_rear_man_n.png
clm_ic_rear_man_d = image://AppHomeScreen/climate/clm_ic_rear_man_d.png
clm_btn_temp_ctl_minus_n = image://AppHomeScreen/climate/clm_btn_temp_ctl_minus_n.png
clm_btn_temp_ctl_minus_f = image://AppHomeScreen/climate/clm_btn_temp_ctl_minus_f.png
clm_btn_temp_ctl_minus_p = image://AppHomeScreen/climate/clm_btn_temp_ctl_minus_p.png
clm_btn_temp_ctl_minus_s = image://AppHomeScreen/climate/clm_btn_temp_ctl_minus_s.png
clm_btn_temp_ctl_minus_d = image://AppHomeScreen/climate/clm_btn_temp_ctl_minus_d.png
clm_btn_temp_ctl_plus_n = image://AppHomeScreen/climate/clm_btn_temp_ctl_plus_n.png
clm_btn_temp_ctl_plus_f = image://AppHomeScreen/climate/clm_btn_temp_ctl_plus_f.png
clm_btn_temp_ctl_plus_p = image://AppHomeScreen/climate/clm_btn_temp_ctl_plus_p.png
clm_btn_temp_ctl_plus_s = image://AppHomeScreen/climate/clm_btn_temp_ctl_plus_s.png
clm_btn_temp_ctl_plus_d = image://AppHomeScreen/climate/clm_btn_temp_ctl_plus_d.png
clm_etc_temp_level_00_n = image://AppHomeScreen/climate/clm_etc_temp_level_00_n.png
clm_etc_temp_level_00_f = image://AppHomeScreen/climate/clm_etc_temp_level_00_f.png
clm_etc_temp_level_01_n = image://AppHomeScreen/climate/clm_etc_temp_level_01_n.png
clm_etc_temp_level_01_f = image://AppHomeScreen/climate/clm_etc_temp_level_01_f.png
clm_etc_temp_level_02_n = image://AppHomeScreen/climate/clm_etc_temp_level_02_n.png
clm_etc_temp_level_02_f = image://AppHomeScreen/climate/clm_etc_temp_level_02_f.png
clm_etc_temp_level_03_n = image://AppHomeScreen/climate/clm_etc_temp_level_03_n.png
clm_etc_temp_level_03_f = image://AppHomeScreen/climate/clm_etc_temp_level_03_f.png
clm_etc_temp_level_04_n = image://AppHomeScreen/climate/clm_etc_temp_level_04_n.png
clm_etc_temp_level_04_f = image://AppHomeScreen/climate/clm_etc_temp_level_04_f.png
clm_etc_temp_level_05_n = image://AppHomeScreen/climate/clm_etc_temp_level_05_n.png
clm_etc_temp_level_05_f = image://AppHomeScreen/climate/clm_etc_temp_level_05_f.png
clm_etc_temp_level_06_n = image://AppHomeScreen/climate/clm_etc_temp_level_06_n.png
clm_etc_temp_level_06_f = image://AppHomeScreen/climate/clm_etc_temp_level_06_f.png
clm_btn_fan_ctl_plus_n = image://AppHomeScreen/climate/clm_btn_fan_ctl_plus_n.png
clm_btn_fan_ctl_plus_f = image://AppHomeScreen/climate/clm_btn_fan_ctl_plus_f.png
clm_btn_fan_ctl_plus_p = image://AppHomeScreen/climate/clm_btn_fan_ctl_plus_p.png
clm_btn_fan_ctl_plus_s = image://AppHomeScreen/climate/clm_btn_fan_ctl_plus_s.png
clm_btn_fan_ctl_plus_d = image://AppHomeScreen/climate/clm_btn_fan_ctl_plus_d.png
clm_btn_fan_ctl_minus_n = image://AppHomeScreen/climate/clm_btn_fan_ctl_minus_n.png
clm_btn_fan_ctl_minus_f = image://AppHomeScreen/climate/clm_btn_fan_ctl_minus_f.png
clm_btn_fan_ctl_minus_p = image://AppHomeScreen/climate/clm_btn_fan_ctl_minus_p.png
clm_btn_fan_ctl_minus_s = image://AppHomeScreen/climate/clm_btn_fan_ctl_minus_s.png
clm_btn_fan_ctl_minus_d = image://AppHomeScreen/climate/clm_btn_fan_ctl_minus_d.png
clm_etc_fan_level_00_n = image://AppHomeScreen/climate/clm_etc_fan_level_00_n.png
clm_etc_fan_level_00_f = image://AppHomeScreen/climate/clm_etc_fan_level_00_f.png
clm_etc_fan_level_01_n = image://AppHomeScreen/climate/clm_etc_fan_level_01_n.png
clm_etc_fan_level_01_f = image://AppHomeScreen/climate/clm_etc_fan_level_01_f.png
clm_etc_fan_level_02_n = image://AppHomeScreen/climate/clm_etc_fan_level_02_n.png
clm_etc_fan_level_02_f = image://AppHomeScreen/climate/clm_etc_fan_level_02_f.png
clm_etc_fan_level_03_n = image://AppHomeScreen/climate/clm_etc_fan_level_03_n.png
clm_etc_fan_level_03_f = image://AppHomeScreen/climate/clm_etc_fan_level_03_f.png
clm_etc_fan_level_04_n = image://AppHomeScreen/climate/clm_etc_fan_level_04_n.png
clm_etc_fan_level_04_f = image://AppHomeScreen/climate/clm_etc_fan_level_04_f.png
clm_etc_fan_level_05_n = image://AppHomeScreen/climate/clm_etc_fan_level_05_n.png
clm_etc_fan_level_05_f = image://AppHomeScreen/climate/clm_etc_fan_level_05_f.png
clm_etc_fan_level_06_n = image://AppHomeScreen/climate/clm_etc_fan_level_06_n.png
clm_etc_fan_level_06_f = image://AppHomeScreen/climate/clm_etc_fan_level_06_f.png
clm_etc_temp_atc_level_bg = image://AppHomeScreen/climate/clm_etc_temp_atc_level_bg.png
clm_etc_temp_atc_level_n = image://AppHomeScreen/climate/clm_etc_temp_atc_level_n.png
clm_etc_temp_atc_level_f = image://AppHomeScreen/climate/clm_etc_temp_atc_level_f.png
clm_etc_fan_atc_level_00_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_00_n.png
clm_etc_fan_atc_level_00_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_00_f.png
clm_etc_fan_atc_level_01_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_01_n.png
clm_etc_fan_atc_level_01_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_01_f.png
clm_etc_fan_atc_level_02_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_02_n.png
clm_etc_fan_atc_level_02_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_02_f.png
clm_etc_fan_atc_level_03_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_03_n.png
clm_etc_fan_atc_level_03_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_03_f.png
clm_etc_fan_atc_level_04_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_04_n.png
clm_etc_fan_atc_level_04_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_04_f.png
clm_etc_fan_atc_level_05_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_05_n.png
clm_etc_fan_atc_level_05_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_05_f.png
clm_etc_fan_atc_level_06_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_06_n.png
clm_etc_fan_atc_level_06_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_06_f.png
clm_etc_fan_atc_level_07_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_07_n.png
clm_etc_fan_atc_level_07_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_07_f.png
clm_etc_fan_atc_level_08_n = image://AppHomeScreen/climate/clm_etc_fan_atc_level_08_n.png
clm_etc_fan_atc_level_08_f = image://AppHomeScreen/climate/clm_etc_fan_atc_level_08_f.png
clm_bg_temp_off = image://AppHomeScreen/climate/clm_bg_temp_off.png
clm_bg_temp_off_s = image://AppHomeScreen/climate/clm_bg_temp_off_s.png
clm_bg_temp_text = image://AppHomeScreen/climate/clm_bg_temp_text.png
clm_bg_temp_text_s = image://AppHomeScreen/climate/clm_bg_temp_text_s.png
clm_bg_temp_01 = image://AppHomeScreen/climate/clm_bg_temp_01.png
clm_bg_temp_01_s = image://AppHomeScreen/climate/clm_bg_temp_01_s.png
clm_bg_temp_02 = image://AppHomeScreen/climate/clm_bg_temp_02.png
clm_bg_temp_02_s = image://AppHomeScreen/climate/clm_bg_temp_02_s.png
clm_bg_temp_03 = image://AppHomeScreen/climate/clm_bg_temp_03.png
clm_bg_temp_03_s = image://AppHomeScreen/climate/clm_bg_temp_03_s.png
clm_bg_temp_04 = image://AppHomeScreen/climate/clm_bg_temp_04.png
clm_bg_temp_04_s = image://AppHomeScreen/climate/clm_bg_temp_04_s.png
clm_bg_temp_05 = image://AppHomeScreen/climate/clm_bg_temp_05.png
clm_bg_temp_05_s = image://AppHomeScreen/climate/clm_bg_temp_05_s.png
clm_bg_temp_lv_01 = image://AppHomeScreen/climate/clm_bg_temp_lv_01.png
clm_bg_temp_lv_01_s = image://AppHomeScreen/climate/clm_bg_temp_lv_01_s.png
clm_bg_temp_lv_02 = image://AppHomeScreen/climate/clm_bg_temp_lv_02.png
clm_bg_temp_lv_02_s = image://AppHomeScreen/climate/clm_bg_temp_lv_02_s.png
clm_bg_temp_lv_03 = image://AppHomeScreen/climate/clm_bg_temp_lv_03.png
clm_bg_temp_lv_03_s = image://AppHomeScreen/climate/clm_bg_temp_lv_03_s.png
clm_bg_temp_lv_04 = image://AppHomeScreen/climate/clm_bg_temp_lv_04.png
clm_bg_temp_lv_04_s = image://AppHomeScreen/climate/clm_bg_temp_lv_04_s.png
clm_bg_temp_lv_05 = image://AppHomeScreen/climate/clm_bg_temp_lv_05.png
clm_bg_temp_lv_05_s = image://AppHomeScreen/climate/clm_bg_temp_lv_05_s.png
clm_ic_wind = image://AppHomeScreen/climate/clm_ic_wind.png
clm_ic_wind_s = image://AppHomeScreen/climate/clm_ic_wind_s.png
clm_ic_wind_xs = image://AppHomeScreen/climate/clm_ic_wind_xs.png
clm_ic_wind_off = image://AppHomeScreen/climate/clm_ic_wind_off.png
clm_car_rear_bg = image://AppHomeScreen/climate/clm_car_rear_bg.png
clm_btn_temp_plus_ = image://AppHomeScreen/climate/clm_btn_temp_plus_
clm_btn_temp_plus_n = image://AppHomeScreen/climate/clm_btn_temp_plus_n.png
clm_btn_temp_plus_f = image://AppHomeScreen/climate/clm_btn_temp_plus_f.png
clm_btn_temp_plus_p = image://AppHomeScreen/climate/clm_btn_temp_plus_p.png
clm_btn_temp_plus_d = image://AppHomeScreen/climate/clm_btn_temp_plus_d.png
clm_btn_temp_minus_ = image://AppHomeScreen/climate/clm_btn_temp_minus_
clm_btn_temp_minus_n = image://AppHomeScreen/climate/clm_btn_temp_minus_n.png
clm_btn_temp_minus_f = image://AppHomeScreen/climate/clm_btn_temp_minus_f.png
clm_btn_temp_minus_p = image://AppHomeScreen/climate/clm_btn_temp_minus_p.png
clm_btn_temp_minus_d = image://AppHomeScreen/climate/clm_btn_temp_minus_d.png
clm_btn_wind_t_0 = image://AppHomeScreen/climate/clm_btn_wind_t_0
clm_btn_wind_m_0 = image://AppHomeScreen/climate/clm_btn_wind_m_0
clm_btn_wind_b_0 = image://AppHomeScreen/climate/clm_btn_wind_b_0
clm_wind_top = image://AppHomeScreen/climate/clm_wind_top
clm_wind_mid_01_l = image://AppHomeScreen/climate/clm_wind_mid_01_l
clm_wind_mid_01_r = image://AppHomeScreen/climate/clm_wind_mid_01_r
clm_wind_mid_02_l = image://AppHomeScreen/climate/clm_wind_mid_02_l
clm_wind_mid_02_r = image://AppHomeScreen/climate/clm_wind_mid_02_r
clm_wind_bot_l = image://AppHomeScreen/climate/clm_wind_bot_l
clm_wind_bot_r = image://AppHomeScreen/climate/clm_wind_bot_r
clm_btn_wind_auto_n = image://AppHomeScreen/climate/clm_btn_wind_auto_n.png
clm_btn_wind_auto_d = image://AppHomeScreen/climate/clm_btn_wind_auto_d.png
clm_btn_wind_def_01_n = image://AppHomeScreen/climate/clm_btn_wind_def_01_n.png
clm_btn_wind_def_01_d = image://AppHomeScreen/climate/clm_btn_wind_def_01_d.png
clm_btn_wind_def_02_n = image://AppHomeScreen/climate/clm_btn_wind_def_02_n.png
clm_btn_wind_def_02_d = image://AppHomeScreen/climate/clm_btn_wind_def_02_d.png
clm_btn_wind_def_auto_n = image://AppHomeScreen/climate/clm_btn_wind_def_auto_n.png
clm_btn_wind_def_auto_d = image://AppHomeScreen/climate/clm_btn_wind_def_auto_d.png
clm_btn_wind_man_n = image://AppHomeScreen/climate/clm_btn_wind_man_n.png
clm_btn_wind_man_d = image://AppHomeScreen/climate/clm_btn_wind_man_d.png
clm_top_temp_bg_n = image://AppHomeScreen/climate/clm_top_temp_bg_n.png
clm_top_temp_bg_d = image://AppHomeScreen/climate/clm_top_temp_bg_d.png
clm_side_temp_bg_n = image://AppHomeScreen/climate/clm_side_temp_bg_n.png
clm_side_temp_bg_d = image://AppHomeScreen/climate/clm_side_temp_bg_d.png
clm_bg_gauge_veiw_h = image://AppHomeScreen/climate/clm_bg_gauge_veiw_h.png
clm_bg_gauge_veiw_v = image://AppHomeScreen/climate/clm_bg_gauge_veiw_v.png
clm_side_temp_gauge_dot = image://AppHomeScreen/climate/clm_side_temp_gauge_dot.png
clm_top_temp_btn_bg_ = image://AppHomeScreen/climate/clm_top_temp_btn_bg_
clm_top_temp_btn_bg_d = image://AppHomeScreen/climate/clm_top_temp_btn_bg_d.png
clm_top_temp_btn_bg_b = image://AppHomeScreen/climate/clm_top_temp_btn_bg_b.png
clm_top_temp_btn_bg_r = image://AppHomeScreen/climate/clm_top_temp_btn_bg_r.png
clm_top_temp_btn_bg_00 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_00.png
clm_top_temp_btn_bg_01 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_01.png
clm_top_temp_btn_bg_02 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_02.png
clm_top_temp_btn_bg_03 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_03.png
clm_top_temp_btn_bg_04 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_04.png
clm_top_temp_btn_bg_05 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_05.png
clm_top_temp_btn_bg_1 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_1.png
clm_top_temp_btn_bg_2 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_2.png
clm_top_temp_btn_bg_3 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_3.png
clm_top_temp_btn_bg_4 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_4.png
clm_top_temp_btn_bg_5 = image://AppHomeScreen/climate/clm_top_temp_btn_bg_5.png
clm_bg_gauge_h_on_ = image://AppHomeScreen/climate/clm_bg_gauge_h_on_
clm_bg_gauge_h_on_1 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_1.png
clm_bg_gauge_h_on_2 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_2.png
clm_bg_gauge_h_on_3 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_3.png
clm_bg_gauge_h_on_4 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_4.png
clm_bg_gauge_h_on_5 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_5.png
clm_bg_gauge_h_on_v_1 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_v_1.png
clm_bg_gauge_h_on_v_2 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_v_2.png
clm_bg_gauge_h_on_v_3 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_v_3.png
clm_bg_gauge_h_on_v_4 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_v_4.png
clm_bg_gauge_h_on_v_5 = image://AppHomeScreen/climate/clm_bg_gauge_h_on_v_5.png
clm_popup_bg = image://AppHomeScreen/climate/clm_popup_bg.png
clm_popup_btn_b_effect_01 = image://AppHomeScreen/climate/clm_popup_btn_b_effect_01.png
clm_popup_btn_b_effect_02 = image://AppHomeScreen/climate/clm_popup_btn_b_effect_02.png
clm_popup_btn_b_effect_03 = image://AppHomeScreen/climate/clm_popup_btn_b_effect_03.png
clm_popup_btn_b_effect_04 = image://AppHomeScreen/climate/clm_popup_btn_b_effect_04.png
clm_popup_btn_a_effect_01 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_01.png
clm_popup_btn_a_effect_02 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02.png
clm_popup_btn_a_effect_03 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03.png
clm_popup_btn_a_effect_04 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_04.png
clm_popup_btn_a_effect_05 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_05.png
clm_popup_btn_a_effect_06 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_06.png
clm_popup_btn_a_effect_07 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_07.png
clm_popup_btn_a_effect_02_01 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_01.png
clm_popup_btn_a_effect_02_02 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_02.png
clm_popup_btn_a_effect_02_03 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_03.png
clm_popup_btn_a_effect_02_04 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_04.png
clm_popup_btn_a_effect_02_05 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_05.png
clm_popup_btn_a_effect_02_06 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_06.png
clm_popup_btn_a_effect_02_07 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_07.png
clm_popup_btn_a_effect_02_08 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_02_08.png
clm_popup_btn_a_effect_03_01 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_01.png
clm_popup_btn_a_effect_03_02 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_02.png
clm_popup_btn_a_effect_03_03 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_03.png
clm_popup_btn_a_effect_03_04 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_04.png
clm_popup_btn_a_effect_03_05 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_05.png
clm_popup_btn_a_effect_03_06 = image://AppHomeScreen/climate/clm_popup_btn_a_effect_03_06.png
clm_popup_ic_wind_t = image://AppHomeScreen/climate/clm_popup_ic_wind_t
clm_popup_ic_wind_m = image://AppHomeScreen/climate/clm_popup_ic_wind_m
clm_popup_ic_wind_b = image://AppHomeScreen/climate/clm_popup_ic_wind_b
clm_popup_ic_wind_man_n = image://AppHomeScreen/climate/clm_popup_ic_wind_man_n.png
clm_popup_ic_wind_man_s = image://AppHomeScreen/climate/clm_popup_ic_wind_man_s.png
clm_popup_ic_wind_def_n = image://AppHomeScreen/climate/clm_popup_ic_wind_def_n.png
clm_popup_ic_wind_def_s = image://AppHomeScreen/climate/clm_popup_ic_wind_def_s.png
clm_popup_ic_wind_auto_n = image://AppHomeScreen/climate/clm_popup_ic_wind_auto_n.png
clm_popup_ic_wind_auto_s = image://AppHomeScreen/climate/clm_popup_ic_wind_auto_s.png
clm_ic_bottom_defogauto_n = image://AppHomeScreen/climate/clm_ic_bottom_defogauto_n.png
clm_ic_bottom_defogauto_d = image://AppHomeScreen/climate/clm_ic_bottom_defogauto_d.png
clm_ic_bottom_modeauto_n = image://AppHomeScreen/climate/clm_ic_bottom_modeauto_n.png
clm_ic_bottom_modeauto_d = image://AppHomeScreen/climate/clm_ic_bottom_modeauto_d.png
clm_popup_wind_man = image://AppHomeScreen/climate/clm_popup_wind_man.png
clm_popup_img_sync = image://AppHomeScreen/climate/clm_popup_img_sync.png
clm_popup_ic_sync_n = image://AppHomeScreen/climate/clm_popup_ic_sync_n.png
clm_popup_ic_sync_s = image://AppHomeScreen/climate/clm_popup_ic_sync_s.png
clm_popup_wind_def = image://AppHomeScreen/climate/clm_popup_wind_def.png
clm_popup_wind_t = image://AppHomeScreen/climate/clm_popup_wind_t
clm_popup_wind_m = image://AppHomeScreen/climate/clm_popup_wind_m
clm_popup_wind_b = image://AppHomeScreen/climate/clm_popup_wind_b
clm_rear_wind_02_b_0 = image://AppHomeScreen/climate/clm_rear_wind_02_b_0
clm_rear_wind_02_g_0 = image://AppHomeScreen/climate/clm_rear_wind_02_g_0
clm_rear_wind_02_r_0 = image://AppHomeScreen/climate/clm_rear_wind_02_r_0
clm_rear_wind_b_l_b_0 = image://AppHomeScreen/climate/clm_rear_wind_b_l_b_0
clm_rear_wind_b_l_g_0 = image://AppHomeScreen/climate/clm_rear_wind_b_l_g_0
clm_rear_wind_b_l_r_0 = image://AppHomeScreen/climate/clm_rear_wind_b_l_r_0
clm_rear_wind_b_r_b_0 = image://AppHomeScreen/climate/clm_rear_wind_b_r_b_0
clm_rear_wind_b_r_g_0 = image://AppHomeScreen/climate/clm_rear_wind_b_r_g_0
clm_rear_wind_b_r_r_0 = image://AppHomeScreen/climate/clm_rear_wind_b_r_r_0
clm_rear_wind_01_b_0 = image://AppHomeScreen/climate/clm_rear_wind_01_b_0
clm_rear_wind_01_g_0 = image://AppHomeScreen/climate/clm_rear_wind_01_g_0
clm_rear_wind_01_r_0 = image://AppHomeScreen/climate/clm_rear_wind_01_r_0
clm_bg_wind_rear_1_blue_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_1_blue_0
clm_bg_wind_rear_1_red_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_1_red_0
clm_bg_wind_rear_1_gray_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_1_gray_0
clm_bg_wind_rear_2_blue_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_2_blue_0
clm_bg_wind_rear_2_red_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_2_red_0
clm_bg_wind_rear_2_gray_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_2_gray_0
clm_bg_wind_rear_3_blue_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_3_blue_0
clm_bg_wind_rear_3_red_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_3_red_0
clm_bg_wind_rear_3_gray_0 = image://AppHomeScreen/climate/clm_bg_wind_rear_3_gray_0
clm_ic_seat_controls_01_n = image://AppHomeScreen/climate/clm_ic_seat_controls_01_n.png
clm_ic_seat_controls_01_f = image://AppHomeScreen/climate/clm_ic_seat_controls_01_f.png
clm_ic_seat_controls_01_p = image://AppHomeScreen/climate/clm_ic_seat_controls_01_p.png
clm_ic_seat_controls_01_d = image://AppHomeScreen/climate/clm_ic_seat_controls_01_d.png
clm_ic_seat_controls_01_s = image://AppHomeScreen/climate/clm_ic_seat_controls_01_s.png
clm_ic_seat_controls_02_n = image://AppHomeScreen/climate/clm_ic_seat_controls_02_n.png
clm_ic_seat_controls_02_f = image://AppHomeScreen/climate/clm_ic_seat_controls_02_f.png
clm_ic_seat_controls_02_p = image://AppHomeScreen/climate/clm_ic_seat_controls_02_p.png
clm_ic_seat_controls_02_d = image://AppHomeScreen/climate/clm_ic_seat_controls_02_d.png
clm_ic_seat_controls_02_s = image://AppHomeScreen/climate/clm_ic_seat_controls_02_s.png
clm_ic_seat_controls_03_n = image://AppHomeScreen/climate/clm_ic_seat_controls_03_n.png
clm_ic_seat_controls_03_f = image://AppHomeScreen/climate/clm_ic_seat_controls_03_f.png
clm_ic_seat_controls_03_p = image://AppHomeScreen/climate/clm_ic_seat_controls_03_p.png
clm_ic_seat_controls_03_d = image://AppHomeScreen/climate/clm_ic_seat_controls_03_d.png
clm_ic_seat_controls_03_s = image://AppHomeScreen/climate/clm_ic_seat_controls_03_s.png
clm_bg_list_bg = image://AppHomeScreen/climate/clm_bg_list_bg.png
clm_bg_blower_divider = image://AppHomeScreen/climate/clm_bg_blower_divider.png
clm_ic_pop_fan_speed_down_n = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_down_n.png
clm_ic_pop_fan_speed_down_p = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_down_p.png
clm_ic_pop_fan_speed_down_d = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_down_d.png
clm_ic_pop_fan_speed_up_n = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_up_n.png
clm_ic_pop_fan_speed_up_p = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_up_p.png
clm_ic_pop_fan_speed_up_d = image://AppHomeScreen/climate/clm_ic_pop_fan_speed_up_d.png
clm_ic_aircleaner = image://AppHomeScreen/climate/clm_ic_aircleaner_
clm_ic_aircleaner_l = image://AppHomeScreen/climate/clm_ic_aircleaner_l_
split_ic_aircleaner_l = image://AppHomeScreen/split/clm/split_ic_aircleaner_l_
clm_btn_circle_aircleaner_n = image://AppHomeScreen/climate/clm_btn_circle_aircleaner_n.png
clm_btn_circle_aircleaner_p = image://AppHomeScreen/climate/clm_btn_circle_aircleaner_p.png
clm_btn_led_04_on = image://AppHomeScreen/climate/clm_btn_led_04_on.png
clm_btn_led_04_off = image://AppHomeScreen/climate/clm_btn_led_04_off.png
clm_btn_pop_n = image://AppHomeScreen/climate/clm_btn_pop_n.png
clm_btn_pop_p = image://AppHomeScreen/climate/clm_btn_pop_p.png
clm_btn_pop_d = image://AppHomeScreen/climate/clm_btn_pop_d.png
clm_btn_wind_t_n = image://AppHomeScreen/climate/clm_btn_wind_t_n.png
clm_btn_wind_t_d = image://AppHomeScreen/climate/clm_btn_wind_t_d.png
clm_btn_wind_m_n = image://AppHomeScreen/climate/clm_btn_wind_m_n.png
clm_btn_wind_m_d = image://AppHomeScreen/climate/clm_btn_wind_m_d.png
clm_btn_wind_b_n = image://AppHomeScreen/climate/clm_btn_wind_b_n.png
clm_btn_wind_b_d = image://AppHomeScreen/climate/clm_btn_wind_b_d.png
clm_btn_pop_1_n = image://AppHomeScreen/climate/clm_btn_pop_1_n.png
clm_btn_pop_1_f = image://AppHomeScreen/climate/clm_btn_pop_1_f.png
clm_btn_pop_1_p = image://AppHomeScreen/climate/clm_btn_pop_1_p.png
clm_btn_pop_2_n = image://AppHomeScreen/climate/clm_btn_pop_2_n.png
clm_btn_pop_2_f = image://AppHomeScreen/climate/clm_btn_pop_2_f.png
clm_btn_pop_2_p = image://AppHomeScreen/climate/clm_btn_pop_2_p.png
clm_fan_popup_bg = image://AppHomeScreen/climate/clm_fan_popup_bg.png
clm_fan_popup_s_bg = image://AppHomeScreen/climate/clm_fan_popup_s_bg.png
clm_fan_popup_ss_bg = image://AppHomeScreen/climate/clm_fan_popup_ss_bg.png
clm_fan_popup_sss_bg = image://AppHomeScreen/climate/clm_fan_popup_sss_bg.png
clm_fan_popup_level_0 = image://AppHomeScreen/climate/clm_fan_popup_level_0
clm_fan_popup_level_s_0 = image://AppHomeScreen/climate/clm_fan_popup_level_s_0
clm_fan_popup_level_ss_0 = image://AppHomeScreen/climate/clm_fan_popup_level_ss_0
clm_fan_popup_level_sss_0 = image://AppHomeScreen/climate/clm_fan_popup_level_sss_0
clm_fan_popup_auto_02 = image://AppHomeScreen/climate/clm_fan_popup_auto_02.png
clm_fan_popup_auto_04 = image://AppHomeScreen/climate/clm_fan_popup_auto_04.png
clm_fan_popup_auto_01 = image://AppHomeScreen/climate/clm_fan_popup_auto_01.png
clm_fan_popup_auto_03 = image://AppHomeScreen/climate/clm_fan_popup_auto_03.png
clm_ic_pop_fan_2 = image://AppHomeScreen/climate/clm_ic_pop_fan_2.png
clm_popup_wind_auto = image://AppHomeScreen/climate/clm_popup_wind_auto.png
clm_bg_def = image://AppHomeScreen/climate/clm_bg_def.png
clm_bg_list_title = image://AppHomeScreen/climate/clm_bg_list_title.png
clm_bg_line = image://AppHomeScreen/climate/clm_bg_line.png
clm_btn_ic_up_d = image://AppHomeScreen/climate/clm_btn_ic_up_d.png
clm_btn_ic_up_n = image://AppHomeScreen/climate/clm_btn_ic_up_n.png
clm_btn_ic_up_r = image://AppHomeScreen/climate/clm_btn_ic_up_r.png
clm_btn_ic_down_d = image://AppHomeScreen/climate/clm_btn_ic_down_d.png
clm_btn_ic_down_n = image://AppHomeScreen/climate/clm_btn_ic_down_n.png
clm_btn_ic_down_b = image://AppHomeScreen/climate/clm_btn_ic_down_b.png
clm_btn_bg_bottom_n = image://AppHomeScreen/climate/clm_btn_bg_bottom_n.png
clm_btn_bg_bottom_f = image://AppHomeScreen/climate/clm_btn_bg_bottom_f.png
clm_btn_bg_bottom_p = image://AppHomeScreen/climate/clm_btn_bg_bottom_p.png
clm_car_wheel_bg = image://AppHomeScreen/climate/clm_car_wheel_bg.png
clm_car_wheel = image://AppHomeScreen/climate/clm_car_wheel.png
clm_wheel_wind_top_b = image://AppHomeScreen/climate/clm_wheel_wind_top_b_0
clm_wheel_wind_top_g = image://AppHomeScreen/climate/clm_wheel_wind_top_g_0
clm_wheel_wind_top_r = image://AppHomeScreen/climate/clm_wheel_wind_top_r_0
clm_wheel_wind_top_off = image://AppHomeScreen/climate/clm_wheel_wind_top_off+_0
clm_wheel_wind_mid_l_b = image://AppHomeScreen/climate/clm_wheel_wind_mid_l_b_0
clm_wheel_wind_mid_l_g = image://AppHomeScreen/climate/clm_wheel_wind_mid_l_g_0
clm_wheel_wind_mid_l_r = image://AppHomeScreen/climate/clm_wheel_wind_mid_l_r_0
clm_wheel_wind_mid_l_off = image://AppHomeScreen/climate/clm_wheel_wind_mid_l_off+_0
clm_wheel_wind_mid_r_b = image://AppHomeScreen/climate/clm_wheel_wind_mid_r_b_0
clm_wheel_wind_mid_r_g = image://AppHomeScreen/climate/clm_wheel_wind_mid_r_g_0
clm_wheel_wind_mid_r_r = image://AppHomeScreen/climate/clm_wheel_wind_mid_r_r_0
clm_wheel_wind_mid_r_off = image://AppHomeScreen/climate/clm_wheel_wind_mid_r_off+_0
clm_wheel_wind_bot_b = image://AppHomeScreen/climate/clm_wheel_wind_bot_b_0
clm_wheel_wind_bot_g = image://AppHomeScreen/climate/clm_wheel_wind_bot_g_0
clm_wheel_wind_bot_r = image://AppHomeScreen/climate/clm_wheel_wind_bot_r_0
clm_wheel_wind_bot_off = image://AppHomeScreen/climate/clm_wheel_wind_bot_off+_0
clm_bg_fan = image://AppHomeScreen/climate/clm_bg_fan.png
clm_bg_gauge_v_02 = image://AppHomeScreen/climate/clm_bg_gauge_v_02.png
clm_bg_seat = image://AppHomeScreen/climate/clm_bg_seat.png
clm_bg_wind_b = image://AppHomeScreen/climate/clm_bg_wind_b.png
clm_bg_wind_m = image://AppHomeScreen/climate/clm_bg_wind_m.png
clm_bg_wind_t = image://AppHomeScreen/climate/clm_bg_wind_t.png
clm_btn_bg_nor = image://AppHomeScreen/climate/clm_btn_bg_nor.png
clm_btn_bg_nor_02 = image://AppHomeScreen/climate/clm_btn_bg_nor_02.png
clm_btn_bg_push = image://AppHomeScreen/climate/clm_btn_bg_push.png
clm_btn_bg_push_02 = image://AppHomeScreen/climate/clm_btn_bg_push_02.png
clm_btn_bubble_l = image://AppHomeScreen/climate/clm_btn_bubble_l.png
clm_btn_bubble_r = image://AppHomeScreen/climate/clm_btn_bubble_r.png
clm_ic_set_air_top_8 = image://AppHomeScreen/climate/clm_ic_set_air_top_8.png
clm_ic_sw_heat_dis_02 = image://AppHomeScreen/climate/clm_ic_sw_heat_dis_02.png
clm_ic_sw_heat_nor_02 = image://AppHomeScreen/climate/clm_ic_sw_heat_nor_02.png
clm_ic_top_fan_01 = image://AppHomeScreen/climate/clm_ic_top_fan_01.png
clm_ic_top_fan_02 = image://AppHomeScreen/climate/clm_ic_top_fan_02.png
clm_btn_top_fan_push = image://AppHomeScreen/climate/clm_btn_top_fan_push.png
clm_ic_seat_controls_01_nor = image://AppHomeScreen/climate/clm_ic_seat_controls_01_nor.png
clm_ic_seat_controls_03_nor = image://AppHomeScreen/climate/clm_ic_seat_controls_03_nor.png
clm_ic_seat_controls_r_01_nor = image://AppHomeScreen/climate/clm_ic_seat_controls_r_01_nor.png
clm_ic_seat_controls_r_03_nor = image://AppHomeScreen/climate/clm_ic_seat_controls_r_03_nor.png
clm_btn_led_02_blue_on = image://AppHomeScreen/climate/clm_btn_led_02_blue_on.png
clm_btn_led_02_red_on = image://AppHomeScreen/climate/clm_btn_led_02_red_on.png
clm_btn_led_off_04 = image://AppHomeScreen/climate/clm_btn_led_off_04.png
clm_btn_led_red_on_02 = image://AppHomeScreen/climate/clm_btn_led_red_on_02.png
clm_ic_seat_nor = image://AppHomeScreen/climate/clm_ic_seat_nor.png
clm_ic_seat_heat_nor = image://AppHomeScreen/climate/clm_ic_seat_heat_nor.png
clm_ic_seat_vent_nor = image://AppHomeScreen/climate/clm_ic_seat_vent_nor.png
clm_bg_line_new = image://AppHomeScreen/climate/clm_bg_line_new.png
BATTERY_RANGE_COUNT = 20
am = image://AppHomeScreen/general/am.png
co_bg_drop_scroll = image://AppHomeScreen/general/co_bg_drop_scroll.png
co_bg_drop_scroll_b = image://AppHomeScreen/general/co_bg_drop_scroll_b.png
co_bg_drop_scroll_m = image://AppHomeScreen/general/co_bg_drop_scroll_m.png
co_bg_drop_scroll_t = image://AppHomeScreen/general/co_bg_drop_scroll_t.png
co_bg_drop_scroll_bar_b = image://AppHomeScreen/general/co_bg_drop_scroll_bar_b.png
co_bg_drop_scroll_bar_m = image://AppHomeScreen/general/co_bg_drop_scroll_bar_m.png
co_bg_drop_scroll_bar_t = image://AppHomeScreen/general/co_bg_drop_scroll_bar_t.png
co_btn_bg_list_324_l_f = image://AppHomeScreen/general/co_btn_bg_list_324_l_f.png
co_btn_bg_list_324_l_p = image://AppHomeScreen/general/co_btn_bg_list_324_l_p.png
co_btn_bg_list_324_l_s = image://AppHomeScreen/general/co_btn_bg_list_324_l_s.png
co_btn_bg_list_324_m_f = image://AppHomeScreen/general/co_btn_bg_list_324_m_f.png
co_btn_bg_list_324_m_p = image://AppHomeScreen/general/co_btn_bg_list_324_m_p.png
co_btn_bg_list_324_m_s = image://AppHomeScreen/general/co_btn_bg_list_324_m_s.png
co_btn_bg_list_324_r_f = image://AppHomeScreen/general/co_btn_bg_list_324_r_f.png
co_btn_bg_list_324_r_p = image://AppHomeScreen/general/co_btn_bg_list_324_r_p.png
co_btn_bg_list_324_r_s = image://AppHomeScreen/general/co_btn_bg_list_324_r_s.png
co_btn_bg_list_141_l_f = image://AppHomeScreen/general/co_btn_bg_list_141_l_f.png
co_btn_bg_list_141_l_p = image://AppHomeScreen/general/co_btn_bg_list_141_l_p.png
co_btn_bg_list_141_l_s = image://AppHomeScreen/general/co_btn_bg_list_141_l_s.png
co_btn_bg_list_141_m_f = image://AppHomeScreen/general/co_btn_bg_list_141_m_f.png
co_btn_bg_list_141_m_p = image://AppHomeScreen/general/co_btn_bg_list_141_m_p.png
co_btn_bg_list_141_m_s = image://AppHomeScreen/general/co_btn_bg_list_141_m_s.png
co_btn_bg_list_141_r_f = image://AppHomeScreen/general/co_btn_bg_list_141_r_f.png
co_btn_bg_list_141_r_p = image://AppHomeScreen/general/co_btn_bg_list_141_r_p.png
co_btn_bg_list_141_r_s = image://AppHomeScreen/general/co_btn_bg_list_141_r_s.png
co_btn_bg_list_147_l_f = image://AppHomeScreen/general/co_btn_bg_list_147_l_f.png
co_btn_bg_list_147_l_p = image://AppHomeScreen/general/co_btn_bg_list_147_l_p.png
co_btn_bg_list_147_l_s = image://AppHomeScreen/general/co_btn_bg_list_147_l_s.png
co_btn_bg_list_147_m_f = image://AppHomeScreen/general/co_btn_bg_list_147_m_f.png
co_btn_bg_list_147_m_p = image://AppHomeScreen/general/co_btn_bg_list_147_m_p.png
co_btn_bg_list_147_m_s = image://AppHomeScreen/general/co_btn_bg_list_147_m_s.png
co_btn_bg_list_147_r_f = image://AppHomeScreen/general/co_btn_bg_list_147_r_f.png
co_btn_bg_list_147_r_p = image://AppHomeScreen/general/co_btn_bg_list_147_r_p.png
co_btn_bg_list_147_r_s = image://AppHomeScreen/general/co_btn_bg_list_147_r_s.png
co_btn_bg_list_174_l_f = image://AppHomeScreen/general/co_btn_bg_list_174_l_f.png
co_btn_bg_list_174_l_p = image://AppHomeScreen/general/co_btn_bg_list_174_l_p.png
co_btn_bg_list_174_l_s = image://AppHomeScreen/general/co_btn_bg_list_174_l_s.png
co_btn_bg_list_174_m_f = image://AppHomeScreen/general/co_btn_bg_list_174_m_f.png
co_btn_bg_list_174_m_p = image://AppHomeScreen/general/co_btn_bg_list_174_m_p.png
co_btn_bg_list_174_m_s = image://AppHomeScreen/general/co_btn_bg_list_174_m_s.png
co_btn_bg_list_174_r_f = image://AppHomeScreen/general/co_btn_bg_list_174_r_f.png
co_btn_bg_list_174_r_p = image://AppHomeScreen/general/co_btn_bg_list_174_r_p.png
co_btn_bg_list_174_r_s = image://AppHomeScreen/general/co_btn_bg_list_174_r_s.png
co_btn_bg_list_186_l_f = image://AppHomeScreen/general/co_btn_bg_list_186_l_f.png
co_btn_bg_list_186_l_p = image://AppHomeScreen/general/co_btn_bg_list_186_l_p.png
co_btn_bg_list_186_l_s = image://AppHomeScreen/general/co_btn_bg_list_186_l_s.png
co_btn_bg_list_186_m_f = image://AppHomeScreen/general/co_btn_bg_list_186_m_f.png
co_btn_bg_list_186_m_p = image://AppHomeScreen/general/co_btn_bg_list_186_m_p.png
co_btn_bg_list_186_m_s = image://AppHomeScreen/general/co_btn_bg_list_186_m_s.png
co_btn_bg_list_186_r_f = image://AppHomeScreen/general/co_btn_bg_list_186_r_f.png
co_btn_bg_list_186_r_p = image://AppHomeScreen/general/co_btn_bg_list_186_r_p.png
co_btn_bg_list_186_r_s = image://AppHomeScreen/general/co_btn_bg_list_186_r_s.png
co_btn_check_off_f = image://AppHomeScreen/general/co_btn_check_off_f.png
co_btn_check_on_d = image://AppHomeScreen/general/co_btn_check_on_d.png
co_btn_check_on_f = image://AppHomeScreen/general/co_btn_check_on_f.png
co_ic_arrow_close = image://AppHomeScreen/general/co_ic_arrow_close.png
co_ic_arrow_open_d = image://AppHomeScreen/general/co_ic_arrow_open_d.png
co_ic_arrow_open_n = image://AppHomeScreen/general/co_ic_arrow_open_n.png
co_ic_drop_check = image://AppHomeScreen/general/co_ic_drop_check.png
co_ic_my_arrow_d = image://AppHomeScreen/general/co_ic_my_arrow_d.png
co_ic_my_arrow_n = image://AppHomeScreen/general/co_ic_my_arrow_n.png
ev_btn_bar_d = image://AppHomeScreen/ev/ev_btn_bar_d.png
ev_btn_bar_n = image://AppHomeScreen/ev/ev_btn_bar_n.png
ev_btn_bar_p = image://AppHomeScreen/ev/ev_btn_bar_p.png
ev_btn_box_h102_n = image://AppHomeScreen/ev/ev_btn_box_h102_n.png
ev_btn_box_h102_p = image://AppHomeScreen/ev/ev_btn_box_h102_p.png
ev_btn_box_h170_n = image://AppHomeScreen/ev/ev_btn_box_h170_n.png
ev_btn_box_h170_p = image://AppHomeScreen/ev/ev_btn_box_h170_p.png
ev_btn_box_h177_l_p = image://AppHomeScreen/ev/ev_btn_box_h177_l_p.png
ev_btn_box_h177_l_n = image://AppHomeScreen/ev/ev_btn_box_h177_l_n.png
ev_btn_box_h177_m_p = image://AppHomeScreen/ev/ev_btn_box_h177_m_p.png
ev_btn_box_h177_m_n = image://AppHomeScreen/ev/ev_btn_box_h177_m_n.png
ev_btn_box_h177_r_p = image://AppHomeScreen/ev/ev_btn_box_h177_r_p.png
ev_btn_box_h177_r_n = image://AppHomeScreen/ev/ev_btn_box_h177_r_n.png
ev_btn_set_f = image://AppHomeScreen/ev/ev_btn_set_f.png
ev_btn_set_n = image://AppHomeScreen/ev/ev_btn_set_n.png
ev_btn_set_p = image://AppHomeScreen/ev/ev_btn_set_p.png
ev_btn_set_l_f = image://AppHomeScreen/ev/ev_btn_set_l_f.png
ev_btn_set_l_n = image://AppHomeScreen/ev/ev_btn_set_l_n.png
ev_btn_set_l_p = image://AppHomeScreen/ev/ev_btn_set_l_p.png
ev_btn_set_l_d = image://AppHomeScreen/ev/ev_btn_set_l_d.png
ev_btn_zoomin_d = image://AppHomeScreen/ev/ev_btn_zoomin_d.png
ev_btn_zoomin_f = image://AppHomeScreen/ev/ev_btn_zoomin_f.png
ev_btn_zoomin_n = image://AppHomeScreen/ev/ev_btn_zoomin_n.png
ev_btn_zoomin_p = image://AppHomeScreen/ev/ev_btn_zoomin_p.png
ev_btn_zoomout_d = image://AppHomeScreen/ev/ev_btn_zoomout_d.png
ev_btn_zoomout_f = image://AppHomeScreen/ev/ev_btn_zoomout_f.png
ev_btn_zoomout_n = image://AppHomeScreen/ev/ev_btn_zoomout_n.png
ev_btn_zoomout_p = image://AppHomeScreen/ev/ev_btn_zoomout_p.png
ev_etc_bar_bubble = image://AppHomeScreen/ev/ev_etc_bar_bubble.png
ev_etc_bar_l = image://AppHomeScreen/ev/ev_etc_bar_l.png
ev_etc_bar_m = image://AppHomeScreen/ev/ev_etc_bar_m.png
ev_etc_bar_r = image://AppHomeScreen/ev/ev_etc_bar_r.png
ev_etc_graph_battery_l = image://AppHomeScreen/ev/ev_etc_graph_battery_l.png
ev_etc_graph_battery_m = image://AppHomeScreen/ev/ev_etc_graph_battery_m.png
ev_etc_graph_battery_r = image://AppHomeScreen/ev/ev_etc_graph_battery_r.png
ev_etc_graph_climate_l = image://AppHomeScreen/ev/ev_etc_graph_climate_l.png
ev_etc_graph_climate_m = image://AppHomeScreen/ev/ev_etc_graph_climate_m.png
ev_etc_graph_climate_r = image://AppHomeScreen/ev/ev_etc_graph_climate_r.png
ev_etc_graph_drive_l = image://AppHomeScreen/ev/ev_etc_graph_drive_l.png
ev_etc_graph_drive_m = image://AppHomeScreen/ev/ev_etc_graph_drive_m.png
ev_etc_graph_drive_r = image://AppHomeScreen/ev/ev_etc_graph_drive_r.png
ev_etc_graph_electro_l = image://AppHomeScreen/ev/ev_etc_graph_electro_l.png
ev_etc_graph_electro_m = image://AppHomeScreen/ev/ev_etc_graph_electro_m.png
ev_etc_graph_electro_r = image://AppHomeScreen/ev/ev_etc_graph_electro_r.png
ev_etc_peak_only = image://AppHomeScreen/ev/ev_etc_peak_only.png
ev_etc_peak_priority = image://AppHomeScreen/ev/ev_etc_peak_priority.png
ev_etc_charging_progress_bar = image://AppHomeScreen/ev/ev_etc_charging_progress_bar.png
ev_etc_charging_progress_bg = image://AppHomeScreen/ev/ev_etc_charging_progress_bg.png
ev_etc_set_au = image://AppHomeScreen/ev/ev_etc_set_au.png
ev_etc_set_eu = image://AppHomeScreen/ev/ev_etc_set_eu.png
ev_etc_set_s_au = image://AppHomeScreen/ev/ev_etc_set_s_au.png
ev_etc_set_s_eu = image://AppHomeScreen/ev/ev_etc_set_s_eu.png
ev_etc_set_box = image://AppHomeScreen/ev/ev_etc_set_box.png
ev_etc_set = image://AppHomeScreen/ev/ev_etc_set.png
ev_etc_set_s = image://AppHomeScreen/ev/ev_etc_set_s.png
ev_ic_charge_station = image://AppHomeScreen/ev/ev_ic_charge_station.png
ev_ic_battery_bg = image://AppHomeScreen/ev/ev_ic_battery_bg.png
ev_ic_battery_charging = image://AppHomeScreen/ev/ev_ic_battery_charging.png
ev_ic_battery_charging_02 = image://AppHomeScreen/ev/ev_ic_battery_charging_02.png
ev_ic_battery_p_bar_l = image://AppHomeScreen/ev/ev_ic_battery_p_bar_l.png
ev_ic_battery_p_bar_m = image://AppHomeScreen/ev/ev_ic_battery_p_bar_m.png
ev_ic_battery_p_bar_r = image://AppHomeScreen/ev/ev_ic_battery_p_bar_r.png
ev_ic_battery_p_bar_red_l = image://AppHomeScreen/ev/ev_ic_battery_p_bar_red_l.png
ev_ic_battery_p_bar_red_m = image://AppHomeScreen/ev/ev_ic_battery_p_bar_red_m.png
ev_ic_battery_p_bar_red_r = image://AppHomeScreen/ev/ev_ic_battery_p_bar_red_r.png
ev_ic_battery_p_bar_red = image://AppHomeScreen/ev/ev_ic_battery_p_bar_red.png
ev_ic_battery_p_bg = image://AppHomeScreen/ev/ev_ic_battery_p_bg.png
ev_ic_eco_co2 = image://AppHomeScreen/ev/ev_ic_eco_co2.png
ev_ic_eco_co2_arrow = image://AppHomeScreen/ev/ev_ic_eco_co2_arrow.png
ev_ic_co2 = image://AppHomeScreen/ev/ev_ic_co2.png
ev_ic_co2_arrow = image://AppHomeScreen/ev/ev_ic_co2_arrow.png
ev_ic_energy_charging = image://AppHomeScreen/ev/ev_ic_energy_charging.png
ev_ic_graph_climate = image://AppHomeScreen/ev/ev_ic_graph_climate.png
ev_ic_graph_driving = image://AppHomeScreen/ev/ev_ic_graph_driving.png
ev_ic_graph_electronics = image://AppHomeScreen/ev/ev_ic_graph_electronics.png
ev_ic_graph_battery = image://AppHomeScreen/ev/ev_ic_graph_battery.png
ev_ic_highest_fuel = image://AppHomeScreen/ev/ev_ic_highest_fuel.png
ev_ic_info_climate = image://AppHomeScreen/ev/ev_ic_info_climate.png
ev_ic_sch_charging = image://AppHomeScreen/ev/ev_ic_sch_charging.png
ev_ic_sch_climate = image://AppHomeScreen/ev/ev_ic_sch_climate.png
ev_ic_title_eco = image://AppHomeScreen/ev/ev_ic_title_eco.png
ev_list_history_l_s = image://AppHomeScreen/ev/ev_list_history_l_s.png
ev_list_history_m_s = image://AppHomeScreen/ev/ev_list_history_m_s.png
ev_list_history_r_s = image://AppHomeScreen/ev/ev_list_history_r_s.png
ev_list_line_l = image://AppHomeScreen/ev/ev_list_line_l.png
ev_list_line_m = image://AppHomeScreen/ev/ev_list_line_m.png
ev_list_line_r = image://AppHomeScreen/ev/ev_list_line_r.png
ico_ev = image://AppHomeScreen/ev/ev_ic_title_ev.png
ico_ev_arrow_down_d = image://AppHomeScreen/ev/ev_ic_arrow_down_d.png
ico_ev_arrow_down_n = image://AppHomeScreen/ev/ev_ic_arrow_down_n.png
ico_ev_arrow_up_d = image://AppHomeScreen/ev/ev_ic_arrow_up_d.png
ico_ev_arrow_up_n = image://AppHomeScreen/ev/ev_ic_arrow_up_n.png
ico_ev_charger_01 = image://AppHomeScreen/ev/ev_ic_charger_01.png
ico_ev_charger_02 = image://AppHomeScreen/ev/ev_ic_charger_02.png
ico_ev_charger_03 = image://AppHomeScreen/ev/ev_ic_charger_03.png
ico_ev_charging = image://AppHomeScreen/ev/ev_ic_charging.png
ico_ev_climate_n = image://AppHomeScreen/ev/ev_ic_climate_n.png
ico_ev_climate_d = image://AppHomeScreen/ev/ev_ic_climate_d.png
ico_ev_dash = image://AppHomeScreen/ev/ev_etc_dash.png
ico_ev_defog = image://AppHomeScreen/ev/ev_ic_defog.png
ico_ev_divider_b = image://AppHomeScreen/ev/ev_etc_divider_b.png
ico_ev_divider_m = image://AppHomeScreen/ev/ev_etc_divider_m.png
ico_ev_divider_t = image://AppHomeScreen/ev/ev_etc_divider_t.png
ico_ev_energy_bar = image://AppHomeScreen/ev/ev_ic_energy_bar.png
ico_ev_energy_bg = image://AppHomeScreen/ev/ev_ic_energy_bg.png
ico_ev_energy_s = image://AppHomeScreen/ev/ev_ic_energy_s.png
ico_ev_energy_sd = image://AppHomeScreen/ev/ev_ic_sd.png
ico_ev_energy_sd_s = image://AppHomeScreen/ev/ev_ic_sd_s.png
ico_ev_etc_bg = image://AppHomeScreen/ev/ev_etc_bg.png
ico_ev_setting_s = image://AppHomeScreen/ev/ev_ic_setting_s.png
ico_ev_title_info = image://AppHomeScreen/ev/ev_ic_title_info.png
ico_title_setting = image://AppHomeScreen/ev/ev_ic_title_setting.png
ev_ic_title_charge = image://AppHomeScreen/ev/ev_ic_title_charge.png
ev_ic_title_consumption = image://AppHomeScreen/ev/ev_ic_title_consumption.png
ev_ic_title_fuel = image://AppHomeScreen/ev/ev_ic_title_fuel.png
light = image://AppHomeScreen/popup/light.png
loading_01 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_01.png
loading_02 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_02.png
loading_03 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_03.png
loading_04 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_04.png
loading_05 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_05.png
loading_06 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_06.png
loading_07 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_07.png
loading_08 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_08.png
loading_09 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_09.png
loading_10 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_10.png
loading_11 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_11.png
loading_12 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_12.png
loading_13 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_13.png
loading_14 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_14.png
loading_15 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_15.png
loading_16 = image://AppHomeScreen/general/loading_l/co_ic_loading_l_16.png
img_loading = ?
loading_01_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_01.png
loading_02_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_02.png
loading_03_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_03.png
loading_04_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_04.png
loading_05_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_05.png
loading_06_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_06.png
loading_07_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_07.png
loading_08_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_08.png
loading_09_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_09.png
loading_10_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_10.png
loading_11_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_11.png
loading_12_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_12.png
loading_13_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_13.png
loading_14_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_14.png
loading_15_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_15.png
loading_16_s = image://AppHomeScreen/general/loading_s/co_ic_loading_s_16.png
loading_s = ?
co_ic_loading_00 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_00.png
co_ic_loading_01 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_01.png
co_ic_loading_02 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_02.png
co_ic_loading_03 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_03.png
co_ic_loading_04 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_04.png
co_ic_loading_05 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_05.png
co_ic_loading_06 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_06.png
co_ic_loading_07 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_07.png
co_ic_loading_08 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_08.png
co_ic_loading_09 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_09.png
co_ic_loading_10 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_10.png
co_ic_loading_11 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_11.png
co_ic_loading_12 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_12.png
co_ic_loading_13 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_13.png
co_ic_loading_14 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_14.png
co_ic_loading_15 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_15.png
co_ic_loading_16 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_16.png
co_ic_loading_17 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_17.png
co_ic_loading_18 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_18.png
co_ic_loading_19 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_19.png
co_ic_loading_20 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_20.png
co_ic_loading_21 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_21.png
co_ic_loading_22 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_22.png
co_ic_loading_23 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_23.png
co_ic_loading_24 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_24.png
co_ic_loading_25 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_25.png
co_ic_loading_26 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_26.png
co_ic_loading_27 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_27.png
co_ic_loading_28 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_28.png
co_ic_loading_29 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_29.png
co_ic_loading_30 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_30.png
co_ic_loading_31 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_31.png
co_ic_loading_32 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_32.png
co_ic_loading_33 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_33.png
co_ic_loading_34 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_34.png
co_ic_loading_35 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_35.png
co_ic_loading_36 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_36.png
co_ic_loading_37 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_37.png
co_ic_loading_38 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_38.png
co_ic_loading_39 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_39.png
co_ic_loading_40 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_40.png
co_ic_loading_41 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_41.png
co_ic_loading_42 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_42.png
co_ic_loading_43 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_43.png
co_ic_loading_44 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_44.png
co_ic_loading_45 = image://AppHomeScreen/general/loading/co_ic_loading_l/co_ic_loading_l_45.png
co_ic_loading_s = ?
co_ic_loading_s_00 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_00.png
co_ic_loading_s_01 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_01.png
co_ic_loading_s_02 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_02.png
co_ic_loading_s_03 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_03.png
co_ic_loading_s_04 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_04.png
co_ic_loading_s_05 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_05.png
co_ic_loading_s_06 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_06.png
co_ic_loading_s_07 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_07.png
co_ic_loading_s_08 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_08.png
co_ic_loading_s_09 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_09.png
co_ic_loading_s_10 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_10.png
co_ic_loading_s_11 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_11.png
co_ic_loading_s_12 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_12.png
co_ic_loading_s_13 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_13.png
co_ic_loading_s_14 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_14.png
co_ic_loading_s_15 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_15.png
co_ic_loading_s_16 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_16.png
co_ic_loading_s_17 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_17.png
co_ic_loading_s_18 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_18.png
co_ic_loading_s_19 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_19.png
co_ic_loading_s_20 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_20.png
co_ic_loading_s_21 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_21.png
co_ic_loading_s_22 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_22.png
co_ic_loading_s_23 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_23.png
co_ic_loading_s_24 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_24.png
co_ic_loading_s_25 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_25.png
co_ic_loading_s_26 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_26.png
co_ic_loading_s_27 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_27.png
co_ic_loading_s_28 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_28.png
co_ic_loading_s_29 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_29.png
co_ic_loading_s_30 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_30.png
co_ic_loading_s_31 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_31.png
co_ic_loading_s_32 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_32.png
co_ic_loading_s_33 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_33.png
co_ic_loading_s_34 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_34.png
co_ic_loading_s_35 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_35.png
co_ic_loading_s_36 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_36.png
co_ic_loading_s_37 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_37.png
co_ic_loading_s_38 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_38.png
co_ic_loading_s_39 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_39.png
co_ic_loading_s_40 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_40.png
co_ic_loading_s_41 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_41.png
co_ic_loading_s_42 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_42.png
co_ic_loading_s_43 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_43.png
co_ic_loading_s_44 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_44.png
co_ic_loading_s_45 = image://AppHomeScreen/general/loading/co_ic_loading_s/co_ic_loading_s_45.png
pm = image://AppHomeScreen/general/pm.png
scroll = image://AppHomeScreen/popup/scroll.png
scroll_bg = image://AppHomeScreen/popup/scroll_bg.png
set_ic_colon = image://AppHomeScreen/setup/set_ic_colon.png
ev_img_rangewarning = image://AppHomeScreen/ev/ev_img_rangewarning.png
co_menu_check_on_f = image://AppHomeScreen/general/co_menu_check_on_f.png
co_menu_check_on_p = image://AppHomeScreen/general/co_menu_check_on_p.png
co_menu_check_on_d = image://AppHomeScreen/general/co_menu_check_on_d.png
co_menu_check_on_df = image://AppHomeScreen/general/co_menu_check_off_df.png
co_menu_check_off_f = image://AppHomeScreen/general/co_menu_check_off_f.png
co_menu_check_off_p = image://AppHomeScreen/general/co_menu_check_off_p.png
co_menu_check_off_d = image://AppHomeScreen/general/co_menu_check_off_d.png
co_menu_check_off_df = image://AppHomeScreen/general/co_menu_check_off_df.png
ev_ic_phev_battery_bg = image://AppHomeScreen/ev/ev_ic_phev_battery_bg.png
co_btn_title_tab_96_l_n = image://AppHomeScreen/general/co_btn_title_tab_96_l_n.png
co_btn_title_tab_96_l_p = image://AppHomeScreen/general/co_btn_title_tab_96_l_p.png
co_btn_title_tab_96_l_s = image://AppHomeScreen/general/co_btn_title_tab_96_l_s.png
co_btn_title_tab_96_m_n = image://AppHomeScreen/general/co_btn_title_tab_96_m_n.png
co_btn_title_tab_96_m_p = image://AppHomeScreen/general/co_btn_title_tab_96_m_p.png
co_btn_title_tab_96_m_s = image://AppHomeScreen/general/co_btn_title_tab_96_m_s.png
co_btn_title_tab_96_r_n = image://AppHomeScreen/general/co_btn_title_tab_96_r_n.png
co_btn_title_tab_96_r_p = image://AppHomeScreen/general/co_btn_title_tab_96_r_p.png
co_btn_title_tab_96_r_s = image://AppHomeScreen/general/co_btn_title_tab_96_r_s.png
co_bg_bottom_tabline = image://AppHomeScreen/general/co_bg_bottom_tabline.png
ev_etc_indi_aver = image://AppHomeScreen/ev/ev_etc_indi_aver.png
ev_etc_indi_fuel = image://AppHomeScreen/ev/ev_etc_indi_fuel.png
ev_etc_indi_electric = image://AppHomeScreen/ev/ev_etc_indi_electric.png
ev_etc_graph_bg = image://AppHomeScreen/ev/ev_etc_graph_bg.png
ev_etc_graph_fuel = image://AppHomeScreen/ev/ev_etc_graph_fuel.png
ev_etc_graph_electric = image://AppHomeScreen/ev/ev_etc_graph_electric.png
ev_etc_graph_aver_dot = image://AppHomeScreen/ev/ev_etc_graph_aver_dot.png
ev_etc_eco_aver_line = image://AppHomeScreen/ev/ev_etc_eco_aver_line.png
ev_ic_title_flow = image://AppHomeScreen/ev/ev_ic_title_flow.png
ev_etc_eco_graph_bg_02 = image://AppHomeScreen/ev/ev_etc_eco_graph_bg_02.png
ev_etc_eco_aver_line_02 = image://AppHomeScreen/ev/ev_etc_eco_aver_line_02.png
ev_ic_energy_info = image://AppHomeScreen/ev/ev_ic_energy_info.png
ev_etc_eco_graph_bg = image://AppHomeScreen/ev/ev_etc_eco_graph_bg.png
hev_etc_bg_line = image://AppHomeScreen/ev/hev_etc_bg_line.png
hev_flow_car_bg = image://AppHomeScreen/ev/hev_flow_car_bg.png
hev_flow_bg_1 = image://AppHomeScreen/ev/hev_flow_bg_1.png
hev_flow_bg_3 = image://AppHomeScreen/ev/hev_flow_bg_3.png
hev_flow_car_bg_s = image://AppHomeScreen/ev/hev_flow_car_bg_s.png
hev_flow_bg_s_1 = image://AppHomeScreen/ev/hev_flow_bg_s_1.png
hev_flow_engine = image://AppHomeScreen/ev/hev_flow_engine.png
hev_flow_mortor = image://AppHomeScreen/ev/hev_flow_mortor.png
hev_flow_battery = image://AppHomeScreen/ev/hev_flow_battery.png
hev_flow_battery_02 = image://AppHomeScreen/ev/hev_flow_battery_02.png
hev_ic_battery_bar_l = image://AppHomeScreen/ev/hev_ic_battery_bar_l.png
hev_ic_battery_bar_m = image://AppHomeScreen/ev/hev_ic_battery_bar_m.png
hev_ic_battery_bar_r = image://AppHomeScreen/ev/hev_ic_battery_bar_r.png
hev_flow_wheel = image://AppHomeScreen/ev/hev_flow_wheel.png
hev_flow_eng_mot_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_01.png
hev_flow_eng_mot_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_02.png
hev_flow_eng_mot_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_03.png
hev_flow_eng_mot_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_04.png
hev_flow_eng_mot_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_05.png
hev_flow_eng_mot_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_06.png
hev_flow_eng_mot_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_07.png
hev_flow_eng_mot_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_08.png
hev_flow_eng_mot_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_09.png
hev_flow_eng_mot_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_10.png
hev_flow_eng_mot_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_11.png
hev_flow_eng_mot_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_12.png
hev_flow_eng_mot_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_13.png
hev_flow_eng_mot_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_14.png
hev_flow_eng_mot_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_15.png
hev_flow_eng_mot_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_16.png
hev_flow_eng_mot_a_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_hr.png
hev_flow_mot_bat_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_01.png
hev_flow_mot_bat_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_02.png
hev_flow_mot_bat_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_03.png
hev_flow_mot_bat_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_04.png
hev_flow_mot_bat_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_05.png
hev_flow_mot_bat_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_06.png
hev_flow_mot_bat_a_hr = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_hr.png
hev_flow_mot_bat_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_07.png
hev_flow_mot_bat_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_08.png
hev_flow_mot_bat_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_09.png
hev_flow_mot_bat_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_10.png
hev_flow_mot_bat_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_11.png
hev_flow_mot_bat_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_12.png
hev_flow_mot_bat_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_13.png
hev_flow_mot_bat_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_14.png
hev_flow_mot_bat_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_15.png
hev_flow_mot_bat_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_16.png
hev_flow_mot_bat_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_17.png
hev_flow_mot_bat_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_18.png
hev_flow_mot_bat_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_19.png
hev_flow_mot_bat_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_20.png
hev_flow_mot_bat_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_21.png
hev_flow_mot_bat_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_22.png
hev_flow_mot_bat_a_23 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_23.png
hev_flow_mot_bat_a_24 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_24.png
hev_flow_mot_bat_a_25 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_25.png
hev_flow_mot_bat_a_26 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_26.png
hev_flow_mot_bat_a_27 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_27.png
hev_flow_mot_bat_a_28 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_28.png
hev_flow_mot_bat_a_29 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_29.png
hev_flow_mot_bat_a_30 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_30.png
hev_flow_mot_bat_a_31 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_31.png
hev_flow_mot_bat_a_32 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_32.png
hev_flow_mot_bat_a_33 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_33.png
hev_flow_mot_bat_a_34 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_34.png
hev_flow_mot_bat_a_35 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_35.png
hev_flow_mot_bat_a_36 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_36.png
hev_flow_bat_mot_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_01.png
hev_flow_bat_mot_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_02.png
hev_flow_bat_mot_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_03.png
hev_flow_bat_mot_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_04.png
hev_flow_bat_mot_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_05.png
hev_flow_bat_mot_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_06.png
hev_flow_bat_mot_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_07.png
hev_flow_bat_mot_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_08.png
hev_flow_bat_mot_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_09.png
hev_flow_bat_mot_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_10.png
hev_flow_bat_mot_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_11.png
hev_flow_bat_mot_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_12.png
hev_flow_bat_mot_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_13.png
hev_flow_bat_mot_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_14.png
hev_flow_bat_mot_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_15.png
hev_flow_bat_mot_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_16.png
hev_flow_bat_mot_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_17.png
hev_flow_bat_mot_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_18.png
hev_flow_eng_whe_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_01.png
hev_flow_eng_whe_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_02.png
hev_flow_eng_whe_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_03.png
hev_flow_eng_whe_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_04.png
hev_flow_eng_whe_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_05.png
hev_flow_eng_whe_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_06.png
hev_flow_eng_whe_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_07.png
hev_flow_eng_whe_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_08.png
hev_flow_eng_whe_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_09.png
hev_flow_eng_whe_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_10.png
hev_flow_eng_whe_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_11.png
hev_flow_eng_whe_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_12.png
hev_flow_eng_whe_a_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_hr.png
hev_flow_whe_eng_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_01.png
hev_flow_whe_eng_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_02.png
hev_flow_whe_eng_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_03.png
hev_flow_whe_eng_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_04.png
hev_flow_whe_eng_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_05.png
hev_flow_whe_eng_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_06.png
hev_flow_whe_eng_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_07.png
hev_flow_whe_eng_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_08.png
hev_flow_whe_eng_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_09.png
hev_flow_whe_eng_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_10.png
hev_flow_whe_eng_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_11.png
hev_flow_whe_eng_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_12.png
hev_flow_whe_mot_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_01.png
hev_flow_whe_mot_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_02.png
hev_flow_whe_mot_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_03.png
hev_flow_whe_mot_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_04.png
hev_flow_whe_mot_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_05.png
hev_flow_whe_mot_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_06.png
hev_flow_whe_mot_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_07.png
hev_flow_whe_mot_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_08.png
hev_flow_whe_mot_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_09.png
hev_flow_whe_mot_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_10.png
hev_flow_whe_mot_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_11.png
hev_flow_whe_mot_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_12.png
hev_flow_whe_mot_a_hr = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_hr.png
hev_flow_mot_whe_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_01.png
hev_flow_mot_whe_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_02.png
hev_flow_mot_whe_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_03.png
hev_flow_mot_whe_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_04.png
hev_flow_mot_whe_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_05.png
hev_flow_mot_whe_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_06.png
hev_flow_mot_whe_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_07.png
hev_flow_mot_whe_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_08.png
hev_flow_mot_whe_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_09.png
hev_flow_mot_whe_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_10.png
hev_flow_mot_whe_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_11.png
hev_flow_mot_whe_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_12.png
hev_flow_eng_mot_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_01.png
hev_flow_eng_mot_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_02.png
hev_flow_eng_mot_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_03.png
hev_flow_eng_mot_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_04.png
hev_flow_eng_mot_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_05.png
hev_flow_eng_mot_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_06.png
hev_flow_eng_mot_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_07.png
hev_flow_eng_mot_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_08.png
hev_flow_eng_mot_b_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_hr.png
hev_flow_mot_bat_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_01.png
hev_flow_mot_bat_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_02.png
hev_flow_mot_bat_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_03.png
hev_flow_mot_bat_b_hr = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_hr.png
hev_flow_mot_bat_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_04.png
hev_flow_mot_bat_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_05.png
hev_flow_mot_bat_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_06.png
hev_flow_mot_bat_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_07.png
hev_flow_mot_bat_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_08.png
hev_flow_mot_bat_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_09.png
hev_flow_mot_bat_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_10.png
hev_flow_mot_bat_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_11.png
hev_flow_mot_bat_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_12.png
hev_flow_mot_bat_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_13.png
hev_flow_mot_bat_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_14.png
hev_flow_mot_bat_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_15.png
hev_flow_mot_bat_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_16.png
hev_flow_mot_bat_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_17.png
hev_flow_mot_bat_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_18.png
hev_flow_mot_bat_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_19.png
hev_flow_mot_bat_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_20.png
hev_flow_mot_bat_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_21.png
hev_flow_mot_bat_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_22.png
hev_flow_mot_bat_b_23 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_23.png
hev_flow_mot_bat_b_24 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_24.png
hev_flow_mot_bat_b_25 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_25.png
hev_flow_mot_bat_b_26 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_26.png
hev_flow_mot_bat_b_27 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_27.png
hev_flow_mot_bat_b_28 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_28.png
hev_flow_mot_bat_b_29 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_29.png
hev_flow_mot_bat_b_30 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_30.png
hev_flow_mot_bat_b_31 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_31.png
hev_flow_mot_bat_b_32 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_32.png
hev_flow_mot_bat_b_33 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_33.png
hev_flow_mot_bat_b_34 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_34.png
hev_flow_mot_bat_b_35 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_35.png
hev_flow_mot_bat_b_36 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_36.png
hev_flow_mot_bat_b_37 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_37.png
hev_flow_mot_bat_b_38 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_38.png
hev_flow_mot_bat_b_39 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_39.png
hev_flow_mot_bat_b_40 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_40.png
hev_flow_mot_bat_b_41 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_41.png
hev_flow_mot_bat_b_42 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_42.png
hev_flow_mot_bat_b_43 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_43.png
hev_flow_mot_bat_b_44 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_b_44.png
hev_flow_bat_mot_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_01.png
hev_flow_bat_mot_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_02.png
hev_flow_bat_mot_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_03.png
hev_flow_bat_mot_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_04.png
hev_flow_bat_mot_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_05.png
hev_flow_bat_mot_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_06.png
hev_flow_bat_mot_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_07.png
hev_flow_bat_mot_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_08.png
hev_flow_bat_mot_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_09.png
hev_flow_bat_mot_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_10.png
hev_flow_bat_mot_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_11.png
hev_flow_bat_mot_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_12.png
hev_flow_bat_mot_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_13.png
hev_flow_bat_mot_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_14.png
hev_flow_bat_mot_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_15.png
hev_flow_bat_mot_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_16.png
hev_flow_bat_mot_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_17.png
hev_flow_bat_mot_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_18.png
hev_flow_bat_mot_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_19.png
hev_flow_bat_mot_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_20.png
hev_flow_bat_mot_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_21.png
hev_flow_bat_mot_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_b_22.png
hev_flow_sol_bat_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_01.png
hev_flow_sol_bat_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_02.png
hev_flow_sol_bat_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_03.png
hev_flow_sol_bat_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_04.png
hev_flow_sol_bat_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_05.png
hev_flow_sol_bat_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_06.png
hev_flow_sol_bat_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_07.png
hev_flow_sol_bat_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_08.png
hev_flow_sol_bat_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_09.png
hev_flow_sol_bat_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_10.png
hev_flow_sol_bat_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_11.png
hev_flow_sol_bat_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_12.png
hev_flow_sol_bat_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_13.png
hev_flow_sol_bat_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_14.png
hev_flow_sol_bat_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_15.png
hev_flow_sol_bat_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_16.png
hev_flow_sol_bat_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_17.png
hev_flow_sol_bat_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_18.png
hev_flow_sol_bat_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_19.png
hev_flow_sol_bat_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_20.png
hev_flow_sol_bat_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_21.png
hev_flow_sol_bat_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_b_22.png
hev_flow_eng_whe_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_01.png
hev_flow_eng_whe_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_02.png
hev_flow_eng_whe_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_03.png
hev_flow_eng_whe_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_04.png
hev_flow_eng_whe_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_05.png
hev_flow_eng_whe_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_06.png
hev_flow_eng_whe_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_07.png
hev_flow_eng_whe_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_08.png
hev_flow_eng_whe_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_09.png
hev_flow_eng_whe_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_10.png
hev_flow_eng_whe_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_11.png
hev_flow_eng_whe_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_12.png
hev_flow_eng_whe_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_13.png
hev_flow_eng_whe_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_14.png
hev_flow_eng_whe_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_15.png
hev_flow_eng_whe_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_16.png
hev_flow_eng_whe_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_17.png
hev_flow_eng_whe_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_18.png
hev_flow_eng_whe_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_19.png
hev_flow_eng_whe_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_20.png
hev_flow_eng_whe_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_21.png
hev_flow_eng_whe_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_22.png
hev_flow_eng_whe_b_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_b_hr.png
hev_flow_whe_eng_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_01.png
hev_flow_whe_eng_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_02.png
hev_flow_whe_eng_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_03.png
hev_flow_whe_eng_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_04.png
hev_flow_whe_eng_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_05.png
hev_flow_whe_eng_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_06.png
hev_flow_whe_eng_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_07.png
hev_flow_whe_eng_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_08.png
hev_flow_whe_mot_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_01.png
hev_flow_whe_mot_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_02.png
hev_flow_whe_mot_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_03.png
hev_flow_whe_mot_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_04.png
hev_flow_whe_mot_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_05.png
hev_flow_whe_mot_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_06.png
hev_flow_whe_mot_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_07.png
hev_flow_whe_mot_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_08.png
hev_flow_whe_mot_b_hr = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_hr.png
hev_flow_mot_whe_b_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_01.png
hev_flow_mot_whe_b_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_02.png
hev_flow_mot_whe_b_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_03.png
hev_flow_mot_whe_b_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_04.png
hev_flow_mot_whe_b_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_05.png
hev_flow_mot_whe_b_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_06.png
hev_flow_mot_whe_b_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_07.png
hev_flow_mot_whe_b_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_08.png
hev_flow_eng_mot_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_01.png
hev_flow_eng_mot_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_02.png
hev_flow_eng_mot_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_03.png
hev_flow_eng_mot_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_04.png
hev_flow_eng_mot_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_05.png
hev_flow_eng_mot_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_06.png
hev_flow_eng_mot_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_07.png
hev_flow_eng_mot_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_08.png
hev_flow_eng_mot_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_09.png
hev_flow_eng_mot_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_10.png
hev_flow_eng_mot_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_11.png
hev_flow_eng_mot_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_12.png
hev_flow_eng_mot_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_13.png
hev_flow_eng_mot_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_14.png
hev_flow_eng_mot_c_15 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_15.png
hev_flow_eng_mot_c_16 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_16.png
hev_flow_eng_mot_c_17 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_17.png
hev_flow_eng_mot_c_18 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_18.png
hev_flow_eng_mot_c_19 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_19.png
hev_flow_eng_mot_c_20 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_20.png
hev_flow_eng_mot_c_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_c_hr.png
hev_flow_mot_bat_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_01.png
hev_flow_mot_bat_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_02.png
hev_flow_mot_bat_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_03.png
hev_flow_mot_bat_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_04.png
hev_flow_mot_bat_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_05.png
hev_flow_mot_bat_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_06.png
hev_flow_mot_bat_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_07.png
hev_flow_mot_bat_c_hr = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_hr.png
hev_flow_mot_bat_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_08.png
hev_flow_mot_bat_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_09.png
hev_flow_mot_bat_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_10.png
hev_flow_mot_bat_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_11.png
hev_flow_mot_bat_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_12.png
hev_flow_mot_bat_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_13.png
hev_flow_mot_bat_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_14.png
hev_flow_mot_bat_c_15 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_15.png
hev_flow_mot_bat_c_16 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_16.png
hev_flow_mot_bat_c_17 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_17.png
hev_flow_mot_bat_c_18 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_18.png
hev_flow_mot_bat_c_19 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_19.png
hev_flow_mot_bat_c_20 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_20.png
hev_flow_mot_bat_c_21 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_21.png
hev_flow_mot_bat_c_22 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_22.png
hev_flow_mot_bat_c_23 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_23.png
hev_flow_mot_bat_c_24 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_24.png
hev_flow_mot_bat_c_25 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_25.png
hev_flow_mot_bat_c_26 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_26.png
hev_flow_mot_bat_c_27 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_27.png
hev_flow_mot_bat_c_28 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_28.png
hev_flow_mot_bat_c_29 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_29.png
hev_flow_mot_bat_c_30 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_30.png
hev_flow_mot_bat_c_31 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_31.png
hev_flow_mot_bat_c_32 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_32.png
hev_flow_mot_bat_c_33 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_33.png
hev_flow_mot_bat_c_34 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_34.png
hev_flow_mot_bat_c_35 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_35.png
hev_flow_mot_bat_c_36 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_36.png
hev_flow_mot_bat_c_37 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_37.png
hev_flow_mot_bat_c_38 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_38.png
hev_flow_mot_bat_c_39 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_39.png
hev_flow_mot_bat_c_40 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_c_40.png
hev_flow_bat_mot_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_01.png
hev_flow_bat_mot_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_02.png
hev_flow_bat_mot_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_03.png
hev_flow_bat_mot_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_04.png
hev_flow_bat_mot_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_05.png
hev_flow_bat_mot_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_06.png
hev_flow_bat_mot_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_07.png
hev_flow_bat_mot_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_08.png
hev_flow_bat_mot_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_09.png
hev_flow_bat_mot_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_10.png
hev_flow_bat_mot_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_11.png
hev_flow_bat_mot_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_12.png
hev_flow_bat_mot_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_13.png
hev_flow_bat_mot_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_14.png
hev_flow_bat_mot_c_15 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_15.png
hev_flow_bat_mot_c_16 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_16.png
hev_flow_bat_mot_c_17 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_17.png
hev_flow_bat_mot_c_18 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_18.png
hev_flow_bat_mot_c_19 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_19.png
hev_flow_bat_mot_c_20 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_c_20.png
hev_flow_eng_whe_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_01.png
hev_flow_eng_whe_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_02.png
hev_flow_eng_whe_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_03.png
hev_flow_eng_whe_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_04.png
hev_flow_eng_whe_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_05.png
hev_flow_eng_whe_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_06.png
hev_flow_eng_whe_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_07.png
hev_flow_eng_whe_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_08.png
hev_flow_eng_whe_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_09.png
hev_flow_eng_whe_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_10.png
hev_flow_eng_whe_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_11.png
hev_flow_eng_whe_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_12.png
hev_flow_eng_whe_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_13.png
hev_flow_eng_whe_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_14.png
hev_flow_eng_whe_c_hr = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_c_hr.png
hev_flow_whe_eng_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_01.png
hev_flow_whe_eng_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_02.png
hev_flow_whe_eng_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_03.png
hev_flow_whe_eng_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_04.png
hev_flow_whe_eng_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_05.png
hev_flow_whe_eng_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_06.png
hev_flow_whe_eng_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_07.png
hev_flow_whe_eng_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_08.png
hev_flow_whe_eng_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_09.png
hev_flow_whe_eng_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_10.png
hev_flow_whe_eng_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_11.png
hev_flow_whe_eng_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_12.png
hev_flow_whe_eng_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_13.png
hev_flow_whe_eng_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_c_14.png
hev_flow_whe_mot_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_01.png
hev_flow_whe_mot_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_02.png
hev_flow_whe_mot_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_03.png
hev_flow_whe_mot_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_04.png
hev_flow_whe_mot_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_05.png
hev_flow_whe_mot_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_06.png
hev_flow_whe_mot_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_07.png
hev_flow_whe_mot_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_08.png
hev_flow_whe_mot_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_09.png
hev_flow_whe_mot_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_10.png
hev_flow_whe_mot_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_11.png
hev_flow_whe_mot_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_12.png
hev_flow_whe_mot_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_13.png
hev_flow_whe_mot_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_14.png
hev_flow_whe_mot_c_hr = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_c_hr.png
hev_flow_mot_whe_c_01 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_01.png
hev_flow_mot_whe_c_02 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_02.png
hev_flow_mot_whe_c_03 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_03.png
hev_flow_mot_whe_c_04 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_04.png
hev_flow_mot_whe_c_05 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_05.png
hev_flow_mot_whe_c_06 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_06.png
hev_flow_mot_whe_c_07 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_07.png
hev_flow_mot_whe_c_08 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_08.png
hev_flow_mot_whe_c_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_09.png
hev_flow_mot_whe_c_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_10.png
hev_flow_mot_whe_c_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_11.png
hev_flow_mot_whe_c_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_12.png
hev_flow_mot_whe_c_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_13.png
hev_flow_mot_whe_c_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_c_14.png
phev_etc_indi_aver = image://AppHomeScreen/ev/phev_etc_indi_aver.png
phev_ic_Nearby = image://AppHomeScreen/ev/phev_ic_Nearby.png
phev_ic_arrow = image://AppHomeScreen/ev/phev_ic_arrow.png
phev_ic_arrow_ = image://AppHomeScreen/ev/phev_ic_arrow_
phev_ic_battery_bg = image://AppHomeScreen/ev/phev_ic_battery_bg.png
phev_ic_battery_charging = image://AppHomeScreen/ev/phev_ic_battery_charging.png
phev_ic_main_charging = image://AppHomeScreen/ev/phev_ic_main_charging.png
phev_ic_main_climate = image://AppHomeScreen/ev/phev_ic_main_climate.png
phev_ic_main_eco = image://AppHomeScreen/ev/phev_ic_main_eco.png
phev_ic_main_flow = image://AppHomeScreen/ev/phev_ic_main_flow.png
phev_bg_list_line_h = image://AppHomeScreen/ev/phev_bg_list_line_h.png
phev_bg_box_info_02 = image://AppHomeScreen/ev/phev_bg_box_info_02.png
phev_bg_box_info_01 = image://AppHomeScreen/ev/phev_bg_box_info_01.png
ev_bg_box_105_d = image://AppHomeScreen/ev/ev_bg_box_105_d.png
ev_bg_box_105_f = image://AppHomeScreen/ev/ev_bg_box_105_f.png
ev_bg_box_105_n = image://AppHomeScreen/ev/ev_bg_box_105_n.png
ev_bg_box_105_p = image://AppHomeScreen/ev/ev_bg_box_105_p.png
ev_bg_box_78_d = image://AppHomeScreen/ev/ev_bg_box_78_d.png
ev_bg_box_78_f = image://AppHomeScreen/ev/ev_bg_box_78_f.png
ev_bg_box_78_n = image://AppHomeScreen/ev/ev_bg_box_78_n.png
ev_bg_box_78_p = image://AppHomeScreen/ev/ev_bg_box_78_p.png
ev_btn_bg_h66_l_f = image://AppHomeScreen/ev/ev_btn_bg_h66_l_f.png
ev_btn_bg_h66_l_n = image://AppHomeScreen/ev/ev_btn_bg_h66_l_n.png
ev_btn_bg_h66_l_p = image://AppHomeScreen/ev/ev_btn_bg_h66_l_p.png
ev_btn_bg_h66_l_s = image://AppHomeScreen/ev/ev_btn_bg_h66_l_s.png
ev_btn_bg_h66_m_f = image://AppHomeScreen/ev/ev_btn_bg_h66_m_f.png
ev_btn_bg_h66_m_n = image://AppHomeScreen/ev/ev_btn_bg_h66_m_n.png
ev_btn_bg_h66_m_p = image://AppHomeScreen/ev/ev_btn_bg_h66_m_p.png
ev_btn_bg_h66_m_s = image://AppHomeScreen/ev/ev_btn_bg_h66_m_s.png
ev_btn_bg_h66_r_f = image://AppHomeScreen/ev/ev_btn_bg_h66_r_f.png
ev_btn_bg_h66_r_n = image://AppHomeScreen/ev/ev_btn_bg_h66_r_n.png
ev_btn_bg_h66_r_p = image://AppHomeScreen/ev/ev_btn_bg_h66_r_p.png
ev_btn_bg_h66_r_s = image://AppHomeScreen/ev/ev_btn_bg_h66_r_s.png
ev_btn_bg_h72_f = image://AppHomeScreen/ev/ev_btn_bg_h72_f.png
ev_btn_bg_h72_n = image://AppHomeScreen/ev/ev_btn_bg_h72_n.png
ev_btn_bg_h72_p = image://AppHomeScreen/ev/ev_btn_bg_h72_p.png
ev_btn_bg_h77_f = image://AppHomeScreen/ev/ev_btn_bg_h77_f.png
ev_btn_bg_h77_n = image://AppHomeScreen/ev/ev_btn_bg_h77_n.png
ev_btn_bg_h77_p = image://AppHomeScreen/ev/ev_btn_bg_h77_p.png
ev_btn_box_h102_f = image://AppHomeScreen/ev/ev_btn_box_h102_f.png
ev_btn_box_h170_f = image://AppHomeScreen/ev/ev_btn_box_h170_f.png
ev_btn_box_h189_01_f = image://AppHomeScreen/ev/ev_btn_box_h189_01_f.png
ev_btn_box_h189_01_n = image://AppHomeScreen/ev/ev_btn_box_h189_01_n.png
ev_btn_box_h189_01_p = image://AppHomeScreen/ev/ev_btn_box_h189_01_p.png
ev_btn_box_h189_02_f = image://AppHomeScreen/ev/ev_btn_box_h189_02_f.png
ev_btn_box_h189_02_n = image://AppHomeScreen/ev/ev_btn_box_h189_02_n.png
ev_btn_box_h189_02_p = image://AppHomeScreen/ev/ev_btn_box_h189_02_p.png
ev_btn_box_h189_03_f = image://AppHomeScreen/ev/ev_btn_box_h189_03_f.png
ev_btn_box_h189_03_n = image://AppHomeScreen/ev/ev_btn_box_h189_03_n.png
ev_btn_box_h189_03_p = image://AppHomeScreen/ev/ev_btn_box_h189_03_p.png
ev_btn_box_h189_04_f = image://AppHomeScreen/ev/ev_btn_box_h189_04_f.png
ev_btn_box_h189_04_n = image://AppHomeScreen/ev/ev_btn_box_h189_04_n.png
ev_btn_box_h189_04_p = image://AppHomeScreen/ev/ev_btn_box_h189_04_p.png
ev_btn_box_h324_01_f = image://AppHomeScreen/ev/ev_btn_box_h324_01_f.png
ev_btn_box_h324_01_n = image://AppHomeScreen/ev/ev_btn_box_h324_01_n.png
ev_btn_box_h324_01_p = image://AppHomeScreen/ev/ev_btn_box_h324_01_p.png
ev_btn_box_h324_02_f = image://AppHomeScreen/ev/ev_btn_box_h324_02_f.png
ev_btn_box_h324_02_n = image://AppHomeScreen/ev/ev_btn_box_h324_02_n.png
ev_btn_box_h324_02_p = image://AppHomeScreen/ev/ev_btn_box_h324_02_p.png
ev_btn_box_h522_01_n = image://AppHomeScreen/ev/ev_btn_box_h522_01_n.png
ev_btn_box_h522_01_p = image://AppHomeScreen/ev/ev_btn_box_h522_01_p.png
ev_btn_box_h522_02_n = image://AppHomeScreen/ev/ev_btn_box_h522_02_n.png
ev_btn_box_h522_02_p = image://AppHomeScreen/ev/ev_btn_box_h522_02_p.png
ev_btn_time_h291_f = image://AppHomeScreen/ev/ev_btn_time_h291_f.png
ev_btn_time_h291_n = image://AppHomeScreen/ev/ev_btn_time_h291_n.png
ev_btn_time_h291_p = image://AppHomeScreen/ev/ev_btn_time_h291_p.png
ev_btn_time_h291_s = image://AppHomeScreen/ev/ev_btn_time_h291_s.png
ev_btn_time_h60_f = image://AppHomeScreen/ev/ev_btn_time_h60_f.png
ev_btn_time_h60_n = image://AppHomeScreen/ev/ev_btn_time_h60_n.png
ev_btn_time_h60_p = image://AppHomeScreen/ev/ev_btn_time_h60_p.png
ev_btn_time_h60_s = image://AppHomeScreen/ev/ev_btn_time_h60_s.png
ev_btn_time_h75_f = image://AppHomeScreen/ev/ev_btn_time_h75_f.png
ev_btn_time_h75_n = image://AppHomeScreen/ev/ev_btn_time_h75_n.png
ev_btn_time_h75_p = image://AppHomeScreen/ev/ev_btn_time_h75_p.png
ev_btn_time_h75_s = image://AppHomeScreen/ev/ev_btn_time_h75_s.png
ev_etc_auto_off = image://AppHomeScreen/ev/ev_etc_auto_off.png
ev_etc_auto_on = image://AppHomeScreen/ev/ev_etc_auto_on.png
ev_etc_bar = image://AppHomeScreen/ev/ev_etc_bar.png
ev_etc_bg = image://AppHomeScreen/ev/ev_etc_bg.png
ev_etc_charging_progress_bar_01 = image://AppHomeScreen/ev/ev_etc_charging_progress_bar_01.png
ev_etc_charging_progress_bar_02 = image://AppHomeScreen/ev/ev_etc_charging_progress_bar_02.png
ev_etc_charging_progress_bar_03 = image://AppHomeScreen/ev/ev_etc_charging_progress_bar_03.png
ev_etc_charging_progress_bar_04 = image://AppHomeScreen/ev/ev_etc_charging_progress_bar_04.png
ev_etc_charging_progress_bar_05 = image://AppHomeScreen/ev/ev_etc_charging_progress_bar_05.png
ev_etc_dash = image://AppHomeScreen/ev/ev_etc_dash.png
ev_etc_divider_b = image://AppHomeScreen/ev/ev_etc_divider_b.png
ev_etc_divider_m = image://AppHomeScreen/ev/ev_etc_divider_m.png
ev_etc_divider_t = image://AppHomeScreen/ev/ev_etc_divider_t.png
ev_etc_eco_graph_dot = image://AppHomeScreen/ev/ev_etc_eco_graph_dot.png
ev_etc_s_off = image://AppHomeScreen/ev/ev_etc_s_off.png
ev_etc_s_on = image://AppHomeScreen/ev/ev_etc_s_on.png
ev_ic_arrow_down_d = image://AppHomeScreen/ev/ev_ic_arrow_down_d.png
ev_ic_arrow_down_n = image://AppHomeScreen/ev/ev_ic_arrow_down_n.png
ev_ic_arrow_up_d = image://AppHomeScreen/ev/ev_ic_arrow_up_d.png
ev_ic_arrow_up_n = image://AppHomeScreen/ev/ev_ic_arrow_up_n.png
ev_ic_arr_01 = image://AppHomeScreen/ev/ev_ic_arr_01.png
ev_ic_arr_02 = image://AppHomeScreen/ev/ev_ic_arr_02.png
ev_ic_arr_03 = image://AppHomeScreen/ev/ev_ic_arr_03.png
ev_ic_arr_04 = image://AppHomeScreen/ev/ev_ic_arr_04.png
ev_ic_arr_05 = image://AppHomeScreen/ev/ev_ic_arr_05.png
ev_ic_arr_06 = image://AppHomeScreen/ev/ev_ic_arr_06.png
ev_ic_arr_07 = image://AppHomeScreen/ev/ev_ic_arr_07.png
ev_ic_arr_08 = image://AppHomeScreen/ev/ev_ic_arr_08.png
ev_ic_arr_09 = image://AppHomeScreen/ev/ev_ic_arr_09.png
ev_ic_arr_10 = image://AppHomeScreen/ev/ev_ic_arr_10.png
ev_ic_arr_11 = image://AppHomeScreen/ev/ev_ic_arr_11.png
ev_ic_arr_12 = image://AppHomeScreen/ev/ev_ic_arr_12.png
ev_ic_arr_13 = image://AppHomeScreen/ev/ev_ic_arr_13.png
ev_ic_arr_14 = image://AppHomeScreen/ev/ev_ic_arr_14.png
ev_ic_arr_15 = image://AppHomeScreen/ev/ev_ic_arr_15.png
ev_ic_arr_16 = image://AppHomeScreen/ev/ev_ic_arr_16.png
ev_ic_battery_bar = image://AppHomeScreen/ev/ev_ic_battery_bar.png
ev_ic_battery_bar_red_l = image://AppHomeScreen/ev/ev_ic_battery_bar_red_l.png
ev_ic_battery_bar_red_m = image://AppHomeScreen/ev/ev_ic_battery_bar_red_m.png
ev_ic_battery_bar_red_r = image://AppHomeScreen/ev/ev_ic_battery_bar_red_r.png
ev_ic_battery_bar_white_l = image://AppHomeScreen/ev/ev_ic_battery_bar_white_l.png
ev_ic_battery_bar_white_m = image://AppHomeScreen/ev/ev_ic_battery_bar_white_m.png
ev_ic_battery_bar_white_r = image://AppHomeScreen/ev/ev_ic_battery_bar_white_r.png
ev_ic_battery_p_bar = image://AppHomeScreen/ev/ev_ic_battery_p_bar.png
ev_ic_charger_01 = image://AppHomeScreen/ev/ev_ic_charger_01.png
ev_ic_charger_02 = image://AppHomeScreen/ev/ev_ic_charger_02.png
ev_ic_charger_03 = image://AppHomeScreen/ev/ev_ic_charger_03.png
ev_ic_charger_s_01 = image://AppHomeScreen/ev/ev_ic_charger_s_01.png
ev_ic_charger_s_02 = image://AppHomeScreen/ev/ev_ic_charger_s_02.png
ev_ic_charger_s_03 = image://AppHomeScreen/ev/ev_ic_charger_s_03.png
ev_ic_climate_d = image://AppHomeScreen/ev/ev_ic_climate_d.png
ev_ic_climate_n = image://AppHomeScreen/ev/ev_ic_climate_n.png
ev_ic_defog = image://AppHomeScreen/ev/ev_ic_defog.png
ev_ic_energy_s = image://AppHomeScreen/ev/ev_ic_energy_s.png
ev_ic_setting = image://AppHomeScreen/ev/ev_ic_setting.png
ev_ic_setting_d = image://AppHomeScreen/ev/ev_ic_setting_d.png
ev_ic_warning = image://AppHomeScreen/ev/ev_ic_warning.png
ev_list_history_s = image://AppHomeScreen/ev/ev_list_history_s.png
hev_ic_battery_bar_01 = image://AppHomeScreen/ev/hev_ic_battery_bar_01.png
hev_ic_battery_bar_02 = image://AppHomeScreen/ev/hev_ic_battery_bar_02.png
hev_ic_battery_bar_03 = image://AppHomeScreen/ev/hev_ic_battery_bar_03.png
hev_ic_battery_bar_04 = image://AppHomeScreen/ev/hev_ic_battery_bar_04.png
hev_ic_battery_bar_05 = image://AppHomeScreen/ev/hev_ic_battery_bar_05.png
hev_ic_battery_bar_06 = image://AppHomeScreen/ev/hev_ic_battery_bar_06.png
hev_ic_battery_bar_07 = image://AppHomeScreen/ev/hev_ic_battery_bar_07.png
hev_ic_battery_bar_08 = image://AppHomeScreen/ev/hev_ic_battery_bar_08.png
hev_ic_battery_charging = image://AppHomeScreen/ev/hev_ic_battery_charging.png
phev_bg_box_info_03 = image://AppHomeScreen/ev/phev_bg_box_info_03.png
phev_bg_info_line = image://AppHomeScreen/ev/phev_bg_info_line.png
phev_btn_box_h256_01_f = image://AppHomeScreen/ev/phev_btn_box_h256_01_f.png
phev_btn_box_h256_01_n = image://AppHomeScreen/ev/phev_btn_box_h256_01_n.png
phev_btn_box_h256_01_p = image://AppHomeScreen/ev/phev_btn_box_h256_01_p.png
phev_btn_box_h256_02_f = image://AppHomeScreen/ev/phev_btn_box_h256_02_f.png
phev_btn_box_h256_02_n = image://AppHomeScreen/ev/phev_btn_box_h256_02_n.png
phev_btn_box_h256_02_p = image://AppHomeScreen/ev/phev_btn_box_h256_02_p.png
phev_btn_box_h522_01_f = image://AppHomeScreen/ev/phev_btn_box_h522_01_f.png
phev_btn_box_h522_01_n = image://AppHomeScreen/ev/phev_btn_box_h522_01_n.png
phev_btn_box_h522_01_p = image://AppHomeScreen/ev/phev_btn_box_h522_01_p.png
phev_btn_box_h522_02_f = image://AppHomeScreen/ev/phev_btn_box_h522_02_f.png
phev_btn_box_h522_02_n = image://AppHomeScreen/ev/phev_btn_box_h522_02_n.png
phev_btn_box_h522_02_p = image://AppHomeScreen/ev/phev_btn_box_h522_02_p.png
phev_btn_box_h522_03_f = image://AppHomeScreen/ev/phev_btn_box_h522_03_f.png
phev_btn_box_h522_03_n = image://AppHomeScreen/ev/phev_btn_box_h522_03_n.png
phev_btn_box_h522_03_p = image://AppHomeScreen/ev/phev_btn_box_h522_03_p.png
phev_ic_arrow_00 = image://AppHomeScreen/ev/phev_ic_arrow_00.png
phev_ic_arrow_01 = image://AppHomeScreen/ev/phev_ic_arrow_01.png
phev_ic_arrow_02 = image://AppHomeScreen/ev/phev_ic_arrow_02.png
phev_ic_arrow_03 = image://AppHomeScreen/ev/phev_ic_arrow_03.png
phev_ic_arrow_04 = image://AppHomeScreen/ev/phev_ic_arrow_04.png
phev_ic_arrow_05 = image://AppHomeScreen/ev/phev_ic_arrow_05.png
phev_ic_arrow_06 = image://AppHomeScreen/ev/phev_ic_arrow_06.png
phev_ic_arrow_07 = image://AppHomeScreen/ev/phev_ic_arrow_07.png
phev_ic_battery_bar = image://AppHomeScreen/ev/phev_ic_battery_bar.png
phev_ic_battery_bar_white_l = image://AppHomeScreen/ev/phev_ic_battery_bar_white_l.png
phev_ic_battery_bar_white_m = image://AppHomeScreen/ev/phev_ic_battery_bar_white_m.png
phev_ic_battery_bar_white_r = image://AppHomeScreen/ev/phev_ic_battery_bar_white_r.png
ev_bg_graph_shadow_01 = image://AppHomeScreen/ev/ev_bg_graph_shadow_01.png
ev_etc_battery_l_01 = image://AppHomeScreen/ev/ev_etc_battery_l_01.png
ev_etc_battery_l_02 = image://AppHomeScreen/ev/ev_etc_battery_l_02.png
ev_etc_battery_l_03 = image://AppHomeScreen/ev/ev_etc_battery_l_03.png
ev_etc_battery_l_04 = image://AppHomeScreen/ev/ev_etc_battery_l_04.png
ev_etc_battery_l_05 = image://AppHomeScreen/ev/ev_etc_battery_l_05.png
ev_etc_battery_l_06 = image://AppHomeScreen/ev/ev_etc_battery_l_06.png
ev_etc_battery_l_07 = image://AppHomeScreen/ev/ev_etc_battery_l_07.png
ev_etc_battery_l_08 = image://AppHomeScreen/ev/ev_etc_battery_l_08.png
ev_etc_battery_l_09 = image://AppHomeScreen/ev/ev_etc_battery_l_09.png
ev_etc_battery_l_10 = image://AppHomeScreen/ev/ev_etc_battery_l_10.png
ev_etc_battery_l_11 = image://AppHomeScreen/ev/ev_etc_battery_l_11.png
ev_etc_battery_l_12 = image://AppHomeScreen/ev/ev_etc_battery_l_12.png
ev_etc_battery_l_13 = image://AppHomeScreen/ev/ev_etc_battery_l_13.png
ev_etc_battery_l_14 = image://AppHomeScreen/ev/ev_etc_battery_l_14.png
ev_etc_battery_l_15 = image://AppHomeScreen/ev/ev_etc_battery_l_15.png
ev_etc_battery_l_16 = image://AppHomeScreen/ev/ev_etc_battery_l_16.png
ev_etc_battery_l_17 = image://AppHomeScreen/ev/ev_etc_battery_l_17.png
ev_etc_battery_l_18 = image://AppHomeScreen/ev/ev_etc_battery_l_18.png
ev_etc_battery_l_charging = image://AppHomeScreen/ev/ev_etc_battery_l_charging.png
ev_etc_battery_l_cover = image://AppHomeScreen/ev/ev_etc_battery_l_cover_@.png
ev_etc_battery_s_01 = image://AppHomeScreen/ev/ev_etc_battery_s_01.png
ev_etc_battery_s_02 = image://AppHomeScreen/ev/ev_etc_battery_s_02.png
ev_etc_battery_s_03 = image://AppHomeScreen/ev/ev_etc_battery_s_03.png
ev_etc_battery_s_04 = image://AppHomeScreen/ev/ev_etc_battery_s_04.png
ev_etc_battery_s_05 = image://AppHomeScreen/ev/ev_etc_battery_s_05.png
ev_etc_battery_s_06 = image://AppHomeScreen/ev/ev_etc_battery_s_06.png
ev_etc_battery_s_07 = image://AppHomeScreen/ev/ev_etc_battery_s_07.png
ev_etc_battery_s_08 = image://AppHomeScreen/ev/ev_etc_battery_s_08.png
ev_etc_battery_s_09 = image://AppHomeScreen/ev/ev_etc_battery_s_09.png
ev_etc_battery_s_10 = image://AppHomeScreen/ev/ev_etc_battery_s_10.png
ev_etc_battery_s_11 = image://AppHomeScreen/ev/ev_etc_battery_s_11.png
ev_etc_battery_s_12 = image://AppHomeScreen/ev/ev_etc_battery_s_12.png
ev_etc_battery_s_13 = image://AppHomeScreen/ev/ev_etc_battery_s_13.png
ev_etc_battery_s_14 = image://AppHomeScreen/ev/ev_etc_battery_s_14.png
ev_etc_battery_s_15 = image://AppHomeScreen/ev/ev_etc_battery_s_15.png
ev_etc_battery_s_16 = image://AppHomeScreen/ev/ev_etc_battery_s_16.png
ev_etc_battery_s_17 = image://AppHomeScreen/ev/ev_etc_battery_s_17.png
ev_etc_battery_s_18 = image://AppHomeScreen/ev/ev_etc_battery_s_18.png
ev_etc_battery_s_charging = image://AppHomeScreen/ev/ev_etc_battery_s_charging.png
ev_etc_battery_s_cover = image://AppHomeScreen/ev/ev_etc_battery_s_cover.png
co_btn_bg_list_108_f = image://AppHomeScreen/general/co_btn_bg_list_108_f.png
co_btn_bg_list_108_sf = image://AppHomeScreen/general/co_btn_bg_list_108_sf.png
co_btn_bg_list_141_f = image://AppHomeScreen/general/co_btn_bg_list_141_f.png
co_btn_bg_list_141_p = image://AppHomeScreen/general/co_btn_bg_list_141_p.png
co_btn_bg_list_141_s = image://AppHomeScreen/general/co_btn_bg_list_141_s.png
co_btn_bg_list_141_sf = image://AppHomeScreen/general/co_btn_bg_list_141_sf.png
co_btn_bg_list_147_f = image://AppHomeScreen/general/co_btn_bg_list_147_f.png
co_btn_bg_list_147_p = image://AppHomeScreen/general/co_btn_bg_list_147_p.png
co_btn_bg_list_147_s = image://AppHomeScreen/general/co_btn_bg_list_147_s.png
co_btn_bg_list_147_sf = image://AppHomeScreen/general/co_btn_bg_list_147_sf.png
co_btn_bg_list_174_f = image://AppHomeScreen/general/co_btn_bg_list_174_f.png
co_btn_bg_list_174_p = image://AppHomeScreen/general/co_btn_bg_list_174_p.png
co_btn_bg_list_174_s = image://AppHomeScreen/general/co_btn_bg_list_174_s.png
co_btn_bg_list_174_sf = image://AppHomeScreen/general/co_btn_bg_list_174_sf.png
co_btn_bg_list_186_f = image://AppHomeScreen/general/co_btn_bg_list_186_f.png
co_btn_bg_list_186_p = image://AppHomeScreen/general/co_btn_bg_list_186_p.png
co_btn_bg_list_186_s = image://AppHomeScreen/general/co_btn_bg_list_186_s.png
co_btn_bg_list_186_sf = image://AppHomeScreen/general/co_btn_bg_list_186_sf.png
co_btn_bg_list_207_p = image://AppHomeScreen/general/co_btn_bg_list_207_p.png
co_btn_bg_list_207_s = image://AppHomeScreen/general/co_btn_bg_list_207_s.png
co_btn_bg_list_207_sf = image://AppHomeScreen/general/co_btn_bg_list_207_sf.png
co_btn_bg_list_t2_108_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_108_sf.png
co_btn_bg_list_t2_141_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_141_sf.png
co_btn_bg_list_t2_147_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_147_sf.png
co_btn_bg_list_t2_174_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_174_sf.png
co_btn_bg_list_t2_180_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_180_sf.png
co_btn_bg_list_t2_186_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_186_sf.png
co_btn_bg_list_t2_207_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_207_sf.png
co_btn_bg_list_t2_213_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_213_sf.png
co_btn_bg_list_t2_219_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_219_sf.png
co_btn_bg_list_t2_225_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_225_sf.png
co_btn_bg_list_t2_246_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_246_sf.png
co_btn_bg_list_t2_252_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_252_sf.png
co_btn_bg_list_t2_285_sf = image://AppHomeScreen/general/co_btn_bg_list_t2_285_sf.png
co_btn_bg_list_c_108_sf = image://AppHomeScreen/general/co_btn_bg_list_c_108_sf.png
co_btn_bg_list_c_141_sf = image://AppHomeScreen/general/co_btn_bg_list_c_141_sf.png
co_btn_bg_list_c_147_sf = image://AppHomeScreen/general/co_btn_bg_list_c_147_sf.png
co_btn_bg_list_c_174_sf = image://AppHomeScreen/general/co_btn_bg_list_c_174_sf.png
co_btn_bg_list_c_180_sf = image://AppHomeScreen/general/co_btn_bg_list_c_180_sf.png
co_btn_bg_list_c_186_sf = image://AppHomeScreen/general/co_btn_bg_list_c_186_sf.png
co_btn_bg_list_c_207_sf = image://AppHomeScreen/general/co_btn_bg_list_c_207_sf.png
co_btn_bg_list_c_213_sf = image://AppHomeScreen/general/co_btn_bg_list_c_213_sf.png
co_btn_bg_list_c_219_sf = image://AppHomeScreen/general/co_btn_bg_list_c_219_sf.png
co_btn_bg_list_c_225_sf = image://AppHomeScreen/general/co_btn_bg_list_c_225_sf.png
co_btn_bg_list_c_246_sf = image://AppHomeScreen/general/co_btn_bg_list_c_246_sf.png
co_btn_bg_list_c_252_sf = image://AppHomeScreen/general/co_btn_bg_list_c_252_sf.png
co_btn_bg_list_c_285_sf = image://AppHomeScreen/general/co_btn_bg_list_c_285_sf.png
co_btn_bg_list_c_108_f = image://AppHomeScreen/general/co_btn_bg_list_c_108_f.png
co_btn_bg_list_c_141_f = image://AppHomeScreen/general/co_btn_bg_list_c_141_f.png
co_btn_bg_list_c_147_f = image://AppHomeScreen/general/co_btn_bg_list_c_147_f.png
co_btn_bg_list_c_174_f = image://AppHomeScreen/general/co_btn_bg_list_c_174_f.png
co_btn_bg_list_c_180_f = image://AppHomeScreen/general/co_btn_bg_list_c_180_f.png
co_btn_bg_list_c_186_f = image://AppHomeScreen/general/co_btn_bg_list_c_186_f.png
co_btn_bg_list_c_207_f = image://AppHomeScreen/general/co_btn_bg_list_c_207_f.png
co_btn_bg_list_c_213_f = image://AppHomeScreen/general/co_btn_bg_list_c_213_f.png
co_btn_bg_list_c_219_f = image://AppHomeScreen/general/co_btn_bg_list_c_219_f.png
co_btn_bg_list_c_225_f = image://AppHomeScreen/general/co_btn_bg_list_c_225_f.png
co_btn_bg_list_c_246_f = image://AppHomeScreen/general/co_btn_bg_list_c_246_f.png
co_btn_bg_list_c_252_f = image://AppHomeScreen/general/co_btn_bg_list_c_252_f.png
co_btn_bg_list_c_285_f = image://AppHomeScreen/general/co_btn_bg_list_c_285_f.png
co_btn_bg_list_c_108_p = image://AppHomeScreen/general/co_btn_bg_list_c_108_p.png
co_btn_bg_list_c_141_p = image://AppHomeScreen/general/co_btn_bg_list_c_141_p.png
co_btn_bg_list_c_147_p = image://AppHomeScreen/general/co_btn_bg_list_c_147_p.png
co_btn_bg_list_c_174_p = image://AppHomeScreen/general/co_btn_bg_list_c_174_p.png
co_btn_bg_list_c_180_p = image://AppHomeScreen/general/co_btn_bg_list_c_180_p.png
co_btn_bg_list_c_186_p = image://AppHomeScreen/general/co_btn_bg_list_c_186_p.png
co_btn_bg_list_c_207_p = image://AppHomeScreen/general/co_btn_bg_list_c_207_p.png
co_btn_bg_list_c_213_p = image://AppHomeScreen/general/co_btn_bg_list_c_213_p.png
co_btn_bg_list_c_219_p = image://AppHomeScreen/general/co_btn_bg_list_c_219_p.png
co_btn_bg_list_c_225_p = image://AppHomeScreen/general/co_btn_bg_list_c_225_p.png
co_btn_bg_list_c_246_p = image://AppHomeScreen/general/co_btn_bg_list_c_246_p.png
co_btn_bg_list_c_252_p = image://AppHomeScreen/general/co_btn_bg_list_c_252_p.png
co_btn_bg_list_c_285_p = image://AppHomeScreen/general/co_btn_bg_list_c_285_p.png
co_btn_bg_list_c_108_s = image://AppHomeScreen/general/co_btn_bg_list_c_108_s.png
co_btn_bg_list_c_141_s = image://AppHomeScreen/general/co_btn_bg_list_c_141_s.png
co_btn_bg_list_c_147_s = image://AppHomeScreen/general/co_btn_bg_list_c_147_s.png
co_btn_bg_list_c_174_s = image://AppHomeScreen/general/co_btn_bg_list_c_174_s.png
co_btn_bg_list_c_180_s = image://AppHomeScreen/general/co_btn_bg_list_c_180_s.png
co_btn_bg_list_c_186_s = image://AppHomeScreen/general/co_btn_bg_list_c_186_s.png
co_btn_bg_list_c_207_s = image://AppHomeScreen/general/co_btn_bg_list_c_207_s.png
co_btn_bg_list_c_213_s = image://AppHomeScreen/general/co_btn_bg_list_c_213_s.png
co_btn_bg_list_c_219_s = image://AppHomeScreen/general/co_btn_bg_list_c_219_s.png
co_btn_bg_list_c_225_s = image://AppHomeScreen/general/co_btn_bg_list_c_225_s.png
co_btn_bg_list_c_246_s = image://AppHomeScreen/general/co_btn_bg_list_c_246_s.png
co_btn_bg_list_c_252_s = image://AppHomeScreen/general/co_btn_bg_list_c_252_s.png
co_btn_bg_list_c_285_s = image://AppHomeScreen/general/co_btn_bg_list_c_285_s.png
co_btn_bg_tab_wide_list_108_p = image://AppHomeScreen/general/co_btn_bg_tab_wide_list_108_p.png
co_btn_bg_list_t2_108_s = image://AppHomeScreen/general/co_btn_bg_list_t2_108_s.png
co_btn_bg_list_t2_141_s = image://AppHomeScreen/general/co_btn_bg_list_t2_141_s.png
co_btn_bg_list_t2_147_s = image://AppHomeScreen/general/co_btn_bg_list_t2_147_s.png
co_btn_bg_list_t2_174_s = image://AppHomeScreen/general/co_btn_bg_list_t2_174_s.png
co_btn_bg_list_t2_180_s = image://AppHomeScreen/general/co_btn_bg_list_t2_180_s.png
co_btn_bg_list_t2_186_s = image://AppHomeScreen/general/co_btn_bg_list_t2_186_s.png
co_btn_bg_list_t2_207_s = image://AppHomeScreen/general/co_btn_bg_list_t2_207_s.png
co_btn_bg_list_t2_213_s = image://AppHomeScreen/general/co_btn_bg_list_t2_213_s.png
co_btn_bg_list_t2_219_s = image://AppHomeScreen/general/co_btn_bg_list_t2_219_s.png
co_btn_bg_list_t2_225_s = image://AppHomeScreen/general/co_btn_bg_list_t2_225_s.png
co_btn_bg_list_t2_246_s = image://AppHomeScreen/general/co_btn_bg_list_t2_246_s.png
co_btn_bg_list_t2_252_s = image://AppHomeScreen/general/co_btn_bg_list_t2_252_s.png
co_btn_bg_list_t2_270_s = image://AppHomeScreen/general/co_btn_bg_list_t2_270_s.png
co_btn_bg_list_t2_285_s = image://AppHomeScreen/general/co_btn_bg_list_t2_285_s.png
co_btn_bg_list_t2_324_s = image://AppHomeScreen/general/co_btn_bg_list_t2_324_s.png
co_btn_bg_list_t2_108_p = image://AppHomeScreen/general/co_btn_bg_list_t2_108_p.png
co_btn_bg_list_t2_141_p = image://AppHomeScreen/general/co_btn_bg_list_t2_141_p.png
co_btn_bg_list_t2_147_p = image://AppHomeScreen/general/co_btn_bg_list_t2_147_p.png
co_btn_bg_list_t2_174_p = image://AppHomeScreen/general/co_btn_bg_list_t2_174_p.png
co_btn_bg_list_t2_180_p = image://AppHomeScreen/general/co_btn_bg_list_t2_180_p.png
co_btn_bg_list_t2_186_p = image://AppHomeScreen/general/co_btn_bg_list_t2_186_p.png
co_btn_bg_list_t2_207_p = image://AppHomeScreen/general/co_btn_bg_list_t2_207_p.png
co_btn_bg_list_t2_213_p = image://AppHomeScreen/general/co_btn_bg_list_t2_213_p.png
co_btn_bg_list_t2_219_p = image://AppHomeScreen/general/co_btn_bg_list_t2_219_p.png
co_btn_bg_list_t2_225_p = image://AppHomeScreen/general/co_btn_bg_list_t2_225_p.png
co_btn_bg_list_t2_246_p = image://AppHomeScreen/general/co_btn_bg_list_t2_246_p.png
co_btn_bg_list_t2_252_p = image://AppHomeScreen/general/co_btn_bg_list_t2_252_p.png
co_btn_bg_list_t2_270_p = image://AppHomeScreen/general/co_btn_bg_list_t2_270_p.png
co_btn_bg_list_t2_285_p = image://AppHomeScreen/general/co_btn_bg_list_t2_285_p.png
co_btn_bg_list_t2_324_p = image://AppHomeScreen/general/co_btn_bg_list_t2_324_p.png
co_btn_bg_list_t2_108_f = image://AppHomeScreen/general/co_btn_bg_list_t2_108_f.png
co_btn_bg_list_t2_141_f = image://AppHomeScreen/general/co_btn_bg_list_t2_141_f.png
co_btn_bg_list_t2_147_f = image://AppHomeScreen/general/co_btn_bg_list_t2_147_f.png
co_btn_bg_list_t2_174_f = image://AppHomeScreen/general/co_btn_bg_list_t2_174_f.png
co_btn_bg_list_t2_180_f = image://AppHomeScreen/general/co_btn_bg_list_t2_180_f.png
co_btn_bg_list_t2_186_f = image://AppHomeScreen/general/co_btn_bg_list_t2_186_f.png
co_btn_bg_list_t2_207_f = image://AppHomeScreen/general/co_btn_bg_list_t2_207_f.png
co_btn_bg_list_t2_213_f = image://AppHomeScreen/general/co_btn_bg_list_t2_213_f.png
co_btn_bg_list_t2_219_f = image://AppHomeScreen/general/co_btn_bg_list_t2_219_f.png
co_btn_bg_list_t2_225_f = image://AppHomeScreen/general/co_btn_bg_list_t2_225_f.png
co_btn_bg_list_t2_246_f = image://AppHomeScreen/general/co_btn_bg_list_t2_246_f.png
co_btn_bg_list_t2_252_f = image://AppHomeScreen/general/co_btn_bg_list_t2_252_f.png
co_btn_bg_list_t2_270_f = image://AppHomeScreen/general/co_btn_bg_list_t2_270_f.png
co_btn_bg_list_t2_285_f = image://AppHomeScreen/general/co_btn_bg_list_t2_285_f.png
co_btn_bg_list_t2_324_f = image://AppHomeScreen/general/co_btn_bg_list_t2_324_f.png
co_btn_bg_list_108_p = image://AppHomeScreen/general/co_btn_bg_list_108_p.png
co_btn_bg_list_108_s = image://AppHomeScreen/general/co_btn_bg_list_108_s.png
co_btn_bg_list_s_108_p = image://AppHomeScreen/general/co_btn_bg_list_s_108_p.png
co_btn_bg_list_s_108_s = image://AppHomeScreen/general/co_btn_bg_list_s_108_s.png
nm_bg_gforce_graph = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_graph.png
nm_icon_bubblepop_blue = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue.png
nm_icon_bubblepop_blue_l = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_blue_l.png
nm_icon_bubblepop_red = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red.png
nm_icon_bubblepop_red_l = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_red_l.png
nm_icon_bubblepop_gray = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_gray.png
nm_icon_bubblepop_gray_m = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_gray_m.png
nm_etc_line_time = image://AppHomeScreen/{nmode|performance}/nm_etc_line_time.png
nm_edit_line_time_m = image://AppHomeScreen/{nmode|performance}/nm_edit_line_time_m.png
nm_icon_time = image://AppHomeScreen/{nmode|performance}/nm_icon_time.png
nm_icon_time_red = image://AppHomeScreen/{nmode|performance}/nm_icon_time_red.png
nm_icon_bubblepop_time = image://AppHomeScreen/{nmode|performance}/nm_icon_bubblepop_time.png
nm_icon_power = image://AppHomeScreen/{nmode|performance}/nm_icon_power.png
nm_icon_gforce = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce.png
nm_icon_gforce_real = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_real.png
co_bg_tabline = image://AppHomeScreen/general/co_bg_tabline.png
co_btn_title_tab_l_p = image://AppHomeScreen/general/co_btn_title_tab_l_p.png
co_btn_title_tab_r_p = image://AppHomeScreen/general/co_btn_title_tab_r_p.png
co_btn_title_tab_m_p = image://AppHomeScreen/general/co_btn_title_tab_m_p.png
co_btn_title_tab_l_s = image://AppHomeScreen/general/co_btn_title_tab_l_s.png
co_btn_title_tab_r_s = image://AppHomeScreen/general/co_btn_title_tab_r_s.png
co_btn_title_tab_m_s = image://AppHomeScreen/general/co_btn_title_tab_m_s.png
co_btn_title_tab_p = image://AppHomeScreen/general/co_btn_title_tab_p.png
co_btn_title_tab_s = image://AppHomeScreen/general/co_btn_title_tab_s.png
nm_bg_gage_racing = image://AppHomeScreen/{nmode|performance}/gage/nm_bg_gage_racing
nm_bg_gage_racing_c = image://AppHomeScreen/{nmode|performance}/gage_c/nm_bg_gage_racing
co_btn_bg_tab_push = image://AppHomeScreen/general/co_btn_bg_tab_push.png
co_btn_bg_tab_push_55 = image://AppHomeScreen/general/co_btn_bg_tab_push_55.png
co_btn_bg_tab_sel = image://AppHomeScreen/general/co_btn_bg_tab_sel.png
co_btn_bg_tab_sel_55 = image://AppHomeScreen/general/co_btn_bg_tab_sel_55.png
ev_etc_bg_l = image://AppHomeScreen/ev/ev_etc_bg_l.png
ev_etc_bg_m = image://AppHomeScreen/ev/ev_etc_bg_m.png
ev_etc_bg_r = image://AppHomeScreen/ev/ev_etc_bg_r.png
ev_ic_charge_station_d = image://AppHomeScreen/ev/ev_ic_charge_station_d.png
ev_ic_charge_station_f = image://AppHomeScreen/ev/ev_ic_charge_station_f.png
ev_ic_charge_station_n = image://AppHomeScreen/ev/ev_ic_charge_station_n.png
ev_ic_charge_station_p = image://AppHomeScreen/ev/ev_ic_charge_station_p.png
ev_ic_charge_station_s = image://AppHomeScreen/ev/ev_ic_charge_station_s.png
home_ic_battery_charging = image://AppHomeScreen/home/home_ic_battery_charging.png
home_ic_battery_unit_nor = image://AppHomeScreen/home/home_ic_battery_unit_nor.png
home_ic_battery_unit_war = image://AppHomeScreen/home/home_ic_battery_unit_war.png
home_ic_battery_bg_nor = image://AppHomeScreen/home/home_ic_battery_bg_nor_@.png
home_ic_battery_bg_war = image://AppHomeScreen/home/home_ic_battery_bg_war_@.png
home_ic_battery_cover = image://AppHomeScreen/home/home_ic_battery_cover_@.png
home_bg_wallpaper = image://AppHomeScreen/home/home_bg_wallpaper.png
home_ic_battery_bg_p = image://AppHomeScreen/home/home_ic_battery_bg_p.png
ev_ic_v2x_n = image://AppHomeScreen/ev/ev_ic_v2x_n.png
ev_ic_v2x_d = image://AppHomeScreen/ev/ev_ic_v2x_d.png
ev_ic_v2l_n = image://AppHomeScreen/ev/ev_ic_v2l_n.png
ev_ic_v2l_d = image://AppHomeScreen/ev/ev_ic_v2l_d.png
set_slider_loading_01 = image://AppHomeScreen/setup/slider loading/set_slider_loading_01.png
set_slider_loading_02 = image://AppHomeScreen/setup/slider loading/set_slider_loading_02.png
set_slider_loading_03 = image://AppHomeScreen/setup/slider loading/set_slider_loading_03.png
set_slider_loading_04 = image://AppHomeScreen/setup/slider loading/set_slider_loading_04.png
set_slider_loading_05 = image://AppHomeScreen/setup/slider loading/set_slider_loading_05.png
set_slider_loading_06 = image://AppHomeScreen/setup/slider loading/set_slider_loading_06.png
set_slider_loading_07 = image://AppHomeScreen/setup/slider loading/set_slider_loading_07.png
set_slider_loading_08 = image://AppHomeScreen/setup/slider loading/set_slider_loading_08.png
set_slider_loading_09 = image://AppHomeScreen/setup/slider loading/set_slider_loading_09.png
set_slider_loading_10 = image://AppHomeScreen/setup/slider loading/set_slider_loading_10.png
set_slider_loading_11 = image://AppHomeScreen/setup/slider loading/set_slider_loading_11.png
set_slider_loading_12 = image://AppHomeScreen/setup/slider loading/set_slider_loading_12.png
set_slider_loading_13 = image://AppHomeScreen/setup/slider loading/set_slider_loading_13.png
set_slider_loading_14 = image://AppHomeScreen/setup/slider loading/set_slider_loading_14.png
set_slider_loading_15 = image://AppHomeScreen/setup/slider loading/set_slider_loading_15.png
set_slider_loading_16 = image://AppHomeScreen/setup/slider loading/set_slider_loading_16.png
co_ic_loading_xs = ?
ev_etc_charge_flow_list = ?
ev_etc_discharge_flow_list = ?
ev_ic_default_n = image://AppHomeScreen/ev/ev_ic_default_n.png
ev_ic_default_d = image://AppHomeScreen/ev/ev_ic_default_d.png
me_bg_list_line_t = image://AppHomeScreen/media/me_bg_list_line_t.png
co_bg_list_line_t2 = image://AppHomeScreen/general/co_bg_list_line_t2.png
me_bg_list_line = image://AppHomeScreen/media/me_bg_list_line.png
co_bg_list_line_1920 = image://AppHomeScreen/general/co_bg_list_line.png
ev_btn_bottom_n_l = image://AppHomeScreen/ev/ev_btn_bottom_n_l.png
ev_btn_bottom_p_l = image://AppHomeScreen/ev/ev_btn_bottom_p_l.png
ev_btn_bottom_n_m = image://AppHomeScreen/ev/ev_btn_bottom_n_m.png
ev_btn_bottom_p_m = image://AppHomeScreen/ev/ev_btn_bottom_p_m.png
ev_btn_bottom_n_r = image://AppHomeScreen/ev/ev_btn_bottom_n_r.png
ev_btn_bottom_p_r = image://AppHomeScreen/ev/ev_btn_bottom_p_r.png
ev_ic_battery_bg_nor_cartype = image://AppHomeScreen/ev/ev_ic_battery_bg_@_nor.png
ev_ic_battery_bg_war_cartype = image://AppHomeScreen/ev/ev_ic_battery_bg_@_war.png
ev_ic_battery_cover_cartype = image://AppHomeScreen/ev/ev_ic_battery_cover_@.png
ev_ic_charging_01_n = image://AppHomeScreen/ev/ev_ic_charging_01_n.png
ev_ic_charging_01_d = image://AppHomeScreen/ev/ev_ic_charging_01_d.png
ev_bg_energyinfo_bg = image://AppHomeScreen/ev/ev_bg_energyinfo_bg.png
ev_bg_energyinfo_bar = image://AppHomeScreen/ev/ev_bg_energyinfo_bar.png
ev_etc_battery = image://AppHomeScreen/ev/ev_etc_battery_@.png
ev_etc_battery_red = image://AppHomeScreen/ev/ev_etc_battery_red_@.png
ev_bg_graph_shadow_l = image://AppHomeScreen/ev/ev_bg_graph_shadow_l.png
ev_bg_graph_shadow_r = image://AppHomeScreen/ev/ev_bg_graph_shadow_r.png
ev_ic_solar_info = image://AppHomeScreen/ev/ev_ic_solar_info.png
ev_etc_electricityuse_bg = image://AppHomeScreen/ev/ev_etc_electricityuse_bg.png
ev_ic_default_01_n = image://AppHomeScreen/ev/ev_ic_default_01_n.png
ev_ic_default_01_d = image://AppHomeScreen/ev/ev_ic_default_01_d.png
ev_ic_nextdeparture = image://AppHomeScreen/ev/ev_ic_nextdeparture.png
ev_ic_discharge = image://AppHomeScreen/ev/ev_ic_discharge.png
ev_ic_v2x_n_s = image://AppHomeScreen/ev/ev_ic_v2x_n_s.png
ev_ic_v2l_n_s = image://AppHomeScreen/ev/ev_ic_v2l_n_s.png
ev_ic_setting_s_d = image://AppHomeScreen/ev/ev_ic_setting_s_d.png
ev_ic_setting_s_n = image://AppHomeScreen/ev/ev_ic_setting_s_n.png
usm_ed_012_soft = image://AppHomeScreen/ev/usm_ed_012_soft.png
usm_ed_012_medium = image://AppHomeScreen/ev/usm_ed_012_medium.png
usm_ed_012_strong = image://AppHomeScreen/ev/usm_ed_012_strong.png
usm_ed_012 = image://AppHomeScreen/ev/usm_ed_012_@.png
usm_ed_005 = image://AppHomeScreen/ev/usm_ed_005.png
usm_ed_006 = image://AppHomeScreen/ev/usm_ed_006.png
usm_ed_007 = image://AppHomeScreen/ev/usm_ed_007.png
ev_ed_008_00 = image://AppHomeScreen/ev/ev_ed_008_@_00.png
ev_ed_008_01 = image://AppHomeScreen/ev/ev_ed_008_@_01.png
ev_ed_008_02 = image://AppHomeScreen/ev/ev_ed_008_@_02.png
ev_ed_008_03 = image://AppHomeScreen/ev/ev_ed_008_@_03.png
ev_ed_010_00 = image://AppHomeScreen/ev/ev_ed_010_@_00.png
ev_ed_010_01 = image://AppHomeScreen/ev/ev_ed_010_@_01.png
ev_ed_010_02 = image://AppHomeScreen/ev/ev_ed_010_@_02.png
ev_ed_010_03 = image://AppHomeScreen/ev/ev_ed_010_@_03.png
ev_ed_010_04 = image://AppHomeScreen/ev/ev_ed_010_@_04.png
ev_ic_list_arrow_up = image://AppHomeScreen/ev/ev_ic_list_arrow_up.png
ev_ic_list_arrow_down = image://AppHomeScreen/ev/ev_ic_list_arrow_down.png
ev_ic_unplug_n = image://AppHomeScreen/ev/ev_ic_unplug_n.png
mn_bg_split_01_l = image://AppHomeScreen/{nmode|performance}/mn_bg_split_01_l.png
mn_bg_split_01_m = image://AppHomeScreen/{nmode|performance}/mn_bg_split_01_m.png
mn_bg_split_01_r = image://AppHomeScreen/{nmode|performance}/mn_bg_split_01_r.png
nm_best_lap_bg_l = image://AppHomeScreen/{nmode|performance}/nm_best_lap_bg_l.png
nm_best_lap_bg_s = image://AppHomeScreen/{nmode|performance}/nm_best_lap_bg_s.png
nm_bg_new_image = image://AppHomeScreen/{nmode|performance}/nm_bg_new_image.png
nm_bg_custom_car_c = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_car_c.png
nm_bg_custom_car = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_car.png
nm_bg_custom_c = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_c.png
nm_bg_custom = image://AppHomeScreen/{nmode|performance}/nm_bg_custom.png
nm_bg_custom_racing_car = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_racing_car.png
nm_bg_custom_racing_widget_d = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_racing_widget_d.png
nm_bg_custom_racing_widget_n = image://AppHomeScreen/{nmode|performance}/nm_bg_custom_racing_widget_n.png
nm_bg_engine_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_normal.png
nm_bg_engine_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_sport_plus.png
nm_bg_engine_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_sport.png
nm_bg_esc_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_normal.png
nm_bg_esc_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_sport_plus.png
nm_bg_esc_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_sport.png
nm_bg_esc_off = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_off.png
nm_bg_exhaustsound_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_normal.png
nm_bg_exhaustsound_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_sport_plus.png
nm_bg_exhaustsound_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_sport.png
nm_bg_engine_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_normal_c.png
nm_bg_engine_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_sport_plus_c.png
nm_bg_engine_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_engine_sport_c.png
nm_bg_esc_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_normal_c.png
nm_bg_esc_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_sport_plus_c.png
nm_bg_esc_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_sport_c.png
nm_bg_esc_off_c = image://AppHomeScreen/{nmode|performance}/nm_bg_esc_off_c.png
nm_bg_exhaustsound_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_normal_c.png
nm_bg_exhaustsound_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_sport_plus_c.png
nm_bg_exhaustsound_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_exhaustsound_sport_c.png
nm_bg_gforce_02_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_02_c.png
nm_bg_gforce_02_dot_wave_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_02_dot_wave_c.png
nm_bg_gforce_02_dot_wave = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_02_dot_wave.png
nm_bg_gforce_02 = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_02.png
nm_bg_gforce_bg_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_bg_c.png
nm_bg_gforce_bg = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_bg.png
nm_bg_gforce_edit_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_edit_c.png
nm_bg_gforce_edit_dot_wave_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_edit_dot_wave_c.png
nm_bg_gforce_edit_dot_wave = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_edit_dot_wave.png
nm_bg_gforce_edit = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_edit.png
nm_bg_gforce_r_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_r_c.png
nm_bg_gforce_r_dot_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_r_dot_c.png
nm_bg_gforce_r_dot = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_r_dot.png
nm_bg_gforce_r = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_r.png
nm_bg_gforce_wave_dot_c = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_wave_dot_c.png
nm_bg_gforce_wave_dot = image://AppHomeScreen/{nmode|performance}/nm_bg_gforce_wave_dot.png
nm_bg_graph_bar_01_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_bar_01_c.png
nm_bg_graph_bar_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_bar_01.png
nm_bg_graph_custom_a_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_a_c.png
nm_bg_graph_custom_a = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_a.png
nm_bg_graph_custom_b_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_b_c.png
nm_bg_graph_custom_b = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_b.png
nm_bg_graph_custom_a_c_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_a_c_01.png
nm_bg_graph_custom_a_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_a_01.png
nm_bg_graph_custom_b_c_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_b_c_01.png
nm_bg_graph_custom_b_01 = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_custom_b_01.png
nm_dot_b = image://AppHomeScreen/{nmode|performance}/nm_dot_b.png
nm_bg_graph_power_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_power_c.png
nm_bg_graph_power = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_power.png
nm_bg_graph_torque_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_torque_c.png
nm_bg_graph_torque = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_torque.png
nm_bg_graph_turbo_c = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_turbo_c.png
nm_bg_graph_turbo = image://AppHomeScreen/{nmode|performance}/nm_bg_graph_turbo.png
nm_bg_image_c = image://AppHomeScreen/{nmode|performance}/nm_bg_image_c.png
nm_bg_image = image://AppHomeScreen/{nmode|performance}/nm_bg_image.png
nm_bg_performancetimer_c = image://AppHomeScreen/{nmode|performance}/nm_bg_performancetimer_c.png
nm_bg_performancetimer = image://AppHomeScreen/{nmode|performance}/nm_bg_performancetimer.png
nm_bg_performancetimer_racing = image://AppHomeScreen/{nmode|performance}/nm_bg_performancetimer_racing.png
nm_bg_performancetimer_racing_widget_d = image://AppHomeScreen/{nmode|performance}/nm_bg_performancetimer_racing_widget_d.png
nm_bg_performancetimer_racing_widget_n = image://AppHomeScreen/{nmode|performance}/nm_bg_performancetimer_racing_widget_n.png
nm_bg_performance_optic = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_optic.png
nm_bg_performance_optic_c = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_optic_c.png
nm_bg_performance_launchcontrol = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_launchcontrol.png
nm_bg_performance_shiftlight = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_shiftlight.png
nm_bg_performance_senseshift = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_senseshift.png
nm_bg_performance_powershift = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_powershift.png
nm_bg_performance_creep_start = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_creep_start.png
nm_bg_performance_shiftlight_on = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_shiftlight_on.png
nm_bg_performance_shiftlight_off = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_shiftlight_off.png
nm_bg_performance_ntrack_off = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_ntrack_off.png
nm_bg_performance_ntrack_on = image://AppHomeScreen/{nmode|performance}/nm_bg_performance_ntrack_on.png
usm_dm_007 = image://AppHomeScreen/setup/usm/usm_dm_007
set_n_setting = image://AppHomeScreen/setup/usm/set_n_setting
nm_bg_power_c = image://AppHomeScreen/{nmode|performance}/nm_bg_power_c.png
nm_bg_power = image://AppHomeScreen/{nmode|performance}/nm_bg_power.png
nm_bg_racing_c = image://AppHomeScreen/{nmode|performance}/nm_bg_racing_c.png
nm_bg_racing = image://AppHomeScreen/{nmode|performance}/nm_bg_racing.png
nm_bg_racing_widget_center = image://AppHomeScreen/{nmode|performance}/nm_bg_racing_widget_center.png
nm_bg_racing_widget_center_s = image://AppHomeScreen/{nmode|performance}/nm_bg_racing_widget_center_s.png
nm_bg_racing_widget_left = image://AppHomeScreen/{nmode|performance}/nm_bg_racing_widget_left.png
nm_bg_racing_widget_left_s = image://AppHomeScreen/{nmode|performance}/nm_bg_racing_widget_left_s.png
nm_bg_revmatching_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_normal.png
nm_bg_revmatching_off = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_off.png
nm_bg_revmatching_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_sport_plus.png
nm_bg_revmatching_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_sport.png
nm_bg_revmatching_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_normal_c.png
nm_bg_revmatching_off_c = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_off_c.png
nm_bg_revmatching_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_sport_plus_c.png
nm_bg_revmatching_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_revmatching_sport_c.png
nm_bg_split_01_l = image://AppHomeScreen/{nmode|performance}/nm_bg_split_01_l.png
nm_bg_split_01_m = image://AppHomeScreen/{nmode|performance}/nm_bg_split_01_m.png
nm_bg_split_01_r = image://AppHomeScreen/{nmode|performance}/nm_bg_split_01_r.png
nm_bg_steering_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_normal.png
nm_bg_steering_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_sport_plus.png
nm_bg_steering_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_sport.png
nm_bg_suspension_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_normal.png
nm_bg_suspension_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_sport_plus.png
nm_bg_suspension_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_sport.png
nm_bg_steering_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_normal_c.png
nm_bg_steering_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_sport_plus_c.png
nm_bg_steering_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_steering_sport_c.png
nm_bg_suspension_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_normal_c.png
nm_bg_suspension_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_sport_plus_c.png
nm_bg_suspension_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_suspension_sport_c.png
nm_bg_torque_c = image://AppHomeScreen/{nmode|performance}/nm_bg_torque_c.png
nm_bg_torque = image://AppHomeScreen/{nmode|performance}/nm_bg_torque.png
nm_bg_traction_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_normal.png
nm_bg_traction_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_sport_plus.png
nm_bg_traction_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_sport.png
nm_bg_transmission_eco = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_eco.png
nm_bg_transmission_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_normal.png
nm_bg_transmission_sport_plus = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_sport_plus.png
nm_bg_transmission_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_sport.png
nm_bg_hud_normal = image://AppHomeScreen/{nmode|performance}/nm_bg_hud_normal.png
nm_bg_hud_sport = image://AppHomeScreen/{nmode|performance}/nm_bg_hud_sport.png
nm_bg_traction_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_normal_c.png
nm_bg_traction_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_sport_plus_c.png
nm_bg_traction_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_traction_sport_c.png
nm_bg_transmission_eco_c = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_eco_c.png
nm_bg_transmission_normal_c = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_normal_c.png
nm_bg_transmission_sport_plus_c = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_sport_plus_c.png
nm_bg_transmission_sport_c = image://AppHomeScreen/{nmode|performance}/nm_bg_transmission_sport_c.png
nm_bg_turbo_c = image://AppHomeScreen/{nmode|performance}/nm_bg_turbo_c.png
nm_bg_turbo = image://AppHomeScreen/{nmode|performance}/nm_bg_turbo.png
nm_bg_widget_bg_c_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_bg_c_left.png
nm_bg_widget_bg_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_bg_left.png
nm_bg_widget_bg_shadow_c = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_bg_shadow_c.png
nm_bg_widget_bg_shadow = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_bg_shadow.png
nm_bg_widget_gforce_c = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_gforce_c.png
nm_bg_widget_gforce = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_gforce.png
nm_bg_widget_graph_bar_c_center = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_bar_c_center.png
nm_bg_widget_graph_bar_c_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_bar_c_left.png
nm_bg_widget_graph_bar = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_bar.png
nm_bg_widget_graph_power_c_center = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_power_c_center.png
nm_bg_widget_graph_power_c_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_power_c_left.png
nm_bg_widget_graph_power = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_power.png
nm_bg_widget_graph_torque_c_center = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_torque_c_center.png
nm_bg_widget_graph_torque_c_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_torque_c_left.png
nm_bg_widget_graph_torque = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_torque.png
nm_bg_widget_graph_turbo_c_center = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_turbo_c_center.png
nm_bg_widget_graph_turbo_c_left = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_turbo_c_left.png
nm_bg_widget_graph_turbo = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_graph_turbo.png
nm_bg_widget_timer_bg_c = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_timer_bg_c.png
nm_bg_widget_timer_bg = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_timer_bg.png
nm_bg_widget_optic_bg_c = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_optic_bg_c.png
nm_bg_widget_optic_bg = image://AppHomeScreen/{nmode|performance}/nm_bg_widget_optic_bg.png
nm_btn_bg_start_d = image://AppHomeScreen/{nmode|performance}/nm_btn_bg_start_d.png
nm_btn_bg_start_f = image://AppHomeScreen/{nmode|performance}/nm_btn_bg_start_f.png
nm_btn_bg_start_n = image://AppHomeScreen/{nmode|performance}/nm_btn_bg_start_n.png
nm_btn_bg_start_p = image://AppHomeScreen/{nmode|performance}/nm_btn_bg_start_p.png
nm_btn_bg_start_s = image://AppHomeScreen/{nmode|performance}/nm_btn_bg_start_s.png
nm_btn_control_d_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_d_d.png
nm_btn_control_d_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_d_n.png
nm_btn_control_d_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_d_p.png
nm_btn_control_d_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_d_s.png
nm_btn_control_s_d_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_d_d.png
nm_btn_control_s_d_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_d_n.png
nm_btn_control_s_d_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_d_p.png
nm_btn_control_s_d_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_d_s.png
nm_btn_control_s_u_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_u_d.png
nm_btn_control_s_u_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_u_n.png
nm_btn_control_s_u_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_u_p.png
nm_btn_control_s_u_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_s_u_s.png
nm_btn_control_u_d = image://AppHomeScreen/{nmode|performance}/nm_btn_control_u_d.png
nm_btn_control_u_n = image://AppHomeScreen/{nmode|performance}/nm_btn_control_u_n.png
nm_btn_control_u_p = image://AppHomeScreen/{nmode|performance}/nm_btn_control_u_p.png
nm_btn_control_u_s = image://AppHomeScreen/{nmode|performance}/nm_btn_control_u_s.png
nm_btn_plus_s = image://AppHomeScreen/{nmode|performance}/nm_btn_plus_s.png
nm_etc_edit_racing_c = image://AppHomeScreen/{nmode|performance}/nm_etc_edit_racing_c.png
nm_ic_edit_arrow = image://AppHomeScreen/{nmode|performance}/nm_ic_edit_arrow.png
nm_ic_set_n = image://AppHomeScreen/{nmode|performance}/nm_ic_set_n.png
nm_icon_dot_blue = image://AppHomeScreen/{nmode|performance}/nm_icon_dot_blue.png
nm_icon_dot_gray = image://AppHomeScreen/{nmode|performance}/nm_icon_dot_gray.png
nm_icon_dot_orange = image://AppHomeScreen/{nmode|performance}/nm_icon_dot_orange.png
nm_icon_dot_red = image://AppHomeScreen/{nmode|performance}/nm_icon_dot_red.png
nm_icon_dot_white = image://AppHomeScreen/{nmode|performance}/nm_icon_dot_white.png
nm_icon_gforce_dot_01 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_01.png
nm_icon_gforce_dot_02 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_02.png
nm_icon_gforce_dot_03 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_03.png
nm_icon_gforce_dot_04 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_04.png
nm_icon_gforce_dot_05 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_05.png
nm_icon_gforce_dot_06 = image://AppHomeScreen/{nmode|performance}/nm_icon_gforce_dot_06.png
nm_icon_line_blue = image://AppHomeScreen/{nmode|performance}/nm_icon_line_blue.png
nm_icon_line_blue_m = image://AppHomeScreen/{nmode|performance}/nm_icon_line_blue_m.png
nm_icon_line_red = image://AppHomeScreen/{nmode|performance}/nm_icon_line_red.png
nm_icon_trophy_l = image://AppHomeScreen/{nmode|performance}/nm_icon_trophy_l.png
nm_icon_trophy_s = image://AppHomeScreen/{nmode|performance}/nm_icon_trophy_s.png
nm_icon_trophy_lap_01 = image://AppHomeScreen/{nmode|performance}/nm_icon_trophy_lap_01.png
nm_icon_trophy_lap_02 = image://AppHomeScreen/{nmode|performance}/nm_icon_trophy_lap_02.png
nm_lap_rank_bg = image://AppHomeScreen/{nmode|performance}/nm_lap_rank_bg.png
nm_lap_rank_bg_s = image://AppHomeScreen/{nmode|performance}/nm_lap_rank_bg_s.png
nm_widget_push_01_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_01_l.png
nm_widget_push_01_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_01_m.png
nm_widget_push_01_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_01_r.png
nm_widget_push_02_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_02_l.png
nm_widget_push_02_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_02_m.png
nm_widget_push_02_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_02_r.png
nm_widget_push_03_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_03_l.png
nm_widget_push_03_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_03_m.png
nm_widget_push_03_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_03_r.png
nm_widget_push_04_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_04_l.png
nm_widget_push_04_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_04_m.png
nm_widget_push_04_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_04_r.png
nm_widget_push_05_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_05_l.png
nm_widget_push_05_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_05_m.png
nm_widget_push_05_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_05_r.png
nm_widget_push_06_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_06_l.png
nm_widget_push_06_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_06_m.png
nm_widget_push_06_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_06_r.png
nm_widget_push_07_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_07_l.png
nm_widget_push_07_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_07_m.png
nm_widget_push_07_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_07_r.png
nm_widget_push_08_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_08_l.png
nm_widget_push_08_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_08_m.png
nm_widget_push_08_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_08_r.png
nm_widget_push_09_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_09_l.png
nm_widget_push_09_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_09_m.png
nm_widget_push_09_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_09_r.png
nm_widget_push_10_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_10_l.png
nm_widget_push_10_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_10_m.png
nm_widget_push_10_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_10_r.png
nm_widget_push_11_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_11_l.png
nm_widget_push_11_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_11_m.png
nm_widget_push_11_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_11_r.png
nm_widget_push_12_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_12_l.png
nm_widget_push_12_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_12_m.png
nm_widget_push_12_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_12_r.png
nm_widget_push_13_l = image://AppHomeScreen/{nmode|performance}/nm_widget_push_13_l.png
nm_widget_push_13_m = image://AppHomeScreen/{nmode|performance}/nm_widget_push_13_m.png
nm_widget_push_13_r = image://AppHomeScreen/{nmode|performance}/nm_widget_push_13_r.png
nm_icon_rec = image://AppHomeScreen/{nmode|performance}/nm_icon_rec.png
ev_bg_graph_shadow = image://AppHomeScreen/ev/ev_bg_graph_shadow.png
ev_bg_reduction = image://AppHomeScreen/ev/ev_bg_reduction.png
ev_bg_wintermode_img = image://AppHomeScreen/ev/ev_bg_wintermode_img.png
ev_bg_conditioning_img = image://AppHomeScreen/ev/ev_bg_conditioning_img_@.png
ev_btn_bg_h99_d = image://AppHomeScreen/ev/ev_btn_bg_h99_d.png
ev_btn_bg_h99_f = image://AppHomeScreen/ev/ev_btn_bg_h99_f.png
ev_btn_bg_h99_n = image://AppHomeScreen/ev/ev_btn_bg_h99_n.png
ev_btn_bg_h99_p = image://AppHomeScreen/ev/ev_btn_bg_h99_p.png
ev_btn_climate_d = image://AppHomeScreen/ev/ev_btn_climate_d.png
ev_btn_climate_f = image://AppHomeScreen/ev/ev_btn_climate_f.png
ev_btn_climate_n = image://AppHomeScreen/ev/ev_btn_climate_n.png
ev_btn_climate_p = image://AppHomeScreen/ev/ev_btn_climate_p.png
ev_btn_climate_s = image://AppHomeScreen/ev/ev_btn_climate_s.png
ev_btn_setting_d = image://AppHomeScreen/ev/ev_btn_setting_d.png
ev_btn_setting_f = image://AppHomeScreen/ev/ev_btn_setting_f.png
ev_btn_setting_n = image://AppHomeScreen/ev/ev_btn_setting_n.png
ev_btn_setting_p = image://AppHomeScreen/ev/ev_btn_setting_p.png
ev_charging_bar_01_l = image://AppHomeScreen/ev/ev_charging_bar_01_l.png
ev_charging_bar_01_m = image://AppHomeScreen/ev/ev_charging_bar_01_m.png
ev_charging_bar_01_r = image://AppHomeScreen/ev/ev_charging_bar_01_r.png
ev_charging_bar_02_l = image://AppHomeScreen/ev/ev_charging_bar_02_l.png
ev_charging_bar_02_m = image://AppHomeScreen/ev/ev_charging_bar_02_m.png
ev_charging_bar_02_r = image://AppHomeScreen/ev/ev_charging_bar_02_r.png
ev_charging_bar_03_l = image://AppHomeScreen/ev/ev_charging_bar_03_l.png
ev_charging_bar_03_m = image://AppHomeScreen/ev/ev_charging_bar_03_m.png
ev_charging_bar_03_r = image://AppHomeScreen/ev/ev_charging_bar_03_r.png
ev_charging_bar_03 = image://AppHomeScreen/ev/ev_charging_bar_03.png
ev_charging_bar_03_s = image://AppHomeScreen/ev/ev_charging_bar_03_s.png
ev_charging_bg_s_01 = image://AppHomeScreen/ev/ev_charging_bg_s_01.png
ev_charging_bg_s_02 = image://AppHomeScreen/ev/ev_charging_bg_s_02.png
ev_charging_bg_w_01 = image://AppHomeScreen/ev/ev_charging_bg_w_01.png
ev_charging_bg_w_02 = image://AppHomeScreen/ev/ev_charging_bg_w_02.png
ev_charging_flow = image://AppHomeScreen/ev/ev_charging_flow.png
ev_charging_flow_bg = image://AppHomeScreen/ev/ev_charging_flow_bg.png
ev_charging_shadow_l = image://AppHomeScreen/ev/ev_charging_shadow_l.png
ev_charging_shadow_m = image://AppHomeScreen/ev/ev_charging_shadow_m.png
ev_charging_shadow_r = image://AppHomeScreen/ev/ev_charging_shadow_r.png
ev_energyinfo_bar = image://AppHomeScreen/ev/ev_energyinfo_bar.png
ev_energyinfo_bg = image://AppHomeScreen/ev/ev_energyinfo_bg.png
ev_etc_charging_pattern_l_01 = image://AppHomeScreen/ev/ev_etc_charging_pattern_l_01.png
ev_etc_charging_pattern_l_02 = image://AppHomeScreen/ev/ev_etc_charging_pattern_l_02.png
ev_etc_charging_pattern_r_01 = image://AppHomeScreen/ev/ev_etc_charging_pattern_r_01.png
ev_etc_charging_pattern_r_02 = image://AppHomeScreen/ev/ev_etc_charging_pattern_r_02.png
ev_etc_charging_pattern_r_03 = image://AppHomeScreen/ev/ev_etc_charging_pattern_r_03.png
ev_etc_charging_plug = image://AppHomeScreen/ev/ev_etc_charging_plug.png
ev_etc_charging_time_bar_l = image://AppHomeScreen/ev/ev_etc_charging_time_bar_l.png
ev_etc_charging_time_bar_m = image://AppHomeScreen/ev/ev_etc_charging_time_bar_m.png
ev_etc_charging_time_bar_r = image://AppHomeScreen/ev/ev_etc_charging_time_bar_r.png
ev_etc_charging_time_line = image://AppHomeScreen/ev/ev_etc_charging_time_line.png
ev_etc_eco_driving_graph = image://AppHomeScreen/ev/ev_etc_eco_driving_graph.png
ev_etc_eco_driving_graph_01 = image://AppHomeScreen/ev/ev_etc_eco_driving_graph_01.png
ev_etc_eco_driving_graph_02 = image://AppHomeScreen/ev/ev_etc_eco_driving_graph_02.png
ev_etc_eco_driving_line_01 = image://AppHomeScreen/ev/ev_etc_eco_driving_line_01.png
ev_etc_eco_driving_line_02 = image://AppHomeScreen/ev/ev_etc_eco_driving_line_02.png
ev_etc_eco_driving_line_03 = image://AppHomeScreen/ev/ev_etc_eco_driving_line_03.png
ev_etc_eu_battery_care_l = image://AppHomeScreen/ev/ev_etc_eu_battery_care_l.png
ev_etc_eu_battery_care_m = image://AppHomeScreen/ev/ev_etc_eu_battery_care_m.png
ev_etc_eu_battery_care_s = image://AppHomeScreen/ev/ev_etc_eu_battery_care_s.png
ev_etc_eu_climate_l = image://AppHomeScreen/ev/ev_etc_eu_climate_l.png
ev_etc_eu_climate_m = image://AppHomeScreen/ev/ev_etc_eu_climate_m.png
ev_etc_eu_climate_s = image://AppHomeScreen/ev/ev_etc_eu_climate_s.png
ev_etc_eu_drivetrain_l = image://AppHomeScreen/ev/ev_etc_eu_drivetrain_l.png
ev_etc_eu_drivetrain_m = image://AppHomeScreen/ev/ev_etc_eu_drivetrain_m.png
ev_etc_eu_drivetrain_s = image://AppHomeScreen/ev/ev_etc_eu_drivetrain_s.png
ev_etc_eu_electronics_l = image://AppHomeScreen/ev/ev_etc_eu_electronics_l.png
ev_etc_eu_electronics_m = image://AppHomeScreen/ev/ev_etc_eu_electronics_m.png
ev_etc_eu_electronics_s = image://AppHomeScreen/ev/ev_etc_eu_electronics_s.png
ev_etc_graph_scroll = image://AppHomeScreen/ev/ev_etc_graph_scroll.png
ev_etc_plug_bg_01 = image://AppHomeScreen/ev/ev_etc_plug_bg_01.png
ev_etc_plug_bg_02 = image://AppHomeScreen/ev/ev_etc_plug_bg_02.png
ev_etc_plug_dash_01 = image://AppHomeScreen/ev/ev_etc_plug_dash_01.png
ev_etc_plug_dash_02 = image://AppHomeScreen/ev/ev_etc_plug_dash_02.png
ev_etc_plug_l = image://AppHomeScreen/ev/ev_etc_plug_l.png
ev_etc_plug_r = image://AppHomeScreen/ev/ev_etc_plug_r.png
ev_ic_carbon_dioxide_redution = image://AppHomeScreen/ev/ev_ic_carbon_dioxide_redution.png
ev_ic_climate_02 = image://AppHomeScreen/ev/ev_ic_climate_02.png
ev_ic_climate_l_d = image://AppHomeScreen/ev/ev_ic_climate_l_d.png
ev_ic_climate_l_f = image://AppHomeScreen/ev/ev_ic_climate_l_f.png
ev_ic_climate_l_n = image://AppHomeScreen/ev/ev_ic_climate_l_n.png
ev_ic_climate_l_p = image://AppHomeScreen/ev/ev_ic_climate_l_p.png
ev_ic_climate_l_s = image://AppHomeScreen/ev/ev_ic_climate_l_s.png
ev_ic_climate_s_d = image://AppHomeScreen/ev/ev_ic_climate_s_d.png
ev_ic_climate_s_n = image://AppHomeScreen/ev/ev_ic_climate_s_n.png
ev_ic_driving_distance = image://AppHomeScreen/ev/ev_ic_driving_distance.png
ev_ic_ev_economy = image://AppHomeScreen/ev/ev_ic_ev_economy.png
ev_ic_l_off = image://AppHomeScreen/ev/ev_ic_l_off.png
ev_ic_l_on = image://AppHomeScreen/ev/ev_ic_l_on.png
ev_ic_off = image://AppHomeScreen/ev/ev_ic_off.png
ev_ic_on = image://AppHomeScreen/ev/ev_ic_on.png
ev_ic_reduction = image://AppHomeScreen/ev/ev_ic_reduction.png
ev_ic_reduction_down = image://AppHomeScreen/ev/ev_ic_reduction_down.png
co_bg_box_106_l_n = image://AppHomeScreen/general/co_bg_box_106_l_n.png
co_bg_box_106_l_d = image://AppHomeScreen/general/co_bg_box_106_l_d.png
co_bg_box_106_l_f = image://AppHomeScreen/general/co_bg_box_106_l_f.png
co_bg_box_106_l_p = image://AppHomeScreen/general/co_bg_box_106_l_p.png
co_bg_box_106_m_n = image://AppHomeScreen/general/co_bg_box_106_m_n.png
co_bg_box_106_m_d = image://AppHomeScreen/general/co_bg_box_106_m_d.png
co_bg_box_106_m_f = image://AppHomeScreen/general/co_bg_box_106_m_f.png
co_bg_box_106_m_p = image://AppHomeScreen/general/co_bg_box_106_m_p.png
co_bg_box_106_r_n = image://AppHomeScreen/general/co_bg_box_106_r_n.png
co_bg_box_106_r_d = image://AppHomeScreen/general/co_bg_box_106_r_d.png
co_bg_box_106_r_f = image://AppHomeScreen/general/co_bg_box_106_r_f.png
co_bg_box_106_r_p = image://AppHomeScreen/general/co_bg_box_106_r_p.png
co_bg_sublist_162_l = image://AppHomeScreen/general/co_bg_sublist_162_l.png
co_bg_sublist_162_m = image://AppHomeScreen/general/co_bg_sublist_162_m.png
co_bg_sublist_162_r = image://AppHomeScreen/general/co_bg_sublist_162_r.png
co_bg_sublist_324_l = image://AppHomeScreen/general/co_bg_sublist_324_l.png
co_bg_sublist_324_m = image://AppHomeScreen/general/co_bg_sublist_324_m.png
co_bg_sublist_324_r = image://AppHomeScreen/general/co_bg_sublist_324_r.png
co_split_bg = image://AppHomeScreen/general/co_split_bg.png
me_ic_beep_on = image://AppHomeScreen/media/me_ic_beep_on.png
me_ic_beep_off = image://AppHomeScreen/media/me_ic_beep_off.png
me_ic_beep_01_on = image://AppHomeScreen/media/me_ic_beep_01_on.png
me_ic_beep_01_off = image://AppHomeScreen/media/me_ic_beep_01_off.png
rear_ic_beep_on = image://AppHomeScreen/rear/rear_ic_beep_on.png
rear_ic_beep_off = image://AppHomeScreen/rear/rear_ic_beep_off.png
rear_ic_sleep = image://AppHomeScreen/rear/re_ic_sleep.png
re_img_quietmode = image://AppHomeScreen/rear/re_img_quietmode.png
ev_bg_charging_shadow_l = image://AppHomeScreen/ev/ev_bg_charging_shadow_l.png
ev_bg_charging_shadow_m = image://AppHomeScreen/ev/ev_bg_charging_shadow_m.png
ev_bg_charging_shadow_r = image://AppHomeScreen/ev/ev_bg_charging_shadow_r.png
ev_bg_discharge_flow_s = image://AppHomeScreen/ev/ev_bg_discharge_flow_s.png
ev_bg_discharge_flow_w = image://AppHomeScreen/ev/ev_bg_discharge_flow_w.png
ev_bg_discharge_shadow = image://AppHomeScreen/ev/ev_bg_discharge_shadow.png
ev_btn_bg_h66_f = image://AppHomeScreen/ev/ev_btn_bg_h66_f.png
ev_btn_bg_h66_n = image://AppHomeScreen/ev/ev_btn_bg_h66_n.png
ev_btn_bg_h66_p = image://AppHomeScreen/ev/ev_btn_bg_h66_p.png
ev_btn_bg_h66_s = image://AppHomeScreen/ev/ev_btn_bg_h66_s.png
ev_btn_bottom_bg_s = image://AppHomeScreen/ev/ev_btn_bottom_bg_s.png
ev_btn_bottom_bg_w = image://AppHomeScreen/ev/ev_btn_bottom_bg_w.png
ev_btn_bottom_l_f = image://AppHomeScreen/ev/ev_btn_bottom_l_f.png
ev_btn_bottom_l_p = image://AppHomeScreen/ev/ev_btn_bottom_l_p.png
ev_btn_bottom_m_f = image://AppHomeScreen/ev/ev_btn_bottom_m_f.png
ev_btn_bottom_m_p = image://AppHomeScreen/ev/ev_btn_bottom_m_p.png
ev_btn_bottom_r_f = image://AppHomeScreen/ev/ev_btn_bottom_r_f.png
ev_btn_bottom_r_p = image://AppHomeScreen/ev/ev_btn_bottom_r_p.png
ev_btn_main_setting_f = image://AppHomeScreen/ev/ev_btn_main_setting_f.png
ev_btn_main_setting_n = image://AppHomeScreen/ev/ev_btn_main_setting_n.png
ev_btn_main_setting_p = image://AppHomeScreen/ev/ev_btn_main_setting_p.png
ev_btn_time_h60_d = image://AppHomeScreen/ev/ev_btn_time_h60_d.png
ev_btn_time_h75_d = image://AppHomeScreen/ev/ev_btn_time_h75_d.png
ev_charging_bar_b_l = image://AppHomeScreen/ev/ev_charging_bar_b_l.png
ev_charging_bar_b_m = image://AppHomeScreen/ev/ev_charging_bar_b_m.png
ev_charging_bar_b_r = image://AppHomeScreen/ev/ev_charging_bar_b_r.png
ev_charging_bar_g_l = image://AppHomeScreen/ev/ev_charging_bar_g_l.png
ev_charging_bar_g_m = image://AppHomeScreen/ev/ev_charging_bar_g_m.png
ev_charging_bar_g_r = image://AppHomeScreen/ev/ev_charging_bar_g_r.png
ev_charging_bg_s = image://AppHomeScreen/ev/ev_charging_bg_s.png
ev_charging_bg_w = image://AppHomeScreen/ev/ev_charging_bg_w.png
ev_charging_flow_bg_l = image://AppHomeScreen/ev/ev_charging_flow_bg_l.png
ev_charging_flow_bg_m = image://AppHomeScreen/ev/ev_charging_flow_bg_m.png
ev_charging_flow_bg_r = image://AppHomeScreen/ev/ev_charging_flow_bg_r.png
ev_charging_shadow_b_l = image://AppHomeScreen/ev/ev_charging_shadow_b_l.png
ev_charging_shadow_b_m = image://AppHomeScreen/ev/ev_charging_shadow_b_m.png
ev_charging_shadow_b_r = image://AppHomeScreen/ev/ev_charging_shadow_b_r.png
ev_charging_shadow_g_l = image://AppHomeScreen/ev/ev_charging_shadow_g_l.png
ev_charging_shadow_g_m = image://AppHomeScreen/ev/ev_charging_shadow_g_m.png
ev_charging_shadow_g_r = image://AppHomeScreen/ev/ev_charging_shadow_g_r.png
ev_discharging_bar = image://AppHomeScreen/ev/ev_discharging_bar.png
ev_etc_battery_01 = image://AppHomeScreen/ev/ev_etc_battery_01.png
ev_etc_battery_02 = image://AppHomeScreen/ev/ev_etc_battery_02.png
ev_etc_battery_03 = image://AppHomeScreen/ev/ev_etc_battery_03.png
ev_etc_battery_04 = image://AppHomeScreen/ev/ev_etc_battery_04.png
ev_etc_battery_05 = image://AppHomeScreen/ev/ev_etc_battery_05.png
ev_etc_battery_06 = image://AppHomeScreen/ev/ev_etc_battery_06.png
ev_etc_battery_07 = image://AppHomeScreen/ev/ev_etc_battery_07.png
ev_etc_battery_08 = image://AppHomeScreen/ev/ev_etc_battery_08.png
ev_etc_battery_09 = image://AppHomeScreen/ev/ev_etc_battery_09.png
ev_etc_battery_10 = image://AppHomeScreen/ev/ev_etc_battery_10.png
ev_etc_battery_11 = image://AppHomeScreen/ev/ev_etc_battery_11.png
ev_etc_battery_12 = image://AppHomeScreen/ev/ev_etc_battery_12.png
ev_etc_battery_13 = image://AppHomeScreen/ev/ev_etc_battery_13.png
ev_etc_battery_14 = image://AppHomeScreen/ev/ev_etc_battery_14.png
ev_etc_battery_15 = image://AppHomeScreen/ev/ev_etc_battery_15.png
ev_etc_battery_16 = image://AppHomeScreen/ev/ev_etc_battery_16.png
ev_etc_battery_17 = image://AppHomeScreen/ev/ev_etc_battery_17.png
ev_etc_battery_18 = image://AppHomeScreen/ev/ev_etc_battery_18.png
ev_etc_battery_charging = image://AppHomeScreen/ev/ev_etc_battery_charging.png
ev_etc_battery_unit_nor = image://AppHomeScreen/ev/ev_etc_battery_unit_nor.png
ev_etc_battery_unit_war = image://AppHomeScreen/ev/ev_etc_battery_unit_war.png
ev_etc_bottom_divider = image://AppHomeScreen/ev/ev_etc_bottom_divider.png
ev_etc_bubble_level = image://AppHomeScreen/ev/ev_etc_bubble_level.png
ev_etc_discharge_target = image://AppHomeScreen/ev/ev_etc_discharge_target.png
ev_ic_charging_info = image://AppHomeScreen/ev/ev_ic_charging_info.png
ev_ic_charging_info_f = image://AppHomeScreen/ev/ev_ic_charging_info_f.png
ev_ic_charging_info_yellow = image://AppHomeScreen/ev/ev_ic_charging_info_yellow.png
ev_etc_level_info = image://AppHomeScreen/ev/ev_etc_level_info.png
ev_etc_line_l = image://AppHomeScreen/ev/ev_etc_line_l.png
ev_etc_line_m = image://AppHomeScreen/ev/ev_etc_line_m.png
ev_etc_line_r = image://AppHomeScreen/ev/ev_etc_line_r.png
ev_etc_main_case1_01 = image://AppHomeScreen/ev/ev_etc_main_case1_01.png
ev_etc_main_case1_02 = image://AppHomeScreen/ev/ev_etc_main_case1_02.png
ev_etc_main_case1_03 = image://AppHomeScreen/ev/ev_etc_main_case1_03.png
ev_etc_main_case1_04 = image://AppHomeScreen/ev/ev_etc_main_case1_04.png
ev_etc_main_case1_05 = image://AppHomeScreen/ev/ev_etc_main_case1_05.png
ev_etc_main_case1_06 = image://AppHomeScreen/ev/ev_etc_main_case1_06.png
ev_etc_main_case2_07 = image://AppHomeScreen/ev/ev_etc_main_case2_07.png
ev_etc_main_case2_08 = image://AppHomeScreen/ev/ev_etc_main_case2_08.png
ev_etc_main_case2_09 = image://AppHomeScreen/ev/ev_etc_main_case2_09.png
ev_etc_main_case2_10 = image://AppHomeScreen/ev/ev_etc_main_case2_10.png
ev_etc_main_case2_11 = image://AppHomeScreen/ev/ev_etc_main_case2_11.png
ev_etc_main_case3_12 = image://AppHomeScreen/ev/ev_etc_main_case3_12.png
ev_etc_main_case3_13 = image://AppHomeScreen/ev/ev_etc_main_case3_13.png
ev_etc_main_case3_14 = image://AppHomeScreen/ev/ev_etc_main_case3_14.png
ev_etc_main_case3_15 = image://AppHomeScreen/ev/ev_etc_main_case3_15.png
ev_etc_main_case3_16 = image://AppHomeScreen/ev/ev_etc_main_case3_16.png
ev_etc_main_case4_17 = image://AppHomeScreen/ev/ev_etc_main_case4_17.png
ev_etc_main_case4_18 = image://AppHomeScreen/ev/ev_etc_main_case4_18.png
ev_etc_main_case4_19 = image://AppHomeScreen/ev/ev_etc_main_case4_19.png
ev_etc_main_case4_20 = image://AppHomeScreen/ev/ev_etc_main_case4_20.png
ev_etc_main_case4_21 = image://AppHomeScreen/ev/ev_etc_main_case4_21.png
ev_etc_main_case4_22 = image://AppHomeScreen/ev/ev_etc_main_case4_22.png
ev_etc_main_c_case1_01 = image://AppHomeScreen/ev/ev_etc_main_c_case1_01.png
ev_etc_main_c_case1_02 = image://AppHomeScreen/ev/ev_etc_main_c_case1_02.png
ev_etc_main_c_case1_03 = image://AppHomeScreen/ev/ev_etc_main_c_case1_03.png
ev_etc_main_c_case1_04 = image://AppHomeScreen/ev/ev_etc_main_c_case1_04.png
ev_etc_main_c_case1_05 = image://AppHomeScreen/ev/ev_etc_main_c_case1_05.png
ev_etc_main_c_case1_06 = image://AppHomeScreen/ev/ev_etc_main_c_case1_06.png
ev_etc_main_c_case2_07 = image://AppHomeScreen/ev/ev_etc_main_c_case2_07.png
ev_etc_main_c_case2_08 = image://AppHomeScreen/ev/ev_etc_main_c_case2_08.png
ev_etc_main_c_case2_09 = image://AppHomeScreen/ev/ev_etc_main_c_case2_09.png
ev_etc_main_c_case2_10 = image://AppHomeScreen/ev/ev_etc_main_c_case2_10.png
ev_etc_main_c_case2_11 = image://AppHomeScreen/ev/ev_etc_main_c_case2_11.png
ev_etc_main_c_case3_12 = image://AppHomeScreen/ev/ev_etc_main_c_case3_12.png
ev_etc_main_c_case3_13 = image://AppHomeScreen/ev/ev_etc_main_c_case3_13.png
ev_etc_main_c_case3_14 = image://AppHomeScreen/ev/ev_etc_main_c_case3_14.png
ev_etc_main_c_case3_15 = image://AppHomeScreen/ev/ev_etc_main_c_case3_15.png
ev_etc_main_c_case3_16 = image://AppHomeScreen/ev/ev_etc_main_c_case3_16.png
ev_etc_main_c_case4_17 = image://AppHomeScreen/ev/ev_etc_main_c_case4_17.png
ev_etc_main_c_case4_18 = image://AppHomeScreen/ev/ev_etc_main_c_case4_18.png
ev_etc_main_c_case4_19 = image://AppHomeScreen/ev/ev_etc_main_c_case4_19.png
ev_etc_main_c_case4_20 = image://AppHomeScreen/ev/ev_etc_main_c_case4_20.png
ev_etc_main_c_case4_21 = image://AppHomeScreen/ev/ev_etc_main_c_case4_21.png
ev_etc_main_c_case4_22 = image://AppHomeScreen/ev/ev_etc_main_c_case4_22.png
ev_etc_plug_dash_03 = image://AppHomeScreen/ev/ev_etc_plug_dash_03.png
ev_ic_battery_bg_nor = image://AppHomeScreen/ev/ev_ic_battery_bg_nor_@.png
ev_ic_battery_bg_war = image://AppHomeScreen/ev/ev_ic_battery_bg_war_@.png
ev_ic_battery_cover = image://AppHomeScreen/ev/ev_ic_battery_cover_@.png
ev_ic_charging_f = image://AppHomeScreen/ev/ev_ic_charging_f.png
ev_ic_charging_n = image://AppHomeScreen/ev/ev_ic_charging_n.png
ev_ic_charging_p = image://AppHomeScreen/ev/ev_ic_charging_p.png
ev_ic_climate_01_f = image://AppHomeScreen/ev/ev_ic_climate_01_f.png
ev_ic_climate_01_n = image://AppHomeScreen/ev/ev_ic_climate_01_n.png
ev_ic_climate_01_p = image://AppHomeScreen/ev/ev_ic_climate_01_p.png
ev_ic_climate_01_d = image://AppHomeScreen/ev/ev_ic_climate_01_d.png
ev_ic_climate_off = image://AppHomeScreen/ev/ev_ic_climate_off.png
ev_ic_climate_on = image://AppHomeScreen/ev/ev_ic_climate_on.png
ev_ic_colon = image://AppHomeScreen/ev/ev_ic_colon.png
ev_ic_defog_d = image://AppHomeScreen/ev/ev_ic_defog_d.png
ev_ic_defog_f = image://AppHomeScreen/ev/ev_ic_defog_f.png
ev_ic_defog_n = image://AppHomeScreen/ev/ev_ic_defog_n.png
ev_ic_defog_p = image://AppHomeScreen/ev/ev_ic_defog_p.png
ev_ic_defog_s = image://AppHomeScreen/ev/ev_ic_defog_s.png
ev_ic_discharge_01 = image://AppHomeScreen/ev/ev_ic_discharge_01.png
ev_ic_discharge_02 = image://AppHomeScreen/ev/ev_ic_discharge_02.png
ev_ic_discharge_f = image://AppHomeScreen/ev/ev_ic_discharge_f.png
ev_ic_discharge_n = image://AppHomeScreen/ev/ev_ic_discharge_n.png
ev_ic_discharge_p = image://AppHomeScreen/ev/ev_ic_discharge_p.png
ev_ic_nextdeparture_f = image://AppHomeScreen/ev/ev_ic_nextdeparture_f.png
ev_ic_nextdeparture_n = image://AppHomeScreen/ev/ev_ic_nextdeparture_n.png
ev_ic_nextdeparture_p = image://AppHomeScreen/ev/ev_ic_nextdeparture_p.png
ev_ic_ns_arrow_01 = image://AppHomeScreen/ev/ev_ic_ns_arrow_01.png
ev_ic_ns_arrow_02 = image://AppHomeScreen/ev/ev_ic_ns_arrow_02.png
ev_ic_ns_arrow_03 = image://AppHomeScreen/ev/ev_ic_ns_arrow_03.png
ev_ic_ns_arrow_04 = image://AppHomeScreen/ev/ev_ic_ns_arrow_04.png
ev_ic_ns_arrow_05 = image://AppHomeScreen/ev/ev_ic_ns_arrow_05.png
ev_ic_ns_s_01_nor = image://AppHomeScreen/ev/ev_ic_ns_s_01_nor.png
ev_ic_ns_s_01_war = image://AppHomeScreen/ev/ev_ic_ns_s_01_war.png
ev_ic_ns_s_02_nor = image://AppHomeScreen/ev/ev_ic_ns_s_02_nor.png
ev_ic_ns_s_02_war = image://AppHomeScreen/ev/ev_ic_ns_s_02_war.png
ev_ic_ns_s_03_nor = image://AppHomeScreen/ev/ev_ic_ns_s_03_nor.png
ev_ic_ns_s_03_war = image://AppHomeScreen/ev/ev_ic_ns_s_03_war.png
ev_ic_ns_s_04_nor = image://AppHomeScreen/ev/ev_ic_ns_s_04_nor.png
ev_ic_ns_s_04_war = image://AppHomeScreen/ev/ev_ic_ns_s_04_war.png
ev_ic_ns_s_05_nor = image://AppHomeScreen/ev/ev_ic_ns_s_05_nor.png
ev_ic_ns_s_05_war = image://AppHomeScreen/ev/ev_ic_ns_s_05_war.png
ev_ic_ns_s_06_nor = image://AppHomeScreen/ev/ev_ic_ns_s_06_nor.png
ev_ic_ns_s_06_war = image://AppHomeScreen/ev/ev_ic_ns_s_06_war.png
ev_ic_ns_s_07_nor = image://AppHomeScreen/ev/ev_ic_ns_s_07_nor.png
ev_ic_ns_s_07_war = image://AppHomeScreen/ev/ev_ic_ns_s_07_war.png
ev_ic_ns_s_08_nor = image://AppHomeScreen/ev/ev_ic_ns_s_08_nor.png
ev_ic_ns_s_08_war = image://AppHomeScreen/ev/ev_ic_ns_s_08_war.png
ev_ic_ns_s_09_nor = image://AppHomeScreen/ev/ev_ic_ns_s_09_nor.png
ev_ic_ns_s_09_war = image://AppHomeScreen/ev/ev_ic_ns_s_09_war.png
ev_ic_ns_s_10_nor = image://AppHomeScreen/ev/ev_ic_ns_s_10_nor.png
ev_ic_ns_s_10_war = image://AppHomeScreen/ev/ev_ic_ns_s_10_war.png
ev_ic_ns_w_01_nor = image://AppHomeScreen/ev/ev_ic_ns_w_01_nor.png
ev_ic_ns_w_01_war = image://AppHomeScreen/ev/ev_ic_ns_w_01_war.png
ev_ic_ns_w_02_nor = image://AppHomeScreen/ev/ev_ic_ns_w_02_nor.png
ev_ic_ns_w_02_war = image://AppHomeScreen/ev/ev_ic_ns_w_02_war.png
ev_ic_ns_w_03_nor = image://AppHomeScreen/ev/ev_ic_ns_w_03_nor.png
ev_ic_ns_w_03_war = image://AppHomeScreen/ev/ev_ic_ns_w_03_war.png
ev_ic_ns_w_04_nor = image://AppHomeScreen/ev/ev_ic_ns_w_04_nor.png
ev_ic_ns_w_04_war = image://AppHomeScreen/ev/ev_ic_ns_w_04_war.png
ev_ic_ns_w_05_nor = image://AppHomeScreen/ev/ev_ic_ns_w_05_nor.png
ev_ic_ns_w_05_war = image://AppHomeScreen/ev/ev_ic_ns_w_05_war.png
ev_ic_ns_w_06_nor = image://AppHomeScreen/ev/ev_ic_ns_w_06_nor.png
ev_ic_ns_w_06_war = image://AppHomeScreen/ev/ev_ic_ns_w_06_war.png
ev_ic_ns_w_07_nor = image://AppHomeScreen/ev/ev_ic_ns_w_07_nor.png
ev_ic_ns_w_07_war = image://AppHomeScreen/ev/ev_ic_ns_w_07_war.png
ev_ic_ns_w_08_nor = image://AppHomeScreen/ev/ev_ic_ns_w_08_nor.png
ev_ic_ns_w_08_war = image://AppHomeScreen/ev/ev_ic_ns_w_08_war.png
ev_ic_ns_w_09_nor = image://AppHomeScreen/ev/ev_ic_ns_w_09_nor.png
ev_ic_ns_w_09_war = image://AppHomeScreen/ev/ev_ic_ns_w_09_war.png
ev_ic_ns_w_10_nor = image://AppHomeScreen/ev/ev_ic_ns_w_10_nor.png
ev_ic_ns_w_10_war = image://AppHomeScreen/ev/ev_ic_ns_w_10_war.png
ev_ic_scheduled_climate_f = image://AppHomeScreen/ev/ev_ic_scheduled_climate_f.png
ev_ic_scheduled_climate_n = image://AppHomeScreen/ev/ev_ic_scheduled_climate_n.png
ev_ic_scheduled_climate_p = image://AppHomeScreen/ev/ev_ic_scheduled_climate_p.png
ev_ic_scheduled_climate_d = image://AppHomeScreen/ev/ev_ic_scheduled_climate_d.png
ev_ic_set_arrow_down_d = image://AppHomeScreen/ev/ev_ic_set_arrow_down_d.png
ev_ic_set_arrow_down_n = image://AppHomeScreen/ev/ev_ic_set_arrow_down_n.png
ev_ic_set_arrow_up_d = image://AppHomeScreen/ev/ev_ic_set_arrow_up_d.png
ev_ic_set_arrow_up_n = image://AppHomeScreen/ev/ev_ic_set_arrow_up_n.png
ev_etc_eu_vehicle_l = image://AppHomeScreen/ev/ev_etc_eu_vehicle_l.png
ev_etc_eu_link_l_01 = image://AppHomeScreen/ev/ev_etc_eu_link_l_01.png
ev_etc_eu_link_l_02 = image://AppHomeScreen/ev/ev_etc_eu_link_l_02.png
ev_etc_eu_link_l_03 = image://AppHomeScreen/ev/ev_etc_eu_link_l_03.png
ev_etc_eu_link_l_04 = image://AppHomeScreen/ev/ev_etc_eu_link_l_04.png
ev_etc_eu_battery_care = image://AppHomeScreen/ev/ev_etc_eu_battery_care.png
ev_etc_eu_climate = image://AppHomeScreen/ev/ev_etc_eu_climate.png
ev_etc_eu_electronics = image://AppHomeScreen/ev/ev_etc_eu_electronics.png
ev_etc_eu_drivetrain = image://AppHomeScreen/ev/ev_etc_eu_drivetrain.png
ev_etc_eu_vehicle_s = image://AppHomeScreen/ev/ev_etc_eu_vehicle_s.png
ev_etc_eu_link_s_01 = image://AppHomeScreen/ev/ev_etc_eu_link_s_01.png
ev_etc_eu_link_s_02 = image://AppHomeScreen/ev/ev_etc_eu_link_s_02.png
ev_etc_eu_link_s_03 = image://AppHomeScreen/ev/ev_etc_eu_link_s_03.png
ev_etc_eu_link_s_04 = image://AppHomeScreen/ev/ev_etc_eu_link_s_04.png
ev_etc_battery_00 = image://AppHomeScreen/ev/ev_etc_battery_00.png
ev_etc_battery_19 = image://AppHomeScreen/ev/ev_etc_battery_19.png
ev_etc_battery_20 = image://AppHomeScreen/ev/ev_etc_battery_20.png
ev_etc_bubble_level_01 = image://AppHomeScreen/ev/ev_etc_bubble_level_01.png
ev_etc_level_info_01 = image://AppHomeScreen/ev/ev_etc_level_info_01.png
ev_ic_battery_pattern_l = image://AppHomeScreen/ev/ev_ic_battery_pattern_l.png
ev_ic_battery_pattern_r = image://AppHomeScreen/ev/ev_ic_battery_pattern_r.png
ev_ic_battery_thunder = image://AppHomeScreen/ev/ev_ic_battery_thunder.png
ev_etc_discharge_flow = image://AppHomeScreen/ev/flow/discharge/ev_etc_discharge_flow
ev_etc_charge_flow = image://AppHomeScreen/ev/flow/charge/ev_etc_charge_flow
ev_btn_bg_list_pop_f = image://AppHomeScreen/ev/ev_btn_bg_list_pop_f.png
ev_btn_bg_list_pop_p = image://AppHomeScreen/ev/ev_btn_bg_list_pop_p.png
ev_btn_bg_list_pop_s = image://AppHomeScreen/ev/ev_btn_bg_list_pop_s.png
ev_charging_bg_01_l = image://AppHomeScreen/ev/ev_charging_bg_01_l.png
ev_charging_bg_01_m = image://AppHomeScreen/ev/ev_charging_bg_01_m.png
ev_charging_bg_01_r = image://AppHomeScreen/ev/ev_charging_bg_01_r.png
ev_charging_bg_l = image://AppHomeScreen/ev/ev_charging_bg_l.png
ev_charging_bg_m = image://AppHomeScreen/ev/ev_charging_bg_m.png
ev_charging_bg_r = image://AppHomeScreen/ev/ev_charging_bg_r.png
ev_discharging_bar_l = image://AppHomeScreen/ev/ev_discharging_bar_l.png
ev_discharging_bar_m = image://AppHomeScreen/ev/ev_discharging_bar_m.png
ev_discharging_bar_r = image://AppHomeScreen/ev/ev_discharging_bar_r.png
ev_etc_divider = image://AppHomeScreen/ev/ev_etc_divider.png
ev_etc_eco_driving_graph_b = image://AppHomeScreen/ev/ev_etc_eco_driving_graph_b.png
ev_etc_eco_driving_graph_m = image://AppHomeScreen/ev/ev_etc_eco_driving_graph_m.png
ev_etc_eco_driving_graph_t = image://AppHomeScreen/ev/ev_etc_eco_driving_graph_t.png
ev_etc_line = image://AppHomeScreen/ev/ev_etc_line.png
ev_ic_climate_off_01 = image://AppHomeScreen/ev/ev_ic_climate_off_01.png
ev_ic_climate_on_01 = image://AppHomeScreen/ev/ev_ic_climate_on_01.png
ev_ic_solar_roof = image://AppHomeScreen/ev/ev_ic_solar_roof.png
ev_solar_roof_00 = image://AppHomeScreen/ev/ev_solar_roof_00.png
ev_solar_roof_01 = image://AppHomeScreen/ev/ev_solar_roof_01.png
ev_solar_roof_02 = image://AppHomeScreen/ev/ev_solar_roof_02.png
ev_solar_roof_03 = image://AppHomeScreen/ev/ev_solar_roof_03.png
ev_solar_roof_bg = image://AppHomeScreen/ev/ev_solar_roof_bg.png
ev_solar_roof_f = image://AppHomeScreen/ev/ev_solar_roof_f.png
ev_solar_roof_p = image://AppHomeScreen/ev/ev_solar_roof_p.png
ev_btn_bottom_s_n = image://AppHomeScreen/ev/ev_btn_bottom_s_n.png
ev_btn_bottom_s_p_l = image://AppHomeScreen/ev/ev_btn_bottom_s_p_l.png
ev_btn_bottom_s_p_r = image://AppHomeScreen/ev/ev_btn_bottom_s_p_r.png
ev_btn_bottom_w_n = image://AppHomeScreen/ev/ev_btn_bottom_w_n.png
ev_btn_bottom_w_p_l = image://AppHomeScreen/ev/ev_btn_bottom_w_p_l.png
ev_btn_bottom_w_p_r = image://AppHomeScreen/ev/ev_btn_bottom_w_p_r.png
ev_charging_bg_cover = image://AppHomeScreen/ev/ev_charging_bg_cover.png
ev_charging_bar_b = image://AppHomeScreen/ev/ev_charging_bar_b.png
ev_charging_bar_g = image://AppHomeScreen/ev/ev_charging_bar_g.png
ev_charging_bg = image://AppHomeScreen/ev/ev_charging_bg.png
ev_charging_bar_flow = image://AppHomeScreen/ev/ev_charging_bar_flow.png
ev_charging_bar_flow_01 = image://AppHomeScreen/ev/ev_charging_bar_flow_01.png
ev_charging_bg_cover_s = image://AppHomeScreen/ev/ev_charging_bg_cover_s.png
ev_charging_bar_b_s = image://AppHomeScreen/ev/ev_charging_bar_b_s.png
ev_charging_bar_g_s = image://AppHomeScreen/ev/ev_charging_bar_g_s.png
ev_charging_bg_01 = image://AppHomeScreen/ev/ev_charging_bg_01.png
ev_charging_bg_cover_01 = image://AppHomeScreen/ev/ev_charging_bg_cover_01.png
ev_charging_bar_02 = image://AppHomeScreen/ev/ev_charging_bar_02.png
ev_charging_bar_flow_02 = image://AppHomeScreen/ev/ev_charging_bar_flow_02.png
ev_charging_bar_flow_03 = image://AppHomeScreen/ev/ev_charging_bar_flow_03.png
ev_charging_bg_01_s = image://AppHomeScreen/ev/ev_charging_bg_01_s.png
ev_charging_bg_cover_01_s = image://AppHomeScreen/ev/ev_charging_bg_cover_01_s.png
ev_charging_bar_02_s = image://AppHomeScreen/ev/ev_charging_bar_02_s.png
ev_btn_bottom_s_01_n = image://AppHomeScreen/ev/ev_btn_bottom_s_01_n.png
ev_btn_bottom_s_01_p = image://AppHomeScreen/ev/ev_btn_bottom_s_01_p.png
ev_btn_bottom_w_01_n = image://AppHomeScreen/ev/ev_btn_bottom_w_01_n.png
ev_btn_bottom_w_01_p = image://AppHomeScreen/ev/ev_btn_bottom_w_01_p.png
ev_bg_utilitymode_img = image://AppHomeScreen/ev/ev_bg_utilitymode_img_@.png
usm_bg_utilitymode_img = image://AppHomeScreen/setup/usm/usm_bg_utilitymode_img.png
ev_bg_temp_box_d = image://AppHomeScreen/ev/ev_bg_temp_box_d.png
ev_bg_temp_box_f = image://AppHomeScreen/ev/ev_bg_temp_box_f.png
ev_bg_temp_box_n = image://AppHomeScreen/ev/ev_bg_temp_box_n.png
ev_bg_temp_box_p = image://AppHomeScreen/ev/ev_bg_temp_box_p.png
ev_btn_temp_d = image://AppHomeScreen/ev/ev_btn_temp_d.png
ev_btn_temp_f = image://AppHomeScreen/ev/ev_btn_temp_f.png
ev_btn_temp_n = image://AppHomeScreen/ev/ev_btn_temp_n.png
ev_btn_temp_p = image://AppHomeScreen/ev/ev_btn_temp_p.png
ev_btn_temp_s = image://AppHomeScreen/ev/ev_btn_temp_s.png
hev_bg_box_line = image://AppHomeScreen/ev/hev_bg_box_line.png
hev_btn_box_n = image://AppHomeScreen/ev/hev_btn_box_n.png
hev_btn_box_p = image://AppHomeScreen/ev/hev_btn_box_p.png
hev_etc_energy_flow_battery_s_thunderbolt = image://AppHomeScreen/ev/hev_etc_energy_flow_battery_s_thunderbolt.png
hev_etc_energy_flow_battery_thunderbolt = image://AppHomeScreen/ev/hev_etc_energy_flow_battery_thunderbolt.png
hev_etc_graph_average = image://AppHomeScreen/ev/hev_etc_graph_average.png
hev_etc_graph_electric_motor_use = image://AppHomeScreen/ev/hev_etc_graph_electric_motor_use.png
hev_flow_bg_2 = image://AppHomeScreen/ev/hev_flow_bg_2.png
hev_flow_bg_s_2 = image://AppHomeScreen/ev/hev_flow_bg_s_2.png
hev_flow_bg_s_3 = image://AppHomeScreen/ev/hev_flow_bg_s_3.png
hev_flow_engine_default = image://AppHomeScreen/ev/hev_flow_engine_default.png
hev_flow_engine_dim = image://AppHomeScreen/ev/hev_flow_engine_dim.png
hev_flow_engine_white = image://AppHomeScreen/ev/hev_flow_engine_white.png
hev_flow_engine_s_white = image://AppHomeScreen/ev/hev_flow_engine_s_white.png
hev_flow_engine_orange = image://AppHomeScreen/ev/hev_flow_engine_orange.png
hev_flow_engine_s_default = image://AppHomeScreen/ev/hev_flow_engine_s_default.png
hev_flow_engine_s_dim = image://AppHomeScreen/ev/hev_flow_engine_s_dim.png
hev_flow_engine_s_orange = image://AppHomeScreen/ev/hev_flow_engine_s_orange.png
hev_flow_mortor_activation = image://AppHomeScreen/ev/hev_flow_mortor_activation.png
hev_flow_mortor_default = image://AppHomeScreen/ev/hev_flow_mortor_default.png
hev_flow_mortor_orange = image://AppHomeScreen/ev/hev_flow_mortor_orange.png
hev_flow_mortor_s_activation = image://AppHomeScreen/ev/hev_flow_mortor_s_activation.png
hev_flow_mortor_s_default = image://AppHomeScreen/ev/hev_flow_mortor_s_default.png
hev_flow_mortor_s_orange = image://AppHomeScreen/ev/hev_flow_mortor_s_orange.png
phev_bg_box_energy_f = image://AppHomeScreen/ev/phev_bg_box_energy_f.png
phev_bg_box_energy_n = image://AppHomeScreen/ev/phev_bg_box_energy_n.png
phev_bg_box_energy_p = image://AppHomeScreen/ev/phev_bg_box_energy_p.png
phev_ic_charging = image://AppHomeScreen/ev/phev_ic_charging.png
phev_ic_ecodriving = image://AppHomeScreen/ev/phev_ic_ecodriving.png
ev_ic_arrow_list = ?
hev_etc_energy_flow_solar_activation_list = ?
hev_etc_energy_flow_solar_default_list = ?
hev_etc_energy_flow_solar_s_activation_list = ?
hev_etc_energy_flow_solar_s_default_list = ?
hev_flow_battery_activation_list = ?
hev_flow_battery_default_list = ?
hev_flow_battery_s_activation_list = ?
hev_flow_battery_s_default_list = ?
hev_flow_bat_mot_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_19.png
hev_flow_bat_mot_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_20.png
hev_flow_bat_mot_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_21.png
hev_flow_bat_mot_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_bat_mot_a_22.png
hev_flow_eng_mot_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_17.png
hev_flow_eng_mot_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_18.png
hev_flow_eng_mot_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_19.png
hev_flow_eng_mot_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_20.png
hev_flow_eng_mot_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_21.png
hev_flow_eng_mot_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_a_22.png
hev_flow_eng_mot_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_09.png
hev_flow_eng_mot_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_10.png
hev_flow_eng_mot_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_11.png
hev_flow_eng_mot_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_12.png
hev_flow_eng_mot_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_13.png
hev_flow_eng_mot_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_14.png
hev_flow_eng_mot_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_15.png
hev_flow_eng_mot_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_16.png
hev_flow_eng_mot_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_17.png
hev_flow_eng_mot_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_18.png
hev_flow_eng_mot_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_19.png
hev_flow_eng_mot_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_20.png
hev_flow_eng_mot_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_21.png
hev_flow_eng_mot_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_mot_b_22.png
hev_flow_eng_whe_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_13.png
hev_flow_eng_whe_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_14.png
hev_flow_eng_whe_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_15.png
hev_flow_eng_whe_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_16.png
hev_flow_eng_whe_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_17.png
hev_flow_eng_whe_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_18.png
hev_flow_eng_whe_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_19.png
hev_flow_eng_whe_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_20.png
hev_flow_eng_whe_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_21.png
hev_flow_eng_whe_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_eng_whe_a_22.png
hev_flow_mot_bat_a_37 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_37.png
hev_flow_mot_bat_a_38 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_38.png
hev_flow_mot_bat_a_39 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_39.png
hev_flow_mot_bat_a_40 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_40.png
hev_flow_mot_bat_a_41 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_41.png
hev_flow_mot_bat_a_42 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_42.png
hev_flow_mot_bat_a_43 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_43.png
hev_flow_mot_bat_a_44 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_bat_a_44.png
hev_flow_mot_whe_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_13.png
hev_flow_mot_whe_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_14.png
hev_flow_mot_whe_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_15.png
hev_flow_mot_whe_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_16.png
hev_flow_mot_whe_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_17.png
hev_flow_mot_whe_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_18.png
hev_flow_mot_whe_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_19.png
hev_flow_mot_whe_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_20.png
hev_flow_mot_whe_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_21.png
hev_flow_mot_whe_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_a_22.png
hev_flow_mot_whe_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_09.png
hev_flow_mot_whe_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_10.png
hev_flow_mot_whe_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_11.png
hev_flow_mot_whe_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_12.png
hev_flow_mot_whe_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_13.png
hev_flow_mot_whe_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_14.png
hev_flow_mot_whe_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_15.png
hev_flow_mot_whe_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_16.png
hev_flow_mot_whe_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_17.png
hev_flow_mot_whe_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_18.png
hev_flow_mot_whe_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_19.png
hev_flow_mot_whe_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_20.png
hev_flow_mot_whe_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_21.png
hev_flow_mot_whe_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_mot_whe_b_22.png
hev_flow_sol_bat_a_01 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_01.png
hev_flow_sol_bat_a_02 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_02.png
hev_flow_sol_bat_a_03 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_03.png
hev_flow_sol_bat_a_04 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_04.png
hev_flow_sol_bat_a_05 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_05.png
hev_flow_sol_bat_a_06 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_06.png
hev_flow_sol_bat_a_07 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_07.png
hev_flow_sol_bat_a_08 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_08.png
hev_flow_sol_bat_a_09 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_09.png
hev_flow_sol_bat_a_10 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_10.png
hev_flow_sol_bat_a_11 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_11.png
hev_flow_sol_bat_a_12 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_12.png
hev_flow_sol_bat_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_13.png
hev_flow_sol_bat_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_14.png
hev_flow_sol_bat_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_15.png
hev_flow_sol_bat_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_16.png
hev_flow_sol_bat_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_17.png
hev_flow_sol_bat_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_18.png
hev_flow_sol_bat_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_19.png
hev_flow_sol_bat_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_20.png
hev_flow_sol_bat_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_21.png
hev_flow_sol_bat_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_sol_bat_a_22.png
hev_flow_whe_eng_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_13.png
hev_flow_whe_eng_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_14.png
hev_flow_whe_eng_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_15.png
hev_flow_whe_eng_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_16.png
hev_flow_whe_eng_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_17.png
hev_flow_whe_eng_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_18.png
hev_flow_whe_eng_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_19.png
hev_flow_whe_eng_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_20.png
hev_flow_whe_eng_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_21.png
hev_flow_whe_eng_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_a_22.png
hev_flow_whe_eng_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_09.png
hev_flow_whe_eng_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_10.png
hev_flow_whe_eng_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_11.png
hev_flow_whe_eng_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_12.png
hev_flow_whe_eng_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_13.png
hev_flow_whe_eng_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_14.png
hev_flow_whe_eng_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_15.png
hev_flow_whe_eng_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_16.png
hev_flow_whe_eng_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_17.png
hev_flow_whe_eng_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_18.png
hev_flow_whe_eng_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_19.png
hev_flow_whe_eng_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_20.png
hev_flow_whe_eng_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_21.png
hev_flow_whe_eng_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_eng_b_22.png
hev_flow_whe_mot_a_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_13.png
hev_flow_whe_mot_a_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_14.png
hev_flow_whe_mot_a_15 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_15.png
hev_flow_whe_mot_a_16 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_16.png
hev_flow_whe_mot_a_17 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_17.png
hev_flow_whe_mot_a_18 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_18.png
hev_flow_whe_mot_a_19 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_19.png
hev_flow_whe_mot_a_20 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_20.png
hev_flow_whe_mot_a_21 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_21.png
hev_flow_whe_mot_a_22 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_a_22.png
hev_flow_whe_mot_b_09 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_09.png
hev_flow_whe_mot_b_10 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_10.png
hev_flow_whe_mot_b_11 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_11.png
hev_flow_whe_mot_b_12 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_12.png
hev_flow_whe_mot_b_13 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_13.png
hev_flow_whe_mot_b_14 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_14.png
hev_flow_whe_mot_b_15 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_15.png
hev_flow_whe_mot_b_16 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_16.png
hev_flow_whe_mot_b_17 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_17.png
hev_flow_whe_mot_b_18 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_18.png
hev_flow_whe_mot_b_19 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_19.png
hev_flow_whe_mot_b_20 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_20.png
hev_flow_whe_mot_b_21 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_21.png
hev_flow_whe_mot_b_22 = image://AppHomeScreen/ev/hevflow/hev_flow_whe_mot_b_22.png
usm_ed_011 = image://AppHomeScreen/setup/usm/usm_ed_011_@.png
usm_ed_011_off = image://AppHomeScreen/setup/usm/usm_ed_011_@_off.png
usm_ed_011_faster = image://AppHomeScreen/setup/usm/usm_ed_011_faster_@.png
usm_ed_011_normal = image://AppHomeScreen/setup/usm/usm_ed_011_normal_@.png
usm_ed_011_slow = image://AppHomeScreen/setup/usm/usm_ed_011_slow_@.png
usm_ed_005_01 = image://AppHomeScreen/setup/usm/usm_ed_005_01.png
usm_ed_005_02 = image://AppHomeScreen/setup/usm/usm_ed_005_02.png
usm_ed_005_03 = image://AppHomeScreen/setup/usm/usm_ed_005_03.png
usm_ed_010_03 = image://AppHomeScreen/setup/usm/usm_ed_010_@_03.png
usm_ed_010_02 = image://AppHomeScreen/setup/usm/usm_ed_010_@_02.png
usm_ed_010_01 = image://AppHomeScreen/setup/usm/usm_ed_010_@_01.png
usm_ed_010_off = image://AppHomeScreen/setup/usm/usm_ed_010_@_off.png
usm_ed_010_01_trailer_mode = image://AppHomeScreen/setup/usm/usm_ed_010_01_@.png
usm_ed_010_04_trailer_mode = image://AppHomeScreen/setup/usm/usm_ed_010_04_@.png
usm_ed_010_03_trailer_mode = image://AppHomeScreen/setup/usm/usm_ed_010_03_@.png
usm_ed_010_02_trailer_mode = image://AppHomeScreen/setup/usm/usm_ed_010_02_@.png
usm_ed_010_00_trailer_mode = image://AppHomeScreen/setup/usm/usm_ed_010_00_@.png
ev_etc_eu_vehicle_nor = image://AppHomeScreen/ev/ev_etc_eu_vehicle_nor_@.png
ev_etc_eu_vehicle_war = image://AppHomeScreen/ev/ev_etc_eu_vehicle_war_@.png
ev_etc_battery_l = image://AppHomeScreen/ev/ev_etc_battery_l_@.png
ev_etc_battery_l_red = image://AppHomeScreen/ev/ev_etc_battery_l_red_@.png
ev_etc_eu_battery_bg = image://AppHomeScreen/ev/ev_etc_eu_battery_bg.png
ev_ic_dot = image://AppHomeScreen/ev/ev_ic_dot.png
ev_bg_charging_bar_gray = image://AppHomeScreen/ev/ev_bg_charging_bar_gray.png
ev_bg_charging_bar_gray_l = image://AppHomeScreen/ev/ev_bg_charging_bar_gray_l.png
ev_bg_charging_bar_gray_m = image://AppHomeScreen/ev/ev_bg_charging_bar_gray_m.png
ev_bg_charging_bar_gray_r = image://AppHomeScreen/ev/ev_bg_charging_bar_gray_r.png
ev_etc_battery_y = image://AppHomeScreen/ev/ev_etc_battery_y_@.png
ev_ic_battery_p_bar_yellow = image://AppHomeScreen/ev/ev_ic_battery_p_bar_yellow.png
```

---

## 4. Fonts

### /app/share/fonts (rootfs, confirmed listing)

| File | Family registered (binary strings ELF 0xc513ea‑0xc51618) |
|---|---|
| GenesisR_Incl_TC_20200120.ttf | `GenesisR Incl TC` |
| GenesisB_Incl_TC_20200120.ttf | `GenesisB Incl TC` |
| Genesis_Interior-Light.ttf | `Genesis` (light) |
| Genesis_Interior-Regular.ttf | `GenesisB` (per binary order) |
| DFHEI5.ttf | `DFHeiW5-A` |
| Droid Naskh Shift Alt.ttf | `Droid Naskh Shift Alt` |
| New_HD2.ttf / New_HD2.0_Condensed.ttf | `New HD2` family |
| New_HD2.1_Regular.ttf | `New HD2.1` |
| New_HD2.1_Medium.ttf | `New HD2.1 Medium` |
| New_HD2.1_Number_S.ttf | `New HD2.1 Number_S` |
| New_HD2.1_Incl_TraditionalChinese-{Regular,Medium}_20190920.ttf | TC variants |
| Cube-Regular.ttf | `Cube Regular` |
| Cube-Bold.ttf | `Cube Bold` |
| Cube_CJKsc-Regular.ttf | `Cube CJKsc Regular` |
| Cube_CJKtc-Bold.ttf | `Cube CJKtc Bold` |
| Cube.ttf / CubeBa1.ttf | (Cube base / bold‑alt) |

The TTF `name` tables on disk are obfuscated (scrambled name records); the
family names above come from the registration strings in the binary, which the
font‑init code (ELF 0x7c0686+) passes to Qt — treat them as authoritative.

Family aliases used in QML: `HKMC_FONT_R` (425 uses), `HKMC_FONT_B`,
`HKMC_FONT_NMODE_NUMBER`, plugin aliases `FONT_R_NORMAL`/`FONT_B_NORMAL`
(plugins/*/qml/Defines.qml), plus direct families `CubeR`/`CubeB`
(ConstantModel.FONT_FAMILY_HDB=`CubeB`, FONT_FAMILY_HDR=`HDR`, AppOSDManager's
ConstantModel carries `CubeR`). `ConstantModel.FONT_DFHEI5 = DFHeiW5-A`.

### font.pixelSize conventions (frequency across stock QML)

| pixelSize | Uses | Role |
|---|---|---|
| 39 | 1223 (`ConstantModel.FONT_SIZE_39` 561 + `DefineModel.FONT_SIZE_39` 505 + literal 463) | **default list‑item / body text** |
| 42 | ~480 | emphasized list item / button label |
| 33 | ~500 | secondary body / settings text |
| 36 | ~265 | mid body |
| 30 | ~265 | caption / small body |
| 27 | ~200 | small caption |
| 24 | ~105 | tiny caption |
| 21 | ~76 | micro text |
| 48 | ~48 | popup title / large value |
| 18/19/23 | 43‑52 | fine print |
| 100‑129 | few | hero numbers (N‑mode gauges) |

Convention: **titles 42‑48 px, body/list 39 px, secondary 33‑36 px, captions 24‑30 px**,
always via `ConstantModel.FONT_SIZE_*` tokens (never magic numbers in new code).

---

## 5. Animation durations & opacities

`ConstantModel.animDuration = 300` (case 0x143) — the house default.

Duration histogram (all stock QML in /tmp/opencode/gen5w_ui):

| ms | uses | typical use |
|---|---|---|
| 100 | 69 | `Behavior on scale/width/font.pixelSize` micro‑interactions (QMenuItem*.qml) |
| 200 | 53 | list/page slide‑in, submenu transitions |
| **300** | **295** | **standard screen/page transition** (NumberAnimation on x/y/opacity) |
| 500 | 56 | popup show/hide, widget edit transitions |
| 1000 | 45 | media gate popup, page‑total loaders (`USM_KEYPAD_DURATION=1000`) |
| 1500 | 25 | splash/boot animations |
| 2000/2400 | 9 | long loading sequences |
| 700/650 | 4 | misc |

Easing conventions: `Easing.Linear` (81 uses, default), `Easing.InOutQuad` (31),
`Easing.InCubic` (29). Pattern: `Behavior on <prop> { NumberAnimation { duration: N; easing.type: Easing.Linear } }`.

Opacity tokens: `ConstantModel.GENERAL_OPACITY_PERCENT_70` (= 0.7, 157 uses),
`GENERAL_OPACITY_PERCENT_60` (0.6, 15), `GENERAL_OPACITY_PERCENT_50` (0.5, 3);
float constants 0.6/0.7 also appear in the metacall (cases 0x63/0x64:
0x3f19999a=0.6, 0x3f333333=0.7).

---

## 6. Day/night theming (dayLightSuffix)

* `AppHomeModel.dayLightSuffix` ∈ **{"#GUI_DAY", "#GUI_NIGHT"}** — set in the
  day/night handler (ELF 0x711806‑0x711826: `cmp $1,mode; cmove → "#GUI_DAY"`
  else `"#GUI_NIGHT"`; strings ELF 0xc46e0a/0xc46e15). mode==1 ⇒ day.
* Marker files **/tmp/GUI_THEME_DAY** and **/tmp/GUI_THEME_NIGHT** exist in the
  binary (strings 0xc522fa/0xc5230d) — cross‑process day/night signaling.
* `Q_ThemeImage.qml` (every app tree): `source: themeSource + AppHomeModel.dayLightSuffix`
  — the suffix rides **inside the image URL** and is interpreted by the C++
  image provider:
  * `@` placeholder in defineModel paths: `ev_etc_battery_y_@.png` +
    `#GUI_DAY/#GUI_NIGHT` → provider serves `..._d.png` / `..._n.png`
    (day/night asset pairs like `ev_ic_arrow_down_d.png`/`_n.png` exist in
    /app/share/images/genesis/ev; `home_soundmood_list_72_d/_n.png` in
    strings.tsv 919‑920). Confidence: confirmed for the suffix values and
    asset naming; the `@`→`d/n` substitution is high‑confidence inference
    (provider code itself was not decompiled).
  * new‑theme assets use explicit day/night **directories**:
    `/app/share/images/new_gui/{new2.0|eco3.0}/{hmc|kmc}/{day|night}/…`
    (loading‑gif table §2).
* QML day/night branching idiom (confirmed, 30+ files):
  `property bool isDay: AppHomeModel.dayLightSuffix == "#GUI_DAY"`.
* The same flag drives ConstantModel/defineModel day/night color variants
  (§1, §3) and ThemeHelper palettes (§2).

---

## 7. Methodology / reproducibility

* ELF: `/mnt/shares/Software/state_freeze/ioniq_rooting/analysis/ioniq5_260128/work/rootfs/app/bin/AppHomeScreen`
  (image base 0x100000; .rodata vaddr==file offset; RW segment file offset = vaddr−0x200000).
* QMLConstantModel: metaobject ELF 0x10e0880 → stringdata 0xcb6100 (328 strings),
  data 0xcb5160 (327 props ×3 uints), static_metacall switch ELF 0xb4b860,
  jump table 0xcb4c40 (327 int32 rel. offsets).
* QMLdefineModel: metaobject 0x10e0940 → stringdata 0xcfd2c0 (3905 strings),
  data 0xcede80 (3901 props), jump table 0xce9f60.
* ThemeHelper: metaobject 0x10e2b00 → stringdata 0xd80580, data 0xd7fc00
  (58 props), getters FUN_00be8d00+i*0x50, palette static init ELF 0x683000‑0x687000,
  record arrays from ELF 0x129c4e8 to 0x129e9a8 (stride 0x38).
* Ghidra decompile: `../../mirror/export/bin_3208391a859693e9/functions_0005.c`
  (metacalls), `functions_0003.c` (color helpers), `functions_0006.c` (ThemeHelper).
* Confidence legend: **confirmed** = read from binary/jump table and
  cross‑checked (anchors, QML, assets); **probable** = decoded but a single
  attribution step is inferred; **inferred** = mechanism conclusion from
  converging evidence.
