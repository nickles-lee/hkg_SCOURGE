# 17 — Unsigned normal-USB update: CONFIRMED

STATUS: **car-confirmed 2026-11-15.** A custom 260128 rootfs was installed
through the ordinary head-unit updater (`Settings > General > Software
Update > Update via USB`) from an exFAT flash drive. No TOCTOU rig, no race,
no partition write from userspace.

The **TOCTOU path** — the same update delivered by the dynamic gadget instead of
a pre-written drive — was car-validated on 2026-11-21; the evidence is in *The
TOCTOU path* below.

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128` (EU) —
the 2026-11-21 TOCTOU run below was a **2021 Ioniq 5 RWD** (`prm-gen-5`, kernel
`4.1.27-abl`).

## The TOCTOU path — car-validated 2026-11-21

Same car, different mechanism: instead of the stub packages above, the patched
rootfs went through the dynamic-media race. The head unit mounted the rig's
image (volume serial `7FFF-77F0` — that is bare exFAT `out/usb.img`) and the
update ran from `Settings > General > Software Update > Update via USB`.

Rig side, `/var/log/toctou.log`, session `705e3006`:

| pass | served | bytes | duration | rate |
|---|---|---|---|---|
| #1 — the engine's md5 sweep of the container | `GENUINE` | 879,108,096 | 186.71 s | 4.7 MB/s |
| `*** SWITCH at pass #2 ***` | | | 12:39:07 UTC | |
| #2 — the install read | `EVIL` | 861,851,648 | open until shutdown | (lower bound only) |

Head unit side, `/log_data/AppUpgrade/upgrade_log`: `mkfs.ext4` on
`/dev/mmcblk0p5` at 12:39:03, rootfs install 12:39:06 → 12:40:59 (**113 s for
879.1 MB = 7.8 MB/s**), `[UPDATE_DONE]` 13:08:34, `System Update Complete!!, 100`
13:08:42 — **34 min 21 s** from the first read. A stock-stick run on 2026-07-26
(volume `9C14-E5DD`) spent the same **113 s** on that step, so the rig does not
slow the update: the head unit's decrypt → `zcat` → `tar` → ext4 pipeline is the
limit, and its md5 sweep is the slowest single read at 4.7 MB/s.

That the *installed* tree was the swapped one is visible without trusting the
log: the engine reformatted the rootfs partition first, and the next boot's
preflight shows `/etc/initial_root/{run_stuff_loader.sh,updater_guard.sh}` dated
into the install window (2026-11-21 12:40) — stock has no `/etc/initial_root`.

## What the image contains

Stage-1 **build 5**: the four patched binaries with **eleven stubs**, plus
the persistent SMACK permissive flag and the hardened failsafe.

> **Build 6 (all-language boot message) is WIP.** Changing the message `.qm`
> files does not alter the eleven stubs or the security model, but it changes
> every rootfs-derived hash. The artifacts below are the car-validated build 5;
> a build-6 image needs its own table and car validation. An already-rooted car
> can take the message refresh over SSH without rebuilding the image (docs/08).

| Binary | Function | File offset | Patch |
|---|---|---:|---|
| AppUpgrade (SCOMO) | `FUN_002f5a00` | 2,086,500 | `31 C0 C3` |
| AppUpgrade (SCOMO) | `FUN_002f5b60` | 2,086,852 | `31 C0 C3` |
| AppUpgrade (SCOMO) | `FUN_002f5ba0` | 2,086,916 | `31 C0 C3` |
| AppUpgrade (SCOMO) | `FUN_002f5c40` | 2,087,076 | `31 C0 C3` |
| **AppUpgrade (FUMO)** | **`FUN_0028f5a0`** | **7,346,024** | **`31 C0 C3`** |
| AppScomoDM | `FUN_001e3430` / `FUN_001e3470` | 930,864 / 930,928 | `31 C0 C3` |
| AppDMClient | `FUN_00221000` / `FUN_00221040` | 1,183,744 / 1,183,808 | `31 C0 C3` |
| AppLauncher | `FUN_00115b50` / `FUN_00112100` | 88,912 / 73,984 | `B8 01 00 00 00 C3` |

