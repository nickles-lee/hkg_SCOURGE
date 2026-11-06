# Gen5W UI/UX Styleguide — building native-looking apps for the Ioniq 5 head unit

Firmware `NE_E.EUR.S5W_L.001.001.260128` (x86-64 Linux, Qt 5 / QML, Wayland IVI).
Theme variant for this car: **`qml_eco3.0`** (`-gui:3`, HMC eco3 family).
Read this *before* writing any QML for the head unit, then consult the companions:

| Doc | Contents |
|---|---|
| `deployment-modes.md` | Every surface a custom UI can live on (modes A–L): verdicts, recipes, risks |
| `ux-patterns.md` | Four behavioral rules-cards: driving lockout, edit mode, foreground loss, trio/daemon |
| `glossary.md` | Colloquial ("app", "icon", "card") → precise technical term |
| `components.md` | The stock component library and when to use which |
| `tokens.md` | Design tokens: `ConstantModel` values, colors, fonts, durations |
| `reports/*.md` | Raw research evidence (15 reports, every claim cited `file:line` or decompile address) |

Companion scope docs in this directory (authoritative for their scope):
`UI_CONTEXT.md` (geometry + foreground rules),
`APP_OVERLAY_CHECKLIST.md` (overlay pattern),
`TERMINOLOGY.md` (original term table; `glossary.md` here expands it).

---

## 1. System model — what you are actually building on

```
weston (ivi-shell, application layer 4000, 300 ms layer transitions)
 └─ AppHomeScreen (process, app ID 11) — owns the home surface
     QML: /app/share/AppHomeScreen/qml_eco3.0/
       main.qml
        ├─ loader_1/loader_2   double-buffered content screens
        ├─ loader_3            AssistantWindow (640×720 sibling layer; disabled here)
        ├─ loader_4            Statusbar (embedded libStatusbar.so, top 90 px, z 2)
        ├─ OSD loaders (z 4)   up to 4 C++-injected OSD QMLs
        └─ dimBG / dimStatusBar (z 3–4, black 70 %)
 └─ ApplicationManager — foreground/stack/key routing (session bus)
 └─ AppLauncher — spawns apps, verifies .sign RSA signature, sets env
 └─ NotificationManager — SystemPopup surface (above all apps)
 └─ AppOSDManager — OSD strips (90 px top / 75 px bottom, above all apps)
```

Facts that change how you build (all decompile-confirmed, see `reports/lifecycle.md`):

- **App IDs are hardcoded** in AppLauncher/ApplicationManager tables. The home
  grid is a C++ item model, not config. `/rw_data/applaunch-config.json` only
  holds *preload groups*.
- **Home ≠ close.** Going home is a surface transaction (GoHome + history
  push); the previous app process keeps running with all state. Real kill only
  via `terminate(appid)` / `appkill(appname)`.
- **AppLauncher verifies every spawned app's `.sign` ELF section** (SHA256 +
  RSA vs `/tmp/app_normal.key`). The private key is not in the firmware →
  **you cannot deploy a new official separate-process app without patching
  AppLauncher**. This is why the recommended deployment is inside
  AppHomeScreen (§9).
- **QML never sees hardware keys.** Steering-wheel Home/Star, faceplate keys
  travel CAN → micom D-Bus → ApplicationManager → per-app session-bus calls
  (`NotifyRequestHomeScreen`, `swrckey`, `custom_key_value`). No `Keys.on*`
  handlers exist in stock QML (15 legacy hits only).
- Every app binary registers its **own** `ConstantModel`/`defineModel`/
  theme-helper singletons with different values. Values in `tokens.md` are
  AppHomeScreen's; AppSetup's are in `reports/constant_model_table.txt`.

## 2. Screen geometry

| Item | Value |
|---|---|
| Panel | 1280×720 (narrow) or 1920×720 (wide) |
| Status bar | top **90 px** — reserved, app-embedded, z 2 |
| Usable full-screen content | **y = 90, height = 630** |
| Wide/narrow switch | `AppHomeModel.isWideScr` (root: `isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`) |
| Split screen (AW) | 640 px right panel — **disabled on this car** (`screenNotSupportSS = true`) |

**Rule 1 — never draw under the top 90 px.** Full-screen pages/overlays start at
`y: ConstantModel.MENU_BAR_Y` and fit within 630 px. Popups use the conditional
`y: (currentScreen === EVT_GOTO_SCREEN_WIDE_ADVANCED && currentHomeAdvancedPageIdx === 0) ? 0 : 90`
(full-bleed only on the widget page, stock `popup/QHMI_AppHomePopup_OK.qml:9-11`).

