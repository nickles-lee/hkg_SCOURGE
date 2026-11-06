# WiFi / Wireless CarPlay / SSH app — Ioniq 5 Gen5W (260128)

A QML home-screen app plus a static C daemon that controls the head unit's
onboard Broadcom WLAN (`wlan0`), starts a wireless CarPlay access point, and
runs a small SSH server.

It follows the same deployment model as the Hello and Battery apps: a USB
`run_stuff.sh` installer, a QML page/tile inside `AppHomeScreen`, a daemon in
`/rw_data/wifi`, and a systemd service. Nothing is flashed; the updater,
bootloader and partitions are untouched.

**Status:** built and packaged; VM-validated on the vendor kernel/Qt
(daemon, command forms, QML, installer/restore, Home/Star overlay close).
On-car retest is pending for the radio paths; Wireless CarPlay is
implemented but still needs the on-car iPhone test.

---

## What it does

- Adds a **WiFi** tile to:
  - the Advanced home screen, using the first free app-grid cell across
    pages 1-3 (on the 1920 car this is often page 2; page 3 is possible),
  - the Wide home screen (next to All Menu / Hello),
  - All Menus and Setup via the shared menu-list component.
- Places custom tiles with one global allocator and an install-time ordinal
  (Hello=0, Battery=1, WiFi=2, Tweaks=3), so the tiles do not fight over page
  1. See `docs/UI-TILES.md`.
- Detects both Battery designs: the older page-based Battery app and the
  later Battery-tile app. The matching `battery_wifi` /
  `hello_battery_wifi` or `battery_tile_wifi` / `hello_battery_tile_wifi`
  variant is installed, so a WiFi reinstall does not drop the Battery tile.
- Refreshes the Hello/Battery tile components and their blue icons as part of
  the install, so all custom tiles follow the light/dark theme instead of
  drawing white text on a light background.
- Opens a full-screen WiFi page with four tabs. The page sits below the
  90px status bar with 24 px header clearance, owns all input (nothing
  falls through to the home grid), and follows the repo overlay checklist:
  `open()`/`close()`, foreground gating, and auto-close on navigation. The
  steering-wheel Home/Star key is a session-bus event, so the daemon tails
  it and writes `home.json`; the page closes on a new counter value:
  - **Status** — WLAN on/off, mode, IP, AP state, SSH state, import button.
  - **Client** — imported SSID, scan results, connect/disconnect, import.
  - **CarPlay AP** — SSID/key, band/channel, AP state, CarPlay transport,
    start/stop/restart.
  - **SSH** — enable/disable, port, imported keys, credentials, command.
- Imports credentials from **`wifi.txt`** at the USB root.
- Imports SSH public keys from **`authorized_keys`** at the USB root.
- Runs `dropbear` with password auth for `car` / `electric` by default.
- Writes status to `/rw_data/wifi/status.json`; the QML page polls it and
  falls back to the local HTTP API.
- Uses the SMACK floor label `_` for the daemon and dropbear (the label the
  Battery app proves works for D-Bus/socket access) and masks the OEM
  `supplicantmanager.path`/`.service` so it cannot respawn and steal the
  wpa_supplicant control socket. Restore unmasks them.

---

## Files

```
wifi_app/
  src/wifi_daemon.c              static C daemon (HTTP + state machine)
  src/Makefile
  scripts/build_artifacts.py     generates patched QML variants
  scripts/build_payload.sh       builds out/usb
  scripts/run_stuff.sh           installer (copy to USB root)
  scripts/run_stuff_restore.sh   restore (rename to run_stuff.sh)
  scripts/run_stuff_diag.sh      read-only diagnostic (rename to run_stuff.sh)
  scripts/make_icons.py          generates the WiFi glyphs
  artifacts/qml/...              WifiPage + patched home-screen QML
  artifacts/assets/wifi/         wifi_icon_n.png, wifi_icon_d.png
  artifacts/systemd/             wifi-daemon.service
  base/                          stock/Hello/Battery QML used to build variants
  wifi.txt.sample                example USB credential file
  out/usb/                       assembled USB tree
  docs/FINDINGS.md               firmware evidence and mode analysis
  docs/PLAN.md                   phased implementation/test plan
  docs/UI-TILES.md               general tile placement/theming guide
  SPEC.md                        technical specification
```

