# Persistent Android Auto -> HUD (installed on the car 2026-11-26 11:55 UTC)

Status: car-validated for one full reboot (boot guard "boot confirmed", HUD arrow + distance working) and
one earlier session-mode drive. Roundabouts / slight / sharp / ramps not yet confirmed.

## Pieces
| Unit (`/etc/systemd/system`) | Script | Role |
|---|---|---|
| `hudnav-storeguard.service` | `/rw_data/hudnav-storeguard/hudnav-storeguard.sh check` | before pas-daemon: restore the shared settings index from the saved good copy if missing/damaged (the 2026-11-26 incident) |
| `hudnav-bootguard.service` | `/rw_data/hudnav-bootguard/hudnav-bootguard.sh early` | count unconfirmed UI boots; two in a row -> rollback (kit renamed, preload/launcher/carplay reverted) |
| `hudnav-bootok.service` | same script, `ok` | confirm a UI boot healthy (pids stable, no crashes, index ok); background wakes not counted |
| `hudnav-premount.service` (new) | `/rw_data/hudnav/bin/hudnav-premount.sh` | oneshot, `After=local-fs.target`, `Before=carplay_iAP2.service ApplicationLauncher.service hudnav-aa.service`: verify stock hashes + kit checksums, bind-mount the DT_NEEDED copies, create the CP library gate, restart nothing; fail closed (REFUSE -> stock boot) |
| `hudnav-aa.service` | `/rw_data/hudnav/bin/hudnav-aa-boot.sh start` | start hudnavd (tx) after the session bus exists; detect the premount overlays and never restart the vendor services |

Kit: `/rw_data/hudnav/bin` (SHA256SUMS covers AppProjection.aa, aa_shim.so, hudnavd, hudnav-run.sh,
micom-send, micom-send-tp, CANService, aa-session.sh, hudnav-aa-boot.sh; SHA256SUMS.cp covers
iAP2Service-2.0.cp and cp_rg.so). The owner must also copy `hudnav-premount.sh` and
`hudnav-premount.service` into that folder before running install-persist.sh.
Switches: `/rw_data/hudnav/enabled` (master), `/rw_data/hudnav/aa-nav-enable` (AA overlay),
`/rw_data/hudnav/cp-enabled` (CarPlay overlay). `/rw_data/hudnav/cp-rg-enable` is the CP library
gate created by the premount unit and removed by uninstall-persist.sh.

## Install / undo
- Install: `sh install-persist.sh` (report) then `--yes`, from the kit folder. Tests: `tests/test_persist.sh`.
- Disable without removing: `rm /rw_data/hudnav/enabled` (next boot is stock).
- Remove: `sh /rw_data/hudnav/bin/uninstall-persist.sh --yes`.
- No SSH: the v4 USB recovery stick renames the kit (feature off next boot); the boot guard does the same
  by itself after two failed UI boots.

## Boot restart fix (2026-11-28, offline + host-tested; CAR UNVERIFIED)
Previously the overlays were installed by `aa-session.sh start --yes` / `cp-session.sh start --yes`
~10-12 s into boot, restarting AppProjection and carplay_iAP2.service. A phone already plugged in at
boot was claimed by the original service and the restarted one never saw it (AA also hit "not available
on user agreement screen"). Now `hudnav-premount.service` does the mounts before the vendor services
start; the boot script only starts hudnavd and verifies the mounts. Manual `aa-session.sh` /
`cp-session.sh` start/stop are unchanged for interactive use (they refuse `start` while premount's
bind mount is active, which is expected: use `stop --yes` first to switch back to a session overlay).
Car-tested on the owner's AU 260319 car (2026-11-28): premount active at 4.3 s, before
carplay_iAP2.service (4.5 s) and ApplicationLauncher.service (4.5 s); both shims mapped in the first
process; iPhone and Android phone left plugged in through a reboot connected without a replug. The
old restart path also caused an intermittent "CarPlay never connects" (AppProjection kept a dead
iAP2 socket, ENOTSOCK in iAp2AppCarplayOnOff); premount removes that race.
