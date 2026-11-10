# WiFi / Wireless CarPlay / SSH app — Ioniq 5 Gen5W (260128)

A QML home-screen app plus a static C daemon that controls the head unit's
onboard Broadcom WLAN (`wlan0`), starts a wireless CarPlay access point, and
runs a small SSH server.

It follows the same deployment model as the Hello and Battery apps: a USB
`run_stuff.sh` installer, a QML page/tile inside `AppHomeScreen`, a daemon in
`/rw_data/wifi`, and a systemd service. Nothing is flashed; the updater,
bootloader and partitions are untouched.

**Status: host-tested, car validation pending.** The latest audit fixes include
behavioral C tests, host Qt6 mouse hit-tests, and fake-root installer/rollback
tests. They have not been validated on the car's Qt5/Wayland stack or real radio.
Wireless CarPlay still needs the on-car iPhone test. Do not treat a local build
or an installer success line as proof of association or CarPlay readiness.

Client association and DHCP now have one owner: ConnMan. Credentials are sent
only after its agent requests them; there is no direct `wpa_cli` association
fallback. New hidden-network provisioning is not supported and fails explicitly.
Importing credentials or public keys never enables WLAN, a mode, or SSH.

AP startup, status, watchdog, teardown, scanning and SSH now share an actual-state
probe. A lagging `Tethering=False` no longer causes a verified AP to be torn down.
These AP-state fixes are host-tested only; the Nov 9 car bundles predate them.

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
  it and writes `home.json`; the page closes on a new counter value.
  `WifiPage` explicitly synchronizes `visible` and `requested`, so an older
  Tweaks/Battery combo that assigns `wifiPage.visible = true` cannot leave
  Exit ineffective by destroying a QML binding:
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
- Requests and verifies the SMACK exec label `_` for the daemon and dropbear;
  effective runtime permissions still require on-car validation. It masks the OEM
  `supplicantmanager.path`/`.service` so it cannot respawn and steal the
  wpa_supplicant control socket. Restore preserves pre-existing mask state.

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
  scripts/run_stuff_apdiag.sh    opt-in bounded AP/D-Bus header trace
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
static `dropbearmulti` from the package's pinned build (or `SCOURGE_DROPBEAR`
owner-provided static binary), and assembles the output below. Set
`SCOURGE_ROOTFS` to your extracted, pinned 260128 rootfs if it is not staged
at `work/rootfs`; it is also used for the disarmed mDNS experiment artifact.

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

The ZIP is regenerated from that same staging tree. `payload/build-id.txt`,
`payload/build.json`, daemon status and install results identify the build.
The installer requires a newly written status file with the expected identity;
it reports `REBOOT_REQUIRED` rather than claiming the UI restarted when its PID
did not change. SMACK exec-label failures abort and roll back; no global policy
is disabled. QML access labels are best-effort: `onlycap=Privileged` rejects
`security.SMACK64` writes from the `_` USB installer context, so the copied QML
keeps its inherited `_` floor label (readable by AppHomeScreen) and the install
logs a WARNING instead of rolling back on that step. An unexpected inherited
label still aborts. Existing ambiguous legacy account/VBT ownership is not guessed.
New firewall rules live in `WIFI_APP_SSH` / `WIFI_APP_CP` chains, so cancelling
a worker cannot lose track of a chosen SSH port. Legacy untagged firewall rules
are not automatically deleted because they may belong to vendor/owner tooling.

Run `make test` for behavioral, Qt input, packaging and fake-root install/restore
checks (host Qt6 development tools and Node.js are needed for UI tests). Tests
use private paths under `/tmp/opencode`; they do not run vehicle binaries or
modify the host account database. Exported logs are filtered event summaries,
not raw historical credential-bearing transcripts; original logs remain private.

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

### Multiple client networks (priority order)

The client (station) mode supports up to 4 networks in priority order:
the daemon scans once and connects to the **first configured network
that is visible**. The plain `ssid`/`psk` pair is network 1 (highest
priority); `ssid2`/`psk2` … `ssid4`/`psk4` add lower-priority fallbacks
(e.g. phone hotspot first, home network second).

```ini
ssid=iPhone
psk=iphone-hotspot-password
ssid2=MyHomeWiFi
psk2=MyHomePassword

carplay_ssid=IONIQ5-CarPlay
carplay_psk=ChangeMe-CarPlay
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2

ssh_port=2222
```

