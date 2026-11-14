# Tweaks app — Ioniq 5 Gen5W (260128)

The Tweaks package ships **two home-screen apps**: the Tweaks controls and a
fourth **Reboot** overlay, plus the shared daemon.

**Night mode:**

- **Instrument Cluster Night Mode Fix** — keeps the cluster's amber blue-light
  tint active while projection is displayed, even though the vendor flips the
  blue-light setting off when CarPlay/Android Auto takes over.
- **Infotainment Night Mode Fix** — applies the same Hyundai warmth to the
  CarPlay / Android Auto screen.
- **Deep red night mode** (optional toggle, off by default) — while night
  mode is effective, the front/projection display gets a fixed deep red
  (`128:32:16`) via the wrandr brightness channel and the instrument cluster
  is driven to blue-light level 9 (the vendor maximum) through the same
  CAN 0x063F signal the stock setting uses.

Both follow the vehicle's existing **Setup > Display > Blue Light Filter**
setting and use Hyundai's own gamma table. There is no new warmth slider.
The daemon must run on the SMACK floor label `_` (the installer stamps it
that way); `Privileged` has no rule for the system bus or micom service, which
silently blocks every cluster CAN write while the front display still works.

**Startup, time and updates:**

- **Safety disclaimer auto-accept** — accepts the boot disclaimer after a
  selectable 1–10 second delay. Any touch before the timeout cancels it for
  that boot. It confirms the last-used driver.
- **Automatic DST** — flips `APP_SETTING_CLOCK_DAYLIGHTSAVETIME` from a
  selectable IANA timezone. The build downloads the latest IANA tzdata and
  bundles a compiled `zoneinfo` tree.
- **Hyundai OTA block** — uses the vendor `AppDMClient.setOFFlineMode()` hook,
  persists through `/update/offline_status`, and leaves local USB updates
  available. Off by default.

**Reboot app (fourth overlay):**

- Adds a **Reboot** tile in the next free home-screen slot (after Tweaks) and
  a stock-style Yes/No confirmation modal.
- **Yes** asks the daemon to run `/bin/systemctl reboot --no-block`.
- If the clean reboot has not taken effect after 20 seconds, the daemon
  `sync()`s and sends the vendor micom hard-reset call
  (`SetETCCPUWDTReset array:byte:0x00`) — the same call `/etc/weston-crash.sh`
  uses. This is the software equivalent of the pinhole reset when systemd is
  stuck.
- Refuses while `AppHomeModel.drivingRestriction` is true, while an update
  process (`AppUpgrade`/`AppScomoDM`) is running, while a system popup is
  showing, or when the daemon is unreachable. No/outside/Home closes it
  without rebooting.
- The modal follows the stock popup rules: 70 % dim, 100 ms InOutQuad fade,
  `APP_HOME_POPUP_*` geometry, stock buttons, beep on release, and the
  steering-wheel Home/Star key closes it via the daemon `home_seq` counter.

---

## Why this exists

The cluster's amber tint is driven by the micom service over CAN **0x063F**
(status bit 26, level bit 12) — the instrument cluster is its own ECU-driven
display, not a head-unit CRTC. The vendor flips that setting off when CarPlay
or Android Auto takes over, which loses the cluster tint as collateral damage.

The Infotainment side is separate: projection renders on its own IVI surface
(`mfxsink`, surface `4010`), so the native UI tint does not cover it. The
vendor tints the front display through
`wrandr --crtc=1 --brightness=<r:g:b>`.

The app keeps the cluster asserted through the same micom CAN signals the
vendor uses, and applies the same Hyundai warmth to the front display while
projection is foreground. Deep red night mode asserts the vendor maximum
cluster level (9) and a deep-red front brightness.

The micom service only accepts CAN sends from a process whose executable
basename is whitelisted (`AppHomeScreen`, `TmsRemoteApp`,
`micommanager_userspace`, `CANService`); a plain `dbus-send` child is dropped
silently. The daemon therefore sends through the helper the Battery app ships:
`/rw_data/battery/bin/micom-send`, which runs a `dbus-send` copy named
`CANService`. If Battery is not installed — or its helper predates the
`fixed <idhi> <idlo> <start> <size> <data>` mode — the Tweaks installer
creates its own fixed-capable helper under `/rw_data/tweaks/bin/`. The active
path is `can_service` in the config and `can_source` in `status.json`. The
installer result reports `can_source=battery|local|config|dbus` (`config` is
a custom fixed-capable path already present in `config.conf`). See
`packages/battery/docs/MICOM_WHITELIST.md` for the decompiled evidence.

## Firmware evidence

- `AppSetup` — `APP_SETTING_DISP_GAMMA_R_G_B`,
  `APP_SETTING_DISP_BLUE_LIGHT_STATUS`,
  `APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL`
