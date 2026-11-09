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
4. Fully mimic the WiCAN start/stop sequences: the `0x0C7` burst at 40 ms
   pacing plus the `0x4E8`/`0x4CC` cluster countdown display, with a manual
   "Cluster display test" toggle usable whether or not preheat is running.
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

CLUSTER 0x4E8: 04 E8 08 <tenths|unit> 00 00 00 <dist lo> <dist hi> <flag> 00  (100 ms)
       0x4CC: 04 CC 08 02 00 00 00 00 00 00 00                        (with every 0x4E8)

DOOR  0x4A2:  04 A2 08 FF FF FF FF FF FF FF FF   (open, all-FF)
              04 A2 08 FF FF FF DF FF FF FF FF   (open, commit)
      0x4C5:  04 C5 08 00 00 00 00 00 00 00 00   (close — distinct ID)
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
| `0x036` | gear | raw frame kept; `gear_b3` = `byte3 & 3`, `gear_b4` = `byte4 & 0x0F` — the two factory binaries disagree on the byte (AppHomeScreen reads byte3, TmsRemoteApp byte4), so both are reported and the on-car sniff settles it | `AppNModeModel.gearPosRightTxtImg` |
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

### 5.3 Cluster display (full WiCAN mimicry)

The start/stop sequences replicate the WiCAN `i-precondition` firmware
exactly:

- `0x0C7` burst: 3x start (`00 00 00 40 03 00 00 00`) then 3x commit
  (`00 00 00 E0 07 00 00 00`), one frame every **40 ms** (the WiCAN sm tick).
- While a start is in flight (`starting`/`preparing`), the daemon injects a
  `0x4E8` countdown (seconds remaining of the 70 s started timeout, retry
  count in the tenths digit, unit YD/KM/M, flag DESTINATION/BLUE_1/BLUE_4)
  plus `0x4CC` (`data[0]=0x02`) every 100 ms. The display stops once the
  BMS reports started (`0x15`), exactly like the WiCAN.
- While a stop is in flight and the BMS state is visible, it injects the
  stop countdown (seconds to the next retry, unit FT/MI, flag NONE), hidden
  at max retries.
- Without any `0x2AD` status on the bus, one burst is sent and after the
  started timeout the daemon **assumes active** (WiCAN `wait_starting_tick`).
- Start retries exhausted -> one cleanup stop burst without stop retries,
  then idle (WiCAN "retries exhausted in once mode").
- The manual "Cluster display test" toggle sends the same `0x4E8`+`0x4CC`
  frames standalone (independent of preheat), so the display can be tested
  without a preheat attempt.
- The factory streaming its own `0x4E8` (detected on can0) backs ours off
  for 3 s.

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

Door frames are byte-identical to the decompiled factory TmsRemoteApp
(`EVRemoteChargeDoorOpen/Close`): open = all-FF `0x4A2` then `0x4A2` with
`DF` in data[3]; close = `0x4C5` all-zero. The factory applies no Park/cable
gate in the send path (only a remote-service-state whitelist); our Park/cable
gates are stricter than stock and stay.

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
`door_message`, `batt_cond`, `batt_cond_known`, `gear_raw`, `gear_b3`,
`gear_b4`, `smack`, `smack_expected`, `smack_ok`, `smack_send`,
`smack_monitor`, `home_seq`, `home_source`, `last_app_key`, `home_monitor`,
`ver`, `send_ok`, `send_fail`, `last_send`.

The daemon also mirrors the JSON to `/rw_data/battery/status.json` every second.

### 5.6 Send-health semantics (VM-validated on the firmware's dbus 1.8.20)

`send_ok`/`send_fail` count the exit status of each `dbus-send` child. The
firmware's dbus-send 1.8 **without `--print-reply` exits 0 as soon as the
message is queued**, even when the destination service is absent — so
`send_ok` means "accepted by the bus", not "handled by micom". This matches
the factory's fire-and-forget pattern (TmsRemoteApp also sends without
waiting); if the micom service itself is down the head unit is broken well
beyond this app. The daemon never uses `--address` with dbus-send (only
`--system`, which every version supports); `--address` is only used for
`dbus-monitor`, which dbus 1.8 supports.

The exact two-rule monitor invocation was validated in the 260128 VM against
the firmware's own dbus-daemon/dbus-monitor/dbus-send 1.8.20: the Star
signal (`member=custom_key_value` + `   int32 7`) and a routed Home method
call (`member=NotifyRequestHomeScreen`) both appear in the monitor output in
the format the daemon parses. Note dbus 1.8 eavesdropping only sees messages
the bus actually routes, so the Home request is visible exactly when
AppHomeScreen (the destination) is running — which is also the only time the
overlay exists to close.

### 5.7 Session-bus Home/Star monitor

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

### 5.8 Bus sniffer

`battery_daemon --sniff [seconds]` opens `can0` without a filter and prints the
IDs seen, frame counts, and last payloads. Use it to confirm which bus the head
unit exposes.

## 6. QML integration

### 6.1 Layout rule

The top **90 px** are the system status bar. The Battery overlay starts at
`y: ConstantModel.MENU_BAR_Y` and is `DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y`
(630 px) high. See `docs/ui/UI_CONTEXT.md` in this repo.

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
4. Remount `/` rw, install assets, daemon, service and QML.
   - Read the installer's own SMACK label from `/proc/self/attr/current`
     (`_` in the normal initial-root/USB flow) and stamp it as
     `security.SMACK64EXEC` on the daemon and on copies of `dbus-send` /
     `dbus-monitor` under `/rw_data/battery/bin/`. The readback must match or
     the install fails; there is no silent fallback.
   - Remove each destination before copying: `cp -f` keeps the destination
     inode and its xattrs, so a stale exec label would otherwise survive.
   - Write the expected label to `/rw_data/battery/smack_expected`; the
     daemon reports `smack`, `smack_expected`, `smack_ok`, `smack_send` and
     `smack_monitor` in `/status`.
   - After starting the service, read the live daemon's
     `/proc/<pid>/attr/current` into `daemon_smack`; a mismatch is reported
     as `smack_check=mismatch` and fails the install.
   - `SmackProcessLabel=` is deliberately not used: the vendor 4.1.27 kernel
     only implements the `current` procattr, so systemd 225 fails with
     status 237/SMACK_PROCESS_LABEL.
5. Clear QML caches.
6. Enable and start `battery-daemon.service` while `/` is still rw.
7. Remount `/` ro.
8. Restart `AppHomeScreen` via AppLauncher app ID 11.

Environment overrides used by the offline test:
`BATTERY_PATH`, `BATTERY_QML_ROOT`, `BATTERY_RW_DIR`, `BATTERY_SERVICE`,
`BATTERY_SMACK_SELF` (force the expected label), `BATTERY_DAEMON_SMACK`
(force the runtime readback). The live path never sets the last two.

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
| `tests/test_daemon.py` on `vcan0` | pass (0x2FC temp, micom 0x0C7 at 40 ms, 0x4E8/0x4CC cluster display, assume-active, cleanup stop burst, factory door frames 0x4A2/0x4C5, door gates, optimistic feedback, door send-fail not masked by dash frames, BMS 0x00 stop exit, cluster display without preheat, unknown-cable door, 0x25D batt_cond, 0x036 raw gear, session-bus Home/Star monitor, sniff) |
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