`client_networks` in `status.json` lists the imported networks in
priority order (never the passphrases); the WiFi page shows the list and
which one is connected.

### CarPlay identity (`carplay_btaddr` is required)

`carplay_btaddr` is the car's **Bluetooth MAC** and is required for the
CarPlay information element: without it the daemon skips the IE with
"Bluetooth address unknown" and the iPhone will not offer wireless
CarPlay. Set the actual Bluetooth MAC of your head unit; no universal
WiFi-to-Bluetooth MAC offset is assumed.

Optional CarPlay identity overrides:

```ini
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
carplay_id=IONIQ5-260128
```

The app never logs the PSK. `config.conf` is mode `0600`.

---

## Optional mdnsd "continue as root" patch (`MDNSD_ROOT_ARM`)

**Experimental, not VM/car-validated; leave disarmed.** The vendor `mdnsd`
drops to `nobody`, and multicast `EPERM` was observed on-car. Whether the
uid drop causes it is unresolved. `scripts/patch_mdnsd.py` generates
`payload/bin/mdnsd.rooted` from owner-provided firmware with SHA256 pins
in `config/firmware_locks.env` and exact call-byte verification. The
five-byte `setuid` call at `0x43c2` is NOPed; no vendor binary is committed.
Retaining root is a diagnostic experiment, not a confirmed CarPlay fix.

The patch is **opt-in**: create the marker file `MDNSD_ROOT_ARM` at the
USB stick root before installing. The installer then:

1. verifies the on-car `/app/bin/mdnsd` matches the stock md5 (or is
   already the patched md5) and the payload copy matches the patched
   md5 — anything else aborts the patch step without touching the
   vendor binary;
2. backs the stock binary up to `/rw_data/wifi/backup/mdnsd.stock`;
3. copies the patched binary over it (`cp -f` keeps the inode and
   therefore the vendor `SMACK64EXEC=App::projection` label);
4. verifies the installed md5 and restarts `carplay_mdnsd.service`.

`run_stuff_restore.sh` restores the stock binary from the backup and
removes the `carplay_mdnsd` restart drop-in. The installer also
installs a systemd drop-in (`Restart=always`) for `carplay_mdnsd`
unconditionally: the vendor unit is `RemainAfterExit=true`, so a dead
mdnsd still reports "active" and nothing restarts it.

---

## Firewall

The daemon's `WIFI_APP_CP`/`WIFI_APP_SSH` chains are hooked into **both**
`INPUT` and `OUTPUT` on `wlan0`. The OUTPUT hook matters: the vendor
`HARDEN_LOUT` chain silently drops all multicast (`224.0.0.0/4`,
`ff00::/8`) unless a rule accepts it first, and the stock rules only do
that for the USB `ncm0` interface — without the OUTPUT hook the phone
never sees the car's `_airplay._tcp` records even with the AP up.

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

### AP readiness and recovery

- Readiness requires a fresh AP role, the **actual** expected SSID from `iw`,
  WEXT or completed supplicant AP status, and a usable IPv4 address. ConnMan's
  configured `TetheringIdentifier` is not broadcast evidence. Conflicting or
  unsupported observations stay unknown. A recorded previous station address
  (`ap.station-ip`) is excluded, including across worker/daemon restarts.
- Startup requires three ready samples and allows up to 30 seconds for an
  interface/address to settle. A working AP is reused even if `Tethering=False`.
  An existing AP with a different SSID is not automatically torn down; use an
  explicit Restart or Stop/Start after changing settings.
- A single 5 GHz → 2.4 GHz fallback is allowed only if an AP was **never
  observed**, absence is confirmed, and teardown completes. A late AP blocks
  fallback teardown. `enabled_then_lost`, pending/unknown state, and permission
  errors do not cause a blind frequency retry. The watchdog backs off; pending
  observations receive a 30-second grace period, stale/unknown probes do not
  trigger radio resets, and recovery reuses an existing AP.
- Explicit Stop, Restart and switching to client mode may stop a real AP, but
  must confirm actual non-AP state and `Tethering=False` before another enable.
  Off is not successful merely because the property is false.