- Hyundai gamma table — `128:120:112` … `128:112:96`, neutral `128:128:128`
- `ApplicationManager` — `wrandr --crtc=1 --gamma=... --brightness=...`
- `AppProjection` — `... ! mfxsink display=2 ivi-surface-id=4010 ...`
- `weston_1920.ini` — HDMI1 `1920x720`, HDMI2 `1280x720`

## Files

```text
tweaks_app/
  src/tweaks_daemon.c          static daemon (HTTP + display logic)
  src/wrandr_wrapper.c         static wrandr interposer
  src/tweaks_persist.c         dynamic PCL helper (vehicle settings)
  scripts/build_artifacts.py   QML variants + AppStandbyClock patches
  scripts/fetch_tzdata.py      downloads/compiles the latest IANA tzdata
  scripts/build_payload.sh     builds out/usb
  scripts/build_target_helper.sh builds tweaks_persist against vendor glibc
  scripts/run_stuff.sh         installer
  scripts/run_stuff_restore.sh restore
  scripts/run_stuff_diag.sh    read-only diagnostic
  artifacts/qml/...            TweaksPage/Tile/Switch/Button/ZonePicker,
                               RebootPage/RebootTile + patched QML
  artifacts/qml/AppStandbyClock/ disclaimer auto-accept patches
  artifacts/systemd/           tweaks-daemon.service
  tests/                       daemon, QML, wrapper and installer/restore tests
  base/                        stock/Hello/Battery/WiFi QML bases
  out/usb/                     assembled USB tree (includes tzdata)
```

## Home-screen integration

Tweaks and Reboot are two tiles. Tweaks opens the full-screen
`TweaksPage.qml`; Reboot opens `RebootPage.qml`, which immediately shows the
confirmation modal.

- **Advanced view:** both tiles use the shared first-empty-slot allocator,
  after existing Hello/Battery/WiFi tiles. Reboot takes the slot after Tweaks;
  if the grid is full it stays hidden rather than overlapping.
- **Wide view:** both tiles are appended after the existing Hello/WiFi tiles.
- **All Menus / Setup:** both tiles appear in the shared menu list.

The pages start below the 90 px system status bar and fit the 630 px content
area. Tweaks keeps its existing full-page click guard. Reboot owns its
visibility (`requested && !isAtOtherApp`), has a full-page input shield, and
closes on Back, Home/Star (`home_seq`), app switch, screen/page changes, and
`requestHomeByPP`. Host QML only calls `open()`/`close()` and never assigns
`visible`.

This is a QML tile/page inside `AppHomeScreen`, the same deployment model as
the Hello and WiFi apps — not a separate AppLauncher app ID or GUI process.

## Build

```sh
cd ~/ioniq/tweaks_app
sh scripts/build_payload.sh
```

This builds the static daemon/wrapper, builds `tweaks_persist` against the
vendor 260128 rootfs, generates the QML variants/icons, downloads and compiles
the latest IANA tzdata with host `zic`, and assembles:

```text
out/usb/
  payload/
  backup/
  manifest.md5
  run_stuff.sh
  run_stuff_restore.sh
  run_stuff_diag.sh
  README.txt
```

### Copying tzdata to a FAT/exFAT stick

The compiled `zoneinfo` tree contains hardlinks: `zic` stores aliases such as
`US/Eastern` as hardlinks to `America/New_York`. FAT/exFAT cannot represent
hardlinks, so `cp -a` can fail with:

    cp: cannot create hard link '.../US/Eastern' to '.../America/New_York': Operation not permitted

Copy the tree with ordinary-file semantics:

    cp -R out/usb/. /path/to/stick/
    # or: rsync -r out/usb/ /path/to/stick/

Do not use `cp -a` or `rsync -aH` for this tree. The manifest lists every zone
path separately, so the materialized tree still verifies with
`md5sum -c manifest.md5`. The combo builder uses `cp -R` for this reason.

Copy the **contents** of `out/usb/` to the root of a FAT/exFAT stick.

## Install

Plug the stick into the head unit. The `initial_root` failsafe runs
`run_stuff.sh` as root. Check `run_stuff_result.txt`.

The installer:

- detects the current QML base (stock / Hello / Battery / WiFi and combinations),
- backs up the current QML, `AppStandbyClock` QML and `/usr/bin/wrandr`,
- installs the daemon, persistence helper, wrapper, service, QML, icons,
  `zoneinfo`, `zones.txt` and the disclaimer auto-accept patches,
- starts `tweaks-daemon.service`,
- restarts `AppHomeScreen`. The disclaimer patch applies on the next boot.

Firmware updates overwrite the rootfs changes; rerun the sideload to reinstall.

## Restore