---

## Build

```sh
cd packages/wifi
sh scripts/build_payload.sh
```

This builds `src/wifi_daemon`, regenerates QML artifacts and icons, copies the
static `dropbearmulti` from the earlier owner-access project, and assembles:

```text
out/usb/            copied to the USB stick
out/wifi_app_usb.zip
  payload/
  backup/
  manifest.md5
  run_stuff.sh
  run_stuff_restore.sh
  run_stuff_diag.sh
  README.txt
  README-wifi.txt
```

Copy the **contents** of `out/usb/` to the root of a FAT/exFAT USB stick.

---

## Install

1. Copy `out/usb/*` to the USB root.
2. Optional: put your `wifi.txt` at the USB root (see below).
3. Optional: put `authorized_keys` at the USB root.
4. Plug the stick into the head unit.
5. The `initial_root` failsafe runs `run_stuff.sh` as root within ~8 s.
6. Check `run_stuff_result.txt` on the stick.
7. **Reboot the head unit.** The daemon updates via systemd, but
   `AppHomeScreen` keeps the already-loaded QML in memory: without a
   restart of the UI process the old page layout stays active. The
   installer restarts it via AppLauncher and reports
   `restart=... verified=1`; if it says `REBOOT_REQUIRED`, the reboot is
   mandatory for the new UI.

The installer:

- verifies `manifest.md5`,
- detects the current QML base (stock / Hello / Battery / Hello+Battery /
  previous WiFi),
- backs up the original QML to `/rw_data/wifi/backup/<timestamp>/` and to
  the USB `backup_current/`,
- remounts `/` rw only for the copy,
- installs the daemon, dropbear, icons, QML and service,
- creates the `car` SSH user,
- imports `wifi.txt` and `authorized_keys` if present,
- stops the OEM `supplicantmanager` path/service and removes `/tmp/no_unece`,
- enables and starts `wifi-daemon.service`,
- restarts `AppHomeScreen` through AppLauncher app ID 11, verifies it is
  running again and reports `REBOOT_REQUIRED` in the result file when it
  is not (the QML disk cache under `/root/.cache/qmlcache` is cleared too).

The installer keeps the daemon in **off** mode by default. Open the WiFi tile
and press **On** or **CarPlay AP**.

---

## `wifi.txt`

Put this at the USB root. Key=value is preferred; a plain two-line
`SSID` / `password` file also works.

```ini
ssid=MyHomeWiFi
psk=MyHomePassword

carplay_ssid=IONIQ5-CarPlay
carplay_psk=ChangeMe-CarPlay
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2

ssh=1
ssh_port=2222
```

Optional CarPlay identity overrides:

```ini
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
carplay_id=IONIQ5-260128
carplay_btaddr=00:00:00:00:00:00
```

The app never logs the PSK. `config.conf` is mode `0600`.

---

## SSH

Default credentials, per the build request:

```text
user: car
pass: electric
port: 2222
```

The `car` account is created with UID 0 so it is useful for head-unit
diagnostics. This also means it remains a root shell account when the
firmware's secure-console mode is enabled (`/etc/passwd.secure`); that is an
intentional part of this app. Change the password after install if you keep
password auth.

The firmware keeps `/etc/passwd` in `/tmp/passwd` and regenerates it on every
boot from `/etc/passwd.legacy` or `/etc/passwd.secure`. The installer patches
both templates (and the daemon re-checks at startup), so the `car` account
survives a reboot. SSH defaults to port 2222; if that port is busy, the daemon
falls back to 2223/2224/... and reports the actual port in the UI.

Recommended hardening:

1. Put `authorized_keys` at the USB root and press **Import SSH keys**.
2. Disable password auth later by changing the daemon/dropbear launch.
3. Keep SSH enabled only while WLAN is on.

