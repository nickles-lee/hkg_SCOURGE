# Gen5W UX-behavior patterns — rules cards for custom UI

Four behavioral patterns every custom surface must implement correctly.
Companion to `deployment-modes.md` (where your UI lives) and `styleguide.md`
(how it looks). Evidence in `reports/ux-patterns.md`.

---

## 1. Driving-mode lockout

**The flag is readable in home QML — no daemon needed for the gate:**
`AppHomeModel.drivingRestriction` (bool; C++ Q_PROPERTY fed by
`NotifyDrivingRestrictionFromUSM`; signal `drivingRestrictionChanged`;
variant `drivingRestrictionTypeB`). AppSetup uses `AppSettingModel.
drivingRestriction` (same pattern). A daemon only adds gear granularity
(battery daemon decodes CAN `0x036` byte4: 0=P, 5=D, 6=N, 7=R) for
Park-gated actions.

**Stock reaction patterns** (in order of severity — copy these):

| Situation | Stock reaction | Example |
|---|---|---|
| Whole screen restricted | Swap content for a lockout/description pane — no popup | `Q_RightMenuDescription` with `STR_SETUP_POP_TRY_AGAIN_PARKING_VEHICLE`, gated `visible: drivingRestriction` |
| Single action restricted | Popup **on attempted action** (not before) | Reset button sends `KEY_EVT_POPUP_CAR_DRIVING_PROFILE` when `drivingRestriction` (`Q_AppSettingGeneralDefault.qml:147-165`) |
| Mid-flow restriction | Kick out: on `onDrivingRestrictionChanged` → close all popups + show driving popup | `Screen_AppSettingWifiAddNetwork.qml:476-486` |
| Passive info | Leave usable, or visual-degrade only | Manual screen swaps to `STR_HOME_POP_TRY_AGAIN_DRIVING_VEHICLE`; `isNeedStroke: !drivingRestriction` |

**Never**: silently disable a control that looks enabled (stock has zero
`enabled: drivingRestriction` row-disables), or swallow a click without
feedback. Toned-down stock tiles do no-beep-no-click — but they *look* disabled.

**Rules card:**
- [ ] CHECK: `typeof AppHomeModel !== "undefined" ? AppHomeModel.drivingRestriction : false`
- [ ] REACT by scope: whole surface → pane swap; single action → popup on attempt; passive → degrade visually only
- [ ] POPUP (we can't reach the C++ popup tables — they load by path on
      `KEY_EVT_SHOW_POPUP_*`): build it in-QML, copying `QHMI_AppHomePopup_OK.qml`
      geometry — `QPopupAnimationItem` card, 100 ms InOutQuad fade, dim 0.7 at
      y:90/h:630, outside-press closes, OK left, body 39 px
- [ ] Watch `onDrivingRestrictionChanged` and abort in-progress flows (text entry, toggles) when it flips true
- [ ] For a global, above-any-app warning use a SystemPopup (deployment-modes.md §E) instead

---

## 2. Edit mode (rearrange / home-layout / widget editing)

**The three edit screens** and how they're entered:

| Screen | Entry | What it edits |
|---|---|---|
| EditHomeLayout | option menu | Advanced ↔ Basic home type (`AppHomeModel.isAdvancedMode`) |
| AllMenus_Rearrange (Edit Menu) | **long-press 650 ms** on a grid tile, or option menu | app icon order (`AppHomeModel.editAllMenus/updateAllMenus`) |
| EditWidget(+Advanced) | long-press 650 ms on a widget, or option menu (page 0 only) | widget page slots (`editSettingWidget`, persisted in CustomLayoutDB) |

**The key fact**: all three are separate screens loaded through `main.qml`'s
double-buffered loaders, and `switch_current_loader()` **destroys** the
previous screen — the entire Advanced home (carousel, cards, overlays,
tiles) is destroyed on entry and recreated on return. Only the page index
survives (`goHistorypage()` restores `currentHomeAdvancedPageIdx`).

**Consequences for custom UI:**
1. **Don't participate in rearrange.** Your tile/card is destroyed during edit
   — it's never visible, draggable or removable there, and it must never join
   `AppHomeModel.listHomePage` or the C++ grid models.
2. There is **no `customizeMode` property on the Advanced home to bind**
   (`customizeModeHK_PAGE0..4` are declared-but-unused; only the Basic home
   uses them). Don't look for one.
3. Tile position must be a **re-evaluating binding**: `property var slot:
   firstEmptySlot(n)` + `visible: slot !== null && slot.page === N` — after
   edits, `onRequestUpdateAllMenu` rebuilds the grid and the slot must
   re-resolve; a full grid degrades to hidden, not overlapping.
4. Overlay: closes on `onCurrentScreenChanged` — which covers every
   edit-screen entry for free.
5. Card: assume destruction on every screen change; rehydrate volatile state
   from the daemon in `Component.onCompleted`.
