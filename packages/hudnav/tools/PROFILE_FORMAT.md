<!-- SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->
# HUD Nav firmware profile format (`hudnav-firmware-profile/1`)

`port_check.py` turns a directory of **stock** firmware files (or the decrypted
rootfs tarball itself) into a JSON profile of every firmware-specific value that
`docs/hudnav/PORTABILITY_INVENTORY.md` says the HUD Nav hooks
depend on. It is a *host-side, read-only* generator: no car, no network, no
binary is modified, and **no OEM binary is copied into the profile** — only short unique-match code fingerprints (see the signature catalog below),
hashes, offsets, strings and symbol names.

```
python -I tools/port_check.py \
    --input work/stock-plain.tar.gz \
    --output profile.260319.json

python -I tools/port_check.py \
    --input /path/to/extracted-rootfs --report
```

JSON has no comment syntax, so the SPDX headers are carried as the two
top-level fields shown above; every generated profile has them.

`--input` accepts either a directory (files may be nested, e.g. `app/bin/...`)
or a tar/tar.gz (the tool extracts only the needed members into a temporary
directory). `--firmware-id` overrides auto-detection from a `*.ver` header.
`--report` prints a Discord-pasteable OK / MISSING / AMBIGUOUS matrix instead of
JSON. Exit status is 0 for a completed analysis, 2 for a bad input path; the
verdict lives in the JSON, not in the exit code. Python 3 standard library only.

## JSON shape

```json
{
  "SPDX-FileCopyrightText": "2026 The SCOURGE Project contributors",
  "SPDX-License-Identifier": "AGPL-3.0-only",
  "schema": "hudnav-firmware-profile/1",
  "tool_version": "1.0.0",
  "generated_utc": "2026-11-28T00:00:00Z",
  "input": "/abs/path/to/rootfs",
  "firmware": {
    "id": "NE_E.AUS.S5W_L.001.001.260319",
    "version": "260319",
    "brand": "HM",
    "region": "AUS",
    "platform": "S5W_L",
    "detected_from": "/abs/path/2021_22_IONIQ_5_AU.ver"
  },
  "verdict": {
    "carplay": "ready",
    "android_auto": "ready",
    "overall": "CarPlay ready + AA ready"
  },
  "summary": {"found": 37, "missing": 0, "ambiguous": 0,
              "different": 0, "expected": 1, "total": 38},
  "items": { "<key>": { "<row fields>" } }
}
```

`items` is keyed by stable dotted names (`hash.*`, `cp.sig.*`, `cp.plt.*`,
`cp.got.*`, `cp.dynsym.*`, `string.*`, `aa.sym.*`, `aa.build_tag`,
`aa.probe.*`). Each row has:

| Field | Meaning |
|---|---|
| `category` | `hash`, `signature`, `dynsym`, `string`, `build_tag`, `feature_probe` |
| `status` | `found` (unique match / value present), `missing` (0 matches or file absent), `ambiguous` (>1 match where uniqueness is required) |
| `expected` | The 260319 value this row was derived from (hex string, int offset, or description) |
| `value` | The value actually derived from the supplied firmware; `null` if missing |
| `reference_match` | `true` iff `value == expected`. `false` means a *unique* match at a different offset/hash (portable-looking but needs RE review), not a failed scan |
| `source` | Where the hard-coded literal lives today (`src/...`, script, or research note) |
| `note` | Human explanation of the hook's purpose |
| `side` | `carplay`, `android_auto`, or `both` — drives the verdict |
| `required` | If `false`, informational only (evidence anchors, feature probes, boot-guard file hashes) |
| extra | `signature_hex`, `signature_length`, `match_count`, `rationale`, `got_offset`, `dynamic_symbol`, `library`, `parse_error`, `bytes`, … |

### Verdict rules

For each side, every `required` row must be `found` **and** `reference_match`
for `ready`. If all are `found` but at least one differs from 260319, the side
is `needs-review` (a different build: the signatures are unique but the
rel32/semantic checks have not been re-derived). Anything else is `not-ready`.
The overall string is one of `CarPlay ready + AA ready`, `CarPlay ready`,
`AA ready`, `unsupported (… review required)` or `unsupported`.

**A failed gate is a stop, not a workaround.** `ambiguous` is deliberately
treated as a hard failure: patching a call site with two identical byte
patterns present would be a wrong-site patch.

