# tools/update-debug — signatureless-update tooling (WIP)

STATUS: **work in progress.** The goal: install custom firmware as an
*ordinary USB update package* on a rooted car. The stage-1 rootfs bakes in
the AppUpgrade engine stubs and the AppScomoDM stubs; the remaining gates
below are deployed by these stick-based deployers while car validation
continues. See `docs/08-postroot-updates.md` for the full gate inventory,
the VM/car evidence, and the current status table.

## Deployers (newest first)

| File | What |
|---|---|
| `dmclient_gate.sh` | **The real update-button gate: AppDMClient's `checkUSBUpdate()`** verifies the drive's `AppUpgrade` wrapper AND `.lge.upgrade.xml` against `teruten5_pub.key`. Stubs its two verifier entries (`FUN_00221000` @ 1,183,744, `FUN_00221040` @ 1,183,808, `31 C0 C3`). **Prerequisite stage:** also patches AppLauncher's two `.sign` gates (`FUN_00115b50` @ 88,912, `FUN_00112100` @ 73,984, `B8 01 00 00 00 C3`) — the stock launcher refuses to spawn a modified AppDMClient at boot, so the launcher stage runs first and the dmclient stage is skipped if it fails. Re-verifies the installed AppUpgrade/AppScomoDM stubs, verifies/swaps the update drive wrapper + `.ver` row 341142, starts the 45-min watcher, collects `dmclient_gate_diag.tar.gz`. **REBOOT REQUIRED.** VM-validated (A0/A1/A2: the grayed-button reproduction, the stock baseline, and the fixed gate with `APPLAUNCHER_STUB_EXEC_OK`/`PHASE4_OK`). REVERT marker restores both binaries. |
| `button_gate.sh` | The AppScomoDM button-gate deployer (v4 combo stage): verifies/applies the AppScomoDM stubs + the installed four-stub build, verifies/swaps the drive wrapper + `.ver` row 341142 (stock backup + REVERT), starts the watcher, collects `button_gate_diag.tar.gz`. Supersedes `both_copies.sh` (same installed/drive/watcher/diag work plus the AppScomoDM stubs). |
| `applauncher_sigstub.sh` | AppLauncher `.sign`-check stubs alone (startup-group/preload spawns; on-demand launches already skip the check on stock). Respawns AppLauncher in-session and verifies the `com.lge.avn.applauncher` bus name comes back. `_restore.sh` / `_diag.sh` counterparts included. |
| `both_copies.sh` | Legacy (v3-era): installed four-stub verify/patch + drive wrapper swap + watcher + diag, without the AppScomoDM stubs. Kept for history; prefer `button_gate.sh`. |
| `collect_logs.sh` | Stick-side log collector for post-attempt evidence. |

## Constants (all pinned to 260128; see config/firmware_locks.env)

```
AppLauncher stock md5    04156392f77a5c2948772dbd31690d41   (407,720 B)
AppLauncher patched md5  6eeb054566e3bc9a0203ecc01bde04b8
AppDMClient  stock md5   d863bd9c15ae2defcd05553ed46eff46   (2,226,072 B)
AppDMClient  patched md5 49c24ca88ce27b3e642a9f703b0fd1f6
AppUpgrade   four-stub   c17c2655aecef461d2d9ddae15af775b
AppScomoDM   two-stub    45f881b3b08f40e6e07c03988e441341
wrapper patched md5      67a9fa486dbcae63e1a20ceee6eb2d5d
```

## Payload file

The deployers expect the patched wrapper container
(`AppUpgrade.patched.te2`, built by
`scripts/07_build_patched_usb_appupgrade.sh`) next to them on the payload
stick.

## Watcher

The 45-minute watcher (process transitions + `/tmp/AppUpgrade` capture) is
a `nohup` process, **not** a systemd service: a head-unit reboot kills it.
Do not reboot between starting it and the update attempt.

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
All embedded hashes/offsets are pinned to that firmware — see
`config/firmware_locks.env`.
