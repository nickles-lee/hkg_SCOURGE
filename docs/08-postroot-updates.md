# 08 — Custom firmware updates after root (WORK IN PROGRESS)

STATUS: **work in progress.** The goal: after the stage-1 flash, install
custom firmware as an *ordinary USB update package* — no rig, no race —
because the modified rootfs bakes in updater stubs and the remaining gates
are stubbed by stick-based deployers. Offline gates are green and the
button-gate chain is **VM-validated**; **car validation of the full update
is still pending.** Treat this document as a runbook plus an honest status
record.

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU).

## The gate inventory

A custom update must pass several signature gates on 260128. The map has
been revised twice from on-car and VM evidence; as of **build 4, all four
patch sets are baked into the stage-1 rootfs** (ten stubs), re-applied at
boot by the guard, and the deployers remain for cars already rooted with
an earlier build:

| # | Gate | Where | SCOURGE's answer | State |
|---|---|---|---|---|
| 1 | Engine validators (TE2 metadata parse + final hash/RSA verify) | `/app/bin/AppUpgrade` — four entry points | four `31 C0 C3` stubs **baked into the stage-1 rootfs** (re-applied at boot by the guard) | Car-validated |
| 2 | First button-gate discovery | `/app/bin/AppScomoDM` — two verifier entries (`FUN_001e3430` @ 930,864, `FUN_001e3470` @ 930,928) | two `31 C0 C3` stubs **baked into the stage-1 rootfs** | Baked + offline-verified; car validation pending |
| 3 | **The real update-button gate: AppDMClient's `checkUSBUpdate()`** — verifies the drive's `AppUpgrade` wrapper AND `.lge.upgrade.xml` against `/app/share/AppUpgrade/teruten5_pub.key` and controls the Update-button enable/disable; a re-encrypted wrapper fails it, so the button stays grayed | `/app/bin/AppDMClient` — `FUN_00221000` @ 1,183,744 + `FUN_00221040` @ 1,183,808 | two `31 C0 C3` stubs **baked into the stage-1 rootfs** (build 4); `dmclient_gate` deployer for already-rooted cars; **reboot required** | **VM-validated** (A0/A1/A2); car pending |
| 4 | **Launcher prerequisite for gate 3:** the stock AppLauncher `.sign`-verifies every startup-group/preload spawn against `/tmp/app_normal.key` and **refuses to spawn a modified AppDMClient at boot** | `/app/bin/AppLauncher` — `FUN_00115b50` @ 88,912 (readSection gate) + `FUN_00112100` @ 73,984 (navi CheckSign) | two `B8 01 00 00 00 C3` stubs (true-on-success) **baked into the stage-1 rootfs** (build 4); deployer stage ordering guarantees the launcher is patched before the dmclient stage | **VM-validated**; car pending |
| 5 | Wrapper container class `0xf5` | the drive's `AppUpgrade` | re-encrypted patched wrapper + `.ver` row 341142 (the OEM signature on the wrapper is necessarily invalid; gates 1–4 exist to compensate) | Built + verified offline |

Gate-4 note: on-demand `request.launch` spawns already skip the `.sign`
check on the stock binary; only startup-group/preload spawns verify —
which is exactly the path a patched AppDMClient needs at boot.

## VM test evidence (2026-11-06, gate 3 + 4)

- **A0** — stock AppDMClient + patched wrapper: `errAppUpgrade = -403`,
  `errXML = 0`, no launch — the grayed-button reproduction.
- **A1** — stock AppDMClient + stock wrapper: `errAppUpgrade = 0`,
  launch — the baseline.
- **A2** — patched AppDMClient + patched wrapper: `errAppUpgrade = 0`,
  `errXML = 0`, `No Hash error …`, `APPLAUNCHER_STUB_EXEC_OK`,
  `PHASE4_OK` — **the button gate is fixed.**
- Engine validation (Test B) is blocked in the VM by the AppUpgrade Qt
  GUI not reaching the engine; the car/bench is the real test.

## The packages