Connection examples:

```sh
# CarPlay AP mode (default AP address)
ssh car@10.42.0.1 -p 2222

# Client mode: use the IP shown on the Status tab
ssh car@<head-unit-ip> -p 2222
```

The daemon adds a firewall rule for `wlan0` and the selected port and removes
it when SSH stops.

---

## Wireless CarPlay

CarPlay mode does:

1. stop client mode and the OEM `supplicantmanager`,
2. load `cfg80211` / `bcmdhd`,
3. start and keep our own `/usr/sbin/wpa_supplicant -u -Dnl80211 -iwlan0`
   (the OEM manager otherwise leaves no supplicant for ConnMan),
4. start ConnMan AP tethering with the option form this vendor
   `connmanctl` actually parses:
   `connmanctl tether wifi on <ssid> <psk> --hidden no --frequency <freq>
   --maxsta <n> --appinfo 1`
   (the positional form is rejected with `unknown option`; `--appinfo 1`
   marks the AP as CarPlay),
5. add the CarPlay information element:
   `connmanctl tether_carplay_ie wifi on <manufacturer> <model> <name> <id> <btaddr>`,
6. start `carplay_iAP2.service` and `carplay_mdnsd.service` if needed,
7. set `ENABLE_WIRELESS_CARPLAY=true` in `/app/data/vbt/VBTConfig.json` and
   restart `vbtd` only if the file changed.

**Start CarPlay AP** (on the CarPlay AP tab) additionally:

1. enables Bluetooth powered/discoverable/pairable through the vendor `vbtd`
   CommonAPI adapter on the **session bus** (`/tmp/session_appmgr.env`),
2. reads the real Bluetooth address for the CarPlay IE,
3. starts the AP + iAP2/mDNS stack in the same action,
4. shows the address, discoverable/pairable state, and the iPhone steps.

On the iPhone use **Settings > Bluetooth > IONIQ5-CarPlay**, or
**Settings > General > CarPlay > Available Cars**. **Stop pairing** turns
discoverability off again.

If pairing reports “Bluetooth control unavailable”, the exact vendor D-Bus
name differs on that build; the diagnostic stick prints the session-bus names
and the adapter introspection XML so the candidate list can be updated.

### Firmware-verified command syntax

The car binaries were executed with the firmware's own loader to capture
their parser behaviour, not just their help text:

| Command | Verified form |
|---|---|
| AP tether | `connmanctl tether wifi on <ssid> <psk> --hidden no --frequency 5180 --maxsta 2 --appinfo 1` |
| CarPlay IE | `connmanctl tether_carplay_ie wifi on <mfg> <model> <name> <id> <btaddr>` |
| Client key | ConnMan agent flow: `agent on`, `connect <service>`, passphrase on stdin |
| `wpa_cli` | `wpa_cli -p /var/run/wpa_supplicant -i wlan0 ...` |
| `wpa_supplicant` v2.5 | no `-f`, no `-B`; `-u -Dnl80211,wext -i wlan0 -c <conf> -P <pid>` |

This build has **no `connmanctl config --wifi`**; the old command printed
`Error '--wifi': Invalid argument` and connmand then refused the connect
with `Not registered` because no agent was registered. The daemon now runs
the agent flow and redacts the passphrase from captured output.

---

## Diagnostics

Rename `run_stuff_diag.sh` to `run_stuff.sh` on the stick and plug it in. It
writes `wifi_diag.txt` to the stick root and `/rw_data/wifi/diag.txt`.

### Live console

On the 1920 (wide) layout the app content shifts left and a **live console**
appears on the right side of the page:

- **Daemon** / **Supplicant** tabs show the tail of `daemon.log` and
  `wpa_supplicant.log` (2 s refresh, auto-scroll);
- **Dump** writes a full log bundle to the stick immediately.

The console reads through `GET /logs?source=daemon|wpa|ssh`, which returns
only the last 16 KB, so it is cheap for the single-threaded daemon. On the
1280 layout the console is hidden (the content stays centred).

### Log dumps to the stick

