# 06 — The run_stuff failsafe and package authoring

The failsafe is the "standing door" the TOCTOU opens: a tiny systemd
service that, on every boot, watches the head unit's USB mounts and runs a
`run_stuff.sh` script from any stick whose content changed — as root. All
SCOURGE packages (docs/07) deploy through it.

## The contract (what a package author can rely on)

- `run_stuff.sh` at the **root** of a FAT/exFAT stick is executed with
  `/bin/sh` as **root** within ~8 s of a content change.
- The environment variable `USB_DIR` names the stick's mount directory
  (typically `/media/usb/<N>/<VOLID>/`).
- Execution is **md5-gated per file**: the loader records the md5 of each
  script it runs and will not re-run an unchanged script. To re-run,
  change the file (even one byte — the combo master uses a `MODE=` line
  for exactly this).
- Output convention: write `run_stuff_result.txt` (or
  `<pkg>_result.txt`) back to the stick; also copy to
  `/rw_data/<pkg>/result.txt` for post-hoc reading.
- The loader re-applies the updater stubs at startup
  (`/etc/initial_root/updater_guard.sh`), so a package that restores a
  stock updater does not silently un-root the box.

## The loader (payload/rootfs/etc/initial_root/run_stuff_loader.sh)

Scans, in order: `/media/usb/*/*/run_stuff.sh`, `/media/usb/*/run_stuff.sh`,
`/media/usb/run_stuff.sh`, `/media/*/run_stuff.sh`,
`/run/media/*/run_stuff.sh`, `/run/media/*/*/run_stuff.sh`,
`/mnt/*/run_stuff.sh` — then raw-mounts unmounted `/dev/sd?1` devices
read-only and checks them too. First new script wins; runs with `USB_DIR`
set; logs to `/rw_data/initial_root.log`.

## Writing a package (the house rules)

A package stick carries, at its root:

```
run_stuff.sh            installer
run_stuff_restore.sh    restore (user renames it over run_stuff.sh)
run_stuff_diag.sh       read-only diagnostic (same rename trick)
payload/                everything the installer copies to the car
backup/                 the stock reference files the installer guards on
manifest.md5            md5 manifest of payload/ + backup/
README.txt              human instructions
```

Rules that every SCOURGE package follows (learned the hard way):

0. **Match the car's real fstype when scanning `/proc/mounts`.** The
   unit's exFAT is the Tuxera driver (`texfat.ko`) and reports as
   **`texfat`**, not `exfat` — accept `vfat|texfat|exfat|ntfs|msdos`, or
   better, match by content (marker files) rather than fstype. A fstype
   miss fails silently (docs/02, docs/11).
1. **Verify before write.** Check `manifest.md5`
   (`cd "$USB_DIR" && md5sum -c manifest.md5`) before touching the car.
2. **Guard on stock hashes.** Before patching any stock file, verify its
   hash matches the 260128 reference; abort with a clear message if the
   car's file is neither stock nor a known previous install state.
3. **Back up first.** Originals go to `/rw_data/<pkg>/backup/<stamp>/`
   and to the stick's `backup_current/`.
4. **Remount minimally.** Remount `/` rw only for the copy, restore the
   previous ro/rw state after.
5. **Preserve modes/owner/SMACK labels** on copy (`cp -p`); the car's
   LSM expects them.
6. **Restart the owning process properly** — via the AppLauncher D-Bus
   pattern (`terminate <id>` + `launch <id> "<name>" "<path>"`, bus
   address from `/tmp/session_appmgr.env`), e.g. app ID 11 for
   AppHomeScreen. Never `kill -9` the UI.
7. **Be idempotent.** A second run detects the installed state and
   no-ops (or upgrades) rather than double-patching.
8. **Never fake success.** Every stage writes explicit `status=OK|FAIL`
   lines; a failed gate aborts before the next stage.
9. **Firmware updates revert `/app`.** Say so in the README: re-run the
   installer after any firmware update.
10. **UI work must follow the styleguide** — `docs/ui/styleguide.md`,
    `deployment-modes.md`, `ux-patterns.md`, and the overlay checklist
    before any QML is written.

## The sample

`payload/run_stuff.sample.sh` is a complete, safe example: environment
proof (uid, SMACK label, mounts), a file copied each way between stick and
`/rw_data`, optional stock-updater restore (with `stock/KEEP_STOCK` to
leave it stock), and result files. Start from it.

## Combo sticks

`packages/compose.sh` assembles several packages onto one stick with a
master `run_stuff.sh` (MODE= line) that runs the per-package installers in
dependency-safe order — battery → wifi → tweaks, since each installer
detects the current QML base — and a reverse-order restore mode. See
docs/07.
