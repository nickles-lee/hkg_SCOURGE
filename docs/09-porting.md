# 09 — Porting: other firmwares, regions, and vehicles

SCOURGE is validated on exactly one target: **Ioniq 5 Gen5W_L (Linux), EU,
firmware `NE_E.EUR.S5W_L.001.001.260128`.** Everything firmware-specific is
pinned in `config/firmware_locks.env`, and every patcher aborts on any
mismatch. This document is the rulebook for extending to anything else.

## The ELF-hash rule (non-negotiable)

**Never apply a binary patch to a firmware package unless the stock ELF's
sha256 matches the hash the patch table was derived from.** A same-size
binary from a different build is NOT the same binary; offsets can shift
between builds even when the file size matches. The patch scripts enforce
this mechanically — never bypass the check, never "best-effort" a patch.

The same rule applies to data patches: `.qm` translation keys, `.ver` row
IDs, QML base files, CAN IDs, micom command arrays — all re-derived from
the new firmware's own content, never copied blindly.

## What is firmware/vehicle/region-specific

| Item | Where pinned | Porting action |
|---|---|---|
| AppUpgrade engine: 4 stub offsets + stock bytes + size + sha256 | `firmware_locks.env` | re-derive (docs/10) |
| AppScomoDM button gate: 2 stub offsets + stock bytes + size + sha256 | `firmware_locks.env` | re-derive (docs/10) |
| Embedded engine ELF offset (0x7C64 on 260128) | `firmware_locks.env` | verify with `tools/carve_engine.py --scan` |
| Rootfs container size/md5/IV, tar entry count | `firmware_locks.env` | recompute from the new package |
| `.ver` row IDs (341174/341142), stock `.ver` size/hash | `firmware_locks.env` | find rows by filename in the new `.ver` |
| Boot-message `.qm` keys + length budget (206 chars) | `firmware_locks.env` | re-read the new firmware's `.ts`/`.qm` |
| Fail-safe mount points (`/media/usb/<N>/…`), systemd, `/rw_data` | payload scripts | verify on the new rootfs |
| QML base hashes (stock/Hello/… variants) | package installers | register the new stock QML |
| CAN IDs (`0x2FC`, `0x326`, `0x036`), micom commands (`0x0C7`, `0x4ED`, `0x4A2`) | package daemons | confirm with `battery_daemon --sniff`; vehicle-specific |
| Vendor glibc (2.25) + PCL (7.1.1) for `tweaks_persist` | `build_target_helper.sh` | re-link against the new rootfs |
| Theme variant (`qml_eco3.0`, `-gui:3`) | docs/ui | re-check (new2.0/genesis differ) |

## The porting decision tree

```
New firmware / region / vehicle
└─► Gate 0 — platform: Gen5W_L Linux? (S5W_M/Android is a different
      architecture; this repo does not apply. No claims are made about
      other platforms, including vehicles whose update schemes are
      known to differ.)
└─► Gate 1 — acquire: nudl (-b hyundai -m <MODEL> -r <REGION> -v <VER>)
      or the official package; 01_stage_inputs.sh + a NEW locks section.
└─► Gate 2 — updater shape: strings-scan the new plain AppUpgrade for the
      two-pass command templates ("ionice -c3 md5sum %1 …",
      "DecryptToPIPE %1 | tar …").
      ├─ present, rootfs read in place → TOCTOU plausibly open; proceed
      └─ copy-to-local-then-verify instead → THE DOOR IS CLOSED; stop.
          Recognize the fixed patterns before burning a car session: see
          the upstream fix-patterns documentation via docs/13.
└─► Gate 3 — carve + hash: tools/carve_engine.py
      ├─ sha256 matches an existing locks section → fast path: reuse the
      │   pinned patch table as-is; rebuild via docs/03.
      └─ differs → docs/10 (Ghidra re-derivation)
└─► Gate 4 — package shape: TE2 decrypt integrity under the key, `.ver`
      discovery by filename, `.lge.upgrade.xml` present, rootfs paths
      (/app/bin/AppUpgrade, /media/usb, /rw_data, systemd).
└─► Gate 5 — data patches: .qm keys + message budget; .ver rows; QML
      base hashes; CAN/micom confirmation on the bench.
└─► Full rebuild via docs/03 with the new locks; every offline gate green.
└─► Bench unit first. Always. Then the car (docs/05), with the recovery
      plan.
```

## Adding a new target to this repo

Add a **new section** to `config/firmware_locks.env` (never edit the
260128 pins) with the new firmware ID → stock hashes → offsets → expected
bytes → `.ver` rows, and extend `firmware_locks.env` consumers to select by
target. Keep a compatibility matrix row in the README. If the new target's
stub offsets were derived with docs/10, record the derivation notes
(anchor strings found, call-convergence evidence) in the section comment so
the next person can re-verify.

## What we deliberately do not ship

- The USB automount command-injection finding (a separate vulnerability,
  unused by SCOURGE, withheld until/unless a porting need makes it
  relevant).
- Any patch that has not at least passed its offline + VM gates:
  car-unvalidated patch sets are clearly marked WIP everywhere they
  appear (the AppDMClient button gate + its AppLauncher prerequisite are
  VM-validated, baked into the build-4 rootfs, and marked car-pending —
  docs/08), never presented as finished.
