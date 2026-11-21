# 08 — Custom firmware updates after root (CONFIRMED)

STATUS: **car-confirmed 2026-11-15.** A custom 260128 rootfs was installed
through the ordinary USB updater from an exFAT stick — no rig, no race, no
userspace partition write. This document is the gate history and runbook;
`docs/17-usb-update-confirmed.md` is the one-page confirmed procedure.

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128` (EU).

## The gate inventory

The map has been revised from on-car and VM evidence. As of **build 5, all
eleven stubs are baked into the stage-1 rootfs**, re-applied at boot by the
guard. A fresh TOCTOU flash therefore has the USB path ready; the
`tools/update-debug/` deployers remain for cars rooted with an earlier
build.

| # | Gate | Where | SCOURGE's answer | State |
|---|---|---|---|---|
| 1 | Engine validators (TE2 metadata parse + final hash/RSA verify) | `/app/bin/AppUpgrade` — **five** entry points: SCOMO `FUN_002f5a00` @ 2,086,500, `FUN_002f5b60` @ 2,086,852, `FUN_002f5ba0` @ 2,086,916, `FUN_002f5c40` @ 2,087,076; **FUMO `FUN_0028f5a0` @ 7,346,024** | five `31 C0 C3` stubs baked into the stage-1 rootfs | Car-validated |
| 2 | UsbUpdateManager verifiers | `/app/bin/AppScomoDM` — `FUN_001e3430` @ 930,864, `FUN_001e3470` @ 930,928 | two `31 C0 C3` stubs baked into the stage-1 rootfs | Baked + offline-verified |
| 3 | **The real update-button gate: AppDMClient's `checkUSBUpdate()`** — verifies the drive's `AppUpgrade` wrapper AND `.lge.upgrade.xml` against `/app/share/AppUpgrade/teruten5_pub.key` and controls the Update-button enable/disable | `/app/bin/AppDMClient` — `FUN_00221000` @ 1,183,744 + `FUN_00221040` @ 1,183,808 | two `31 C0 C3` stubs baked into the stage-1 rootfs | **Car-validated** (button active; `errAppUpgrade=0`) |
| 4 | **Launcher prerequisite for gate 3:** the stock AppLauncher `.sign`-verifies every startup-group/preload spawn and refuses to spawn a modified AppDMClient at boot | `/app/bin/AppLauncher` — `FUN_00115b50` @ 88,912 + `FUN_00112100` @ 73,984 | two `B8 01 00 00 00 C3` stubs (true-on-success) baked into the stage-1 rootfs | **Car-validated** (patched client starts) |
| 5 | Wrapper container class `0xf5` | the drive's `AppUpgrade` | re-encrypted patched wrapper + `.ver` row 341142; the OEM signature is necessarily invalid, gates 1–4 compensate | Car-validated |
| 6 | SMACK label-write restriction (not a signature gate) | `/sys/fs/smackfs/onlycap` | `/etc/smack_permissive` + patched `/etc/smack_setup.sh`: `bringup-switch=1`, `onlycap` left empty | Baked into the car image; post-boot values not yet measured |

Gate-4 note: on-demand `request.launch` spawns already skip the `.sign`
check on the stock binary; only startup-group/preload spawns verify —
which is exactly the path a patched AppDMClient needs at boot.

## The FUMO fifth gate

The normal-USB flow does not run the SCOMO engine. DMClient launches the
wrapper; the wrapper extracts the embedded **FUMO** engine
(`AppFumoUpgrade`, embedded at 5,710,280); the FUMO `checkHash` dispatcher
is `FUN_0028f5a0` at outer offset **7,346,024** (`0x701768`). It is the
structural twin of the SCOMO `FUN_002f5a00`, and it is what returned
`checkHash … (-403)` for a re-encrypted container before build 5. Stubbing
it with `31 C0 C3` makes `checkHash` return 0.

`DecryptToPIPE` enforces no container signature: its AES key is derived
from the container's own metadata (SHA-256 over IV ‖ metadata regions ‖
key store), which the repack reuses verbatim. The only RSA in DTP decrypts
the `-d` device-key file. The fifth gate is the FUMO `checkHash`, not the
wrapper copy path.

## The SMACK permissive flag

The vendor script already sets `bringup-switch=1` on non-UNECE cars, but it
also writes `onlycap=Privileged`. That removes `CAP_MAC_ADMIN` from the `_`
label, which is why payloads could not write SMACK labels with
`setfattr`/`chsmack`. Build 5 adds `/etc/smack_permissive` and patches
`/etc/smack_setup.sh` at the vendor write site: when the flag is present,
`bringup-switch` is forced to 1 and `onlycap` is left empty. This is the
persistent form of the DeSmack runtime state.

The vendor `Privileged` fallback is retained when the flag is absent. The
flag must be handled at the write site while `onlycap` is still empty; a
late boot script cannot clear `onlycap` after the vendor sets it.

Post-boot check (run as root):

```sh
touch /tmp/probe
test -f /etc/smack_permissive && echo "flag=present"
chsmack -a Stage4Probe /tmp/probe && chsmack /tmp/probe   # Stage4Probe
cat /sys/fs/smackfs/bringup-switch                        # 1
cat /sys/fs/smackfs/onlycap                               # empty
```

Post-boot values are **not yet measured** — the car was offline when
this document was written. The commands above are the check; record the
actual output before calling gate 6 car-validated.

## The packages

`scripts/06_build_package.sh` builds two variants from `out/stage1.te2`:

- `package_second` — stock USB wrapper; the eleven stubs + SMACK flag live
  in the rootfs. Wrapper row 341142 stays stock (`609028602`).
- `package_wrapper` — patched `0xf5` wrapper + patched `.ver` rows
  341174/341142. **Car-validated path.**

Both keep `.lge.upgrade.xml` byte-identical to stock and pad the `.ver` to
2,467 B. `scripts/09_preflight.sh` verifies both through
`verify/verify_package.py`.

Build-5 hashes:

```
rootfs        d1eec7cb8861c25facb77a558348bfb648bbd7b72d30f16ad082145d19b21dde
plaintext     f66f1a9b91e7bef630b1ef0b0bbe74c740ccb25acdf93101ada96aa911ae275b
wrapper       9a0df0809a3f15c2d5f1d3e83c19ec0a27f336727e57e01bb2c5ae2e82783876
wrapper md5   f730490b9a9f6d3499b9ba59070d63e8
.ver wrapper  6611fb1f30077adc3305a278078e795f438a0bb4ba3934f8043b9526dcd54657
.ver stock    0850314af773103a8d47eceac2aebcdad1a0d44eca70132639cb935c2b2c8429
rootfs row    311417491
wrapper row   414600538
```

## The deployers (tools/update-debug/)

For cars already rooted with an earlier build (or to patch in place without
a reflash): all stick-based, idempotent, abort on any unknown size/md5/
bytes, and back up before patching. They now include the FUMO row;
each audits the stubs it manages (`dmclient_gate.sh` covers all eleven;
`button_gate.sh` covers the five AppUpgrade engine stubs and the two
AppScomoDM stubs; the legacy `both_copies.sh` covers only the five engine
stubs).

- **`dmclient_gate.sh`** — the full chain for an already-rooted car:
  launcher prerequisite → installed AppUpgrade engine stubs (five) →
  AppScomoDM stubs (two) → AppDMClient stubs (two) → verify/swap the drive
  wrapper + `.ver` row 341142 (stock backup + REVERT marker) → start the
  45-minute watcher → collect `dmclient_gate_diag.tar.gz`. Both running
  binaries are patched atomically (copy + `mv` to a new inode); the
  running process keeps the old inode until reboot.
- **`button_gate.sh`** — the v4 combo stage (gates 1–2 + FUMO + drive +
  watcher + diag); supersedes `both_copies.sh`.
- **`applauncher_sigstub.sh`** — gate 4 alone, with in-session AppLauncher
  respawn + bus-name verification.

The 45-minute (at least) watcher installed by all three deployers logs only
process transitions, not a full `ps -ef` snapshot per change. It drops its
own pipeline helpers and kernel threads (PPID 2), and caps
`/rw_data/update_watch.log` at 2 MiB (`UPDATE_WATCH_MAX_BYTES`), with
`UPDATE_WATCH_MAX_LINE` for the 200-byte line bound and
`UPDATE_WATCH_KEEP_BYTES` for the 32 KB tail kept when trimming. The earlier
build appended a full snapshot on every 2-second transition and reached
roughly 100 MB in one window, which filled a 1.2 GB `/rw_data` and had to be
deleted by hand. The bounded watcher is offline-tested only; car validation
is pending.

## The confirmed test sequence

1. **Fresh stage-1 build-5 flash** (TOCTOU or already-rooted car with the
   eleven stubs): no deployer run is needed.
2. Prepare `out/package_wrapper` on an exFAT stick. Copy the hidden
   `.lge.upgrade.xml` and `.whatsnew.tar.gz` as well.
3. Insert the drive; open `Settings > General > Software Update >
   Update via USB`. The Update button is active (`errAppUpgrade=0`).
4. Confirm the same-version 260128 reinstall. Do not pull the stick or
   power off while the updater is working.
5. The updater reaches the FUMO engine; the `checkHash` stub returns 0;
   the rootfs is decrypted and written to the inactive slot.
6. After the reboot, run the inline SMACK check above and confirm the
   boot message.
7. If the button is still grayed on an older car, run `dmclient_gate.sh`
   and collect `dmclient_gate_diag.tar.gz`. If the Update screen stalls
   before the engine, the issue is UI/foreground, not a signature gate;
   capture the screen/DLT.

## Current status table

| Gate | Result |
|---|---|
| Stage-1 rootfs envelope (eleven stubs + SMACK, build 5) | `VERIFY: OK` |
| `package_second` / `package_wrapper` | `PACKAGE VERIFY: OK` |
| Patched wrapper container | built, sha256 `9a0df080…` (md5 `f730490b…`) |
| `dmclient_gate` deployer | host-tested (patch, idempotent, revert, launcher-fail skip) |
| VM Test A — A0 grayed reproduction | PASS (`errAppUpgrade=-403`, no launch) |
| VM Test A — A1 stock baseline | PASS (`errAppUpgrade=0`, launch) |
| VM Test A — A2 stubbed fix | PASS (`errAppUpgrade=0`, launch, `PHASE4_OK`) |
| VM Test B — engine validation | VM GUI blocker; car is the real test |
| **Car: full custom update via normal USB** | **PASS (2026-11-15)** |

## Boot-message refresh over SSH (WIP)

A rooted car can take a message-only refresh without a USB update. The
patched `.qm` files are installed in place; the eleven stubs, the SMACK flag
and the failsafe are untouched. The patcher and stage generation are
offline-validated on the stock tree; the installer's verify/install/rollback
paths are host-tested with a sandboxed mount table and label tools. Neither
has run on a car yet — the car-validated image remains build 5.

1. Generate the stage with the message-only mode (stock tree or a copy of the
   car's current `localization/` tree):

   ```sh
   "$PYTHON" scripts/03_patch_payload.py \
       --stock-qm-dir work/rootfs/app/share/AppStandbyClock/localization \
       --messages config/messages.json --out work/bootmsg-qm
   ( cd work/bootmsg-qm && sha256sum translation_*.qm > MANIFEST.sha256 )
   ```

2. Copy the stage and the installer to the car over SSH (direct
   `wifi1b@10.55.0.2:2224`, or through the Pi reverse tunnel):

   ```sh
   tar -C work/bootmsg-qm -cf - . | ssh <car> '
       rm -rf /rw_data/scourge/bootmsg.new &&
       mkdir -p /rw_data/scourge/bootmsg.new &&
       tar -C /rw_data/scourge/bootmsg.new -xf -'
   ssh <car> 'cat > /rw_data/scourge/deploy_boot_messages.sh' \
       < tools/deploy_boot_messages.sh
   ```

3. Run the installer. It verifies every staged hash and size, remounts `/`
   read-write only for the copy, backs up the live files under
   `/rw_data/scourge/bootmsg-backup-*`, installs by atomic rename, syncs,
   restores the original mount mode, and rolls back on any failure:

   ```sh
   ssh <car> 'STAGE=/rw_data/scourge/bootmsg.new EXPECTED_COUNT=26 \
       sh /rw_data/scourge/deploy_boot_messages.sh'
   ```

4. Reboot the head unit. AppStandbyClock loads the translations at startup,
   so the screen does not change until then.

## Recovery, always

- The A/B slot: the update writes the inactive slot; a failed install
  falls back on reboot.
- The rig (docs/04) + a stock 260128 package remain the full recovery
  path.
- `REVERT` markers restore AppLauncher + AppDMClient (dmclient_gate),
  AppScomoDM (button_gate), and the drive wrapper + `.ver` row. The
  AppUpgrade engine stubs (including FUMO) are not auto-reverted; the
  `AppUpgrade.before-*` backups on the stick are there for a manual
  restore.
- Never interrupt an update or power off mid-install.
- The OTA blocker in the Tweaks package is **off by default** precisely so
  this USB path stays available.