Reference layouts (see `components.md` for more):

- Home grid: cells **306×249**, 6 columns wide / 4 narrow, 12/8 items per page.
- Settings two-pane: left menu 375 px, right pane 905×540, both at **y=180**.
- Popup widths: MIN 678 / MID 972 / MAX 1260; 1-line height 297, **+45 px per
  text line**; buttons 78 px high, 12 px gutters.
- List rows: 108 px base, +39 per extra title line, +33 per description line.

## 3. Theming, assets, text

1. **Always `Q_ThemeImage` + `QText`** (each app ships its own copies in
   `component/common/` — copy them, don't import across apps). They append
   `AppHomeModel.dayLightSuffix` (`"#GUI_DAY"`/`"#GUI_NIGHT"`) to the image URL
   and apply the theme text color automatically.
2. **Images** come from `defineModel.<key>` (3 901 keys, AppHomeScreen) →
   `image://AppHomeScreen/<category>/<file>.png`; the provider resolves
   day/night and the `@` placeholder. State suffixes: `_n` normal, `_p` press,
   `_f` focus, `_s` selected, `_d` disabled; `_s` prefix-suffix also marks
   *narrow-screen* asset variants (`home_bg_icon_edit_s_p`). Stretchy chrome =
   3-slice images with `fillMode: Image.TileHorizontally` (no border-image).
3. **Colors: never hardcode.** Use `HomeThemeHelper.<role>Color`
   (58 runtime-switched roles: normal/press/selected/focus/disable ×
   text/content/EV/PHEV/climate/checkbox/radio families). eco3.0 anchors:
   normal text `#62797f` day / `#b6c1c8` night, press accent `#07a8df` /
   `#17beb0`. Full palette in `tokens.md` §2.
4. **Fonts** (see `tokens.md` §4): body/list **39 px**, emphasized 42, titles
   42–48, secondary 33–36, captions 24–30. Family `HKMC_FONT_R` / bold
   `HKMC_FONT_B` (registered Cube/New-HD families). Always
   `ConstantModel.FONT_SIZE_*`, never magic numbers. `lineHeightMode:
   Text.FixedHeight`, `lineHeight: 39`, `wrapMode: Text.WordWrap`,
   `elide: Text.ElideRight` by default; marquee only for dynamic long strings
   (1 s pause → scroll → 1.5 s pause, per `QHMI_ScrollText`/`QHMI_MarqueeText`).
5. **Localization**: `TranslationsModel.STR_*` or `getDefineLocalize(label)`,
   **always `+ AppHomeModel.textUpdate`** (forces re-eval on language change).
   26 languages; add the key to *all* ts files (no QML fallback for missing
   keys). Placeholders via `.replace("#"/"@"/"[##]"/"%s", …)`.

## 4. Interaction rules (what makes UI feel native)

1. **Beep before work, on release/long-press — never on press-down.**
   `QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)` in
   AppHomeScreen (794 stock uses); each app has its own event
   (`KEY_EVT_SETTUP_SOUND_BEEP_PLAY` in Setup, `KEY_EVT_REQUEST_BEEP` in
   StandbyClock…). `QButton` does it for you: `usingBeepSound: true` (opt-in,
   default false). No beep on: disabled/toned-down items, slide-off presses,
   free-area taps.
2. **Press = image swap, not opacity/scale.** 4-state icon arrays
   `[normal, press, focus, disable]` (`icons[_PRESSED]`); tiles load a separate
   full-cell press image. Opacity is only for *dim* (text day 0.2 / night 0.35)
   and drag ghosts (0.7). Scale 1.2 only for drag-lift in edit mode.
3. **Slide-out cancels the click.** Every MouseArea tracks `atPressState` and
   resets it in `onPositionChanged: if(!containsMouse) …` — mirror this.
4. **Timings**: long-press **650 ms**, drag-start 500 ms, page transition
   **150 ms** (x from width/2, OutQuad + opacity InCubic, input blocked),
   popup fade **100 ms** InOutQuad, option menu 10 ms, list snap 200–500 ms,
   indicator auto-hide 5 000 ms, spinner 100 ms/frame, touch re-enable 1 000 ms.
   House default `ConstantModel.animDuration = 300`.
5. **Magnify touch targets at edges**: `QEnlargedMouseArea` /
   `widthLeftMultiple`-style 1.25–1.5×.
6. **Gate input on system state**: page roots `isEnable: !AppHomeModel.sysPopupShowing`;
   hide/dim during `visibleOSDCall` / `isOnScrShowing`; vehicle state gates
   (`KEY_EVT_POPUP_CAR_DRIVING` when driving-restricted).
7. **Navigation is event-driven, never Loader-hacking**: launch apps with
   `qmlSendEvtKey(EVT_MAIN_LIST_ITEMCLICKED, itemID)`, change screens with
   `qmlChangeTopMsg(EVT_GOTO_*)`, close popups with
   `(EVT_REQUEST_HIDE_POPUP, _OSD_ID)`. `QML_Handler` (C++ `ScrController`) →
   `AppManager`/`ScreenQueue` → context property `screenNext` → root
   `screen_Transition()` swaps `loader_1/2`.

## 5. Foreground, input ownership, hard keys

**Rule 2 — foreground ownership.** Surface stacking is owned by the window
manager; QML cannot raise its window. `AppHomeModel` exposes the state:
`isAtOtherApp`, `currentShowingAppID`, `currentScreen`, `currentHomeAdvancedPageIdx`,
`requestHomeByPP`, `focusHKThisApp`.

For **overlays inside AppHomeScreen** (deployment model A):

1. `visible: requested && !AppHomeModel.isAtOtherApp` — never paint while
   another app's surface owns the foreground (click fall-through).
2. Close on: `onButtonBackClicked`, `isAtOtherApp` → true, `currentScreen`
   change, `currentHomeAdvancedPageIdx` change, `requestHomeByPP`.
3. Home/Star key: session-bus event, invisible to QML. A helper daemon tails
   `dbus-monitor --address $(cat /tmp/session_appmgr)` with match rules for
   `NotifyRequestHomeScreen` (`com.lge.hkmc.avn.appfunc.appInterface`) and
   `custom_key_value` (`com.lge.avn.appmanager.info`) and exposes a `home_seq`
   counter (e.g. `http://127.0.0.1:8088/status`); the overlay closes on change.
4. Full-screen overlays need the input shield (`APP_OVERLAY_CHECKLIST.md`):

```qml
MouseArea {
    anchors.fill: parent; z: 1
    preventStealing: true; acceptedButtons: Qt.AllButtons
    onPressed: mouse.accepted = true; onReleased: mouse.accepted = true
    onClicked: mouse.accepted = true; onWheel: wheel.accepted = true
}
Item { id: content; z: 2 }   // real controls above the shield
```

**Z-order map** (inside the AppHomeScreen window):

```
z −9999 coverScreen debug   →  0 loader_1/2 + AW bg  →  0/10 loader_3 AW
→ 2 loader_4 statusbar      →  3 bar guards + dimStatusBar
→ 4 OSD loaders + dimBG     →  1000 QRootScreen beep shield
→ 2000 full-screen overlays →  9999 NMode loading cover
```
SystemPopup and AppOSDManager are *separate Wayland surfaces* above the window;
the AW is a sibling layer — an overlay inside the Advanced page can never cover
it (declare at `main.qml` root if ever needed).

## 6. System surfaces (reserved — don't fight them)

| Surface | Owner | Where it draws | Your obligation |
|---|---|---|---|
| Status bar | embedded `libStatusbar.so` (`loader_4`, z 2) | top 90 px | keep content below y=90; buttons/clock/date/10 icon groups are C++-driven, not extendable |
| OSD strips | AppOSDManager process | 90 px top strip / 75 px bottom (volume) | auto-dismissing; dim via `visibleOSDCall`/`isOnScrShowing` |
| SystemPopup | NotificationManager process | centered card + 70 % dim | on `sysPopupShowing`: disable input, `returnToBounds()`, never fight for FG |
| App-local popups | AppHomeScreen C++ loads `popup/*.qml` by path | full-screen dim + centered card | request via `KEY_EVT_SHOW_POPUP_*` events, don't instantiate directly |
| AssistantWindow | `loader_3` sibling, 640×720 | right column | disabled here (`screenNotSupportSS = true`) |

## 7. Page skeleton (copy this shape)

```qml
// New full-screen page inside AppHomeScreen (eco3.0)
import QtQuick 2.2
import "../component"
import "../component/common"
import HMIEVENTS 1.0

QRootScreen {                                   // page base: beep shield z 1000
    id: root
    width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT
    isEnable: !AppHomeModel.sysPopupShowing     // gate input on popups

    // content band: y 90 .. 720
    Item {
        y: ConstantModel.MENU_BAR_Y             // 90
        width: parent.width
        height: ConstantModel.CONTENT_WITH_MENUBAR_AREA_HEIGHT  // 630
    }
    // screen transition (stock idiom)
    QMainScreenAnimation { positionTarget: contentArea; opacityTarget: contentArea }
    // Component.onCompleted: right_left_animation.startAnimation()
}
```

## 8. Localization & loading

- Strings: see §3.5. Every key in all 26 ts files; compile with `lrelease`.
- Loading: shared 117×117 frame spinner (`defineModel.img_loading[]`,
  100 ms/frame) + `STR_HOME_WIDGET_PREPARING` 42 px. Boot/loading screen has a
  10 s watchdog. Busy = dim 0.7 + input shield.
- Date/time: per-region formatters (EU 24 h `d. m. yyyy` / 12 h `MON d, yyyy`);
  Finnish time separator is `.`.

## 9. Deployment models

You have two working deployment shapes, plus the official one that is blocked.
**Model A (app)** and **Model B (card)** are the two you asked for; both are
QML patches inside AppHomeScreen (no signing, no C++ changes).

**The full catalog** (12 modes: widget plugin, system modal popup, Assistant
Window re-enable, own Wayland process, basic-home tile, option-menu entry,
OSD strip, standby-clock and chill-screen customization, …) with recipes,
risks and live-car test checklists is in **`deployment-modes.md`**. The
behavioral patterns (driving lockout, edit mode, foreground loss, daemon/card
trio) are in **`ux-patterns.md`**.

### Model A — "App": tile + full-screen overlay (recommended)

An app has an icon, opens, and gets closed. Proven by Hello/WiFi/Tweaks/Battery.

1. **Tile** — `component/<App>Tile.qml` placed in the first *genuinely* empty
   cell of the target `QMenuPageNoDrag` grid via the shared
   `firstEmptySlot(customIndex)` allocator (page models are padded —
   `getCount()` lies; walk `emptySlots(getPageDisplayListModel(p))`).
   Coordinates: `x = menuPage.x + (slot % k_nbrOfColumns) * k_itemWidth`,
   `y = menuPage.y + floor(slot / k_nbrOfColumns) * k_itemHeight`.
   Icon = plain PNG via `file://` (avoids ASTC re-encode; small `Q_ThemeImage`
   patch skips the day/night suffix for `file://` URLs).
2. **Page** — full-screen overlay at `y: MENU_BAR_Y`, height 630, `z: 2000`,
   `open()`/`close()` owned by the overlay, `visible: requested && !isAtOtherApp`,
   all close hooks + input shield + `home_seq` daemon (§5, and
   `APP_OVERLAY_CHECKLIST.md` verbatim).
3. **Daemon** (optional, for CAN data / Home-Star): systemd unit, HTTP status.
4. **Install**: USB payload → `run_stuff.sh` pattern: manifest + original-hash
   guard, backup, remount rw, copy preserving mode/owner/SMACK labels, clear
   QML caches (`/root/.cache/AppHomeScreen`, `/root/.cache/qtshadercache`,
   `/tmp/qmlcache`, `/tmp/qt-qml-cache`), remount ro, restart UI via
   `applauncher terminate 11` + `launch 11 "AppHomeScreen" "/app/bin/AppHomeScreen"`.
5. **Limitations**: no foreground ownership (must self-close), dies with
   AppHomeScreen restarts, no QML D-Bus (daemon does that), firmware updates
   revert it.

### Model B — "Card": persistent swipe page in the home carousel

A card is reached by swiping from the home screen, is always alive, and is
never "closed". Proven by the swipe-era Battery build (5 pages ran on-car).
Full mechanics + risk list: `reports/card-page.md`.

```
NEW   HomeScreen/MyCardPage.qml   Item root, full-page size, signal goHome(),
                                  NO overlay gating (never self-hide on isAtOtherApp)
EDIT  HomeScreen/QHMI_HomeScreenMain_Advanced.qml  (6 hunks):
      (1) property int totalPageCount: totalPagesNormalMode + 2
      (2) getModelList(): append myCardPage in the 3/2/1-page branches
      (3) Component { id: myCardPage; Item { screen-size;
            MyCardPage { anchors.fill: parent; onGoHome: homepage.currentIndex = 0 } } }
      (4) homepage.model: totalPagesNormalMode == 0 ? 0 : totalPageCount
      (5) indicator width (totalPageCount*39)+((totalPageCount-1)*9); Repeater model: totalPageCount
      (6) goHistorypage(): clamp totalPagesNormalMode → totalPagesNormalMode + 1 (both places)
      (7) [rec] displayMarginBeginning/End: homepage.width * totalPageCount  (always-alive)
```

Card rules:
- **Append at the end only.** Index 0 (widget page) is special-cased in 10+
  places; grid-page indices must stay stable.
- **Never touch `totalPagesNormalMode` / `numberPage`** — C++-owned grid
  pagination (`AllMenuPage1..3Display`). Use the separate `totalPageCount`.
- Never name anything `objectName: "itemWidgetPlaceHolder"` (C++ widget
  injection searches for it).
- The card is a *page*, not an overlay: no `isAtOtherApp` self-hiding, no
  `open()/close()`. It survives app launches; the Home key returns to the
  persisted page index (`currentHomeAdvancedPageIdx`).
- Content starts at y≥90 (the page is full-bleed); the indicator dots overlap
  the bottom ~26 px. Horizontal drags flick the carousel — use `preventStealing`
  on card-internal flickables/sliders.
- Gate expensive work on `visible` or
  `AppHomeModel.currentHomeAdvancedPageIdx === myPageIndex` (timers keep
  running off-screen).
- Exists only in the Advanced home type (Basic home has no carousel — stock
  behavior). Wide 1920 needs no separate file (`isWideScr` ternaries adapt).

### Model C — official separate-process app (advanced; currently blocked)

What the firmware requires: an app ID in two hardcoded tables, a `.sign`-signed
binary (**private key not in the extract — hard blocker**), a
`/app/share/<App>/qml_<variant>/` tree, session-bus registration
(`com.lge.avn.appmanager.registration`, `com.lge.hkmc.avn.appfunc.<Name>`
service accepting `NotifyRequestHomeScreen`), a Wayland surface on ivi layer
4000, and a C++ grid entry in AppHomeScreen. Without the signing key, the only
variants are patching AppLauncher or spawning outside it (no sign check applies
to processes AppLauncher doesn't spawn). See `reports/lifecycle.md` §7.A.

## 10. Agent checklist (copy into every UI task)

Layout
- [ ] Nothing above y=90; content band y 90 / h 630 (`MENU_BAR_Y` / `CONTENT_WITH_MENUBAR_AREA_HEIGHT`)
- [ ] Wide/narrow handled: `isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH`, column counts and `_s` assets per variant
- [ ] Stock spacing rhythm: 12 px gutters, 33 px list text margin, 108 px rows (+39/+33 lines), 306×249 grid cells

Look & feel
- [ ] `Q_ThemeImage`/`QText` only; images from `defineModel.*`, colors from `HomeThemeHelper.*`, fonts `ConstantModel.FONT_SIZE_*`
- [ ] Press = `_p` image swap; 4-state arrays; no scale/opacity press effects
- [ ] Beep on release via the app's event (`EVT_HOME_SOUND_BEEP_PLAY`), `usingBeepSound: true` on buttons
- [ ] 150 ms page transition (OutQuad slide + InCubic fade, input blocked); 100 ms popup fade; 650 ms long-press
- [ ] Strings: `TranslationsModel.STR_* + AppHomeModel.textUpdate`, keys in all 26 ts files

Behavior
- [ ] Navigation via `qmlSendEvtKey`/`qmlChangeTopMsg` only; back = `EVT_BACK_TO_PREV_SCREEN`
- [ ] `isEnable: !AppHomeModel.sysPopupShowing`; dim/hide on `visibleOSDCall`
- [ ] Overlay (model A): `visible: requested && !isAtOtherApp`, all 5 close hooks, `preventStealing` all-buttons shield, `home_seq` daemon for Home/Star
- [ ] Card (model B): no overlay gating, appended last, `totalPageCount` not `totalPagesNormalMode`, widened `goHistorypage()` clamp, display margins raised
- [ ] Touch targets enlarged 1.25–1.5× at edges; slide-off cancels clicks

Deploy
- [ ] Patch only `qml_eco3.0` (active variant); keep stock backup; guard on 260128 hashes
- [ ] Preserve SMACK labels on copy; clear QML caches; restart via `applauncher terminate/launch 11`
- [ ] `qmllint` in the VM lab before flashing the USB payload

## 11. Known unknowns (need live car)

- Live `/rw_data/applaunch-config.json` and `/app/bin/CarOption.csv` contents
  (the extract lacks both; the CarOption fallback table has no NE row, so the
  live file must carry the NE `-gui:3` row).
- Whether ApplicationManager accepts new third-party app IDs at runtime
  (registration permission model / SMACK).
- Signature-enforcement bypass or test modes (`/app/bin/enable_gui`,
  autotest.conf); where `.sign` private keys live (not in extract).
- Weston-side FG-grant internals (`ivi-window-manager.so` not in export set).
