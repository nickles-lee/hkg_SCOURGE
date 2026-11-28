# Projection+

Home-screen app for phone-projection improvements on the Gen5W head unit (AU 260319). First feature:
HUD Nav settings (Android Auto / CarPlay turn-by-turn on the head-up display), per phone source.

**Status: WIP.** Car-validated on one AU Ioniq 5 (260319): install, tile, live status page, CarPlay unaffected. Restore is VM-tested only. Changing settings from the page and Android Auto status are not yet car-tested. See `docs/STATUS.md`.

- Product brief: `PRODUCT.md`; design: `design/APP_SPEC.md`, `design/BUILD_NOTES.md`, screenshots in `design/shots/` (local only, not committed: renders include the stock home screen)
- Backend contract (app ↔ daemon ↔ hudnavd): `design/BACKEND_CONTRACT.md`.
- Daemon: `make -C src` (static), test: `python3 tests/test_daemon.py`.
- Icons: `python3 scripts/make_icons.py`.

Requires the HUD Nav kit in `/rw_data/hudnav` (installed separately); without it the page shows
"not available" and changes nothing.

## Install / uninstall (WIP: car-tested on one AU 260319 car)

Build (host):

```sh
python3 scripts/build_artifacts.py   # patched layouts -> build/qml_eco3.0 + build/manifest.txt
make payload                         # static daemon + stage build/usb/
make test                            # daemon + installer tests (fake root)
```

`build/usb/` holds `run_stuff.sh`, `run_stuff_restore.sh`, `manifest.txt` (sha256 of every file,
plus the car base sha256 each layout replaces) and `payload/`.

Over SSH, as root, after uploading `build/usb/` to `/tmp/projection` on the car:

```sh
sh /tmp/projection/run_stuff.sh --from-dir /tmp/projection --dry-run   # gates + plan, writes nothing
sh /tmp/projection/run_stuff.sh --from-dir /tmp/projection             # install
sh /rw_data/projection/tools/run_stuff_restore.sh                      # uninstall / undo
```

From a USB stick: copy `build/usb/`'s contents to the stick root; the failsafe runs `run_stuff.sh`.
An empty `DRY_RUN` file on the stick makes it a dry run. To uninstall, rename `run_stuff_restore.sh`
to `run_stuff.sh`. The result is in `run_stuff_result.txt` (and `/rw_data/projection/result.txt`).

What the installer does:
- refuses unless it runs as root with SMACK label `_`, every payload file matches `manifest.txt`, and
  every home-screen layout on the car matches the base sha256 it was built from (the car's current,
  Tweaks/Wi-Fi-patched files) — on any mismatch nothing is written;
- backs up each file before replacing it (`/rw_data/projection/backup/<stamp>/`, `latest`), installs the
  page, components, icons, `/rw_data/projection/projection_daemon` (SMACK64EXEC `_`) and
  `projection-daemon.service`, enables and restarts that unit, clears the QML cache and restarts
  AppHomeScreen; a failure rolls back;
- is idempotent: a second run changes nothing and does not restart AppHomeScreen;
- never writes `/rw_data/hudnav` (the HUD Nav kit).

The restore script puts every backed-up file back byte-for-byte, removes everything Projection+
added (including the unit), and restarts AppHomeScreen. It also undoes an interrupted install. It
refuses, writing nothing, if something else changed one of those files after install
(`--force` overrides). Re-run the installer after a firmware update (it reverts `/app`).
Reinstall order with Tweaks/Wi-Fi is not handled yet: restore Projection+ before reinstalling them,
then install Projection+ again (see `docs/STATUS.md`).
