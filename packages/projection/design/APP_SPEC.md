# Projection — app design specification

Paper design only (no QML, no code changes). Working name: **Projection**.
Scope: a home-screen tile plus one full-page overlay inside stock `AppHomeScreen`, theme `qml_eco3.0`.
All copy below is final proposed wording, not placeholders.

Sources: `packages/projection/PRODUCT.md`; `docs/ui/styleguide.md` (SG), `docs/ui/ux-patterns.md` (UX),
`docs/ui/components.md` (CMP), `docs/ui/tokens.md` (TOK), `docs/ui/UI_CONTEXT.md` (CTX),
`docs/ui/glossary.md` (GLO); reference pages `packages/tweaks/.../TweaksPage.qml` (TW) and
`packages/wifi/.../WifiPage.qml` (WF) and their `component/` folders.

---

## A. Information architecture

### A1. Surface and deployment shape

`ProjectionTile.qml` (306×249 grid cell) → tap → `ProjectionPage.qml` (full-screen overlay
starting at the menu-bar y of 90, 630 px high, owning open/close).
This is deployment **Model A** (SG §9.A, GLO "App (ours, model A)") — the same mechanism as Tweaks and
WiFi, so no AppLauncher app ID, no `.sign`, no new process. `PRODUCT.md` §Stack mandates this.

- Tile is placed in the first genuinely empty grid slot via `firstEmptySlot(customIndex)`, with a
  re-evaluating visibility guard so a rebuilt grid hides it rather than overlapping (SG §9.A step 1;
  UX §2 rule card 3).
- Overlay is a page/overlay, not a carousel card: it self-closes on foreground loss and never joins
  any C++ grid model (SG §9.A step 2; UX §2, §3; CTX Rule 2).
- Page root is a `QRootScreen` that disables input while a system popup is showing (SG §7, CMP §1),
  plus an all-buttons input shield that steals presses, releases, drags and wheel events (CTX Rule 3).
  TW/WF use a plain `Item` + shield; keep the shield either way. Z-order follows SG §5 (overlay at
  z 2000); TW happens to use z 100 — see open question Q9.

### A2. Page structure — groups as left-nav cards, one right pane each

The page is a two-pane settings shape: **left group list** + **right detail pane**, under a 90 px
title bar. SG §2 lists "Settings two-pane: left menu 375 px, right pane 905×540" as the stock
settings shape; TW and WF adapt it to an overlay at `y: 90` with a `Column` of `plate` rows on the
left and one `Item` per group on the right, switched by an int (`group` in TW:227, `currentTab` in
WF:177). We copy that exact mechanism.

```
Title bar (90 px)      "Projection"                          [More] [Close]
Left group list        HUD navigation                    ─┐
                       (reserved space for future groups)   ├─ right pane shows the selected group
                       (Diagnostics is NOT here)          ─┘
```

- **Feature group = one card in the left list + one right-pane panel + one config namespace.**
  Today: one group, `HUD navigation`.
- **Diagnostics is one level deeper**, not a peer card: title bar `More` → `QHMI_OptionMenu`
  (CMP §3) with one entry, `Diagnostics` → right pane shows the Diagnostics panel. The main page
  therefore carries zero technical words.
- Driver lockout replaces the whole content band with one description pane (§F), matching UX §1
  "Whole screen restricted → swap content for a lockout/description pane — no popup".

### A3. Adding a future feature group (no redesign)

`EV routing` (or any later item) is added by exactly three edits, with no geometry/layout rewrite:

1. Append one group descriptor to the left-list model (label + subtitle + panel id). The list is a
   `Column`/`QListView` of plates, so the card simply appears in the reserved space (y 274–708).
2. Add one right-pane `Item` gated by the group id (same idiom as TW:424/525/627).
3. Add a new config prefix on the daemon, e.g. `evrouting_enabled` (§C0). No schema change.