Copy `run_stuff_restore.sh` over `run_stuff.sh` on the stick and plug it in.
It stops the daemon, restores the original QML and `wrandr`, removes the app
files and tzdata, and releases the OTA offline flag if Tweaks set it. Reboot
afterwards to clear the in-memory AppStandbyClock disclaimer patch.

## Diagnostics

Copy `run_stuff_diag.sh` over `run_stuff.sh` and plug it in. It writes
`tweaks_diag.txt` with:

- `wrandr` output and DRM connector status
- daemon config/status and desired cluster gamma
- `tweaks_persist read` output, including DST/timezone keys
- disclaimer runtime files, selected zone, tzdata version and compiled zone file
- `/update/offline_status`, `AppDMClient`/`dmengine` state and `GetOTAEnableStatus`
- service/process state
- QML markers for both `AppHomeScreen` and `AppStandbyClock`
- 8 seconds of `com.lge.avn.appmanager.info` D-Bus signals (session bus;
  the address is in `/tmp/session_appmgr`)

## Daemon API

Bound to `127.0.0.1:8091`.

| Method | Path | Meaning |
|---|---|---|
| GET | `/status` | state JSON |
| GET | `/health` | liveness + build id/config version/SMACK label |
| GET | `/logs` | daemon log tail |
| POST | `/set?cluster_fix=0\|1` | cluster toggle |
| POST | `/set?infotainment_fix=0\|1` | projection toggle |
| POST | `/set?deep_red=0\|1` | deep red night mode (front 128:32:16, cluster level 9) |
| POST | `/set?disclaimer_auto=0\|1` | safety disclaimer auto-accept |
| POST | `/set?disclaimer_timeout_ms=1000..10000` | auto-accept delay |
| POST | `/set?auto_dst=0\|1` | automatic DST |
| POST | `/set?dst_zone=Europe%2FAmsterdam` | IANA zone used for DST |
| POST | `/set?ota_block=0\|1` | block Hyundai OTA |
| POST | `/apply` | re-apply immediately |
| POST | `/restore` | turn both night fixes off and release front/cluster overrides |
| POST | `/identify?crtc=N&seconds=3` | briefly tint the front display red (only `main_crtc`) |
| POST | `/reboot` | clean reboot, then the micom WDT reset fallback; requires `X-Reboot-App: 1` |

## Config

`/rw_data/tweaks/config.conf`:

```ini
config_version=2
cluster_fix=1
infotainment_fix=1
cluster_crtc=2
main_crtc=1
warmth_override=0     # 0 = follow vehicle
force_filter_on=0
deep_red=0
projection_always=0
projection_app_id=0   # 0 = learn from D-Bus
poll_ms=1000
disclaimer_auto=1
disclaimer_timeout_ms=2000
auto_dst=1
dst_zone=Europe/Amsterdam
dst_check_ms=60000
ota_block=0
wrandr_path=/usr/bin/wrandr.real
persist_helper=/rw_data/tweaks/tweaks_persist
tzdata_dir=/rw_data/tweaks/zoneinfo
zones_file=/rw_data/tweaks/zones.txt
tzdata_version_file=/rw_data/tweaks/tzdata_version
offline_status_path=/update/offline_status
session_bus_file=/tmp/session_appmgr
dbus_send_path=/usr/bin/dbus-send
can_service=/rw_data/battery/bin/micom-send
xdg_runtime_dir=/var/run/user/root
reboot_command=/bin/systemctl
reboot_wdt_ms=20000
```

`/reboot` is deliberately not exposed as a `/set` key: the QML must send the
custom header, and the daemon refuses while an update process is running.

## Tests

```sh
make test
```

Covers the daemon HTTP/state machine, disclaimer files and clamping, DST
winter/summer evaluation with a fake PCL helper, OTA `setOFFlineMode` calls,
the Reboot endpoint (CSRF header, duplicate request, update guard, clean
command failure, micom WDT fallback, Home/Star counter), QML structure, the
wrandr wrapper, and installer/restore for stock, Hello, Battery, WiFi and
combined bases. The installer tests also exercise the positive and negative
SMACK label gates.

## Troubleshooting

- **Daemon online but “vehicle persistence unavailable”** — `tweaks_persist`
  could not initialise the vendor PCL. Run the diagnostic script and check
  `tweaks_persist read`. On the car PCL needs the system D-Bus and
  `pers_admin_svc` running.
- **Infotainment fix reports “main display not applied”** — the front display
  rejected the brightness write. Use `Identify front` to confirm the output,
  or change `main_crtc` in the config (defaults to the vendor front CRTC 1;
  old 0-based configs are migrated on the next install).
