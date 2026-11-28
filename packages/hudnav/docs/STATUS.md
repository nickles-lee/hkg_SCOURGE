# HUD Nav — component status (honest tiers)

Evidence tiers: **car-proven** (owner's 260319 AU car), **VM-proven** (real vendor
library in the vendor-kernel VM), **offline-only** (host test / static analysis),
**WIP** (unfinished). Nothing below is a multi-car claim.

| Component | Tier | Boundary / caveat |
|---|---|---|
| `hudnavd` conversion, motion, settings, TLV parser | offline-only | 169 C host checks + replay; LHT/RHT mirroring for turns is car-confirmed, angles for plain turns are physical/unmirrored |
| `src/output_can.c` `0x4CC` / `0x63E` / `0x4E8` frames | partly car-proven | F2 highway exits 0xA4/0xA5 (sweep 2026-11-28; 0x51/0x52 are overpass-diverge glyphs on 260319); direction, distance and `0x4E8` layout confirmed on 260319; `0x63E` lookahead fields, `0x4E8` B2/B3 and the approach-bar window remain PROVISIONAL |
| HUD JSON backend | WIP | stock `notifyBasicGuidanceUpdated_HUD(string)` JSON confirmed; consumer not identified; no HUD backend shipped |
| `src/aa_shim.c` vtable interposition | VM-proven + live event path car-proven | `dlsym@GLIBC_2.2.5` pin and no-extra-`NEEDED` gate are real fixes; next-turn argument order PROVISIONAL; global preload activation is not a persistent install |
| `src/cp_shim.c` iAP2 logic model | offline-only | Identify patch, ID registration, `0x5200` request, TLV parse and line protocol unit-tested; param-30 value PROVISIONAL |
| `src/cp_hook.c` production hooks | VM-proven (read binding) | `read@GLIBC_2.2.5` rebind proven against real `iAP2Service-2.0`; `write()` not runtime-tested; not hash-gated/deployed by default |
| `src/cp_rg.c` call-site patcher | car-proven (one car, a few days of owner test drives) + offline gates | hash gate, unique signature, rel32 target check and rollback; **not portable** — every pin/signature must be re-derived on a foreign build |
| `src/cp_read_shim.c` | VM-only | proof shim in `tests/vm/` context; logs/forwards stock bytes; never built into a shipped car artifact |
| CAN/micom `--micom-send` helper path | car-proven (one car) | the path hudnavd uses on the owner's car for every HUD frame; bench-tested with a fake helper |
| `tools/port_check.py`, `tools/profile_to_header.py` | offline-only | profile reproduces all hard-coded 260319 values; negative tests (corrupt → MISSING, duplicated → AMBIGUOUS) pass; needs stock files to run the acceptance test, otherwise SKIP |
| `bootguard/`, `storeguard/` | offline-only | shell tests with fake roots; car soak pending; boot guard two-strike rollback not car-measured |
| `persist/` (persistent boot path) | WIP, **DISABLED by default**; installed on the owner's car (a few days of test drives); `hudnav-premount` no-restart boot car-proven 2026-11-28 | requires explicit `--yes`, boot + store guards, and the Phase-4 week-long soak before any overnight use |
| `recovery/` | offline-only | hash-verified one-command revert and CRLF-proof stick tested against fake roots; car proof pending |
| `profiles/NE_E.AUS.S5W_L.001.001.260319.json` | offline-only metadata | 37 `found`, 1 `EXPECTED`-absent; verdict `CarPlay ready + AA ready` for host ELF hook points only — says nothing about CAN, HUD or your car |
| Projection+ tile / `projection_daemon` | separate package | see `../projection/docs/STATUS.md`; its installer/compose owner gate is **not** satisfied by this package |

## Support matrix

| Scope | Status |
|---|---|
| AU Ioniq 5 `NE_E.AUS.S5W_L.001.001.260319`, HUD fitted, owner's car | **Proven** (2026-11-25/26/27); some fields still INFERENCE |
| EU `260128` / EV6 `260128` | pins/fixtures only; no image, never car-tested here |
| EU `260820` | offline comparison only — see `hudnav/EU_260820_COMPARE.md`; **car-unverified** |
| Any other firmware, region, trim, brand or platform | **Untested. No claims.** |
| `S5W_M`, `ccNC`, `ccIC` | out of scope (different architecture) |

A profile is metadata derived from a stock image. It is not permission to ship that
image, a patch binary or someone's private drive log.
