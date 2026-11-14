# Battery app — Ioniq 5 Gen5W (260128)

A **Battery** tile on the page-2 home-screen app grid, a full-screen Battery
overlay, and a small daemon that starts/stops battery preconditioning and opens
or closes the charge door.

It follows the verified `sideload_app_poc` harness: a QML-only UI change
deployed from an `initial_root` USB stick, with backup and restore. The daemon
is a static x86-64 binary in `/rw_data/battery/`; nothing is flashed and the
updater, bootloader and partitions are untouched.

## What it shows

- Minimum battery cell temperature from I-CAN `0x2FC`
- Exterior temperature
- State of charge
- Battery heater state (Off / Preparing / Active / Requested) and heater power
- An **Engage battery preheater** toggle
- A **Show battery preheat on dash** toggle, enabled while preheating
- An **Open/Close charge door** toggle, enabled whenever no charging cable is
  observed.  Gear/Park is advisory only for now (`0x036` is unreliable).
- Advisory notes for SoC < 20%, utility mode, vehicle-not-ready, start
  timeout, and vehicle-initiated stop
- Preheat is blocked when the minimum battery temperature is above 21 °C

## Bus split

The head unit's `can0` is **I-CAN**, not M-CAN:

| Data | Bus | Path |
|---|---|---|
| Battery temp, SoC, charge cable | I-CAN | daemon reads `0x2FC`, `0x326` |
| Gear / Park | M-CAN | QML `AppNModeModel.gearPosRightTxtImg` |
| READY, utility, heater power, conditioning mode | M-CAN | QML `AppPHEVModel` |
| Preheat, dash, charge-door commands | M-CAN | micom D-Bus `SetCANMicomRecv` |

`battery_daemon --sniff [seconds]` prints the IDs visible on `can0` so the bus
can be confirmed on a new vehicle.

## Layout

```
src/battery_daemon.c        I-CAN daemon + micom commands + HTTP/JSON API
src/Makefile                static build
artifacts/qml/...           BatteryPage.qml, BatteryTile.qml,
                            BatterySwitch.qml, QHMI_HomeScreenMain_Advanced.qml
artifacts/assets/battery/   battery_icon_n.png, battery_icon_d.png
artifacts/systemd/          battery-daemon.service
backup/...                  original 260128 Advanced QML (hash reference)
scripts/run_stuff.sh        installer
scripts/run_stuff_restore.sh
scripts/run_stuff_diag.sh
scripts/build_payload.sh
scripts/patch_advanced.py   regenerates both Advanced variants
scripts/make_icons.py
tests/test_daemon.py        vcan bench test
tests/test_installer.sh     offline install/restore/Hello-coexistence test
```

## UI placement

The Battery app is **not** a swipe page. `patch_advanced.py` adds:

1. `BatteryTile` to the app grid. It uses the shared global allocator
   (`firstEmptySlot`) and takes the next custom-tile slot after Hello, WiFi
   and/or Tweaks. The tile can therefore land on page 2 or page 3 when page
   1 is full; it is not hard-coded to page 2.
2. `BatteryPage` as a full-screen overlay at `z: 2000`, opened with
   `open()`/`close()` and gated on `AppHomeModel.isAtOtherApp` so it never
   shows while another app owns the foreground. Input is disabled while a
   system popup is showing (`sysPopupShowing`).

The overlay starts at `y: 90` and is 630 px high, so it does not draw under
the clock/status bar. Tile coordinates stay relative to `menuPageN`, which is
already below the top menu bar. See `../wifi_app/docs/UI-TILES.md` for the
general rule.

## Build

```sh
make -C src
python3 scripts/make_icons.py
python3 scripts/patch_advanced.py \
    backup/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
    artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml
python3 scripts/patch_advanced.py \
    backup/hello/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
    artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.hello.qml
sh scripts/build_payload.sh /tmp/battery_usb
```

## Install

Copy `run_stuff.sh`, `payload/`, `backup/`, `backup_current/`, `manifest.md5`
and `README.txt` to the root of a FAT/exFAT stick and plug it into the head
unit. The failsafe runs the script as root within ~8 s.

The installer:

1. Verifies `manifest.md5`.
2. Detects the stock QML, Hello, WiFi, Hello+WiFi, or a previous Battery
   install, and picks the matching Advanced variant. This lets Battery be
   installed before or after WiFi without deleting the other app's tile.
3. Backs up the current Advanced QML to `/rw_data/battery/backup/<stamp>/`
   and to the USB `backup_current/`.