## Signature catalog and why each length was chosen

All signatures are byte slices from the 260319 AU stock `iAP2Service-2.0`
(sha256 `e663d512…fa88b`), taken at the hook point or around the function
prologue. Counts below are exact `bytes.find` match counts in that 633,216-byte
file, measured while building this tool; a signature is accepted only when the
count is 1.

| Item key | Offset | Bytes | 260319 hits | Why this length |
|---|---|---|---|---|
| `cp.sig.P` | `0x1cf4a` | 64 | 1 | Shipped fingerprint (`cp_rg.c:62-67`). 12 B gives 8 hits, 16 B gives 1; 64 B keeps margin across builds while covering the whole prologue/canary check. |
| `cp.sig.site1` | `0x22633` | 16 | 1 | Shipped `CPRG_SITE_BYTES` (`cp_rg.c:68-70`): 5-byte rel32 call + distinctive null-check/store tail. |
| `cp.sig.site2` | `0x22f60` | 16 | 1 | Same shape as site1 (`cp_rg.c:71-73`); paired send/receive sites. |
| `cp.sig.site3` | `0x1367c` | 16 | 1 | Shipped dispatcher-call fingerprint (`cp_rg.c:74-76`); includes the following jmp + byte compare so the rel32 target gate stays meaningful. |
| `cp.sig.dispatcher` | `0x13afa` | 16 | 1 | Minimal unique length 13 B; 16 B adds margin without embedding deeper rel32s. |
| `cp.sig.encoder` | `0x1d0a0` | 16 | 1 | Minimal unique 11 B; 16 B covers all argument stores. |
| `cp.sig.packet_ctor` | `0x1abd6` | 40 | 1 | Minimal unique 36 B (16 B repeats 46×); 40 B reaches past the generic canary prologue. |
| `cp.sig.send` | `0x152de` | 48 | 1 | Minimal unique 43 B (2 hits at 16-40 B); 48 B includes callee-saved regs + first argument use. |
| `cp.sig.packet_dtor` | `0x1ad46` | 40 | 1 | Minimal unique 39 B (16 B repeats 252×); 40 B clears the canary prologue. |
| `cp.sig.stock_sender` | `0x204e6` | 16 | 1 | Minimal unique; evidence-only anchor for the stock `0x5200` send sequence. |
| `cp.sig.new_array` | `0x1cfee` | 16 | 1 | Already unique at 8 B; 16 B keeps the null-check tail. Contains a rel32 call — informational, not a gate. |
| `cp.sig.memcpy` | `0x1d03d` | 16 | 1 | Same as `new_array`. |

The 64 B `P` fingerprint embeds one RIP-relative displacement; if a foreign
build changes that target the row becomes `missing` even though the prologue is
intact. That is intentional fail-closed behaviour: an engineer must re-derive
the fingerprint (and the rel32-to-`P` check in `cp_rg.c:320-327`) before use.

### Dynamic-symbol rows (no signature needed)

`aa.sym.*` names are read from the ELF64 `.dynsym` (section headers, `Elf64_Sym`
entries, linked `.dynstr`), parsed directly without `dlsym` or external tools.
Only **defined** symbols count — `liblinux-head-unit.so` also carries an
undefined `_ZTV24NavigationStatusEndpoint` at `st_value=0`, which would
otherwise create a false ambiguity. The `st_value` of each defined symbol is
exactly the `object+offset` printed by the `aa_shim.c:334-359` selftest.

`cp.plt.*` / `cp.got.*` are derived from `.rela.plt` plus the `.plt` `ff 25`
stubs. This is how the hard-coded `operator delete[]` PLT `0x4ef0`
(`cp_rg.c:56`) is recovered **by symbol**: GOT `0x299d58` ← `_ZdaPv` relocation
← PLT stub `0x4ef0`. `read` → GOT `0x299dd0` / PLT `0x4fe0` and `write` → GOT
`0x299a00` / PLT `0x4840` match `cp_shim.c:18-24`.

### Strings and tags