The fifth engine stub is the one that matters for this path. The normal-USB
flow does **not** run the SCOMO engine; DMClient launches the wrapper, the
wrapper extracts the embedded **FUMO** engine, and the FUMO engine's
`checkHash` dispatcher at 7,346,024 is what rejects a re-encrypted
container. Stubbing it makes `checkHash` return 0. The other stubs cover
the button gate (`checkUSBUpdate`), the UsbUpdateManager verifiers, and the
launcher `.sign` prerequisite.

The wrapper on the drive is the class-`0xf5` `AppUpgrade` container,
re-encrypted at the same size/IV. Its OEM signature is necessarily invalid;
the stubs are the acceptance mechanism. The `.ver` rootfs row and wrapper
row are recomputed.

The image also carries:

- `/etc/smack_permissive` + the patched `/etc/smack_setup.sh`: forces
  `bringup-switch=1` and leaves `onlycap` empty at the vendor write site
  (the persistent form of the DeSmack state; `setfattr`/`chsmack` from the
  `_` label should then work — post-boot effect not yet measured, see
  below);
- the hardened stage-3 failsafe (starts before the UI, raw-mounts sticks,
  writes `/rw_data/initial_root_boot.txt`).

## Build

```sh
scripts/build_all.sh     # payload -> repack -> verify -> wrapper
                        # -> packages -> usb.img -> preflight
```

Prerequisites: `scripts/00_env.sh`, `scripts/00_fetch_third_party.sh`,
`scripts/01_stage_inputs.sh`, `scripts/02_decrypt_rootfs.sh` (docs/03).

The standard TOCTOU image build (`scripts/08_build_usb_image.sh`, run by
`scripts/build_all.sh`) uses the same `out/stage1.te2`; a fresh TOCTOU flash
therefore has this USB path ready without a deployer.

## Car-validated artifacts

| Artifact | Value |
|---|---|
| `out/stage1.te2` | 879,126,179 B, sha256 `d1eec7cb8861c25facb77a558348bfb648bbd7b72d30f16ad082145d19b21dde` |
| plaintext | 879,109,759 B, sha256 `f66f1a9b91e7bef630b1ef0b0bbe74c740ccb25acdf93101ada96aa911ae275b` |
| `out/AppUpgrade.patched.te2` | sha256 `9a0df0809a3f15c2d5f1d3e83c19ec0a27f336727e57e01bb2c5ae2e82783876`, md5 `f730490b9a9f6d3499b9ba59070d63e8` |
| `out/package_wrapper/IONIQ5_EU.ver` | sha256 `6611fb1f30077adc3305a278078e795f438a0bb4ba3934f8043b9526dcd54657` |
| `out/package_second/IONIQ5_EU.ver` | sha256 `0850314af773103a8d47eceac2aebcdad1a0d44eca70132639cb935c2b2c8429` |
| rootfs row | `IONIQ5_EU\HU\images|mango-rootfs.tar.gz|341174|311417491|879126179|1` |
| wrapper row | `IONIQ5_EU|AppUpgrade|341142|414600538|10803676|1` |
| boot message (build 5) | sha256 `a00dd663d6f3cb791dac9b7d3d674df59f97c0f42b615a981d20c256114abb33` (`config/message.txt`, retired; build 6 uses `config/messages.json`) |

## Evidence

Pre-update live audit on the car:

- installed `/app/bin` binaries: `AppUpgrade 55e522e3…`,
  `AppScomoDM 45f881b3…`, `AppDMClient 49c24ca8…`,
  `AppLauncher 6eeb0545…` — the eleven-stub set;
- the staged updater at `/update/usb/bin/AppUpgrade` was the same
  `55e522e3…` plaintext, with `31 c0 c3` at 7,346,024;
