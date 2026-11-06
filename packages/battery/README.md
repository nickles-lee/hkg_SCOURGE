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
- An **Open/Close charge door** toggle, enabled only in Park with no charging
  cable plugged in
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
   shows while another app owns the foreground.

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
| POST | `/start` | start preconditioning (micom `0x0C7`) |
| POST | `/stop` | stop preconditioning (micom `0x0C7`) |
| POST | `/dash/on` | enable synthetic `0x4ED` (micom) |
| POST | `/dash/off` | disable synthetic `0x4ED` |
| POST | `/chargedoor/open` | open the charge door (micom `0x4A2`) |
| POST | `/chargedoor/close` | close the charge door (micom `0x4A2`) |
| POST | `/chargedoor/toggle` | toggle the charge door |

The charge-door endpoints accept `?park=1`. The QML passes it when
`AppNModeModel` says Park; the daemon still vetoes if its own `0x036` frame says
otherwise.

The daemon also writes `/rw_data/battery/status.json` once per second; the QML
page falls back to that file if HTTP is unavailable.

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
- Charge door: `0x4A2` `FF FF FF DF FF FF FF FF` open, `EF` close; an
  unknown cable state (no `0x326` on the bus) is advisory and no longer
  blocks the door.
- Spoofed nav frame: an independent "Include spoofed nav frame" toggle sends
  `0x4ED` at 5 Hz whether or not preheat is running (experimental).
- A refused `/start`/`/stop` is shown in the UI ("Blocked: <reason>") instead
  of failing silently.
- `0x2AD` `0x00` counts as "off": a stop completes in under a second instead
  of grinding through ~50 s of retries.

## Safety

- The BMS / factory setting is the authority for READY, SoC and utility mode;
  those are advisory notes, not false start blocks.
- Preheating is refused when the minimum battery temperature is above 21 °C.
- Start and stop bursts retry at most four times, then stop.
- The daemon never starts preconditioning on its own at boot.
- The charge door is refused unless the car is in Park and no charging cable
  is plugged in; stale park/cable signals also block it.
- The charge door command is skipped when the door is already in the requested
  state, and a 500 ms cooldown prevents double taps.
- The charge-door state falls back to the commanded state when `0x3AA` is not
  visible on the bus (`door_source = "command"`).

## Test evidence

- `python3 tests/test_daemon.py` on `vcan0`: `0x2FC` temperature, micom
  `0x0C7`/`0x4ED`/`0x4A2` arrays, the charge-door gates, the park override,
  optimistic feedback, the send-failed path, the session-bus Home/Star
  monitor, and the bus sniffer all pass.
- `sh tests/test_installer.sh /tmp/battery_usb`: first install, reinstall,
  restore, Hello coexistence, and restore-after-Hello all pass.
- QML files pass the vendor Qt 5.4 `qmllint` inside the 260128 VM.
- The daemon runs on the vendor 4.1.27 kernel (`--help` verified in the VM).
- A `dbus-monitor` capture confirms the micom call carries
  `04 a2 08 ff ff ff df ff ff ff ff`.

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
- The steering-wheel Home/Star key is a session-bus event, not a QML key
  event: the daemon tails `dbus-monitor` on `/tmp/session_appmgr` for
  `NotifyRequestHomeScreen` / `custom_key_value` and bumps `home_seq`, which
  closes the overlay
  ID.
- The daemon assumes `can0` is I-CAN; other interfaces can be selected with
  `--iface`.
