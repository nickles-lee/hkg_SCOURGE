# 03 — Stage-1 firmware build (agent runbook)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128` (EU).

The stage-1 firmware is the modified rootfs delivered by the TOCTOU rig
(docs/01, docs/04, docs/05). **Build 5 is the car-validated image; build 6
is the all-language boot-message refresh (WIP, offline-validated only).**
Both builds carry:

1. The **boot message** patched into every AppStandbyClock
   safety-disclaimer translation. Build 5 used `config/message.txt` and only
   `en-gb`/`en-au`; build 6 uses `config/messages.json` and all 26 firmware
   locales, length-preserving inside each stock `.qm` slot.
2. The **hardened run_stuff failsafe**:
   `/etc/initial_root/{run_stuff_loader.sh, updater_guard.sh}` +
   `initial-root-failsafe.service` (docs/06). The service is ordered
   `After=local-fs.target` and `Before=multi-user.target
   ApplicationLauncher.service`; the loader modprobes the USB stack,
   raw-mounts sticks, writes the boot marker and runs `/bin/sh` payloads.
3. The **eleven updater/launcher stubs baked in** (build 5, four binaries —
   docs/08):
   - **five** validator entry points in `/app/bin/AppUpgrade`
     (`31 C0 C3`): the four SCOMO entries plus the FUMO
     `FUN_0028f5a0` @ 7,346,024 — the engine the normal-USB flow runs;
   - two UsbUpdateManager entries in `/app/bin/AppScomoDM` (`31 C0 C3`);
   - two button-gate entries in `/app/bin/AppDMClient` (`31 C0 C3`);
   - two `.sign`-gate entries in `/app/bin/AppLauncher`
     (`B8 01 00 00 00 C3` — true-on-success; prerequisite for a modified
     AppDMClient to survive a boot).

   The boot-time guard re-applies all eleven (idempotent, size-gated; the
   boot-running binaries are patched with an atomic rename).
4. The **SMACK permissive flag**: `/etc/smack_permissive` plus the patched
   `/etc/smack_setup.sh`. When the flag is present, the script forces
   `bringup-switch=1` and leaves `onlycap` empty at the vendor write site,
   so `CAP_MAC_ADMIN` is not restricted to the `Privileged` label.

## Prerequisites

```sh
scripts/00_env.sh                    # venv: pycryptodome, deflate, cryptography (PyQt5 optional)
scripts/00_fetch_third_party.sh      # pinned TOCTOU repo + TE2 key + nudl
scripts/01_stage_inputs.sh --download   # or --from-dir / --from-zip
```

`01_stage_inputs.sh` hash-gates every input against `config/firmware_locks.env`.
A mismatch aborts — never proceed with different firmware (docs/09).

## Build

```sh
scripts/02_decrypt_rootfs.sh          # -> work/stock-plain.tar.gz (879,109,759 B)
source scripts/lib.sh                 # selects $PYTHON and the local paths
"$PYTHON" scripts/03_patch_payload.py # -> work/payload/{AppUpgrade,AppScomoDM,AppDMClient,AppLauncher}.patched + .qm
scripts/04_repack_rootfs.sh           # -> 03b SMACK patch + surgery + failsafe + flag -> out/stage1.te2
load_te2_key
"$PYTHON" scripts/05_verify_rootfs.py
```

Or everything at once (requires the full stock tree for the image):

```sh
scripts/build_all.sh                  # payload -> repack -> verify -> wrapper
                                     # -> packages -> 27 GB usb.img -> preflight
```

`04_repack_rootfs.sh` calls `scripts/03b_patch_smack.py` to regenerate the
patched `/etc/smack_setup.sh` and `/etc/smack_permissive` from the stock
tar. It replaces `/etc/smack_setup.sh` via `surgery.py`, appends the four
failsafe entries plus the flag, and fixes the appended entries' mtimes so
the TE2 hash is deterministic.

## The build is deterministic — your output MUST match

The chain (in-place tar surgery → fixed-order append with fixed mtimes →
libdeflate L10 → exact-size padding → TE2-encrypt with the stock envelope
as metadata template) reproduces byte-identical artifacts. **Compare every
artifact against this table; any mismatch means stop and investigate.**