4. Installs the QML, icons, daemon and systemd unit. On a reinstall, a failed
   QML copy is retried after unlinking the destination (and remounting `/`
   rw if it went ro); the original `cp` error is kept in the install log.
   The daemon and the `CANService`/`dbus-monitor` helpers are stamped with
   the installer's own SMACK label (`_` in the normal USB flow). The install
   fails if the `SMACK64EXEC` readback does not match, and reports the live
   daemon label as `daemon_smack` in `run_stuff_result.txt`.  `CANService` is
   the micom sender; the vendor service only accepts CAN sends from that
   process name.  See `docs/MICOM_WHITELIST.md`.
5. Starts and enables `battery-daemon.service`.
6. Restarts `AppHomeScreen` through AppLauncher app ID 11.

Restore: copy `run_stuff_restore.sh` over `run_stuff.sh` and plug the stick in.
It restores the pre-install QML (including a Hello-patched file) and removes the
daemon, tile and service.

## Daemon API

Bound to `127.0.0.1:8088` only.

| Method | Path | Meaning |
|---|---|---|
| GET | `/status` | full JSON state |
| GET | `/health` | liveness |
| GET | `/logs?source=daemon\|install\|restore` | last 16 KB of the selected log |
| POST | `/logs/dump` | queue a log bundle to the USB stick |
| POST | `/start` | start preconditioning (micom `0x0C7`) |
| POST | `/stop` | stop preconditioning (micom `0x0C7`) |
| POST | `/dash/on` | enable synthetic `0x4ED` (micom) |
| POST | `/dash/off` | disable synthetic `0x4ED` |
| POST | `/chargedoor/open` | open the charge door (micom `0x4A2`) |
| POST | `/chargedoor/close` | close the charge door (micom `0x4A2`) |
| POST | `/chargedoor/toggle` | toggle the charge door |

The charge-door endpoints still accept a legacy `?park=1` query but ignore it.
Gear/Park is not a gate at the moment because `0x036` is unreliable; only an
observed charging cable vetoes the command.

The daemon also writes `/rw_data/battery/status.json` once per second; the QML
page falls back to that file if HTTP is unavailable. `/status` includes
`smack`, `smack_expected`, `smack_ok`, `smack_send`, `smack_monitor`,
`micom_name`, `micom_path`, `batt_b2`, `batt_b4`, `batt_age_ms` and
`temp_byte` so the privileged daemon, its helpers and the temperature source
can be verified without a shell.

### Live console and USB log dumps

On wide (1920) layouts the Battery overlay gets a **Live log** console on the
right with **Daemon** and **Install** tabs, a 2 s refresh, and a **Dump**
button.  On 1280 layouts the console is hidden; a **Save logs** button next to
Home is always available.

`battery_daemon --dump-logs [--usb DIR] [--reason TEXT]` writes
`battery_logs_<UTC>/` to the stick with the daemon/install/restore logs, the
last status, `smack_expected`, and a `state.txt` snapshot (mounts, process and
SMACK state, CAN interface, QML hashes, journal, dmesg, and a 2 s CAN sniff).
The daemon does the same in a forked child for `POST /logs/dump`.
Only the newest 5 bundles are kept.  The stick is found via `/proc/mounts`
under `/media/*` or `/run/media/*` with no filesystem-type whitelist (the car
uses Tuxera `texfat`), and is remounted rw first.

Error dumps are automatic on micom/preheat/charge-door send failures, at most
**once per 10 minutes** (the timestamp persists across daemon restarts).
Manual dumps are unlimited.  `/status` reports `dump_state`, `dump_message`
and `dump_dir`.

## Commands

All M-CAN commands go through the factory micom service:

```
dbus-send --system --type=method_call --dest=com.lge.car.micom /Can \
  com.lge.car.micom.can.SetCANMicomRecv \
  array:byte:0x00,0xc7,0x08,0x00,0x00,0x00,0x40,0x03,0x00,0x00,0x00
```

- Preheat start: `0x0C7` `00 00 00 40 03 00 00 00` ×3, then `E0 07` ×3.
- Preheat stop: `0x0C7` all zero ×3, then `E0 07` ×3.
- Dash: `0x4ED` `00 00 00 00 00 10 A0 00` at 5 Hz.
- Charge door, factory frames: open = `0x4A2` all-FF then `0x4A2` with `DF`
  in data[3]; close = `0x4A2` all-FF then `0x4A2` with `EF` in data[3]
  (from the decompiled TmsRemoteApp `EVRemoteChargeDoorOpen/Close`).
  `0x4C5` is not a door command; it belongs to the target-SoC path.
  An unknown cable state (no `0x326` on the bus) is advisory and no longer
  blocks the door.
