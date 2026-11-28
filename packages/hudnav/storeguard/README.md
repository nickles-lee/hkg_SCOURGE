# hudnav-storeguard — settings-store guard (host-tested, NOT installed)

## Purpose

Restore the three vendor settings index files in `/Data/mnt-wt/shared/public/` from a
known-good copy at boot, **before** `pas-daemon.service` (PAS) starts. The guard never
contacts the car, never touches `wt.itz` / `cached.itz` (user values) and never touches
any `*-lock` file (the persistence library recreates them). A valid `.itz` is a regular
file, non-empty, whose first three bytes are `KdB`.

## The incident in three lines

On 2026-11-25/26 the head unit hung on the Hyundai logo: PAS's startup cleanup deleted the shared-public index and default data early in every boot.
Without the index AppStandbyClock could not answer Valet Mode over D-Bus; AppNavi dereferenced the empty reply and SIGSEGVed, so the UI never came up.
Restoring the index (with `tweaks_persist` disabled) broke the loop; this guard restores it *before* PAS can wipe it again.

## Files (prototype only — nothing is installed on the car)

- `hudnav-storeguard.sh` — the guard: modes `save`, `check`, `status`.
- `hudnav-storeguard.service` — documentation of the proposed boot unit (Type=oneshot,
  DefaultDependencies=no, After=local-fs.target, Before=pas-daemon.service,
  ExecStart=/bin/sh /rw_data/hudnav-storeguard/hudnav-storeguard.sh check,
  WantedBy=multi-user.target).
- `../tests/test_storeguard.sh` — host test against a fake root through WSL.
- State on the car would live in `/rw_data/hudnav-storeguard/`: `armed` (flag),
  `good/` (the three copies + `SHA256SUMS`), `log` (dated lines, last 200 kept).

## How to arm (only on a car that is known healthy)

1. On a healthy car, run: `sh /rw_data/hudnav-storeguard/hudnav-storeguard.sh save`
   (this refuses to change anything unless all three live files are valid).
2. Verify: `sh /rw_data/hudnav-storeguard/hudnav-storeguard.sh status`
3. Only then: `touch /rw_data/hudnav-storeguard/armed`

Without `armed`, `check` is a no-op (dated "not armed" line on stdout, no files written).

## How to refresh the good copy

Run `save` again **after a confirmed healthy boot** (UI up, store complete). Do this after
any firmware update or any change to the persistence stack, so the copy cannot be stale.
`save` rewrites all three copies atomically (`*.tmp` then `mv`) and rewrites `SHA256SUMS`.

## Install steps (documentation only — DO NOT DO THIS YET)

1. Copy the script to `/rw_data/hudnav-storeguard/hudnav-storeguard.sh` (writable ext4).
2. Run `save`, then `status`, then `touch .../armed` as above; verify the log.
3. Only after OPEN QUESTION 1 is answered: remount `/` rw, copy
   `hudnav-storeguard.service` to `/etc/systemd/system/`, `systemctl daemon-reload`,
   `systemctl enable hudnav-storeguard.service`, remount `/` ro, `sync`.
4. Reboot once and read `/rw_data/hudnav-storeguard/log` plus `systemctl status`.
   All car steps require the owner's explicit approval; no step is part of this task.

## Open questions / doubts

1. **OPEN — is `/Data/mnt-wt` mounted before the guard runs?** FACT (read-only, vendor
   rootfs exported as `ROOTFS`):
   `grep -n mnt-wt $R/etc/fstab*` matched
   `fstab_emmc:17`, `fstab_emmc_fde:17` (`/dev/mmcblk0p12 /Data/mnt-wt ext4 ...`) and
   `fstab_usb:13` (`/dev/sda17 /Data/mnt-wt ext3 ...`);
   `grep -n mnt-wt $R/lib/systemd/system/*.mount` → `(no match)`;
   `$R/etc/fstab` is a symlink to `/tmp/fstab` (generated at boot).
   The shipped `pas-daemon.service` has only `Requires=dbus.service` / `After=dbus.service`.
   INFERENCE: `After=local-fs.target` should be enough for fstab mounts, but because
   `/etc/fstab` is generated, this must be verified read-only on the car before install
   (do not assume the guard sees the real store).
2. **OPEN — stale good copy.** An older known-good copy may lack resources added by a
   later firmware update. After any firmware/update flow, re-run `save` only after a
   confirmed healthy boot; the guard restores exactly what was saved, nothing newer.
3. **OPEN — is restoring before PAS sufficient?** The incident also involved a failed
   PAS install (`pas_conf_configureDataForInstallFolder FAILED err=-1310719`) and a
   recurring `/Data/mnt-wt/first` cleanup. If PAS's install fails for another reason,
   restoring these three files may not break the loop. The guard deliberately does not
   disable `tweaks_persist`, does not delete `first`, and does not touch lock files.