`scripts/06_build_package.sh` builds two variants from `out/stage1.te2`:

- `package_second` — stock USB wrapper; the six stubs live in the rootfs.
  **Primary path** once the gates are car-validated.
- `package_wrapper` — patched `0xf5` wrapper + patched `.ver` rows
  341174/341142. Fallback for the case where the head unit runs the
  drive's wrapper.

Both keep `.lge.upgrade.xml` byte-identical to stock and pad the `.ver` to
2,467 B. `09_preflight.sh` verifies both offline.

## The deployers (tools/update-debug/)

For cars already rooted with an earlier build (or to patch in place
without a reflash): all stick-based, idempotent, abort on any unknown
size/md5/bytes, and back up before patching.

- **`dmclient_gate.sh`** — the full chain for an already-rooted car:
  launcher prerequisite → dmclient stubs → re-verify installed
  AppUpgrade/AppScomoDM stubs → verify/swap the drive wrapper + `.ver` row
  341142 (stock backup + REVERT marker) → start the 45-minute watcher →
  collect `dmclient_gate_diag.tar.gz`. Both running binaries are patched
  atomically (copy + `mv` to a new inode); the running process keeps the
  old inode until reboot.
- **`button_gate.sh`** — the v4 combo stage (gates 1–2 + drive + watcher +
  diag); supersedes `both_copies.sh`.
- **`applauncher_sigstub.sh`** — gate 4 alone, with in-session AppLauncher
  respawn + bus-name verification.

## The current test sequence

1. **Stage-1 build 4 flash (fresh root):** the ten stubs are baked in —
   after the TOCTOU flash, reboot, and the gates are in place with no
   deployer run needed.
2. **Already-rooted car:** payload stick with MODE=all (apps →
   button-gate → applauncher), or run `dmclient_gate.sh` directly with the
   update drive inserted. Expected: every stage `status=OK`,
   `reboot_required=1`.
3. **REBOOT the head unit** (AppScomoDM + AppDMClient must reload; the
   launcher needs a clean state). Do not insert the update drive before
   this reboot.
4. **Watcher:** insert the prepared update drive (its own run_stuff.sh
   wrapper starts the 45-minute watcher), then start
   `Settings > General > Software Update > Update via USB` within 45
   minutes. The Update button should now be **active**.
5. If it passes 0%, the normal-USB path works end to end.
6. If the button is still grayed: re-run the deployer (bump its md5) and
   collect `dmclient_gate_diag.tar.gz` — the next target is whatever
   AppDMClient loads or checks beyond the two stubbed entries.

The watcher is a `nohup` process and **does not survive a reboot** — never
reboot between starting it and the update attempt.

## Current status table

| Gate | Result |
|---|---|
| Stage-1 rootfs envelope (ten stubs, build 4) | `VERIFY: OK` |
| `package_second` / `package_wrapper` | `PACKAGE VERIFY: OK` |
| Patched wrapper container | built, sha256 `176d4a8c…` (md5 `67a9fa48…`) |
| `dmclient_gate` deployer | host-tested (patch, idempotent, revert, launcher-fail skip) |
| VM Test A — A0 grayed reproduction | PASS (`errAppUpgrade=-403`, no launch) |
| VM Test A — A1 stock baseline | PASS (`errAppUpgrade=0`, launch) |
| VM Test A — A2 stubbed fix | PASS (`errAppUpgrade=0`, launch, `PHASE4_OK`) |
| VM Test B — engine validation | blocked by the VM AppUpgrade GUI; car/bench |
| **Car: full custom update** | **pending** |

## Recovery, always

- The A/B slot: the update writes the inactive slot; a failed install
  falls back on reboot.
- The rig (docs/04) + a stock 260128 package remain the full recovery
  path.
- `REVERT` markers restore AppLauncher + AppDMClient (dmclient_gate), the
  AppScomoDM stubs (button_gate), and the drive wrapper + `.ver` row.
- Never interrupt an update or power off mid-install.
- The OTA blocker in the Tweaks package is **off by default** precisely so
  this USB path stays available.