> **Build 6 is car-validated as of 2026-11-21.** A 2021 Ioniq 5 RWD installed
> `out/stage1.te2` (`7f3b155e…`) through the TOCTOU rig — the run is in docs/17.
> The build-6 column below is therefore what is on that car, and the build-5
> values are the historical reference: the message refresh changes all patched
> `.qm` files, so every rootfs-derived hash moved while the four payload
> binaries stayed byte-identical (see the table's payload rows).
> `config/firmware_locks.env`'s `KNOWN_GOOD_*` pins now carry the validated
> values, so a build that does not reproduce them stops here. The `.qm`-only SSH
> refresh (`tools/deploy_boot_messages.sh`) does not use this table.

| Artifact | Property | Known-good value (build 5) |
|---|---|---|
| `out/stage1.te2` | size / sha256 | 879,126,179 / `d1eec7cb8861c25facb77a558348bfb648bbd7b72d30f16ad082145d19b21dde` |
| `work/stage1.padded.tar.gz` | sha256 | `f66f1a9b91e7bef630b1ef0b0bbe74c740ccb25acdf93101ada96aa911ae275b` |
| `work/payload/AppUpgrade.patched` | sha256 | `9dda8c03077ff59043d412dd4c351b197247788b97c9a66056792d9c0d3eebf0` |
| `work/payload/AppScomoDM.patched` | sha256 | `b2467b2b7cdf18b4e470b36083867411cae3b9d047739959141b333e5fa1e596` |
| `work/payload/AppDMClient.patched` | sha256 | `e59259a16d3025d07a9f790a07e5eb4837df5cf69b05fcb2b3a64b281221fba5` |
| `work/payload/AppLauncher.patched` | sha256 | `d00efd1363dcaef2c3c5b3c5aefc01d0eea99dce84d60647ab036ce65113e2e0` |
| `out/AppUpgrade.patched.te2` | sha256 | `9a0df0809a3f15c2d5f1d3e83c19ec0a27f336727e57e01bb2c5ae2e82783876` |
| `out/package_second/IONIQ5_EU.ver` | sha256 | `0850314af773103a8d47eceac2aebcdad1a0d44eca70132639cb935c2b2c8429` |
| `out/package_wrapper/IONIQ5_EU.ver` | sha256 | `6611fb1f30077adc3305a278078e795f438a0bb4ba3934f8043b9526dcd54657` |
| `config/message.txt` (build 5, retired) | sha256 | `a00dd663d6f3cb791dac9b7d3d674df59f97c0f42b615a981d20c256114abb33` |
| `out/usb.img` | size / sha256 | 28,894,603,490 / `9b55dafbdfe6b2837e2e77b3e539345aff286b5ad919d11f9ef57a190db558c3` |

Build-6 input (WIP): `config/messages.json` sha256
`f9929bf437ba0bdd763e744b3a1ec47cbfd910973431b88272a346ceb4a266f6`.

### Build 6 measurements (offline-validated 2026-11-19, NOT car-validated)

A full local run of the chain on the OEM 260128 package reproduced every
payload binary byte-for-byte and produced the values below. The build-5 column
above still describes the image on the car; these replace it only once a car
run confirms them.

| Artifact | Property | Measured value (build 6) |
|---|---|---|
| `out/stage1.te2` | size / sha256 | 879,126,179 / `7f3b155e487f10929ea97cb3e12eb95fdeb114e57a38c187ec8b410d1b12fb94` |
| `work/stage1.padded.tar.gz` | sha256 | `32aec017f8e0c16a7cfcdb836c8db346717774d42186cbf47c87dbe23516a8c4` |
| `work/payload/*.patched` (4 files) | sha256 | unchanged — identical to the build-5 pins |
| `out/AppUpgrade.patched.te2` | sha256 | `9a0df0809a3f15c2d5f1d3e83c19ec0a27f336727e57e01bb2c5ae2e82783876` (unchanged) |
| `out/package_second/IONIQ5_EU.ver` | size / sha256 | 2,469 / `c763b6d7947a5bdb70f925db675e284d9a322a6cf7e850a9e0be2f5c6077dd12` |
| `out/package_wrapper/IONIQ5_EU.ver` | size / sha256 | 2,469 / `34bb8bc8966353a4b3084d916cacfd839d07accf43f691c84471be16602f40d3` |
| `out/usb.img` | size / sha256 | 28,894,603,492 / `ab4686bd4144d770d5bd95fcf4b337f9340e1a1d2e3d3966619a7a9ba027dcf5` |
| `out/regions.json` | `off` / `len` | 5,505,024 / 879,126,179 (single extent, `benign_then_evil`, pass 2) |

Two facts about the build-5 column, both established on 2026-11-19 while
reproducing it from a clean download:

1. **`VER_STOCK_SIZE` / `VER_STOCK_SHA256` / `VER_ROOTFS_ROW_STOCK_CRC` were
   wrong, and are corrected in `config/firmware_locks.env`.** The OEM package
   for this version ships a **2,469-byte** `.ver` (sha256 `53da7ec0…80ad7b3`)
   whose rootfs row carries CRC `1243982759` — exactly `crc32` of the genuine
   879,126,179 B envelope, the same convention the wrapper row uses
   (`609028602` == `crc32(AppUpgrade)`). The old pin described a 2,467-byte
   manifest that no official download produces, so `01_stage_inputs.sh`
   refused a genuine package. The row CRC is no longer pinned at all: it is
   derived and checked against the staged envelope on every run. Verified
   2026-11-20 by extracting that gate and running it on purpose: the genuine
   `.ver` exits 0, the same file with the row CRC off by one exits 1.
   The *patched* row CRC had rotted the same way — `VER_ROOTFS_ROW_PATCHED_CRC`
   still said `311417491` while `crc32(out/stage1.te2)` is `287530490`. Nothing
   read it, so nothing broke, which is exactly how a stale pin waits for a
   future script; the pin is gone and the value is derived
   (`verify_package.py`, `09_preflight.sh`). The wrapper pins are current
   (`609028602` stock, `414600538` patched).
2. **The build-5 `.ver` output hashes are therefore unreachable from a fresh
   download** — they were produced from that 2,467-byte base. Re-record them
   from a car-validated run, not from this table.

Envelope shape: class `0xe0`, IV `5412d68d2f22bf3d30f3c1ff1e715326`, body
879,109,759 B, tar entries 66,841 (genuine 66,836 + four failsafe entries +
the SMACK flag).

## Verification gates

`05_verify_rootfs.py` prints `VERIFY: OK` only when all of these hold:

- envelope size == 879,126,179; TE2 metadata integrity valid under the
  basename `mango-rootfs.tar.gz`
- decrypt → exact 879,109,759 B plaintext; `gzip -t`; ≥ 66,836 tar entries
- all four failsafe entries plus `/etc/smack_permissive` present
- the hardened service and loader markers (`After=local-fs.target`,
  `Before=…ApplicationLauncher.service`, USB modprobes, raw scan)
- five `31 c0 c3` stubs in `app/bin/AppUpgrade` (SCOMO four + FUMO); two
  each in `app/bin/AppScomoDM` and `app/bin/AppDMClient`; two
  `b8 01 00 00 00 c3` stubs in `app/bin/AppLauncher` (eleven total)
- the patched `smack_setup.sh` carries the flag logic in both write sites
- every patched `.qm` file carries its locale's boot message

`09_preflight.sh` additionally runs `verify/verify_package.py` over both
package variants (stock diff, `.ver` rows 341174/341142, signed XML, class
`0xf5` wrapper, manifests).

## Pitfalls (all hit at least once during development)

- **Wrong basename on inspect:** `integrity_valid: false` almost always
  means you inspected `stage1.te2` instead of a `mango-rootfs.tar.gz`
  symlink (docs/02).
- **Compression ceiling exceeded:** the additions must compress under
  879,109,759 B; the ceiling check aborts with the overrun amount.
- **Trailing gzip bytes:** the genuine stream has 8 trailing bytes; keep
  them.
- **Append order and mtimes:** the four failsafe entries and the SMACK flag
  are appended in the script's order; `04_repack_rootfs.sh` fixes their
  mtimes to absolute epochs so the TE2 hash does not depend on the host
  timezone or checkout time.
- **exFAT cannot store ownership or mode:** `cp -a` and `rsync -a` abort on an
  exFAT mount (every `chown` is EPERM, root included), which is why
  `08_build_usb_image.sh` assembles the image itself with
  `--no-preserve=ownership,mode` / `rsync -rlDt` instead of delegating to the
  upstream builder. Keep timestamps: they cost nothing and match the package.
- **A `.ver` mismatch is a stop, not an edit:** the row is derived from the
  container, so a refused gate means the staged envelope is not the one the
  manifest describes. Correct the pin only from a measured official package,
  and re-record the affected output hashes.
- **Do not "fix" the loader's "ten" comments:** `run_stuff_loader.sh` is
  byte-frozen as part of the car-validated tar; editing either comment
  changes `out/stage1.te2` from `d1eec7cb…`. The guard actually re-applies
  eleven stubs; docs/06 records the discrepancy.
- **PyQt5 absent:** the `.qm` runtime check is skipped, not failed — the
  byte-level check still runs.

## What is NOT in this build

- No changes beyond the eleven stubs, the SMACK flag, the boot message, and
  the hardened failsafe: the boot images stay genuine (bootloader RSA
  valid) and the signed `.lge.upgrade.xml` stays stock.
- The AppDMClient/AppLauncher/FUMO stubs and the normal-USB path are
  car-validated (2026-11-15); see docs/08 and docs/17.
