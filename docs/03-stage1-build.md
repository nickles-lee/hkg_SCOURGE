# 03 — Stage-1 firmware build (agent runbook)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128` (EU).

The stage-1 firmware is the modified rootfs delivered by the TOCTOU rig
(docs/01, docs/04, docs/05). It carries:

1. The **boot message** (config/message.txt) patched into the
   AppStandbyClock safety-disclaimer translations (length-preserving).
2. The **run_stuff failsafe**: `/etc/initial_root/{run_stuff_loader.sh,
   updater_guard.sh}` + `initial-root-failsafe.service` (docs/06).
3. The **ten updater/launcher stubs baked in** (build 4, four binaries —
   docs/08):
   - four validator entry points in `/app/bin/AppUpgrade` (`31 C0 C3`)
   - two UsbUpdateManager entries in `/app/bin/AppScomoDM` (`31 C0 C3`)
   - two button-gate entries in `/app/bin/AppDMClient` (`31 C0 C3`)
   - two `.sign`-gate entries in `/app/bin/AppLauncher`
     (`B8 01 00 00 00 C3` — true-on-success; prerequisite for a modified
     AppDMClient to survive a boot)
   The boot-time guard re-applies all ten (idempotent, size-gated; the
   boot-running binaries are patched with an atomic rename).

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
"$PYTHON" scripts/03_patch_payload.py # -> work/payload/{AppUpgrade,AppScomoDM,AppDMClient,AppLauncher}.patched + .qm
scripts/04_repack_rootfs.sh           # -> out/stage1.te2
"$PYTHON" scripts/05_verify_rootfs.py
```

Or everything at once (requires the full stock tree for the image):

```sh
scripts/build_all.sh                  # payload -> repack -> verify -> wrapper
                                     # -> packages -> 27 GB usb.img -> preflight
```

## The build is deterministic — your output MUST match

The chain (in-place tar surgery → fixed-order append → libdeflate L10 →
exact-size padding → TE2-encrypt with the stock envelope as metadata
template) reproduces byte-identical artifacts. **Compare every artifact
against this table; any mismatch means stop and investigate.**

| Artifact | Property | Known-good value (build 4) |
|---|---|---|
| `out/stage1.te2` | size / sha256 | 879,126,179 / `9f4e495ddfb9dfc85917517ba001f9059aa4cf135c6e87cca45d3a5248de8b09` |
| `work/stage1.padded.tar.gz` | sha256 | `97486ddf29cb499585ac2012f4185c44f4c94e9658a755773a0d8b5449223f14` |
| `work/payload/AppUpgrade.patched` | sha256 | `5d8422126e5df937dbd7e69aab4652ff8188640a229a6c6f9b674a9da008dc01` |
| `work/payload/AppScomoDM.patched` | sha256 | `b2467b2b7cdf18b4e470b36083867411cae3b9d047739959141b333e5fa1e596` |
| `work/payload/AppDMClient.patched` | sha256 | `e59259a16d3025d07a9f790a07e5eb4837df5cf69b05fcb2b3a64b281221fba5` |
| `work/payload/AppLauncher.patched` | sha256 | `d00efd1363dcaef2c3c5b3c5aefc01d0eea99dce84d60647ab036ce65113e2e0` |
| `out/AppUpgrade.patched.te2` | sha256 | `176d4a8c9dfb409f50231f90374b302773ed181fb01a3d2ded71b13e9363a3d0` |
| `out/package_second/IONIQ5_EU.ver` | sha256 | `2e2d305cb567364dc6364e02ce0265d22a2f6d1c022d6e39dd074f84d149371b` |
| `out/package_wrapper/IONIQ5_EU.ver` | sha256 | `1c7cb4b66489cdadd0d5216e95d5fa569d5ac6b8665ffe83e5a07cee1a32db97` |
| `config/message.txt` | sha256 | `a00dd663d6f3cb791dac9b7d3d674df59f97c0f42b615a981d20c256114abb33` |
| `out/usb.img` | size / sha256 | 28,894,603,490 / `1e0e13fd406e41c5b635230d80c22d86691ce8e787786f6fdea4920d69be0a0f` |

Envelope shape: class `0xe0`, IV `5412d68d2f22bf3d30f3c1ff1e715326`, body
879,109,759 B, tar entries 66,840 (genuine 66,836 + the four failsafe
entries).

## Verification gates

`05_verify_rootfs.py` prints `VERIFY: OK` only when all of these hold:

- envelope size == 879,126,179; TE2 metadata integrity valid under the
  basename `mango-rootfs.tar.gz`
- decrypt → exact 879,109,759 B plaintext; `gzip -t`; ≥ 66,836 tar entries
- all four failsafe entries present
- four `31 c0 c3` stubs in `app/bin/AppUpgrade`; two each in
  `app/bin/AppScomoDM` and `app/bin/AppDMClient`; two `b8 01 00 00 00 c3`
  stubs in `app/bin/AppLauncher` (ten total)
- both patched `.qm` files carry the boot message

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
- **Append order:** only the four failsafe entries are appended, in the
  order the script lists them (determinism).
- **PyQt5 absent:** the `.qm` runtime check is skipped, not failed — the
  byte-level check still runs.

## What is NOT in this build

- No changes beyond the ten stubs, the boot message, and the failsafe:
  the boot images stay genuine (bootloader RSA valid) and the signed
  `.lge.upgrade.xml` stays stock.
- The AppDMClient/AppLauncher stubs ARE part of this build (build 4) —
  VM-validated, car validation still pending (docs/08).