6. Switching home type to Basic removes the whole carousel + your tile (stock
   behavior). Ship a Basic-home tile too (deployment-modes.md §H) if it matters.

**Rules card:**
- [ ] No grid-model membership; no rearrange participation
- [ ] Tile: re-resolving slot binding + `visible: slot !== null` guard
- [ ] Overlay: close hook on `onCurrentScreenChanged`
- [ ] Card/overlay: zero volatile state in QML; daemon is the source of truth
- [ ] No bindings to a home-screen `customizeMode` (doesn't exist)

---

## 3. Foreground-loss etiquette

**Stock evidence**: the Advanced home's `onIsAtOtherAppChanged` handler is
**commented out** (`Advanced.qml:522-529`) — the home scene does *nothing*
when another app takes the foreground; it survives, obscured, underneath.
`QMainScreenAnimation` and `QPopupAnimationItem` force-finish their
animations on `onSwitchToOtherApp` (no half-fades on return). App-local
popups set `opacity: 0` while `sysPopupShowing` (never fight a system popup
for the surface). `requestHomeByPP` is a C++-set bool meaning "a Home request
re-showed the home screen" — overlays close on it; the Advanced screen uses
it to skip the entry animation. The rearrange screen defers heavy rebuilds
until `!isAtOtherApp && visible` (`isPendingConstructModel`).

**Per-surface rules:**

| Surface | On FG loss | On return |
|---|---|---|
| Overlay (Mode A) | **close itself** (`visible: requested && !isAtOtherApp` + all close hooks + `home_seq`) | user reopens |
| Card (Mode B) | **stay** — never add `isAtOtherApp` self-hiding (it would vanish); it's automatically obscured | still there, data fresh via daemon |
| Tile | nothing | nothing |
| Daemon | keep running; `home_seq` matters only to the overlay | continues serving |

QML timers **keep running while obscured** (nothing pauses them) — bind
expensive work to `running: visible` or gate on
`AppHomeModel.currentHomeAdvancedPageIdx === myIndex`; cheap polls can run
always (battery overlay polls 500 ms regardless).

**Rules card:**
- [ ] Overlay self-closes on: back button, `isAtOtherApp`→true, `currentScreen` change, `currentHomeAdvancedPageIdx` change, `requestHomeByPP`, daemon `home_seq` bump
- [ ] Card never self-hides on FG loss
- [ ] Never raise/fight for the window; never draw over system popups (`opacity: 0` while `sysPopupShowing`)
- [ ] Force-finish animations on `onSwitchToOtherApp`
- [ ] Defer heavy rebuilds until `visible && !isAtOtherApp`
- [ ] Pause expensive off-screen work; keep cheap data polls running

---

## 4. Trio pattern (daemon + card + app)

**When you need the daemon** (any one is enough):
- state must survive FG loss / screen changes / edit-mode destruction (QML resets),
- an overlay must close on steering-wheel Home/Star (QML cannot see D-Bus),
- actions need CAN/D-Bus writes or gear/driving granularity.

Pure factory-model reads (SOC, range, charging state via `AppPHEVModel`)
need no daemon — tile+overlay alone suffices.

**Reference architecture** (battery daemon, proven on-car):

```
systemd: battery-daemon.service
  ConditionPathExists=/rw_data/battery/battery_daemon
  ExecStart=… --iface can0 --port 8088 --status-file /rw_data/battery/status.json
  Restart=always  RestartSec=3
daemon (static C):
  SocketCAN decode (0x2FC SoC, 0x036 gear, …)          → data
  M-CAN TX via dbus-send (SetCANMicomRecv)             → actions
  dbus-monitor --address $(cat /tmp/session_appmgr)    → home_seq / home_source
    (match: NotifyRequestHomeScreen, custom_key_value; retry 5 s)
  HTTP 127.0.0.1:8088 (Connection: close):
    GET /status /health        ← flat JSON superset (values + *_known flags + home_seq)
    POST /start /stop /dash/on|off /chargedoor/open|close|toggle
  file mirror: /rw_data/battery/status.json every 1000 ms
QML (each surface polls independently):
  Timer 500 ms (overlay) / 1-2 s (card) + XMLHttpRequest GET /status
  fallback: file:///rw_data/battery/status.json on non-200/parse error
  overlay consumes home_seq/home_source; card ignores them
  failure: "Service unavailable" + disabled controls — never fake freshness
```

**Rules card:**
- [ ] ONE daemon per app, distinct HTTP port (8088 battery, 8090 wifi, …)
- [ ] `GET /status` = flat JSON superset; POST actions with query params
- [ ] 1-2 s `status.json` file mirror as the fallback path
- [ ] `Restart=always` + `RestartSec=3` (~3 s self-heal; no QML reconnect logic needed)
- [ ] QML: `typeof`-guard everything; explicit unavailable state + disabled controls on failure
- [ ] Each surface consumes only the fields it needs (card ≠ overlay)