The daemon also writes log bundles straight to the stick:

- **Automatically ~15 s after it starts** (so every install leaves a
  bundle behind), and **every 10 minutes while the WiFi/CarPlay apply
  keeps failing** — the failure state lands on the stick without anyone
  pressing anything.
- **On demand**: Status tab → **Save logs** (or `POST /logs/dump`, or
  `wifi_daemon --dump-logs`).

Each dump creates `wifi_logs_<timestamp>/` on the stick with:

```text
daemon.log           the daemon's own log
wpa_supplicant.log   supplicant output (why scan/connect/tether failed)
ssh.log              dropbear output
install.log          the installer's log
status.json          last status snapshot
config.conf          config with psk/carplay_psk redacted
state.txt            live snapshot: ps, connmanctl, wpa_cli status and
                     scan_results, iw, ifconfig, iptables, dmesg tail,
                     ENABLE_WIRELESS_CARPLAY
```

Only the newest 5 bundles are kept. The stick is detected via
`/proc/mounts` under `/media/usb`, `/media` or `/run/media` with no
filesystem-type whitelist (the car's exFAT is Tuxera **texfat**, which the
first version wrongly rejected) and remounted rw if needed. The bundle
includes `/proc/mounts` in `state.txt` so the mount layout is always visible.

It collects:

- `pcb_version`, `optUNECE`, `/tmp/no_unece`
- `wpa_cli` ping/status/networks/scan, `/etc/wpa_supplicant.conf`,
  `wpa_supplicant.log`, `iw list` AP-mode support
- `vbtd`, `/sys/class/bluetooth`, `VBTConfig.json`, the session-bus D-Bus
  names and `org.bluez.Adapter.Adapter` introspection
- `lspci`, `lsmod`, `dmesg` Wi-Fi lines
- `ifconfig`, `iw`, `iwconfig`
- `supplicantmanager` / `connman` / CarPlay service status
- `connmanctl technologies` / `services`
- `VBTConfig.json`, `USBSerial.txt`, BT address
- `iptables` rules
- QML hashes and payload markers
- daemon status/log

---

## Restore

Copy `run_stuff_restore.sh` over `run_stuff.sh` on the stick and plug it in.
It restores the pre-install QML, removes the WiFi QML/assets/service, removes
the `car` user, closes the firewall rule, and restarts `AppHomeScreen`.
Backups under `/rw_data/wifi/backup/` are kept; the live config and imported credentials are removed. The refreshed shared `HelloTile.qml` / `BatteryTile.qml` components and their blue icons are left in place; they are compatible with the Hello/Battery apps and are a theme fix, not WiFi state.

---

## VM validation

The app has been validated against the real firmware in the project's
QEMU/KVM harness (vendor kernel + decrypted 260128 rootfs): vendor Qt 5.4
qmllint over every shipped QML file, the daemon on the guest kernel, the
tether/agent command forms against real connmand, the Home/Star session-bus
watcher, log dumps onto a real Tuxera texfat stick, full install/restore on
the real QML tree, and a real AppHomeScreen launch that loads the patched
qml_eco3.0 tree and draws the patched Advanced page with zero QML errors.
See `docs/FINDINGS.md` §5.2f.

## Known limitations / risks

- Wireless CarPlay is not yet proven on this exact car. Apple authentication,
  the `SupportWirelessCP` persistence flag, or the stock Bluetooth trigger may
  still block it.
- `carplay_iAP2.service` runs `/etc/iap2_mod.sh`, which may switch the front
  USB port to gadget mode. If it is already running for wired CarPlay, the
  app leaves it alone; if not, test USB media afterwards.
- The OEM `supplicantmanager` is masked by the installer; a firmware update
  can reset `/etc/systemd/system` and undo the mask. The daemon's watchdog
  still stops a respawned manager, and the diagnostic reports the mask
  state.
- SSH password auth uses a known default password. Change it.
- The app is a QML page inside `AppHomeScreen`, not a separate AppLauncher
  app ID/process (same limitation as Hello/Battery).
