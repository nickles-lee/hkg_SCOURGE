# 02 — Firmware package format (Gen5W_L)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware `NE_E.EUR.S5W_L.001.001.260128` (EU).
All hashes are pinned in `config/firmware_locks.env`.

## The official update package

An official USB update package (obtained from the OEM's update servers — see
`scripts/01_stage_inputs.sh`; SCOURGE never mirrors firmware) is a tree like:

```
IONIQ5_EU/
  .lge.upgrade.xml       signed manifest (TE2-encrypted XML)
  IONIQ5_EU.ver          per-file manifest: path|name|row-id|crc32|size|flag
  AppUpgrade             the USB updater wrapper (TE2 class 0xf5 container)
  HU/images/
    mango-rootfs.tar.gz  the system rootfs (TE2 class 0xe0, 879,126,179 B)
    ...                  other partition images
```

## TE2 containers

Firmware payloads are AES-128-CBC envelopes with a fixed trailing metadata
trailer (no OEM secrets involved — the content key is public community
knowledge, fetched by `scripts/00_fetch_third_party.sh`):

| Property | Value |
|---|---|
| Trailer size | `0x4024` (16,420 B) |
| Magic | `TE2` + a 1-byte class discriminator |
| Classes | `0xe0` rootfs (no valid outer RSA signature), `0xf5` AppUpgrade wrapper (RSA-2048/PKCS#1 v1.5 over `SHA256(IV ‖ ciphertext)`) |
| IV | first 16 B of `SHA256(basename + int32(body_size))` |
| Metadata integrity | `SHA256(IV ‖ metadata[36:]) == metadata[4:36]` |
| Rootfs container | 879,126,179 B total; body 879,109,759 B; IV `5412d68d…` under the basename `mango-rootfs.tar.gz` |

**The IV depends on the basename.** Always inspect/decrypt a container under
its HU-visible name (symlink it into a temp dir); inspecting
`out/stage1.te2` directly reports `integrity_valid: false` — that is the
format working as designed, not a corrupt build.

**The `0xf5` caveat:** the wrapper's outer RSA signature is computed over
the ciphertext, so re-encrypting a modified wrapper invalidates the OEM
signature. The rootfs class `0xe0` carries no such signature — which is why
the TOCTOU delivery (docs/01) and the stubbed updater (docs/08) are the
workable paths.

## `.ver` manifest

One line per file (header line first):

```
+|21393|NE_E.EUR.S5W_L.001.001.260128|HM|IONIQ5_EU|1489|1
IONIQ5_EU\HU\images|mango-rootfs.tar.gz|341174|<crc32-signed>|879126179|1
IONIQ5_EU|AppUpgrade|341142|<crc32-signed>|10803676|1
```

- Header fields: `+|package-id|version|brand|product|number|flag`.
  `brand` is `HM` (Hyundai), `KM` (Kia) or `GN` (Genesis); `product` is the
  package product (`IONIQ5_EU`, `EV6_MY_22_EU`, …). The numeric
  `package-id` is an HMG server counter, **not** a firmware identity: it
  changes between official repacks while every row stays identical.
- `crc32` is the signed 32-bit zlib CRC of the file.
- Row IDs used by SCOURGE: **341174** (rootfs), **341142** (wrapper).
- `tools/patch_ver.py` patches the rootfs row; `tools/patch_ver_row_id.py`
  patches any row by ID. The `.ver` is padded with newlines to at least the
  stock 2,467 B because the engine reads a fixed-size record area.
- **Do not use the `.ver` file hash as the identity check.** HMG's server
  serves the same rows under a different `package-id` counter (and therefore
  a different file size/hash) after a repack. `scripts/01b_audit_inputs.py`
  verifies the header identity and the consumed rows' CRC32/size, and reports a
  server-repack revision as a warning. A *row* that disagrees with its file,
  or with a locks pin, is an error.

## `.lge.upgrade.xml`

The RSA-signed manifest. **Never modify it** — a modified XML makes the
update unrecognizable (the button greys out). SCOURGE packages keep it
byte-identical to stock; `verify/verify_package.py` asserts that.

## Same-version updates

The car accepts a USB update at the *same* version as installed (with a
confirmation prompt). This is the SCOURGE deployment path: the TOCTOU run
and the post-root custom updates both present as same-version reinstalls of
260128.

## The exFAT driver is Tuxera `texfat` — plan for it

The car's exFAT support is the **Tuxera driver** (`kernel/fs/texfat.ko`),
not the kernel's native exfat. Consequences:

- `/proc/mounts` reports the filesystem type as **`texfat`, not `exfat`**.
  Any code that finds USB sticks by matching the fstype column (installers,
  daemons, log-dump paths) must include `texfat` in its accept list or the
  car's actual mount is rejected — usually **silently**. This exact bug
  shipped once: a `find_usb_dir()` accepting only
  `vfat|exfat|exFAT|ntfs` never found the stick on the car while passing
  every host test.
- Sticks are still *formatted* as ordinary exFAT (`mkfs.exfat`, label
  `IONIQ5_EU`); only the driver-side type string differs.
- The fstypes actually seen on the unit: `texfat` (exFAT sticks), `vfat`
  (FAT32), `ntfs`.
- texFAT cannot store hardlinks — package staging copies real files, and
  `build_payload.sh` warns about this.

## Compression (plaintext ceiling)

The OEM packs the rootfs tar.gz tighter than `gzip -9`; the SCOURGE build
uses libdeflate level 10 (concatenated 64 MB gzip members via the fetched
`chunkgz.py`) and pads with `truncate` to exactly 879,109,759 B. The
genuine gzip stream has 8 trailing bytes after the member — `surgery.py`
warns about and ignores them; do not strip them from the plaintext.

## Version strings

- Platform: **Gen5W_L** (Linux, x86-64). This is a different hardware and
  software family from S5W_M (Android) — nothing in this repo applies to
  S5W_M units.
- Firmware ID format: `NE_E.EUR.S5W_L.001.001.YYMMDD` (region `EUR`,
  model code `S5W_L`).