Why this works: the left list is a model of groups, not a hard-coded pair of buttons; the right pane
is a loader-like switch; JSON uses flat, prefixed keys. `PRODUCT.md` §Product Principles 4 and
§Capabilities ("Future (undecided, needs research): EV reporting …") require this. We do **not** show
a dimmed "EV routing — not available yet" card in v1 (see open question Q4): an owner could read that
as a broken install. The empty reserved space is the visible room to grow.

### A4. Shared behaviour of every screen (all wireframes)

- Overlay visibility `requested && !AppHomeModel.isAtOtherApp`; close on back button,
  `isAtOtherApp`, `currentScreen`, `currentHomeAdvancedPageIdx`, `requestHomeByPP`, and daemon
  `home_seq` bump (UX §3, CTX Rule 2; WF:104-113, WF:319-343).
- Popups are app-local `QPopupAnimationItem` cards (CMP §4), hidden while `sysPopupShowing`.
- 150 ms page transition, 100 ms popup fade, 650 ms long-press only if later needed (TOK §7, SG §4).
- Status poll 1 s while visible; `file:///rw_data/hudnav/status.json` mirror as fallback, with a
  fresh `updated_ms` so a crashed daemon cannot leave stale "on" controls (UX §4; WF:187-293).
- No string literals in shipped QML: `TranslationsModel.STR_* + AppHomeModel.textUpdate`, keys in
  all 26 ts files (SG §3.5). The copy in this document is what those keys must say.

---

## B. Screens (wireframes at 1280×720; content y=90…720)

Legend: `[ ]` button/toggle, `<...>` selected segment, `·` passive text, `#` dimmed/unavailable.

### B0. Home-screen tile (grid cell 306×249, not a page)

```
  [icon 99×99]
  Projection          306×249; press = full-cell _p image swap
```
`ProjectionTile.qml` mirrors `TweaksTile.qml`/`WifiTile.qml`: `Q_ThemeImage` bg + plain PNG icon +
`QText` label + whole-cell `MouseArea`. Component type per CMP §2: `QMenuItemNoDrag`.

### B1. Main page — "HUD navigation" group (default view)

```
y=90  ┌────────────────────────────────────────────────────────────────────────────┐
      │ [icon] Projection                                      [More] [Close]      │  title bar
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │ ┌────────────────────┐   ┌────────────────────────────────────────────┐   │
y=178 │ │ > HUD navigation   │   │ Right now                                │   │  128
      │ │   Showing turns    │   │ Showing turns from Android Auto.         │   │
      │ │   from Android Auto│   │ Connected by Android Auto.               │   │
y=306 │ └────────────────────┘   ├────────────────────────────────────────────┤   │
      │  (reserved for future   │ HUD navigation                       [on ] │   │   92
      │   feature groups)       │ Off · the car behaves as it did before.    │   │
      │                         ├────────────────────────────────────────────┤   │
      │                         │ Android Auto                         [on ] │   │   92
      │                         │ Takes turns from Android Auto.            │   │
      │                         ├────────────────────────────────────────────┤   │
      │                         │ CarPlay                              [on ] │   │   92
      │                         │ Takes turns from CarPlay.                 │   │
      │                         ├────────────────────────────────────────────┤   │
      │                         │ Distance steps  [Precise][<Balanced>][Calm]│   │   92
      │                         │ How much the distance changes near a turn. │   │
      │                         ├────────────────────────────────────────────┤   │
y=616 │                         │ Units [<Follow car>][Metric][Imperial]    │   │   92
      │                         │ Uses the unit you picked in Setup.         │   │
y=708 └─────────────────────────┴────────────────────────────────────────────┴───┘
```
Coordinates: content band x=0…1280, y=90…720. Title bar y=90…162. Left list x=48,w=340
(card y=178,h=88). Right pane x=416,w=816,y=170,h=538; rows x=432,w=784.
Status is folded into the master row's two secondary lines so no extra scrolling is needed.

### B2. More menu open (overlay over B1)

