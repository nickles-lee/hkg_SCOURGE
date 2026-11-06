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
| Battery | Tile + full-screen overlay (temp, SoC, heater state/power) | WIP — almost working; installer retries a failed copy after unlinking the destination and verifies the running daemon matches the payload (`daemon_fresh`; force-restarts a stale one) |
| Battery | Preheat engage — full WiCAN sequence mimicry: `0x0C7` burst at 40 ms pacing + `0x4E8`/`0x4CC` cluster countdown; blocked at ≥ 21 °C min-cell temp; a no-status start assumes active after the 70 s timeout; exhausted retries end with a cleanup stop burst | WIP — safety-interlocked |
| Battery | Cluster preheat countdown (WiCAN `0x4E8`+`0x4CC` layout) + standalone "Cluster display test" toggle | WIP — replaces the earlier synthetic `0x4ED` |
| Battery | Charge door open/close (micom `0x4A2`; Park gate; observed-cable veto, unknown cable advisory) | WIP |
| Battery | Steering-wheel Home/Star closes the overlay (session-bus monitor → `home_seq`); micom send health + daemon version in the footer | dbus 1.8.20-validated in the VM; pending on-car |
| WiFi | WLAN client + AP modes on the onboard Broadcom radio; USB detection scans real mount points (no fstype whitelist — the car's exFAT reports as `texfat`, docs/02) | WIP — built on verified firmware components |
| WiFi | SSH server (static dropbear, `car`/`electric`, port 2222) + `wifi.txt`/`authorized_keys` import from the stick | WIP — prior on-car tests |
| WiFi | Wireless CarPlay AP (ConnMan tether + CarPlay IE + iAP2/mdns) | Needs on-car iPhone validation |
| All | Install/restore/diag through the failsafe, base detection, coexistence | Proven pattern (combo v4 flow: install order battery → wifi → tweaks, reverse restore) |

Per-package detail: `packages/<pkg>/README.md` + `SPEC.md` + `docs/`.

## What each package installs

| | Battery | WiFi | Tweaks |
|---|---|---|---|
| QML | `BatteryTile` + `BatteryPage` overlay (z 2000, y 90/h 630) | `WifiTile` + `WifiPage` (4 tabs) | `TweaksTile` + `TweaksPage`; AppStandbyClock disclaimer patches |
| Daemon | `battery_daemon` (static; I-CAN `0x2FC`/`0x326`, micom D-Bus commands incl. the WiCAN preheat sequence; HTTP :8088 + `status.json` mirror; `home_seq` watcher for steering-wheel Home/Star; version + send-health in `/status`) | `wifi_daemon` (static; WLAN/AP state machine; HTTP :8090) | `tweaks_daemon` (static; gamma/projection/DST/OTA; HTTP :8091) + `tweaks_persist` (vendor-dynamic) + `wrandr` wrapper |
| Service | `battery-daemon.service` | `wifi-daemon.service` | `tweaks-daemon.service` |
| Extras | icons (generated) | static `dropbearmulti`, `car` user (UID 0), firewall rule, `wifi.txt`/`authorized_keys` import | IANA tzdata (fetched at build), `zoneinfo` tree |

Tiles are placed by the shared first-empty-slot allocator with a fixed
install ordinal (Battery=1, WiFi=2, Tweaks=3) so they never fight over a
page; installers detect the current base (stock / each other's previous
installs) and pick the matching QML variant.

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
- The installer stops the OEM `supplicantmanager` path and removes
  `/tmp/no_unece`; a watchdog holds `wlan0`.

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
