# SPEC — Battery app for Ioniq 5 Gen5W (260128)

## 1. Target

| Item | Value |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W (LG/Velix head unit), EU |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Platform | x86-64 Linux, Qt 5.4 / QML |
| UI process | `/app/bin/AppHomeScreen` (AppLauncher app ID 11) |
| QML root | `/app/share/AppHomeScreen/qml_eco3.0/` |
| `can0` | **I-CAN** (temperature, SoC, charge connector) |
| M-CAN | reached through the micom D-Bus service |
| Deployment | `initial_root` USB failsafe (`run_stuff.sh` as root) |

The head unit's `can0` is I-CAN, not M-CAN. M-CAN data (gear, READY, BMS
precondition state, charge-door module) is not visible there; the QML page
reads those from `AppPHEVModel` / `AppNModeModel`. M-CAN commands are sent
through `com.lge.car.micom` `/Can` `SetCANMicomRecv`.

## 2. Goals

1. Add a **Battery** app tile to the page-2 app grid and a full-screen overlay.
2. Show battery temperature, exterior temperature, SoC, heater state, and the
   factory conditioning-mode status.
3. Provide a manual preconditioning start/stop toggle.
4. Provide an independent "Include spoofed nav frame" toggle that emits a
   synthetic `0x4ED` nav-precondition frame, usable whether or not preheat
   is running (experimental: tests whether the BMS needs a navigation
   source to accept a preheat start).
5. Show advisory conditions (not READY, utility mode, SoC < 20 %) without
   falsely blocking the BMS, and block preheat when the minimum battery
   temperature is above 21 °C.
6. Provide an **Open/Close charge door** toggle, gated on Park and on no
   charging cable being plugged in.
7. Install and restore through the verified USB harness, coexisting with the
   Hello app if it is installed.

## 3. Non-goals

- No firmware flash, updater, bootloader or partition changes.
- No new AppLauncher app ID or separate UI process.
- No automatic preconditioning at boot.
- No modification of AppNavi or the factory navigation preconditioning logic.
- No `QListViewHomePage` swipe page for Battery.

## 4. CAN protocol

### 4.1 I-CAN (`can0`, RX)

| ID | Use | Decode |
|---|---|---|
| `0x2FC` | SoC and battery temperature | byte7 / 2 = percent; byte2 = minimum °C (the factory `CANService` `m_nEVMinBattTempVal`). Byte 3 is not used. |
| `0x326` | charge connector | `(byte3 >> 2) & 3` non-zero = cable connected; `byte4 & 3` non-zero = charging |

`0x152` byte0 is a min-temperature fallback when `0x2FC` has not been seen for
5 s. The page shows only the minimum cell temperature; there is no max.

### 4.2 M-CAN commands (TX, via micom)

The micom array is `[CAN ID hi, CAN ID lo, DLC, data0..data7]`.

```
START 0x0C7:  00 C7 08 00 00 00 40 03 00 00 00   x3
              00 C7 08 00 00 00 E0 07 00 00 00   x3

STOP  0x0C7:  00 C7 08 00 00 00 00 00 00 00 00   x3
              00 C7 08 00 00 00 E0 07 00 00 00   x3

DASH  0x4ED:  04 ED 08 00 00 00 00 00 10 A0 00   (5 Hz)

DOOR  0x4A2:  04 A2 08 FF FF FF DF FF FF FF FF   (open)
              04 A2 08 FF FF FF EF FF FF FF FF   (close)
```

D-Bus call:

```
destination  com.lge.car.micom
path         /Can
interface    com.lge.car.micom.can
method       SetCANMicomRecv
argument     array of bytes
```

The factory `EVRemoteChargeDoorOpen/Close(int signalMode)` sets `0xDF`/`0xEF`
only when `signalMode == 1`; all-`FF` is the no-command form.

### 4.3 M-CAN status (not visible on `can0`)

The daemon still decodes these if they appear (bench tests, other variants),
but the QML uses the factory models instead:

| ID | Use | Decode | QML source |
|---|---|---|---|
| `0x036` | gear | `byte4 & 0x0F`: 0 P, 5 D, 6 N, 7 R | `AppNModeModel.gearPosRightTxtImg` |
| `0x038` | READY | `(byte0 & 0x0F) == 0x04` | `AppPHEVModel.vcuEvReady` |
| `0x152` | battery temp | byte0/1 signed °C | daemon `0x2FC` |
| `0x2AD` | precondition state | `byte1 & 0x1F`: 00 off, 01 idle, 05 preparing, 15 active | `AppPHEVModel.heatBatteryPercent` |
| `0x2AE` | utility | byte4 01 normal, 06 utility | `AppPHEVModel.utilityMode` |
| `0x3AA` | charge door | `(byte4 >> 1) & 3`: 1 open, 2 closed | daemon last command |

## 5. Daemon

### 5.1 State machine

```
IDLE
  start request -> STARTING
STARTING
  burst complete, 0x2AD 0x05 -> PREPARING
  burst complete, 0x2AD 0x15 -> ACTIVE
  10 s without 0x05 -> retry (max 4) -> TIMEOUT
PREPARING
  0x2AD 0x15 -> ACTIVE
  100 s without 0x15 -> TIMEOUT (recovers if 0x15 arrives later)
ACTIVE
  0x2AD 0x01 for 2 s -> IDLE ("stopped_by_vehicle")
STOPPING
  burst complete, 0x2AD 0x01 -> IDLE
  10 s without 0x01 -> retry (max 4) -> IDLE ("stop_failed")
```

The state machine is best-effort on I-CAN because `0x2AD` is M-CAN. The QML
shows the factory heater state and keeps an optimistic request state.

### 5.2 Start rules

The BMS / factory setting is the authority for READY, SoC and utility mode.
Those are advisory notes in the QML. The one local start block is the
user-requested temperature cutoff: **do not start preheating when the minimum
battery temperature is above 21 °C** (`temp_high`). A stop already in progress
(`stopping`) is also blocked.

While the QML keeps a preheat request active and the heater has not engaged, it
re-posts `/start` every 10 s (up to 4 times) to re-nudge the BMS. This mirrors
the WiCAN firmware's retry behavior.

### 5.3 Spoofed nav frame / dash overlay

`0x4ED` is sent at 5 Hz while the "Include spoofed nav frame" toggle is on.
The toggle is **independent of preheat state** (experimental): it can be
enabled before a preheat start so the nav-precondition status is already
present when the `0x0C7` burst arrives, and it can be tested alone to see
whether the cluster shows the precondition icon. The daemon keeps a
`dash_override` so the frames are not cancelled when its local `0x2AD`
state is idle, and backs off for 3 s whenever the factory navigation sends
its own `0x4ED`.

### 5.4 Charge door

```
IDLE
  open/close request + Park + no cable -> OPENING/CLOSING
OPENING
  door state 1 -> "open"
  no 0x3AA on this bus + the door's own dbus-send exited 0 after 1.2 s -> "open" (commanded)
  2 s without feedback -> retry once -> "no_response"
CLOSING
  door state 2 -> "closed"
  no 0x3AA on this bus + the door's own dbus-send exited 0 after 1.2 s -> "closed" (commanded)
  2 s without feedback -> retry once -> "no_response"
```

Gate:

| Condition | Reason string |
|---|---|
| Park signal missing/older than 2 s and no QML override | `park_unknown` |
| bus gear is not P (even with QML override) | `not_park` |
| cable observed connected or charging (fresh) | `cable_plugged` |