- All micom frames are sent through `/rw_data/battery/bin/micom-send`, a
  wrapper around a copy of the vendor `dbus-send` installed as
  `/rw_data/battery/bin/CANService`.  The vendor micom only accepts CAN sends
  from a short list of process names; `dbus-send` is not on it.  See
  `docs/MICOM_WHITELIST.md`.
- Full WiCAN sequence mimicry: the `0x0C7` burst at 40 ms pacing plus the
  `0x4E8`/`0x4CC` cluster countdown during start and stop; without BMS
  status the start assumes active after the 70 s timeout; exhausted start
  retries end with a cleanup stop burst.
- A refused `/start`/`/stop` is shown in the UI ("Blocked: <reason>") instead
  of failing silently.
- `0x2AD` `0x00` counts as "off": a stop completes in under a second instead
  of grinding through ~50 s of retries.
- The daemon reports its build version (`ver` in `/status`, shown in the
  footer) and micom send health (`send_ok`/`send_fail`); the installer
  verifies the *running* daemon matches the payload binary and force-restarts
  a stale one (`daemon_fresh` in the result).

## Safety

- The BMS / factory setting is the authority for READY, SoC and utility mode;
  those are advisory notes, not false start blocks.
- Preheating is refused when the minimum battery temperature is above 21 °C.
- Start and stop bursts retry at most four times, then stop.
- The daemon never starts preconditioning on its own at boot.
- The charge door is refused only when a charging cable is observed; an
  unknown/stale cable state is advisory.  Gear/Park no longer blocks it while
  the `0x036` decode is unsettled.
- The charge door command is skipped when the door is already in the requested
  state, and a 500 ms cooldown prevents double taps.
- The charge-door state falls back to the commanded state when `0x3AA` is not
  visible on the bus (`door_source = "command"`).

## Test evidence

- `python3 tests/test_daemon.py` on `vcan0`: `0x2FC` b2/b4 temperature with
  transient-zero hold, micom
  `0x0C7`/`0x4ED`/`0x4A2` arrays, the charge-door cable gate (gear gate
  disabled), optimistic feedback, the send-failed path, the session-bus
  Home/Star monitor, the WiCAN sequence tests (assume-active, cleanup stop,
  cluster display), the `/logs` tail, the manual USB dump, the error dump
  with its 10-minute rate limit, and the bus sniffer all pass.
- `sh tests/test_logdump.sh`: bundle contents/reason, pruning to five, and
  the no-USB failure all pass.
- `sh tests/test_installer.sh /tmp/battery_usb`: first install, reinstall,
  restore, Hello coexistence, and restore-after-Hello all pass.
- QML files pass the vendor Qt 5.4 `qmllint` inside the 260128 VM.
- The daemon runs on the vendor 4.1.27 kernel (`--help` verified in the VM).
- A `dbus-monitor` capture confirms the micom call carries
  `04 a2 08 ff ff ff df ff ff ff ff`.
- The `CANService`/`micom-send` helper pair is installed by `run_stuff.sh`
  from the vendor `dbus-send`; the VM validates its D-Bus message.  The micom
  whitelist itself is documented in `docs/MICOM_WHITELIST.md` and verified
  on-car with `battery_daemon --micom-check` / `--micom-test`.

## Known limitations

- `0x3AA` charge-door state is M-CAN and may be invisible on `can0`; the daemon
  then reports the commanded state.
- `0x326` byte3 bit 2 is the AC connector status. DC fast charging also sets
  the charging bit, which the daemon treats as cable present.
- The gear fallback to `0x036` only works if that frame is visible; otherwise
  the QML relies on `AppNModeModel`. The model clears its gear text after a
  gear change, so the page caches the last non-empty value.
- `0x2FC` byte 3 is not used; the factory CANService only decodes byte 2 as the
  minimum. The page shows the minimum alone.
- The page is a QML overlay inside `AppHomeScreen`, not a separate process/app
- UI standards (`docs/ui/styleguide.md`): themed background image and text
  colors (`HomeThemeHelper`, `ConstantModel`), `ConstantModel.FONT_SIZE_*`
  fonts, beep-on-release and slide-off-cancel on every control. Documented
  deviations: English-only strings (no 26-language ts infrastructure) and
  the custom switch graphic.
- The steering-wheel Home/Star key is a session-bus event, not a QML key
  event: the daemon tails `dbus-monitor` on `/tmp/session_appmgr` for
  `NotifyRequestHomeScreen` / `custom_key_value` and bumps `home_seq`, which
  closes the overlay
  ID.
- The daemon assumes `can0` is I-CAN; other interfaces can be selected with
  `--iface`.
