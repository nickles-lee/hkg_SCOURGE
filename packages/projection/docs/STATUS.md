# Projection+ — status (2026-11-27)

**WIP. Car-validated on the owner's AU 260319 car only (see Car log): install, tile, status page. Restore is VM-tested only. Settings changes from the page and Android Auto status are not yet car-tested.**

| Part | State |
|---|---|
| QML page + tile (`artifacts/qml/...`), icon (`scripts/make_icons.py`) | Host-rendered only (`design/shots`). |
| `src/projection_daemon.c` (127.0.0.1:8092, contract §4) | Host-tested: `tests/test_daemon.py`, 57 checks pass. |
| hudnavd side (settings.conf, runtime switches, status file) | In `packages/hudnav` (WIP, car-proven on one AU 260319 car only); host-tested; not deployed here. |
| Tile registration in the home-screen layouts (`scripts/build_artifacts.py` → `build/`) | Separate work item; see its own notes. |
| Installer `scripts/run_stuff.sh`, uninstaller `scripts/run_stuff_restore.sh`, unit `artifacts/systemd/projection-daemon.service`, staging `make payload` | **Car-run once on the owner's car (see Car log); otherwise offline-tested.** `tests/test_installer.sh` (fake root, stubbed systemctl/dbus-send/chsmack): 58 checks on fake layouts + 4 against the real `build/` output on a copy of the 2026-11-27 car snapshot, all pass (WSL dash; not BusyBox ash). |
| compose.sh integration | Not started. |

Installer details (WIP):
- Two entry points: USB failsafe (`USB_DIR`) and SSH (`sh run_stuff.sh --from-dir DIR`). `--dry-run`
  (or a `DRY_RUN` file on the stick) runs every gate and reports the plan, writing nothing on the car.
- Gates before any write: root; SMACK label `_` (same rule as Tweaks; an SSH root shell is `_` per the
  2026-11-27 car readback); every payload file's sha256 against `manifest.txt`; every layout file on the
  car equals the manifest base sha256 (or is already the installed payload with a restore point);
  new files absent or recorded as ours. Any mismatch refuses with nothing written.
- Each file is backed up and recorded in `/rw_data/projection/backup/<stamp>/index` *before* it is
  written (`latest` → active backup). A failure rolls back; an interruption (power cut) is undone by
  the restore script, which the installer copies to `/rw_data/projection/tools/` before its first write.
- Install order: page/components, icons, daemon, unit, then layouts last.
- Restarts only `projection-daemon.service` and AppHomeScreen (AppLauncher D-Bus, app id 11, and only
  when QML changed); clears `/root/.cache/AppHomeScreen`. Never writes `/rw_data/hudnav`.
- Upgrading from one Projection+ build to another with different layouts: restore first, then install
  (the installer refuses otherwise).
- Restore refuses, writing nothing, when a recorded file was changed by something else after install
  (e.g. Tweaks/Wi-Fi reinstalled over a layout); `--force` puts the pre-install originals back anyway.
  Order with Tweaks/Wi-Fi reinstalls is not handled yet: restore Projection+ before reinstalling them.
- Not yet car-checked: the daemon's running SMACK label (`smack_check`), the AppHomeScreen restart with
  the new layouts, the daemon log (`/rw_data/projection/daemon.log`, unbounded like Tweaks').

Behaviour notes:
- Switching HUD Nav / Android Auto / CarPlay off takes effect within ~1 s (hudnavd goes quiet);
  full stock behaviour (no overlays) returns at the next boot. Switching the master back on while
  hudnavd is not running shows "starts with the car".
- Imperial units are car-unverified (feet below 161 m, metric above).

## Before public release (owner requirement, 2026-11-27)
- Installer must be versatile: build the patch from each car's own current home-screen files
  (any combination of Tweaks/Wi-Fi/Battery/Hello/none), not from the owner's car snapshot.
- `compose.sh` integration and order independence: installing/reinstalling/restoring Tweaks or Wi-Fi
  after Projection+ must not silently drop or break the Projection+ tile (and vice versa).
- Test matrix like `packages/tweaks/Makefile` (stock, each combination) plus a VM boot.

## Car log
- 2026-11-27 ~12:10 UTC: first install on the owner's car over SSH (`run_stuff.sh --from-dir`), status=OK,
  SMACK `_` verified on the running daemon. Live AppHomeScreen restart left CarPlay unable to start until a
  head-unit reboot -> installer now defaults to "reboot required" (no live restart; `--restart-ui` opt-in).
- 2026-11-27 ~12:21 UTC: update (tile without extra panel, "no route" wording), 2 files, then reboot.
  Owner confirmed: tile matches Tweaks/Reboot, page shows live status (no route / turns from CarPlay),
  CarPlay starts normally, HUD guidance unchanged. **Car-validated on the owner's car only** (Advanced
  home, this exact package combination); settings changes from the page and Android Auto status not yet
  exercised on the car. Backup of the stock-patched home screen: /rw_data/projection/backup/20261127T121025Z.
- Gotcha: the car's Wi-Fi drops after a few hundred KB of continuous upload (needs a head-unit reboot);
  upload in 16 KB pieces with pauses, and only changed files.