```
y=90   ┌ dim 0.7 over the whole content band ──────────────────────────────────────┐
y=210  │            ┌──────────────────────────────────────────┐                  │
       │            │ Diagnostics                            › │  104 px row      │
y=314  │            └──────────────────────────────────────────┘                  │
y=720  └────────────────────────────────────────────────────────────────────────────┘
```
`QHMI_OptionMenu` (CMP §3); outside press closes. One entry only in v1.

### B3. Diagnostics panel (reached via More → Diagnostics)

```
y=90  ┌────────────────────────────────────────────────────────────────────────────┐
      │ [icon] Projection                                      [More] [Close]      │
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │                         Diagnostics                                      │  42
y=224 │                         ┌────────────────────────────────────────────┐    │
      │                         │ Versions                                  │    │  128
      │                         │ App 1.0 · service 1.0 · firmware 260128   │    │
      │                         ├────────────────────────────────────────────┤    │
      │                         │ Boot guard                                │    │   92
      │                         │ Armed · iAP2 hook is active               │    │
      │                         ├────────────────────────────────────────────┤    │
      │                         │ Last problem                              │    │   92
      │                         │ No problem recorded                       │    │
      │                         ├────────────────────────────────────────────┤    │
      │                         │ [Restore stock behaviour]  [Back]          │    │   78
y=650 │                         │ Turn-by-turn is read through a CAN shim;   │    │
      │                         │ the update uses a bind mount.              │    │
y=708 └────────────────────────────────────────────────────────────────────────────┘
```
Left group list is hidden here (no group selected). `iAP2`, `CAN`, `bind mount` appear only on this
screen (and the confirm popup's technical footnote, if any).

### B4. Restore confirmation popup (over B3)

```
      ┌ dim 0.7 from y=90 ─────────────────────────────────────────────────────────┐
y=189 │      ┌──────────────────────────────────────────────────────┐             │
      │      │ Restore stock behaviour?                             │             │  678×342
      │      │ This turns off HUD navigation and puts the car back   │             │
      │      │ the way it was. You can turn it on again here.        │             │
      │      │ [ Restore ]              [ Cancel ]                  │             │
y=531 │      └──────────────────────────────────────────────────────┘             │
```
Width `POPUP_WIDTH_MIN` 678, height 297 + 45 = 342, primary left / Cancel right (CMP §4, TOK §2).
Outside press = Cancel. Beep on release only (SG §4.1).

### B5. Driving lockout (whole content band swapped)

```
y=90  ┌────────────────────────────────────────────────────────────────────────────┐
      │ [icon] Projection                                             [Close]      │
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │                  ┌──────────────────────────────────────────┐             │
      │                  │ Please park the car to change these      │             │
      │                  │ settings.                                │             │
      │                  │ You can check them when the car is parked.│            │
      │                  └──────────────────────────────────────────┘             │
y=708 └────────────────────────────────────────────────────────────────────────────┘
```
`Q_RightMenuDescription` (CMP §3) using `STR_SETUP_POP_TRY_AGAIN_PARKING_VEHICLE` where that key
exists; no popup, no dimmed-but-clickable rows. `More` button is absent.

### B6. Loading / daemon unreachable / unsupported (three states of the same pane)

```
-- Loading (first 1-2 s or stale status) -----------------------------------------
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │  [spinner 117×117]      Getting the latest status…                        │
      │  (nav + rows dim 0.7, input shield on)                                    │

-- Daemon not running (after ~5 s) ------------------------------------------------
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │  Can't reach the projection service.                                      │
      │  Check that the update is installed, then try again.                      │
      │                                            [ Try again ]                  │
      │  # HUD navigation  # Android Auto  # CarPlay  # Distance  # Units         │

-- Firmware unsupported (supported == false) --------------------------------------
y=162 ├────────────────────────────────────────────────────────────────────────────┤
      │  Not available on this car.                                               │
      │  This feature needs a newer firmware. Nothing has changed on your car.    │
```

### B7. Phone not connected (normal state inside B1, not an error screen)

```
      │ HUD navigation                       [on ] │
      │ On · waiting for a phone.                   │
      │ No phone connected.                         │
```
No warning colour, no popup, no beep. Same pane, master stays on.

### B8. Notes for 1920×720

- Content band stays at y 90 with height 630 (SG §2, TOK §2); title bar, row heights and all y
  coordinates are unchanged. Width follows the wide-screen flag: DEVICE_WIDTH on wide, otherwise
  SCREEN_WIDTH = 1280.
- Two panes only: nav 400 px instead of 340, right pane capped at 1040 px, whole block centred —
  exactly the TW:227-238 approach. Rows keep their 1280 px widths; extra space becomes margins, so
  touch targets and reading distance stay constant.
- Wide background `defineModel.home_main_bg_01` (TW:253, WF:482); narrow `_02`. Use narrow-screen
  `_s` asset variants where a stock component exposes them (SG §10).
- No third "live log" column on wide (WF has one) — it would expose technical text on the main page.
  Diagnostics stays one level deeper on both panels.

---

## C. Controls, copy and data

### C0. Daemon contract (assumed; `hudnavd` at 127.0.0.1)

`GET /status` returns a flat JSON superset with freshness flags (UX §4). QML treats
`updated_ms` older than 8 s, or a failed parse/file read, as **unavailable**.

```
{
  "updated_ms": 1764240000000,
  "supported": true,
  "feature_known": true,
  "hudnav_enabled": false,
  "hudnav_aa_enabled": true,
  "hudnav_cp_enabled": true,
  "hudnav_rounding": "balanced",
  "hudnav_units": "car",
  "guidance_active": false,
  "guidance_known": true,
  "source": "none",
  "source_known": true,
  "health": "disabled",
  "last_error": "",
  "boot_guard": "armed",
  "boot_failures": 0,
  "version_app": "1.0",
  "version_daemon": "1.0",
  "version_firmware": "260128",
  "home_seq": 0,
  "home_epoch": 0,
  "home_source": "none"
}
```

`POST /config` takes a JSON object and merges only the keys present:
```
{"hudnav_enabled": true, "hudnav_aa_enabled": true, "hudnav_cp_enabled": true,
 "hudnav_rounding": "balanced", "hudnav_units": "car"}
```
`{"restore_stock": true}` performs the Restore action. The daemon maps these keys onto the
switch files named in `PRODUCT.md` (`/rw_data/hudnav/enabled`, `aa-nav-enable`, `cp-enabled`, …).
A future group adds its own prefix (`evrouting_*`) — no status/config schema change.

| Key | Type | Meaning / values |
|---|---|---|
| `supported`, `feature_known` | bool | Firmware support probe done / result. |
| `hudnav_enabled` | bool | Master switch. `false` = stock behaviour. |
| `hudnav_aa_enabled`, `hudnav_cp_enabled` | bool | Independent source switches. |
| `hudnav_rounding` | string | `precise` \| `balanced` \| `calm`. |
| `hudnav_units` | string | `car` \| `metric` \| `imperial`. |
| `guidance_active`, `guidance_known` | bool | Turn data currently reaching display / freshness. |
| `source`, `source_known` | string/bool | `none` \| `android_auto` \| `carplay`. |
| `health` | string | `disabled` \| `waiting_phone` \| `not_guiding` \| `guiding` \| `error` \| `unsupported`. Daemon sends a code; the app owns all user-facing text. |
| `last_error`, `boot_guard`, `boot_failures` | string/string/int | Diagnostics only: code/message, `armed` \| `rolled_back` \| `clear`, failed boots. |
| `version_app`, `version_daemon`, `version_firmware` | string | Diagnostics only. |
| `home_seq`, `home_epoch`, `home_source` | int/int/string | Home/Star close plumbing (UX §4); never displayed. |
| `restore_stock` | bool | POST-only action. |

### C1. Distance presets — exact values

Rounding is applied to the distance shown on the head-up display and cluster.

| Preset | Under 100 m | 100–999 m | 1 km and above |
|---|---|---|---|
| Precise | 1 m steps | 1 m steps | 10 m steps (0.01 km) |
| **Balanced** (default) | 1 m steps | 10 m steps | 100 m steps (0.1 km) |
| Calm | 10 m steps | 10 m steps | 100 m steps (0.1 km) |

Example: at 347 m, Precise shows 347 m, Balanced and Calm show 350 m. At 12.36 km, Precise shows
12.36 km, Balanced and Calm show 12.4 km. Boundaries round to nearest step; never below zero.

### C2. Control table

State legend: **ON** switch on / segment selected; **OFF** switch off; **DIS** dimmed, no beep, no
click (unsupported, driving, or no service); **UNAV** daemon unreachable banner + all controls dim;
**ERR** `last_error` set (health line + Diagnostics detail). "No write" = local UI only.
Stock component names are canonical; we ship local copies per CMP §0 (as TW/WF do).

| # | Screen | Control — type | Label / helper text | Default | States | Writes |
|---|---|---|---|---|---|---|
| 1 | Title bar | More — `QButton` / `QTitleButton` (`ProjectionButton`) | "More" / — | — | ON, DIS (driving, unsupported) | No write; opens option menu |
| 2 | Title bar | Close — `QButton` (`ProjectionButton`) | "Close" / — | — | ON | No write; `close()` |
| 3 | Left list | Group card — `QItemMenu` / `Q_HorizontalListButton` (`ProjectionRow`, `plate: true`) | "HUD navigation" / live health line | Selected | ON, DIS | No write; selects pane |
| 4 | B1 row 1 | Master toggle — `QButtonActive` (`ProjectionSwitch`, `interactive: false`) | "HUD navigation" / "Off · the car behaves as it did before." → "On · waiting for a phone." → "Showing turns from Android Auto." / "Connected by Android Auto." | **Off** | ON, OFF, UNAV, DIS | `hudnav_enabled` |
| 5 | B1 row 2 | Source toggle — `QButtonActive` (`ProjectionSwitch`) | "Android Auto" / "Takes turns from Android Auto. Applies when HUD navigation is on." | **On** | ON, OFF, UNAV, DIS | `hudnav_aa_enabled` |
| 6 | B1 row 3 | Source toggle — `QButtonActive` (`ProjectionSwitch`) | "CarPlay" / "Takes turns from CarPlay. Applies when HUD navigation is on." | **On** | ON, OFF, UNAV, DIS | `hudnav_cp_enabled` |
| 7 | B1 row 4 | Segmented buttons ×3 — `QHorizontalButton` (`ProjectionButton`, `selected`) | "Distance steps" / "How much the distance changes near a turn." · buttons "Precise", "Balanced", "Calm" | **Balanced** | SEL ×1, UNAV, DIS | `hudnav_rounding` |
| 8 | B1 row 5 | Segmented buttons ×3 — `QHorizontalButton` (`ProjectionButton`, `selected`) | "Units" / "Uses the unit you picked in Setup." · buttons "Follow car", "Metric", "Imperial" | **Follow car** | SEL ×1, UNAV, DIS | `hudnav_units` |
| 9 | B2 | Option entry — `QHMI_OptionMenu` row | "Diagnostics" / — | — | ON, DIS | No write; shows B3 |
| 10 | B3 | Passive rows — `Q_HorizontalListButton` (`ProjectionRow`, `pressable: false`) | "Versions", "Boot guard", "Last problem" / values from `/status` | — | ON, UNAV | No write |
| 11 | B3 | Push button — `QHorizontalButton` (`ProjectionButton`) | "Restore stock behaviour" / "Turns everything off. Nothing is deleted." | — | ON, UNAV, DIS | `{"restore_stock": true}` (after B4 confirm) |
| 12 | B3 | Push button — `QHorizontalButton` (`ProjectionButton`) | "Back" / — | — | ON | No write; returns to B1 |
| 13 | B4 | Popup buttons — `QHorizontalButton` (CMP §4) | "Restore" (left, primary) / "Cancel" (right) | — | ON | See #11 |
| 14 | B6 | Retry button — `QHorizontalButton` (`ProjectionButton`) | "Try again" / — | — | ON | No write; re-GET `/status` |

Notes on states:
- When the master is **off**, rows 5–8 stay fully clickable (they persist for the next time).
  Their helper says "Applies when HUD navigation is on." This avoids the banned "looks enabled but
  silently ignores a tap" pattern (UX §1 "Never"). Only no-service, unsupported and driving dim them.
- `health` → plain-language line: `disabled` → "Off · the car behaves as it did before."
  `waiting_phone` → "On · waiting for a phone." `guiding` → "Showing turns from Android Auto."
  (or "from CarPlay."). `not_guiding` → "Phone connected, but no turns right now."
  `error` → "Turns are not reaching the display." `unsupported` → "Not available on this car."
- Source line: `android_auto` → "Connected by Android Auto."; `carplay` → "Connected by CarPlay.";
  `none` → "No phone connected." Use `source_known === false` → "Checking the phone connection…".
- Diagnostics values: `boot_guard` `armed` → "Armed · iAP2 hook is active", `rolled_back` →
  "Rolled back after 2 failed starts · stock behaviour is back", `clear` → "Clear";
  `last_error` empty → "No problem recorded", otherwise show the daemon code verbatim.
- Beep on release only, via `EVT_HOME_SOUND_BEEP_PLAY` / `usingBeepSound: true` (SG §4.1, CMP §1);
  no beep on dimmed/disabled rows, slide-off or free-area taps.

---

## D. Copy rules

1. Sentence case, short, no punctuation at the end of a single short line. No abbreviations except
   "CarPlay", "Android Auto", "Setup" and the group name "HUD navigation".
2. Main page (B0–B2, B5–B7) uses "head-up display" in explanations, never `iAP2`, `CAN`,
   "bind mount", "hook", "socket", "daemon", "firmware version", "shim", "ELF" or "cluster CAN".
3. Those technical words are allowed only in Diagnostics (B3) and its confirmation popup.
4. Every switch line says what changes, not how: "Takes turns from Android Auto.", not
   "Intercepts NextTurnEnum from AppProjection".
5. No blame and no dead ends: an error line always pairs with a next step (Try again / turn it off
   and on / check the update).
6. All strings are `TranslationsModel` keys with `+ AppHomeModel.textUpdate` (SG §3.5); 26 ts files.

---

## E. Empty / error / loading states

| Condition | Detection | UI | Controls |
|---|---|---|---|
| First open, status stale | `!serviceOk` or `updated_ms` > 8 s old | B6 loading: `QLoadingComponent` spinner + "Getting the latest status…"; nav/rows dim 0.7 | Shielded, no beep; 5 s watchdog → daemon row |
| Daemon not running | GET fails and `file:///rw_data/hudnav/status.json` unreadable/stale | B6: "Can't reach the projection service." + "Check that the update is installed, then try again." + "Try again" | All dim; never show remembered "on" state as live (UX §4) |
| Firmware unsupported | `supported === false && feature_known` | B6: "Not available on this car." + "This feature needs a newer firmware. Nothing has changed on your car." | Master hidden; Close only. Compare TW `otaText()` "Not available on this vehicle" (TW:205) |
| Support probe pending | `!feature_known` | Loading state, not a false "unsupported" | Dim |
| Phone not connected | `source === "none"`, master on | B7 inside B1, neutral ink, no warning colour | All usable |
| Guidance stalled | `health === "not_guiding"` | "Phone connected, but no turns right now." | All usable; no popup |
| Last error set | `last_error !== ""` | Health line "Turns are not reaching the display." + detail in Diagnostics "Last problem" | All usable; no fake success dot |
| Boot guard rolled back | `boot_guard === "rolled_back"` | Diagnostics "Rolled back after 2 failed starts · stock behaviour is back" | Restore still available; no main-page alarm |
| No future group yet | empty group model | Reserved space in left list (no placeholder card) | — |

Never invent freshness: if the daemon is down, show unavailable, not the last known state (UX §4,
WF:188-191). Poll 1 s while visible; pause expensive work when obscured (UX §3).

---

## F. Driving behaviour

> **SUPERSEDED (owner decision 2026-11-27):** no driving lockout. Passengers may change settings while the car moves; screen B5 and the lockout below are dropped. Keep controls large and simple.

**Choice: whole-page lockout pane (B5), with an in-flow abort.** The tile still opens the page;
the content band is swapped for `Q_RightMenuDescription`; `More` and all group cards disappear;
only `Close` remains. When `AppHomeModel.drivingRestriction` flips true while any pane or popup is
open, close the popup and swap to B5 immediately; when it flips false, restore the previous group.

**Why:** `PRODUCT.md` §Operating Context confirms settings are used parked and there is no requirement
while driving. UX §1's severity table says a whole restricted screen uses a description pane with no
popup, and its rules card says to watch `onDrivingRestrictionChanged` and abort in-progress flows.
This is a settings surface where every control is a configuration change — there is no single action
to gate with an on-attempt popup (the row under "Single action restricted"), and no passive content
worth degrading. We do **not** silently `enabled: drivingRestriction` each row: UX §1 explicitly
forbids "silently disable a control that looks enabled" and stock has zero such row-disables.

We do **not** copy the Tweaks Reboot exception (UX §1, documented exception): that exists only so an
owner can recover a hung head unit while moving. HUD navigation has no recovery role, and changing
rounding or source switches while moving is pure distraction.

The gate reads `AppHomeModel.drivingRestriction` with a type guard so the page still loads if that
model is absent (UX §1 rules card). No daemon is needed for this gate. Foreground loss uses the
separate close rules in §A4 and UX §3.

---

## G. Open questions

1. **Public name.** "Projection" is a working name (`PRODUCT.md` §Open). Does the tile read
   "Projection", "Phone display" or "Phone features"?
2. **Port.** Which TCP port for the local HTTP API? Tweaks 8091, WiFi 8090, battery 8088; propose the
   next free port (8092) and confirm no conflict.
3. **Source defaults.** Should CarPlay default **on** as well as Android Auto, given CarPlay is
   car-proven only as of 2026-11-27 and may need a later firmware? Alternative: AA on / CP off.
4. **Future-group visibility.** Show a dimmed "EV routing — not available yet" card in v1 to make
   expansion obvious to repo users, or keep the empty reserved space as specified?
5. **Preset values.** Confirm the three rounding tables against car measurements; the 10 m/100 m
   boundaries come from `PRODUCT.md` but "Precise" and "Calm" are design proposals.
6. **Units source.** Which Setup setting does `car` follow, and what is its value on 260128 EU?
7. **Restore semantics.** Does "Restore stock behaviour" take effect immediately or only after the
   next reboot (the shim is loaded at launch)? If reboot is needed, add a line to B4.
8. **Home/Star plumbing.** Does `hudnavd` expose `home_seq`/`home_epoch`, or must a separate
   `dbus-monitor` tail exist as in WF (UX §4)?
9. **Z-order.** Use `QRootScreen` z 2000 (SG §5) or match the host wiring of TW/WF (z 100)?
10. **Localization.** Are new `TranslationsModel.STR_*` keys acceptable, or must this app ship its
    own translation table to avoid touching all 26 stock ts files?
11. **Boot guard in main page.** Should `rolled_back` be surfaced outside Diagnostics as a one-line
    notice? Currently it is Diagnostics-only by design.
12. **Status file path.** Confirm `/rw_data/hudnav/status.json` mirror cadence and that it is written
    atomically (the file is the fallback when HTTP is down).
