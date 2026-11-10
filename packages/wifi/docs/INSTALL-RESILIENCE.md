# Installer and packaging regression fixes (260128)

Status: host fault-injection and generated-QML tests; **car validation pending**.
The mDNS uid-retention experiment remains disarmed and unvalidated.

## What the evening readback establishes

Battery and WiFi both aborted at the first PNG asset copy. WiFi then logged
rollback failures across root-filesystem destinations; Tweaks installed later.
The captured root mount was read-only and dmesg showed repeated root remounts.
However, the readback omitted the combo log containing raw `cp` stderr.
Thus a concurrent read-only remount is a strong explanation, **not a proven
errno or proven attribution to `smack-setup-recovery.service`**. Do not disable
vendor security/recovery services on that inference.

## Changes

- A manifest-covered shared I/O helper samples root writability before writes,
  retries boundedly after remount/copy errors, and verifies copied bytes.
  Existing destination inodes are never unlinked just to retry a copy.
- Missing/unwritable icons warn and increment `asset_warnings`; they do not
  roll back otherwise valid applications. Critical copies remain fatal.
- WiFi rollback restores regular files in place and restores symlinks without
  first deleting their targets. Backups remain available after failure.
- EXIT/signal cleanup restores an initially read-only root; initially writable
  roots remain writable. No firmware partitions or vendor services are altered
  to work around the suspected race.
- The canonical composer builds from `packages/` and validates the actual final
  Tweaks QML. Every Battery/WiFi/Tweaks/Reboot page instance needs its own
  placement, visibility and size bindings, with unique ordinals.
- The flat wrapper embeds the archive SHA256, changing its failsafe MD5 gate
  whenever the archive changes. It exports current per-stage results, install
  logs and the combo log, including raw copy errors.
- Lower-priority client association and health checks now match the connected
  SSID rather than requiring the primary SSID or selecting a merely visible
  higher-priority service. All four client passwords are redacted from logs.

## Offline validation

Run `make -C packages/wifi test`, the Battery installer tests and Tweaks tests.
Shared regression tests live in `packages/common/test_*.py`. They cover mount
flapping, EROFS during a copy, corrupt copies, persistent icon errors, bounded
failure on permanently read-only roots, rollback and symlink preservation,
full-first-page allocation at both panel widths, real combo dispatch, reverse
restore, false stage success, `REBOOT_REQUIRED` acceptance, stale-result
cleanup, beeps, and mismatched archive rejection. The behavioral allocator run
in `test_layout.py` additionally needs `node` when available (skipped with an
explicit message otherwise; the static gate always runs).

## Next parked-car check

Use one app stick, no firmware/update stick. Expect an OK result (possibly with
icon warnings), unique tiles in the first free app-grid cells (page two when
page one is full), fresh WiFi `build_id`, and no duplicate daemon. Keep the
result files, `combo.log`, and current `*_install.log`. If a write fails again,
those logs should now distinguish EROFS from SMACK/DAC, storage, or other causes.
