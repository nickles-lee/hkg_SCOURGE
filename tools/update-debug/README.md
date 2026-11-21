# tools/update-debug — signatureless-update deployers (build 5)

STATUS: **confirmed on the car (2026-11-15).** A fresh stage-1 build-5
flash already bakes in the eleven stubs + SMACK flag and needs no deployer.
These stick-based deployers remain for cars rooted with an earlier build.
See `docs/08-postroot-updates.md` and `docs/17-usb-update-confirmed.md`.

## Deployers (newest first)

| File | What |
|---|---|
| `dmclient_gate.sh` | **The real update-button gate: AppDMClient's `checkUSBUpdate()`** verifies the drive's `AppUpgrade` wrapper AND `.lge.upgrade.xml` against `teruten5_pub.key`. Stubs its two verifier entries (`FUN_00221000` @ 1,183,744, `FUN_00221040` @ 1,183,808, `31 C0 C3`). **Prerequisite stage:** also patches AppLauncher's two `.sign` gates (`FUN_00115b50` @ 88,912, `FUN_00112100` @ 73,984, `B8 01 00 00 00 C3`) — the stock launcher refuses to spawn a modified AppDMClient at boot, so the launcher stage runs first and the dmclient stage is skipped if it fails. Re-verifies the installed AppUpgrade (five engine stubs)/AppScomoDM (two stubs), verifies/swaps the update drive wrapper + `.ver` row 341142, starts the 45-min watcher, collects `dmclient_gate_diag.tar.gz`. **REBOOT REQUIRED.** VM-validated (A0/A1/A2: the grayed-button reproduction, the stock baseline, and the fixed gate with `APPLAUNCHER_STUB_EXEC_OK`/`PHASE4_OK`). REVERT marker restores both binaries. |
| `button_gate.sh` | The AppScomoDM button-gate deployer (v4 combo stage): verifies/applies the AppScomoDM stubs + the installed five-engine-stub AppUpgrade (SCOMO four + FUMO; eleven stubs total across the four binaries), verifies/swaps the drive wrapper + `.ver` row 341142 (stock backup + REVERT), starts the watcher, collects `button_gate_diag.tar.gz`. Supersedes `both_copies.sh` (same installed/drive/watcher/diag work plus the AppScomoDM stubs). |
| `applauncher_sigstub.sh` | AppLauncher `.sign`-check stubs alone (startup-group/preload spawns; on-demand launches already skip the check on stock). Respawns AppLauncher in-session and verifies the `com.lge.avn.applauncher` bus name comes back. `_restore.sh` / `_diag.sh` counterparts included. |
| `both_copies.sh` | Legacy (v3-era): installed five-engine-stub verify/patch + drive wrapper swap + watcher + diag, without the AppScomoDM stubs. Kept for history; prefer `button_gate.sh`. |
| `collect_logs.sh` | Stick-side log collector for post-attempt evidence. |

## Constants (all pinned to 260128; see config/firmware_locks.env)

```
AppLauncher stock md5    04156392f77a5c2948772dbd31690d41   (407,720 B)
AppLauncher patched md5  6eeb054566e3bc9a0203ecc01bde04b8
AppDMClient  stock md5   d863bd9c15ae2defcd05553ed46eff46   (2,226,072 B)
AppDMClient  patched md5 49c24ca88ce27b3e642a9f703b0fd1f6
AppUpgrade   five-engine 55e522e37e85b9a7c599f9f17fc1a580
AppScomoDM   two-stub    45f881b3b08f40e6e07c03988e441341
wrapper patched md5      f730490b9a9f6d3499b9ba59070d63e8
```

## Payload file

The deployers expect the patched wrapper container
(`AppUpgrade.patched.te2`, built by
`scripts/07_build_patched_usb_appupgrade.sh`) next to them on the payload
stick.

## Watcher

The 45-minute (at least) watcher (process transitions + `/tmp/AppUpgrade` capture) is
a `nohup` process, **not** a systemd service: a head-unit reboot kills it.
Do not reboot between starting it and the update attempt.

It logs only process transitions (`+pid`/`-pid` with the command line), not a
full `ps -ef` snapshot per change, and caps `update_watch.log` at 2 MiB
(`UPDATE_WATCH_MAX_BYTES`, 200-byte lines via `UPDATE_WATCH_MAX_LINE`,
32 KB kept tail via `UPDATE_WATCH_KEEP_BYTES`). The
earlier watcher appended a full snapshot every 2 seconds and reached ~100 MB
in one window, which filled a 1.2 GB `/rw_data`. Delete an old oversized log
before deploying; the new watcher trims it on start. If the rootfs lacks
`awk` or `comm`, the watcher degrades to a full snapshot per transition
(still capped) instead of logging nothing. The bounded watcher
is offline-tested only; car validation is pending.

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
All embedded hashes/offsets are pinned to that firmware — see
`config/firmware_locks.env`.