`string.dev_ttyIAP20` requires a unique occurrence (`0x73375`, `cp_shim.c:21`);
two occurrences set `ambiguous`. The micom devices (`/dev/mfd-iap2`,
`/dev/i2c-1`) are informational because multiple occurrences are normal.
`aa.build_tag` is the receiver build tag `260312102711` (17 occurrences in
`libreceiver-lib.so`, first at `0x1b0d55`), identifying projectionprotocol
1.5 (0.6.9-r3) with only legacy `NavigationStatus*` protobufs. The
`aa.probe.NavigationNotification` row records that this newer protobuf is
absent; absence is the expected 260319 state (`reference_match=true`, rendered
as `EXPECTED`).

## Consuming a profile at build time

`tools/profile_to_header.py` converts a profile into `build/fw_profile.h`, which
`src/cp_rg.c` and `src/aa_shim.c` include. This is a **host build step only**:
nothing in the shipped `.so` files reads a profile, a file or JSON at runtime, so
the existing fingerprint checks, rel32 validation and fail-open wrappers are
unchanged. `scripts/build.sh`, `scripts/make-car-kit.sh` and
`scripts/build-aa-diagnostic.sh` all accept `PROFILE=<json>` (default:
`profiles/NE_E.AUS.S5W_L.001.001.260319.json`):

```sh
PROFILE=profiles/<id>.json sh scripts/make-car-kit.sh out/car-kit-<id>
python3 -I tools/profile_to_header.py --profile profiles/<id>.json \
    --output build/fw_profile.h
```

Required rows (all must be `status: "found"`; a missing/ambiguous/malformed row
stops the build with a message naming it):

| Generated macro(s) | Profile row(s) |
|---|---|
| `CPRG_P_OFF`, `CPRG_P_FP`, `CPRG_P_FP_LEN` | `cp.sig.P` (`value`, `signature_hex`) |
| `CPRG_SITE1/2/3_OFF`, `CPRG_SITE1/2/3_FP`, `CPRG_SITE_BYTES` | `cp.sig.site1`, `cp.sig.site2`, `cp.sig.site3` |
| `CPRG_DISP_OFF` | `cp.sig.dispatcher` |
| `CPRG_ENC_OFF`, `CPRG_PKTC_OFF`, `CPRG_SEND_OFF`, `CPRG_PKTD_OFF` | `cp.sig.encoder`, `cp.sig.packet_ctor`, `cp.sig.send`, `cp.sig.packet_dtor` |
| `CPRG_DELARR_OFF` | `cp.plt._ZdaPv` |
| `HUDNAV_AA_SYM_*` (8 names) | `aa.sym.*` (`dynamic_symbol`) |

The three site fingerprints must have the same length (≥5 bytes, covering the
rel32 call) because `cp_rg.c` uses one `CPRG_SITE_BYTES`; the `P` fingerprint
must be ≥16 bytes. The exported interposer function names in `aa_shim.c` stay
literal — they are the shim's own ABI, not firmware data. A profile whose
signatures match a *different* build (`reference_match: false`) is still usable
only after the engineer has reviewed those rows; the generator never silently
substitutes the 260319 values.

## What is deliberately NOT in this profile

These inventory items cannot be derived from stock file bytes alone and are
therefore not signature/symbol rows:

* **Struct layouts / object offsets** (S1-S5: reader/writer fd at `+0x84/+0x88`,
  device path at `+0x60`, vtable slots `+0x10/+0x18/+0x20`, dummy vtable size
  16, `_ClusterData` fields). They are data-layout facts, not unique byte
  patterns; the shipped hooks avoid them via `open()`/`read()` interposition.
* **CAN IDs and frame layouts, D-Bus names, app IDs, micom opcodes** (D1-D7):
  car/cluster-format data, captured from the car, not from the head-unit ELF.
* **Runtime state and policy**: `count=0x101d0` seam hit, glibc 2.25 symbol
  versions, `AT_SECURE`/SMACK behaviour, `CAndroidAutoMain+0x80831` flag.
  These need a live process or the tester's own diag build.
* **Protocol constants** (iAP2 TLVs, `0x5200..0x5204`, `0x8001`): fixed by the
  protocol, not by the firmware build.
* **Host paths and line templates** (`ROOTFS`, the exact
  `LD_PRELOAD=` / `Environment=` revert lines): build/car environment, not
  firmware content.

They remain configuration/CAR-diagnosis items, as the inventory recommends.
The profile contains no firmware image, binary, or OEM-derived byte blob.
