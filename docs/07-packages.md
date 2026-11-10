# 07 — The packages (Battery, WiFi, Tweaks)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU), theme
`qml_eco3.0`. All three packages deploy through the failsafe (docs/06);
none of them flashes firmware, touches the updater, the bootloader, or
partitions. **Status is marked per feature — these are research-quality
packages under active development, not finished products.**

## Status matrix (2026-11, refreshed)

| Package | Feature | Status |
|---|---|---|
| Tweaks | **Safety-disclaimer auto-accept** (1–10 s delay, touch cancels, confirms last driver) | **Working reliably** |
| Tweaks | Cluster + infotainment amber/night-mode fixes (follow the vehicle's blue-light setting, Hyundai's own gamma table) | WIP — almost working |
| Tweaks | Automatic DST (IANA zone → `APP_SETTING_CLOCK_DAYLIGHTSAVETIME`) | WIP — almost working |
| Tweaks | Hyundai OTA block (vendor `setOFFlineMode`, keeps USB updates) | WIP — off by default |
| Tweaks | **Reboot app** — fourth tile + stock Yes/No modal; clean `systemctl reboot` with a micom `SetETCCPUWDTReset` fallback after 20 s; driving/update guards; SMACK `_` hard gate | WIP — host-tested; VM/car pending |
| Battery | Tile + full-screen overlay (temp, SoC, heater state/power) | WIP — almost working; installer retries a failed copy after unlinking the destination and verifies the running daemon matches the payload (`daemon_fresh`; force-restarts a stale one) |
| Battery | Preheat engage — full WiCAN sequence mimicry: `0x0C7` burst at 40 ms pacing + `0x4E8`/`0x4CC` cluster countdown; blocked at ≥ 21 °C min-cell temp; a no-status start assumes active after the 70 s timeout; exhausted retries end with a cleanup stop burst | WIP — safety-interlocked |
| Battery | Cluster preheat countdown (WiCAN `0x4E8`+`0x4CC` layout) + standalone "Cluster display test" toggle | WIP — replaces the earlier synthetic `0x4ED` |
| Battery | Charge door open/close with the exact factory frames (open = `0x4A2` all-FF + `DF`, close = `0x4C5` all-zero, from decompiled TmsRemoteApp; gear/Park gate disabled while `0x036` is unreliable; observed-cable veto, unknown cable advisory) | WIP — frames dbus-1.8-validated in the VM |
| Battery | `0x25D` battery-conditioning status (`EV_BatteryConditioning`) and raw `0x036` gear frame with both factory byte decodes, shown in the footer | WIP — settles the gear byte offset on-car |
| Battery | Steering-wheel Home/Star closes the overlay (session-bus monitor → `home_seq`); micom send health + daemon version in the footer | dbus 1.8.20-validated in the VM; pending on-car |
| Battery | Privileged daemon/helpers run in the installer's own SMACK context (`_` in the normal USB flow): `SMACK64EXEC` hard readback gate, copied `dbus-send`/`dbus-monitor`, live `/proc/<pid>/attr/current` check, `smack_*` fields in `/status` | WIP — mirrors the WiFi `_`-daemon pattern; `SmackProcessLabel` is unusable on this kernel |
| WiFi | WLAN client + AP modes on the onboard Broadcom radio; USB detection scans real mount points (no fstype whitelist — the car's exFAT reports as `texfat`, docs/02); ConnMan agent-flow client connect; command forms verified against the shipped binaries | WIP — VM-validated on the vendor kernel/Qt; on-car retest pending |
| WiFi | SSH server (static dropbear, `car`/`electric`, port 2222) + `wifi.txt`/`authorized_keys` import; daemon/dropbear carry the SMACK floor label | WIP — VM-validated; on-car retest pending |
| WiFi | Wireless CarPlay AP (ConnMan tether with `--appinfo 1` + CarPlay IE + iAP2/mdns); Home/Star closes the overlay via the session-bus `home_seq` watcher | VM-validated up to the radio; needs on-car iPhone validation |
| All | Install/restore/diag through the failsafe, base detection, coexistence | Proven pattern (combo v4 flow: install order battery → wifi → tweaks, reverse restore) |

Per-package detail: `packages/<pkg>/README.md` + `SPEC.md` + `docs/`.

## What each package installs

| | Battery | WiFi | Tweaks |
|---|---|---|---|
| QML | `BatteryTile` + `BatteryPage` overlay (z 2000, y 90/h 630) | `WifiTile` + `WifiPage` (4 tabs) | `TweaksTile` + `TweaksPage`; `RebootTile` + `RebootPage` modal (z 2000, y 90/h 630); AppStandbyClock disclaimer patches |
| Daemon | `battery_daemon` (static; I-CAN `0x2FC`/`0x326`, micom D-Bus commands incl. the WiCAN preheat sequence; HTTP :8088 + `status.json` mirror; `home_seq` watcher for steering-wheel Home/Star; version + send-health in `/status`) | `wifi_daemon` (static; WLAN/AP state machine; HTTP :8090) | `tweaks_daemon` (static; gamma/projection/DST/OTA/reboot; HTTP :8091; `home_seq` watcher) + `tweaks_persist` (vendor-dynamic) + `wrandr` wrapper |
| Service | `battery-daemon.service` | `wifi-daemon.service` | `tweaks-daemon.service` |
| Extras | icons (generated) | static `dropbearmulti`, `car` user (UID 0), firewall rule, `wifi.txt`/`authorized_keys` import | IANA tzdata (fetched at build), `zoneinfo` tree |

Tiles are placed by the shared first-empty-slot allocator with a fixed
install ordinal (Battery=1, WiFi=2, Tweaks=3, Reboot=4) so they never fight
over a page; installers detect the current base (stock / each other's previous
installs) and pick the matching QML variant. Reboot ships inside the Tweaks
package rather than as a fourth package, so selecting Tweaks also installs
the Reboot tile and daemon endpoint.

## Build + install

```sh
# one package
sh packages/battery/scripts/build_payload.sh   # -> packages/battery/out/usb

# or the combo stick (asks which packages, builds, assembles MODE= master)
sh packages/compose.sh /path/to/stick [battery,wifi,tweaks]
```

Copy the contents to the root of a FAT/exFAT stick, plug it in, read
`run_stuff_result.txt`. Restore: `MODE=restore` (combo) or rename
`run_stuff_restore.sh` over `run_stuff.sh` (single package).

## Validating without the car

Three cheap gates catch most package regressions before a stick reaches the
vehicle:

1. **Vendor QML parser.** The head unit ships its own Qt `/usr/bin/qt5/qmllint`;
   run it over every generated QML file (the page, tiles, and each patched
   host variant). A modern host Qt `qmlscene` accepts constructs the car's
   Qt 5.4 does not.
2. **Vendor kernel/binary checks.** The daemons are static and run on the
   vendor kernel in any firmware VM/rootfs for one-shot and HTTP smoke tests
   and failure-path soaks. For CLI forms (`connmanctl`, `wpa_cli`,
   `wpa_supplicant`), run the car's own binaries with their loader
   (`ld-2.25.so` + firmware libraries) or in the VM and check the parser
   output, not just `--help`. This is how the tether option form, the
   no-`config --wifi` agent flow, and the `int32` Home/Star parsing were
   verified without a car.
3. **Install/restore round-trip in the VM.** Run `run_stuff.sh` against the
   real `/app/share/AppHomeScreen` tree: base detection, SMACK label
   preservation, the passwd/shadow templates, unit masking, and the
   no-systemd manual-start fallback all get exercised. Then
   `run_stuff_restore.sh` must return the stock hashes exactly.

## Binary toolchain policy

The head unit runs kernel 4.1.27 with **glibc 2.25** — a dynamically linked
binary built on a modern host dies on-car. Policy:

| Binary | Build | Gate |
|---|---|---|
| `dropbearmulti` | **static musl** (pinned Alpine container or musl-gcc; `packages/wifi/scripts/build_dropbear.sh`) | `file` must report statically linked; sha256 recorded; `--help` smoke test |
| battery/wifi/tweaks daemons | static via host gcc (validated on the vendor kernel in the VM; no NSS-dependent calls by design) | same static gate |
| `tweaks_persist` | **deliberately dynamic** against vendor glibc 2.25 + `libpersistence_client_library.so.7` (it must call the vendor PCL); links against the extracted vendor rootfs (`SCOURGE_ROOTFS`) | vendor-interpreter check; pinned to 260128 |

## Networking notes (WiFi package)

- Default SSH: user `car` (UID 0), password `electric`, port 2222 (falls
  back 2223+ if busy). **Change the password after first login**; import
  `authorized_keys` and prefer key-only.
- `wifi.txt` at the stick root: `ssid=`, `psk=`, `carplay_ssid=`,
  `carplay_psk=`, `carplay_band/channel`, `ssh=`, `ssh_port=`. Sample:
  `packages/wifi/wifi.txt.sample`. The PSK is never logged; config is 0600.
- The installer stops and **masks** the OEM `supplicantmanager.path`/`.service`
  (it is `Restart=always` and otherwise reclaims the wpa_supplicant control
  socket), removes `/tmp/no_unece`, and the watchdog holds `wlan0`. Restore
  unmasks and restarts the path unit.
- `wifi_daemon` and `dropbearmulti` carry the SMACK floor label `_`
  (`chsmack -e _`) — the label the Battery daemon proved for D-Bus/socket
  access. `Privileged` has no ConnMan/wpa_supplicant rule and produces
  "Permission denied" on tethering and a dead control socket.
- Tethering uses the option form
  `connmanctl tether wifi on <ssid> <psk> --hidden no --frequency <MHz>
  --maxsta <n> --appinfo 1`. The positional form is rejected with
  `unknown option` on this vendor build.
- Client connect uses the ConnMan **agent** flow (`agent on` → `connect
  <service>` → passphrase on stdin) — this `connmanctl` has no
  `config --wifi`. Every `wpa_cli` call passes `-p /var/run/wpa_supplicant`
  explicitly.

## Deployment models (where a package's UI can live)

Read `docs/ui/deployment-modes.md` — the full A–L catalog with recipes.
Summary for package authors:

- **Mode A — tile + overlay** (all three packages): proven on-car.
- **Mode B — card/swipe page**: proven on-car (earlier Battery design).
- **Mode H — basic-home tile**: proven (Hello payload).
- **Mode C — official separate-process app: blocked by default.** AppLauncher
  RSA-verifies every spawned app's `.sign` ELF section (SHA256 with the
  section zeroed, then RSA vs `/tmp/app_normal.key`); the private key is
  not in the firmware. On-demand `request.launch` spawns skip the check on
  the stock binary; only startup-group/preload spawns verify. A sigstub
  patch now ships as part of the signatureless-update chain
  (`tools/update-debug/applauncher_sigstub.sh` for already-rooted cars,
  baked into the build-4 stage-1 rootfs — VM-validated, car validation
  pending, docs/08); until that chain is car-validated, custom apps stay
  on Modes A/B. The alternative route to a separate-process app is
  **Mode G** (own Wayland process outside AppLauncher, where no `.sign`
  check applies — high confidence per the research, with a hard safety
  rule: never crash weston, its death triggers a MICOM watchdog reset of
  the whole unit).

## Vehicle/version/region specificity

Everything above is pinned to 260128 EU (`config/firmware_locks.env`):
QML base hashes, `.qm` keys, micom command IDs, CAN IDs (`0x2FC`, `0x326`,
`0x036`), wrandr behavior, PCL version. On another firmware or vehicle,
re-derive per docs/09 before installing anything.
