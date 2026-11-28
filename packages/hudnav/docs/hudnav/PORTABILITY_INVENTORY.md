# Portability inventory (sanitized) — what a porter must re-derive (WIP)

Offline audit of the one firmware we have (`NE_E.AUS.S5W_L.001.001.260319`). Labels:
**CONFIRMED** = verbatim tool output / re-verified from the owner's own stock files;
**CAR-PROVEN** = observed on the owner's car; **INFERENCE** = reasoned, not observed.
Port risk: **High** = will almost certainly differ elsewhere; **Med** = may differ;
**Low** = stable convention (still verify). No OEM bytes are reproduced.

## 0. Headline

The original blocker was not any single offset: it was that every stock SHA256 pin and
hook offset was a literal in a script or C file. `tools/port_check.py` now derives a
JSON profile from the owner's own stock ELFs, and `tools/profile_to_header.py` injects
it at build time. The profile contains hashes, offsets, strings and symbol names only.

## 1. CarPlay (`iAP2Service-2.0`, 633,216 B, sha256 `e663d512…fa88b`)

- **Patch sites (High):** `P` list-builder prologue (64 B fingerprint, 0x1cf4a),
  two call sites in the Identification builder (0x22633 sent-id list, 0x22f60
  received-id list), dispatcher, encoder, packet ctor/dtor, send wrapper. Each
  required signature must match exactly once; a 12–16 B prefix alone is not unique
  for the longer functions, so the shipped lengths are 16–64 B.
- **PLT/GOT seams (High):** `_ZdaPv` (resolve by dynamic symbol, never a fixed PLT),
  `read` (inbound iAP2 seam), `write` (outbound seam).
- **Strings (Med):** `/dev/ttyIAP20` unique; `/dev/mfd-iap2`, `/dev/i2c-1`
  informational (multiple occurrences normal).
- **Facts (Med/High):** struct/object offsets (`+0x84/+0x88` fds, `+0x60` device path,
  vtable slots), `count=0x101d0` seam, SMACK/AT_SECURE behavior, glibc symbol versions.
  The shipped hooks avoid the struct offsets via `open()`/`read()` interposition.

## 2. Android Auto (`liblinux-head-unit.so`, `libreceiver-lib.so`)

- **Dynamic symbols (High):** eight `NavigationStatusCallback` / setter / endpoint
  mangled names with their `st_value` offsets. Only *defined* symbols count — an
  undefined vtable symbol with `st_value=0` would otherwise create a false ambiguity.
- **Build tag (Med):** `260312102711` identifies projectionprotocol 1.5 / 0.6.9-r3
  with legacy protobufs only; `NavigationNotification` is absent (expected).
- **Runtime (High):** AppLauncher has no per-app environment field; the `.sign` gate
  checks bytes, not env; the rootfs is glibc 2.25; global preload works but is banned
  as a persistent install.

## 3. Car/cluster facts (not derivable from the head-unit ELFs)

CAN IDs and frame layouts (`0x4CC`, `0x63E`, `0x4E8`, `0x4EA`, `0x4E9`, CAN-TP
`0x6E7`/`0x6DF`), AppManager ownership IDs, D-Bus names/paths, micom opcodes, HUD
presence/AR coding and traffic side must be captured on the tester's own car, read-only
first. Do not reuse 260319 values on another build. Publishing frame bytes/hashes is
fine; publishing GPS, road names, phone IDs or LAN addresses is not.

## 4. Prioritized porter flow

1. **P0** Generate a profile from your own stock files with `port_check.py`; read the
   OK / MISSING / AMBIGUOUS / DIFF matrix.
2. **P1** For every MISSING/AMBIGUOUS signature, re-derive a unique fingerprint and
   re-check the rel32 target. Hard stop if uniqueness cannot be achieved.
3. **P2** Enumerate `aa.sym.*` from your own `.dynsym`; add ABI variants; validate
   vtable slots on the live library before installing an override.
4. **P3** Discover process names, service units, device paths and app IDs instead of
   hardcoding them.
5. **P4** Build a car-confirmed D-Bus/CAN table; keep frame writes whitelisted.
6. **P5** Keep the safety architecture: hash gates, fail-open wrappers, rollback, and
   the `diag → dry → tx` ladder. Never loosen a gate to make a build pass.

A `.ver` revision whose header identity and consumed rows verify is a package-map
variant (warning), not a failed gate; a row, file or ELF that disagrees is a stop.