An **unknown** cable state no longer blocks the door: `0x326` is invisible on
some cars, so "cable unknown" is advisory in the QML ("Cable state unknown —
allowed") and the command proceeds. Only a cable that is actually observed
connected vetoes it.

A stop completes as soon as the burst is done and the BMS reports off
(`0x00`) or idle (`0x01`); without any `0x2AD` on the bus the completed burst
itself is the confirmation. Every distinct `0x2AD` value is logged
(`battery_daemon: bms 0x..`) for diagnosis.

The QML passes `?park=1` when `AppNModeModel` says the car is in Park. The
daemon still vetoes if its own `0x036` frame says otherwise. The command is
skipped when the door is already in the requested state; a 500 ms cooldown
prevents double taps. The door tracks the exit status of **its own**
`dbus-send` child (the burst/dash sends fork separate children whose status
must not be mistaken for the door's).

### 5.5 HTTP API

`127.0.0.1:8088`, JSON, `Connection: close`.

```
GET  /status
GET  /health
POST /start
POST /stop
POST /dash/on
POST /dash/off
POST /chargedoor/open[?park=1]
POST /chargedoor/close[?park=1]
POST /chargedoor/toggle[?park=1]
```

`/status` fields: `state`, `precond_raw`, `batt_min_c`, `batt_valid`,
`temp_source`, `soc_pct`, `soc_valid`, `ready`, `utility`,
`blocked`, `blocked_reason`, `dash`, `dash_allowed`, `factory_nav`, `retries`,
`eta_s`, `message`, `park`, `park_known`, `gear`, `cable`, `cable_known`,
`charging`, `door_state`, `door_state_name`, `door_known`, `door_source`,
`door_warn`, `door_opt`, `door_busy`, `door_allowed`, `door_block_reason`,
`door_message`, `home_seq`, `home_source`, `last_app_key`, `home_monitor`.

The daemon also mirrors the JSON to `/rw_data/battery/status.json` every second.

### 5.6 Session-bus Home/Star monitor

The steering-wheel Home/Star key never reaches QML as a key event. The app
manager delivers it over the session bus whose address `Dbuslaunch.sh` writes
to `/tmp/session_appmgr`:

- **Home request**: method call `NotifyRequestHomeScreen(ay)` on
  `com.lge.hkmc.avn.appfunc.appInterface`, destination `AppHomeScreen`.
- **Star/custom key**: signal `custom_key_value(i)` on
  `com.lge.avn.appmanager.info`.

The daemon runs `dbus-monitor --address <addr>` with match rules for those two
members and bumps `home_seq` (`home_source` = `home` or `star`, plus
`last_app_key` for the custom key). The QML overlay closes whenever
`home_seq` changes. If the bus file or the monitor is missing it retries every
5 s and reports `home_monitor: false`; a stale `guid=` in the address file is
stripped so the monitor reconnects after a bus restart. Options:
`--session-bus-file PATH`, `--dbus-monitor PATH`, `--no-session-monitor`.

### 5.7 Bus sniffer

`battery_daemon --sniff [seconds]` opens `can0` without a filter and prints the
IDs seen, frame counts, and last payloads. Use it to confirm which bus the head
unit exposes.

## 6. QML integration

### 6.1 Layout rule

The top **90 px** are the system status bar. The Battery overlay starts at
`y: ConstantModel.MENU_BAR_Y` and is `DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y`
(630 px) high. See `docs/ui/UI_CONTEXT.md` (in the SCOURGE repo).

### 6.2 Tile + overlay

`QHMI_HomeScreenMain_Advanced.qml` is patched to:

1. Add `BatteryTile` to the app grid using the shared `firstEmptySlot`
   allocator. It follows Hello/WiFi/Tweaks and may land on page 2 or page 3.
2. Place it at the first empty cell, or the second empty cell when Hello is
   present (`nthEmptyIndex(menuPage2.pageModel, 0|1)`).
3. Add `BatteryPage` as a full-screen overlay at `z: 2000`, initially hidden.
4. Open it with `batteryOverlay.open()`, close with `close()`; the page owns
   its visibility (`requested && !AppHomeModel.isAtOtherApp`).
5. Close triggers: the Home button, `onButtonBackClicked`,
   `isAtOtherApp` going true, `currentScreen`/`currentHomeAdvancedPageIdx`
   changing, `requestHomeByPP`, and a `home_seq` bump from the daemon's
   session-bus Home/Star monitor.
6. A full-screen input shield (`preventStealing`, all buttons, wheel
   accepted) keeps clicks from reaching the home grid or another app's
   surface; content sits at `z: 2` above the shield at `z: 1`.

The `QListViewHomePage` model and page count are unchanged.

### 6.3 New files

| File | Purpose |
|---|---|
| `HomeScreen/BatteryPage.qml` | the overlay, polling and controls |
| `component/BatteryTile.qml` | page-2 app-grid tile |
| `component/BatterySwitch.qml` | toggle switch |
| `battery/battery_icon_n.png` | white line-art battery icon |
| `battery/battery_icon_d.png` | dim variant |

### 6.4 Data sources in QML

| Value | Source |
|---|---|
| battery temp, SoC, cable, charge door | daemon (I-CAN) |
| gear / Park | `AppNModeModel.gearPosRightTxtImg` (fallback `gearPosLeftTxtImg`); the raw `signalGEAR_TYPE(int)` is cached, and the last non-empty text is kept because the model clears the display string after a gear change |
| READY | `AppPHEVModel.vcuEvReady` |
| utility mode | `AppPHEVModel.utilityMode === 2` |
| SoC fallback | `AppPHEVModel.batLevel` |
| heater power | `AppPHEVModel.heatBatteryPercent` |
| conditioning mode | `AppPHEVModel.batteryConditionMode` |
| exterior temperature | `appClimateModel.ambienTemp` |

All factory-model reads are guarded with `typeof`. The charge-door switch is
enabled only when Park is known, the car is in Park, the cable state is known,
and no cable is connected.

## 7. Installer

`run_stuff.sh` (revision 1) follows the Hello harness:

1. Verify `manifest.md5`.
2. Pick the Advanced variant:
   - contains `BATTERY_PAYLOAD_260128` + `HELLO_PAYLOAD_260128` -> hello
   - contains `BATTERY_PAYLOAD_260128` -> plain
   - contains `HELLO_PAYLOAD_260128` -> hello
   - matches the 260128 reference -> plain
   - otherwise abort.
3. Back up the current Advanced QML; keep the first pre-install file as
   `backup/latest` even across reinstalls.
4. Remount `/` rw, install assets, daemon, service and QML, preserve SMACK
   labels where possible. The daemon keeps the `/rw_data` floor label (`_`),
   which has `rwx` access to `App::micommanager` for the D-Bus calls.
5. Clear QML caches.
6. Enable and start `battery-daemon.service` while `/` is still rw.
7. Remount `/` ro.
8. Restart `AppHomeScreen` via AppLauncher app ID 11.

Environment overrides used by the offline test:
`BATTERY_PATH`, `BATTERY_QML_ROOT`, `BATTERY_RW_DIR`, `BATTERY_SERVICE`.

## 8. Coexistence with Hello

- The installer never touches `HelloTile.qml`, `HelloPopup.qml`,
  `HelloButton.qml`, or the `hello/` assets.
- When Hello is present, the Hello-patched Advanced file is used as the base.
  Hello occupies the first empty page-2 cell; Battery uses the second.
- Restore returns the exact pre-install file, so a Hello install remains
  intact.

## 9. Test evidence

| Test | Result |
|---|---|
| `tests/test_daemon.py` on `vcan0` | pass (0x2FC temp, micom 0x0C7/0x4ED/0x4A2, door gates, optimistic feedback, door send-fail not masked by dash frames, BMS 0x00 stop exit, spoofed nav frame without preheat, unknown-cable door, session-bus Home/Star monitor, sniff) |
| `tests/test_installer.sh` | pass (install, reinstall, restore, Hello, restore-after-Hello) |
| `qmllint` (vendor Qt 5.4, 260128 VM) | pass for all five QML files |
| daemon `--help` on vendor 4.1.27 kernel | pass |
| `dbus-monitor` capture of the micom call | `04 a2 08 ff ff ff df ff ff ff ff` |

## 10. Known limitations

- `0x3AA` charge-door state is M-CAN and may be invisible on `can0`; the daemon
  then reports the commanded state (`door_source = "command"`).
- `0x326` byte3 bit 2 is the AC connector status. DC fast charging also sets
  the charging bit, which the daemon treats as cable present.
- The page is a QML overlay inside `AppHomeScreen`, not a separate process/app
  ID.
- The daemon assumes `can0` is I-CAN; other interfaces can be selected with
  `--iface`.
- The gear fallback to `0x036` only works if that frame is visible; otherwise
  the QML relies on `AppNModeModel`.