- DMClient logged `errAppUpgrade = 0`, `errXML = 0`;
- the update state was clean (`system_update_complete=none`,
  `force_launch=none`, `dm_state=0`, no `system_result.txt`).

Car result: the unsigned normal-USB update completed and the unit booted
the new rootfs (owner-confirmed, 2026-11-15).

Post-boot SMACK check (**expected values; not yet measured** — the car was
offline at last check). Run as root:

```sh
touch /tmp/probe
test -f /etc/smack_permissive && echo "flag=present"
cat /sys/fs/smackfs/bringup-switch    # expect 1
cat /sys/fs/smackfs/onlycap           # expect empty
chsmack -a Stage4Probe /tmp/probe && chsmack /tmp/probe   # expect Stage4Probe
setfattr -n security.SMACK64 -v Stage4Probe /tmp/probe     # expect success
```

Record the actual output before treating gate 6 as car-validated.

## Procedure

1. Build `out/package_wrapper` (patched wrapper). The stock-wrapper
   `package_second` is the fallback variant.
2. Copy the **contents** of the package to the root of an exFAT stick. The
   hidden `.lge.upgrade.xml` and `.whatsnew.tar.gz` must be copied too.
3. Insert the stick; open `Settings > General > Software Update >
   Update via USB`.
4. Confirm the same-version 260128 reinstall.
5. Do not pull the stick or power off while the updater is working.
6. After the reboot, run the inline SMACK check above and confirm the
   boot message.

## The TOCTOU path (car-validated 2026-11-21)

A Pi 4 in USB-C peripheral mode served the 28.9 GB image through `nbdkit` from
an exFAT stick, with one `benign_then_evil` region over the rootfs container
(`off` 5 505 024, `len` 879 126 179, `switch_after_pass` 2). The head unit was
started on `Settings > General > Software Update > Update via USB` and accepted
the same-version 260128 reinstall.

| step | evidence (`out/rig-logs/20261121T131757Z/`) |
|---|---|
| check pass | `PASS #1 END reads=214626 bytes_genuine=879108096 bytes_evil=0 served=GENUINE dur=186.71s` |
| switch | `*** SWITCH at pass #2 (mode=benign_then_evil) ***` at mono 702.563 |
| install pass | 210 413 reads served from `stage1.te2`, `off` 5 505 024 → 884 625 408, mono 702.566 → 3013.237 |
| root channel after boot | `uid=0`, `uname=Linux prm-gen-5 4.1.27-abl`, `smack_flag=present`, loader md5 `3a244ac1…` |

There is no `PASS #2 END` line: the update ended without another sweep, so the
per-read timeline (211 k evil reads covering the whole region) is the record,
not a summary row. The link measured 4.7 MB/s on the check pass and ~0.37 MB/s
on the install pass; `tar` throttles the read, not the wire (`docs/04`).

The updater stubs survived the flash: `31c0c3` at five offsets in `AppUpgrade`
(2 086 500, 2 086 852, 2 086 916, 2 087 076, 7 346 024) and two in `AppScomoDM`
(930 864, 930 928). The installed `AppUpgrade` md5 is `55e522e37e85b9a7c599f9f17fc1a580`,
`AppScomoDM` is `45f881b3b08f40e6e07c03988e441341`.

## Limits and recovery

- Same-version reinstall only; this does not bypass a version downgrade
  policy.
- The update screen must reach the foreground before the engine starts.
  The 2026-11-10 attempt stalled there because of a UI/foreground problem,
  not a signature check. Keep `/rw_data` free (the failsafe/deployer
  writes backups), keep the UI stack healthy, and capture the screen/DLT
  if the screen stalls.
- The A/B slot remains the recovery path; a failed install falls back on
  reboot. The TOCTOU rig plus a stock 260128 package remain the full
  recovery path.
- The class-`0xf5` OEM signature is invalid by construction. The eleven
  stubs are the acceptance mechanism; do not describe this as a valid
  signed update.

See `docs/08-postroot-updates.md` for the full gate history and the
already-rooted-car deployers.
