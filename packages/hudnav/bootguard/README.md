# HUD Nav boot guard (design, offline / host-tested only)

Nothing here is installed on the car. This folder contains the text of two systemd units and one
POSIX `sh` script. Tests are in `../tests/test_bootguard.sh` and run against a fake root.

## What it does

Two modes of the same script, `hudnav-bootguard.sh`:

- `early` runs before the UI starts. If the guard is armed, it counts unconfirmed boots. A boot is
  unconfirmed when `pending` is left behind by a previous `early` run (the `ok` run never deleted
  it). When the counter reaches 2 it performs the **same revert as `recovery/run_stuff.sh`**:
  remove `/etc/ld.so.preload`, restore `AppLauncher.sh` and `carplay_iAP2.service` to their stock
  sha256 pins (sed/grep candidate, else the `*.stock` backup with a matching hash, else leave the
  file untouched), `sync`, remount `/` read-only, and rename `/rw_data/hudnav` to
  `/rw_data/hudnav.off` (or `hudnav.off.<epoch>` if that already exists). It never deletes anything
  else and never reboots. It then writes `rolled_back`, disarms the guard and resets `fails` to 0.
- `ok` runs after the UI starts. For up to 300 s it polls once a second. The boot is confirmed only
  if `pidof AppHomeScreen` returns the **same pid for 60 consecutive seconds** *and*
  `dmesg | grep -c 'AppNavi.*segfault'` is 0. On success it deletes `pending` and writes `fails=0`.
  On timeout it leaves `pending` for the next `early` run.

When not armed, `early` writes a dated `not armed` line to stdout and touches no file; `ok` exits
silently. The stock pins default to the hashes in `docs/hudnav/CP_PROTOCOL.md` and can be
overridden with `HUDNAV_PIN_AL` / `HUDNAV_PIN_CP`.

## Script copy location on the car

The script is copied to **`/rw_data/hudnav-bootguard/hudnav-bootguard.sh`** (writable partition; the
state folder is `/rw_data/hudnav-bootguard/`). Both unit files point at that path with
`/bin/sh`. Nothing under `/etc` or `/app` is added by this design.

## State files (all under `/rw_data/hudnav-bootguard/`)

| File | Meaning |
|---|---|
| `armed` | Guard active. Its presence is the only on/off switch. |
| `pending` | Current boot has not been confirmed by `ok` yet. |
| `fails` | Consecutive unconfirmed boots (missing/empty = 0). |
| `rolled_back` | Written once when the automatic revert fired; contains date and reason. |
| `log` | Dated lines from both modes; rotated to the last 200 lines on every write. |

## Arm / disarm

- Arm: `touch /rw_data/hudnav-bootguard/armed`
- Disarm: `rm -f /rw_data/hudnav-bootguard/armed` (optionally also remove `pending` and reset
  `fails` if you intend to start counting from zero).

Arm only for a session where the HUD kit is installed; the guard reverts the four project changes
after two boots that never reached the home screen.

## How the units would be installed (documentation only - NOT done here)

1. `mount -o remount,rw /`
2. Copy `hudnav-bootguard.service` and `hudnav-bootok.service` to `/etc/systemd/system/`.
3. Copy `hudnav-bootguard.sh` to `/rw_data/hudnav-bootguard/hudnav-bootguard.sh`.
4. `mkdir -p /etc/systemd/system/multi-user.target.wants`
5. Symlink both units from `/etc/systemd/system/multi-user.target.wants/` to
   `/etc/systemd/system/hudnav-bootguard.service` and
   `/etc/systemd/system/hudnav-bootok.service`.
6. `sync`; `mount -o remount,ro /`; `systemctl daemon-reload` (and only then arm the guard).

No step above has been run against the car or a VM.

## Host test hooks (test-only, not used on the car)

- `HUDNAV_ROOT=/fake/root` prefixes every absolute path the script touches.
- `HUDNAV_PATH_PREPEND=/dir` puts fake `pidof` / `dmesg` first in `PATH`.
- `HUDNAV_GUARD_SCALE=N` divides both 300 s and 60 s by N (tests use N=20).
- `HUDNAV_PIN_AL` / `HUDNAV_PIN_CP` replace the two stock sha256 pins.

## OPEN QUESTIONS

- ApplicationLauncher.service shows as 'disabled' on the car and may be started by /lib/systemd/esinit rather than by systemd ordering; whether Before= actually runs us before the UI must be verified in the VM/car.
- `ok` waits 60 s of a stable `AppHomeScreen` pid; the incident shows the manager can keep respawning apps for ~127 s before giving up, so a late respawn after confirmation is not covered. VM/car verification needed.
- Whether `dmesg` still contains the AppNavi segfault line after a fresh boot (buffer persistence) is unverified; if it does not, the segfault check may not catch a repeat of the incident.
- The revert only covers the four project changes; `tweaks_persist` (the suspected cause of the 2026-11-26 hang) is deliberately **not** touched.
- `AppManager_info.txt`/`AppManager_deregister.txt` are rotated or truncated at some size (unknown); the shrink handling treats a shrink as a reset.

## v2 (2026-11-26)

Two review gaps against the car's real behaviour are closed here (host-tested only; nothing installed):

1. **Ignition-off background wakes are not counted.** `early` records the current line counts of
   `/log_data/backtrace/AppManager_info.txt` (`send_boot_type status[` lines -> `boots_seen`) and
   `/log_data/backtrace/AppManager_deregister.txt` (all lines -> `dereg_seen`) in the state folder,
   whenever armed and not rolling back. `ok` waits for a NEW `send_boot_type status[` line and reads N
   from the newest such line. N=10 or 32, or any `ign3[1]` line after that newest line, means a UI boot.
   N=43 with no `ign3[1]` after it by the end of the window means a background wake: `pending` is
   deleted, `fails` is unchanged and the log says `background wake: not counted`. If no new line
   appears in the whole window the class is UNKNOWN: `pending` is kept and the log says
   `boot class unknown: NOT confirmed`. Both car logs are read through `tr -cd '[:print:]\n'` because
   they can contain non-printable bytes; if a file shrank below its baseline, that baseline is reset
   to 0 (see open questions).
2. **Health criterion is stronger.** A UI boot is confirmed only if ALL hold at the same poll:
   `AppHomeScreen` pid unchanged for `need` seconds, `AppNavi` pid unchanged for `need` seconds,
   `dmesg | grep -c 'AppNavi.*segfault'` is 0, `/Data/mnt-wt/shared/public/resource-table-cfg.itz`
   exists and its first 3 bytes (`head -c 3`) are `KdB`, and no NEW deregistration line (beyond
   `dereg_seen`) in `AppManager_deregister.txt` contains `a_nSigNum[11]`, `a_nSigNum[6]` or
   `a_nSigNum[5]` (crash signals; signal 15 = a normal stop and is not counted). On confirm: `pending`
   deleted, `fails=0`, log `boot confirmed`. On timeout: one short log line names the criteria that
   failed (`home_pid`, `navi_pid`, `segfault`, `index`, `dereg`) and `pending` is left in place.

Still not deployable: no VM/car run and no install. The open questions below remain open.