- **Deep red shows no cluster change** — it asserts CAN level 9 whenever the
  cluster fix is on and night mode is effective (filter on, or projection
  active). `blu_status_sent`/`blu_level_sent` mean the frame was **queued to
  the helper**, not that the micom acknowledged it (the factory senders use
  no-reply, so the helper exits 0 by design). Check `can_source` first, then
  the `micom BLU` lines in `failures.log`.
- **`status.json` says `"can_source":"dbus"`** — the micom-send helper is
  missing, not executable, or is an old raw-only Battery wrapper without the
  `fixed` mode. The daemon then falls back to a plain `dbus-send` child that
  the vendor micom silently drops. Reinstall Tweaks, or refresh Battery
  (which provides `/rw_data/battery/bin/micom-send`); the installer result
  records `can_service=` and `can_source=`.
- **Front tint works but the instrument cluster never changes** — the most
  common causes are the daemon's SMACK label and the micom sender whitelist.
  The label must be `_` (floor), not `Privileged`: only the floor label has
  access to `App::micommanager` and the system-bus socket. Check `smack` in
  `/status`/`/health` (must be `_`) and `smack_check=ok`/`smack=_` in
  `run_stuff_result.txt`. The CAN send must come from the whitelisted
  `CANService` basename (`"can_source":"service"`); the diagnostic script
  prints the helper paths and whether each supports `fixed`, the running
  label, and a no-CAN-write micom D-Bus probe (Introspect may activate the
  service). Reinstall if either check fails.
- **Reboot says “service unavailable”** — the daemon is not reachable on
  `127.0.0.1:8091`. Check `systemctl status tweaks-daemon.service`, the
  `smack_check` field in `run_stuff_result.txt`, and the daemon log. The
  Reboot app cannot work if the daemon is not running.
- **Reboot refused during an update** — `AppUpgrade`/`AppScomoDM` is running.
  Let the update finish; the daemon will not hard-reset during a flash.
- **Clean reboot did not start** — after 20 s the daemon sends the vendor
  micom `SetETCCPUWDTReset` call. `reboot_wdt_sent`/`reboot_wdt_ok` in
  `/status` and `wdt_call.out` record the fallback. If both paths fail, the
  pinhole remains the only reset.
- **Testing projection manually** — write `1` or `0` to
  `/rw_data/tweaks/projection_state`; the daemon prefers that file over the
  D-Bus detector.
- **Auto-DST error** — check `dst_state`, `dst_raw`, `dst_expected` and
  `dst_error` in `status.json`. Missing/empty values and the stock Setup
  `Custom` value (`2`) are replaced with the expected `0`/`1` flag. A
  `PCL write rejected (rc=…)` error means the persistence layer refused the
  write for every candidate app name; `daemon.log` has the per-attempt PCL
  return codes, and the diagnostic script collects the resource table needed
  to find the real owner. The selected file must exist under
  `/rw_data/tweaks/zoneinfo/`; rebuild and reinstall to refresh tzdata.
- **DST write failures are also collected automatically** — every failed
  write is appended to `/rw_data/tweaks/failures.log` and, while a USB stick
  is mounted, to `<stick>/tweaks_failures.log`. Entries logged while no
  stick was inserted are flushed to the stick when one appears, and a
  `write ok after failures` line marks the recovery. The helper caches the
  app name whose write succeeded (`/rw_data/tweaks/pcl_write_app`), so
  subsequent writes use that candidate first instead of cycling.
- **OTA block pending** — `/update/offline_status` is already set to `1`; the
  D-Bus call is retried until `AppDMClient` is available. Check
  `tweaks_diag.txt` for `GetOTAEnableStatus`.
- **Disclaimer still manual** — the `AppStandbyClock` patch applies on the
  next boot. Check the QML markers in `tweaks_diag.txt`.
- **`cp: cannot create hard link ... Operation not permitted`** — the
  destination is FAT/exFAT and the compiled tzdata tree contains hardlinks.
  Copy with `cp -R` (or `rsync -r`), not `cp -a`; see "Copying tzdata to a
  FAT/exFAT stick" above.

## Safety

- No firmware flash, updater, bootloader or partition changes.
- Original QML (including `AppStandbyClock`) and `wrandr` are backed up before
  modification.
- Night-mode fixes can be turned off from the UI; `/restore` returns stock
  gamma. The restore script also clears the OTA offline flag.
- Auto-DST only changes the vehicle's DST switch; the normal timezone remains
  controlled by Setup.
- OTA block uses the vendor `setOFFlineMode` state and persists through
  `/update/offline_status`. Local USB updates are not disabled. Restore only
  clears the flag when `ota_block_owned` shows Tweaks set it.
- The daemon only rewrites `status.json` and `cluster_gamma` when their content
  changes, to avoid wearing `/rw_data` flash.
- The vehicle's existing blue-light setting is the source of truth.