- Active WiFi station services are disconnected through ConnMan during AP
  setup, not just the imported SSID. Cellular, unrelated technologies, saved
  profiles and persistent AutoConnect settings are untouched. Unexpected station
  association during startup is reconciled once; unresolved races fail safely.
- SSH's shared start path rechecks the requested role and address. CarPlay
  requires AP readiness, client mode requires association and matching ConnMan
  address readback. A station IP alone cannot start CarPlay SSH.

Status adds `ap_interface`, `ap_state` (0 unknown, 1 absent, 2 interface-up/pending,
3 ready), and `ap_tether_field` (-1 unknown, 0 false, 1 true). `ap_active` means
actual readiness. `observations_fresh` still governs every cached observation.
Stage-specific messages and sanitized `AP stage=` events retain startup history.

The old logs establish AP enable followed by a local D-Bus disconnect; they do
**not** establish who sent it or prove ConnMan property lag caused teardown.
The new logic removes that failure path without claiming to identify the caller.

### Firmware-verified command syntax

The car binaries were executed with the firmware's own loader to capture
their parser behaviour, not just their help text:

| Command | Verified form |
|---|---|
| AP tether | `connmanctl tether wifi on <ssid> <psk> --hidden no --frequency 5180 --maxsta 2 --appinfo 1` |
| CarPlay IE | `connmanctl tether_carplay_ie wifi on <mfg> <model> <name> <id> <btaddr>` |
| Client key | ConnMan agent flow: `agent on`, `connect <service>`, passphrase on stdin |
| `wpa_cli` | `wpa_cli -p /var/run/wpa_supplicant -i wlan0 ...` |
| `wpa_supplicant` v2.5 | no `-f`, no `-B`; `-u -Dnl80211,wext -i wlan0 -c <conf> -O /var/run/wpa_supplicant`; detached with `setsid`, PID captured by the launcher |

This build has **no `connmanctl config --wifi`**; the old command printed
`Error '--wifi': Invalid argument` and connmand then refused the connect
with `Not registered` because no agent was registered. The daemon now runs
the prompt-driven agent flow without logging raw interactive transcripts.

---

## Diagnostics

Rename `run_stuff_diag.sh` to `run_stuff.sh` on the stick and plug it in. It
writes `wifi_diag.txt` to the stick root and `/rw_data/wifi/diag.txt`.

For an **opt-in AP trace**, use `run_stuff_apdiag.sh` instead. It is not run by
the installer or daemon. It captures 25 seconds of D-Bus **profile headers only**
for supplicant Disconnect/interface changes and WiFi technology property calls,
with best-effort sender → PID/executable/SMACK label/capability resolution.
Before/after snapshots include actual interface state, whitelisted ConnMan and
supplicant fields, build identity, and `get ap_scan`. It writes a new
`wifi_apdiag_<timestamp>_<pid>/` directory, never overwriting historical bundles.
It does **not** enable WLAN or trigger a restart: use the app's Restart button
manually during the capture if desired. Individual commands have deadlines;
the whole script is limited to 90 seconds (plus forced-kill headroom).

Vendor `timeout -k`, D-Bus profile mode, WEXT and `get ap_scan` support still need
car validation. Unsupported profile mode never falls back to raw message bodies.
Short-lived bus names may disappear before resolution; missing evidence is not
proof of an absent process or denial. No production `ap_scan` change is made.
Treat these exports as sensitive network/process metadata even without keys.

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
state.txt            build ID and monotonic timestamp; ps, connmanctl,
                      wpa_cli status/get ap_scan/scan_results, iw/iwconfig,
                      ifconfig, iptables, dmesg tail,
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

Earlier revisions were exercised against the real firmware in the project's
QEMU/KVM harness (vendor kernel + decrypted 260128 rootfs): vendor Qt 5.4
qmllint over every shipped QML file, the daemon on the guest kernel, the
tether/agent command forms against real connmand, the Home/Star session-bus
watcher, log dumps onto a real Tuxera texfat stick, full install/restore on
the real QML tree, and a real AppHomeScreen launch that loads the patched
qml_eco3.0 tree and draws the patched Advanced page with zero QML errors.
See `docs/FINDINGS.md` §5.2f.
Those historical runs do not validate the current audit fixes. The current
revision has host behavior/Qt6/fake-root coverage only; the vendor Qt5, real
radio, live SMACK policy and iPhone transitions must be tested again.

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
