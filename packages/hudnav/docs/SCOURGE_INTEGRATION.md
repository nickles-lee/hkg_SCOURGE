# SCOURGE integration notes (WIP)

## Contract

The authoritative contract is
[`../projection/design/BACKEND_CONTRACT.md`](../projection/design/BACKEND_CONTRACT.md).
It is unchanged by the HUD Nav package. If code and contract disagree, change the code
or amend the contract in a coordinated PR — do not implement a second settings path.

Ownership:

- `hudnav` owns `/rw_data/hudnav` (daemon, shims, session kits, guards, logs, backups).
- `projection` owns `/rw_data/projection` and the home-screen overlays. It creates and
  removes the switch files and publishes merged status to the QML page; it never
  starts or stops overlays, sessions, AppProjection or iAP2Service.
- Installers and restorers must not cross-write. The Projection+ restore refuses when
  another package changed a recorded layout file; restore Projection+ before
  reinstalling Tweaks/Wi-Fi. Install/restore order with those packages is still an
  open integration item.

## compose.sh

Not integrated. The existing combo-stick order is
`battery → wifi → tweaks → wifi1b → migrate`. A `hudnav` entry needs a `stage.sh` /
`restore.sh` pair and a documented dependency order. Projection+'s owner requirement
(build the tile patch from each car's own files, then integrate with `compose.sh`)
is a separate gate that this package does not satisfy.

## Profile / hash gates

Stock sha256 pins, firmware IDs and hook offsets/signatures are published in
`profiles/*.json`; no OEM bytes are. `tools/profile_to_header.py` generates the
build-time header; the shipped interposers do not parse JSON or files at runtime.
Every patcher enforces the ELF hash pin and the unique-signature / rel32 checks from
the selected profile. A failed gate aborts — there is no bypass flag.

## Policy gates

- `leaks`: no absolute host paths, personal names/emails, car LAN addresses, GPS or
  street names. Prior-art attribution is public credit, not personal data.
- `spdx`: SCOURGE header on every new first-party code file; markdown/JSON exempt.
- `oem`: no firmware images, ELFs, QML bases, captures or decrypted material here.
- `refs`: in-tree pointers resolve; private evidence folders are not named.
- `ignore`: `build/`, `out/`, compiled binaries and captures are gitignored.

Unattended commits need the rule-11 independent review and a `Second-Opinion:` trailer.
