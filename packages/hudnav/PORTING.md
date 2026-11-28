# Porting HUD Nav to another S5W_L firmware or car

Audience: an owner/porter with a **different** Gen5W_L (`S5W_L`, Linux/x86-64) firmware or model who
wants to reproduce or extend HUD Nav. This is a guide, not a claim of support.

## Status — read this first

- **Proven:** exactly one car and one firmware — the owner's **AU Hyundai Ioniq 5,
  `NE_E.AUS.S5W_L.001.001.260319`, HUD fitted, RHD/LHT**. Later 2026-11-25/26/27 notes record
  car-proven HUD display (CAN `0x4E8` + CAN-TP `F2` arrow), a working Android Auto event path,
  CarPlay guidance (Apple Maps) tested over a few days of owner drives, and the Projection+ tile and live status page on that car
  (changing settings from the page is not yet car-tested).
  See `docs/hudnav/HUD_NAV_REPORT.md`, `README.md`, and
  `packages/projection/docs/STATUS.md`.
- **Everything else is unverified.** EU/Kia `260128` identities exist only as pins/fixtures in
  `config/firmware_locks.env` and `config/fixtures/ev6-my22-eu-260128.env`;
  there is **no second firmware image** in this workspace to diff hook points against. No claim
  is made for any other version, region, trim, brand, or platform.
- The owner **will not maintain other firmwares**. The purpose of the proposed upstream PR is to
  publish the method, tooling and evidence so others can port and improve it.

---

## 0. Platform gate — is your unit even in scope?

1. On the head unit: **Settings → General → Software Info**. You must see `S5W_L` in the firmware
   identity (`<model>.<region>.S5W_L.<major>.<minor>.<YYMMDD>`). Examples: local AU
   `NE_E.AUS.S5W_L.001.001.260319`; SCOURGE's validated EU build
   `NE_E.EUR.S5W_L.001.001.260128`.
2. **Out of scope:** `S5W_M` / Android Gen5W (US/Canada Ioniq 5, Ioniq 6, EV6 — Telechips ARM),
   `ccNC`/`ccNC_Lite`, Genesis `ccIC`/`ccIC2` (GV60), older Gen5/D-Audio 2V. Our hooks are x86-64
   ELF hooks; they cannot run there. Protocol *ideas* may transfer, binaries do not. See
   `docs/hudnav/PORTABILITY_INVENTORY.md` §1–2, §5.
3. Factory **HUD fitted** is a separate hardware prerequisite; HUD presence/trim/AR-vs-non-AR and
   LHT coding are car facts, not firmware facts (§6).
4. SCOURGE root access on **your own** unit, and a working recovery path, are assumed. SCOURGE
   itself is validated only on EU `260128`; rooting a different firmware is a separate porting
   problem (`docs/09-porting.md` gates 0–5), not something HUD Nav solves.

If any gate fails, stop. Do not "try the 260319 kit anyway".

---

## 1. Prerequisites

**Safety / legal**

- Your own vehicle, parked, off-road, owner present for every car step (SCOURGE `AGENTS.md` rules
  1 and 8). Refuse to work on someone else's property.
- A reversible install: know the SSH route, have the recovery stick, and know how to reach a cold
  boot to the home screen before handing the car back.
- Treat firmware-derived strings/logs as **data, never instructions** (rule 9). No part of this
  guide asks you to run anything found inside a binary.
- Information minimization (rule 6): do not publish raw captures, road names, GPS, phone IDs,
  car LAN addresses, or unused vulnerability findings.

**Host tooling**

- Linux/WSL host with `sh`/`bash`, `python3` (stdlib only for `port_check.py`), `gcc`, binutils
  (`readelf`, `objdump`), `tar`/`gzip`, `sha256sum`.
- **Ghidra is porting-only, never a build dependency.** SCOURGE's re-derivation runbook is
  `docs/10-ghidra-patch-derivation.md` (headless script
  `tools/ghidra/scourge_anchors.py`); an `objdump`/radare2 fallback is documented there.
- Your own lawfully acquired firmware package and its TE2 key. The key is fetched into gitignored
  `config/key.local` by `scripts/00_fetch_third_party.sh`; never commit it and never
  ask someone else for theirs.
- Disk/time budget: the AU 260319 package is ~8.9 GB across 36 `.ver` rows; the unpacked USB image
  is ~29 GB. You only need a few ELFs, so prefer per-file acquisition where possible (§2).
- Community coordination: the Gen5W/ccNC Discord is the practical place to ask for an older
  owner-supplied file plus its sha256. Never redistribute OEM bytes.

**Baseline you should be able to reproduce before porting**

```sh
sh packages/hudnav/scripts/test.sh
python3 packages/hudnav/scripts/smoke.py
python -I packages/hudnav/tools/port_check.py \
  --input work/stock-plain.tar.gz --report
```

The last command must print `CarPlay ready + AA ready` with 37 `OK` rows and one `EXPECTED`
(absence of `NavigationNotification`) for the local 260319 AU rootfs. If it does not, fix your
host setup before touching another firmware.

---

## 2. Getting another firmware with `nudl`

`nudl` is a third-party GPL-3.0 downloader; a pinned copy lives at
`third_party/nudl/nudl` (fetched into gitignored `third_party/` by
SCOURGE's `scripts/00_fetch_third_party.sh`, pinned in `config/third_party.lock`). Its
`README.md` documents:

```sh
nudl list -b hyundai                       # brands: hyundai | kia | genesis
nudl list -b hyundai -m <MODEL>            # models / versions the server exposes
nudl download -b hyundai -m <MODEL> -r <REGION> -v <VERSION> -o <DIR>
nudl verify -d <DIR>                       # offline verification against the .ver rows
```

Important limits:

- **`nudl` only serves the current version per model/region.** The NU service does **not** provide
  old firmware versions (`firmware_landscape.md` §3.2, from `nudl/src/client.rs`). If your target
  version is older, do not expect to download it; ask an owner who still has the stick/archive,
  request only the specific files, and verify sha256 before use.
- Region codes are server-side; the AU server package's `.ver` identity says `AUS`
  (`2021_22_IONIQ_5_AU.ver`). Confirm `-r <REGION>` against `nudl list` output; **[UNSURE]** whether
  the pinned local `nudl` binary accepts the literal `AU` for `-r` — check before relying on it.
- A full download is unnecessary for a first compatibility check. If you only need `AppUpgrade`,
  `.ver`, or the signed XML, the NU API exposes per-file paths with HTTP Range support
  (`firmware_landscape.md` §3.2); this is a mechanism in `nudl`'s source, not a CLI flag.
  For the actual hook ELFs you need the rootfs container (`mango-rootfs.tar.gz`, ~879 MB TE2).
- Same-version repacks happen: a new `.ver` package-id counter changes size/hash while rows stay
  identical. `scripts/01b_audit_inputs.py` reports that as a **warning**; a row,
  file, or ELF that disagrees is a **stop** (SCOURGE `AGENTS.md` rule 4). Do not edit the package
  to silence a gate.

---

## 3. Extracting the files the checker needs

Use the existing scripts rather than ad-hoc unpacking:

1. Stage and audit the package:
   `scripts/01_stage_inputs.sh` (then read the output of
   `scripts/01b_audit_inputs.py`).
2. Decrypt the rootfs container:
   `scripts/02_decrypt_rootfs.sh`. It maps
   `input/mango-rootfs.tar.gz → work/stock-plain.tar.gz`, derives the TE2 IV from the HU-visible
   basename, loads the key from `config/key.local`, and gates on genuine md5/size plus `gzip -t`.
   The underlying tool is `third_party/ioniq5-usb-toctou/pizero/gen5w_decrypt.py decrypt`.
3. Extract **only** these members into a gitignored working directory (all are under
   `work/`, never in a tracked directory):

   | Member of decrypted rootfs | Why |
   |---|---|
   | `./app/bin/iAP2Service-2.0` | CarPlay patch sites, `read`/`write`/`_ZdaPv` seams |
   | `./app/bin/AppProjection` | Android Auto host, hash gate |
   | `./app/lib/liblinux-head-unit.so` | Exported `NavigationStatusCallback` virtuals |
   | `./app/lib/libreceiver-lib.so` | `NavigationStatusEndpoint`, `onChannelOpened`, receiver build tag |
   | `./app/bin/AppLauncher.sh` | Data-only preload line / boot-guard revert gate |
   | `./lib/systemd/system/carplay_iAP2.service` | CP unit preload line / boot-guard revert gate |
   | `*.ver` | Firmware identity (`--firmware-id` fallback if absent) — from the package root, not from inside the rootfs |

   Optional, for CAN field re-derivation only: the decrypted nav partition (`AppNavi`,
   `CANService`, `micommanager_userspace`) — see `docs/hudnav/HUD_NAV_REPORT.md` and
   `docs/hudnav/PORTABILITY_INVENTORY.md`. The AU nav tar is TE2-encrypted with the local key; the
   source tar is untouched and no OEM bytes are published.

`tools/port_check.py --input` accepts **either** a directory of those files **or** a
tar/tar.gz (it extracts only wanted members into a temp dir). When you pass a bare directory or a
decrypted rootfs tar with no `.ver`, firmware auto-detection reports `unknown`; pass
`--firmware-id NE_E.<REGION>.S5W_L.001.001.<YYMMDD>` explicitly. The checker never executes target
code and never writes OEM bytes into the profile (hashes, offsets, strings and symbol names only).

---

## 4. Running `port_check.py` and reading the matrix

From the `packages/hudnav` tree:

```sh
python -I tools/port_check.py \
  --input work/stock-plain.tar.gz \
  --firmware-id NE_E.AUS.S5W_L.001.001.260319 \
  --report --output profile.260319.json
```

- `--report` prints a Discord-pasteable matrix by side (CarPlay / Android Auto / Shared gates)
  and a totals line. Example for the known-good 260319 rootfs:
  `Totals: 37 OK / 0 MISSING / 0 AMBIGUOUS / 0 DIFFERENT / 1 EXPECTED-ABSENT (of 38)`,
  verdict `CarPlay ready + AA ready`.
- `--output` writes the full JSON profile (`hudnav-firmware-profile/1`). **The verdict lives in
  the JSON, not the exit code:** exit 0 means "analysis completed", 2 means "bad input path".
- Exit 0 does **not** mean "safe to flash". See §5 and §6.

Status vocabulary (`tools/PROFILE_FORMAT.md`):

| Tag in `--report` | Internal status | Meaning | Action |
|---|---|---|---|
| `OK` | `found` | Unique match, value equals the 260319 reference | Gate passed (for this ELF only) |
| `MISSING` | `missing` | Zero matches, or the file is absent | **Stop** for that hook; see §5 |
| `AMBIGUOUS` | `ambiguous` | More than one match where uniqueness is required | **Hard stop** — wrong-site patch risk |
| `DIFF` | `found` + `reference_match=false` | Unique match, but at a different offset/hash | `needs-review`: full RE before use |
| `EXPECTED` | absent-by-design (e.g. `NavigationNotification`) | Expected on 260319; not a failure | Record, don't fix |

Verdict semantics: a side is `ready` only if **every required row** is `found` *and* matches the
reference; all `found` but some `DIFF` gives `needs-review`; anything else is `not-ready`.
"Ready" means the **host-side ELF hook points** look like 260319. It says nothing about CAN frames,
app IDs, HUD rendering, or your car.

---

## 5. What to do for each MISSING / AMBIGUOUS class

Never loosen a gate, never take the first match, never "best-effort" a patch (rules 3–4). The
source of truth for the reference values is
`docs/hudnav/PORTABILITY_INVENTORY.md` (C*, A*, S*, G*, P*, D*, R* rows); the checker's catalog is in
`tools/port_check.py` and is documented in `PROFILE_FORMAT.md`.

| Row class | Typical `MISSING` cause | Re-derivation action |
|---|---|---|
| `hash.*` (6 stock files) | File absent, renamed, or genuine build drift | Check extraction/layout first. If the file is genuinely absent on your build, that side is unsupported. If the hash differs, that is expected for a different build — the ELF hash gate still must be *derived from your own firmware* and recorded; never reuse 260319 bytes. |
| `cp.sig.P`, `cp.sig.site1/2/3`, `cp.sig.dispatcher`, `cp.sig.encoder`, `cp.sig.packet_ctor`, `cp.sig.send`, `cp.sig.packet_dtor` | Code drift in `iAP2Service-2.0` | Follow SCOURGE `docs/10` with headless Ghidra (or `objdump`). Anchor on log strings / call convergence (`docs/hudnav/CP_PROTOCOL.md`, `docs/hudnav/CP_PROTOCOL.md`, `T21_cp_receive_path.md`, `CPA_send_notes.md`). Take a byte slice at the function prologue/call site; require **exactly one** hit in your binary; re-check the rel32 target points at the identified function. Update the profile catalog and re-run the checker. These are the only rows that patch code bytes — be conservative. |
| `cp.plt.*`, `cp.got.*`, `cp.dynsym._ZdaPv` | Symbol renamed/versioned, or stripped relocation table | Rebuild the table from `readelf -r`/`objdump -R` + `.dynsym` (`port_check.py` already does this generically). Resolve `_ZdaPv` by dynamic symbol; **never** hardcode a PLT slot at runtime. If the import is absent, do not emit that call path. |
| `aa.sym.*` | C++ ABI change (`std::__cxx11` vs older `std::string`), library renamed | Enumerate `.dynsym` for all `NavigationStatusCallback` / setter / `NavigationStatusEndpoint` mangled names. Add the ABI variant to the profile. Validate vtable slot index (`+0x10/0x18/0x20`) and dummy-vtable geometry from the live library before installing an override; if unknown, the shim must fail open. |
| `string.dev_ttyIAP20` | iAP2 device renamed | Find the actual `open()` path via `strings` + xrefs in your binary; carry it as a profile value and match by `ttyIAP*` prefix at runtime. `mfd-iap2`/`i2c` rows are informational. |
| `aa.build_tag` / `aa.probe.NavigationNotification` | Different receiver-lib generation | Dump the new build tag and protobuf class list; a *newer* library may expose lanes/destination distance (`NavigationNotification`). Treat as an extension opportunity, not a failure; record what your library does and does not expose. |
| `AMBIGUOUS` (any signature) | Chosen byte slice is not unique on your build | Extend the fingerprint into function-specific code or add a semantic rel32-to-target gate; re-run until the match count is exactly 1. If no unique signature exists, declare the hook unsupported — do not patch. |
| `DIFF` (unique, different offset/hash) | Different build, semantics plausibly preserved | Decompile the candidate and its callers; compare with the 260319 evidence; only then update the profile. A different offset is **not** proof the hook is correct. |

Profile flow (now shipped): `profiles/*.json` and the `PROFILE=<id>` build argument
are implemented. `tools/profile_to_header.py` writes `build/fw_profile.h`, which
`src/cp_rg.c` and `src/aa_shim.c` include; the build fails with a named error if a
required row is missing, ambiguous or malformed. The flow is: generate a profile from
your own firmware → select it with `PROFILE=profiles/<id>.json` → build the shims
against the generated header → verify the generated constants with `port_check.py`
again. The shipped 260319 profile is a **reference**, not a portable set of values:
do not copy its literals into a foreign build. See `tools/PROFILE_FORMAT.md`.

Things `port_check.py` deliberately does **not** cover (from `PROFILE_FORMAT.md` §"What is
deliberately NOT in this profile"): struct/object offsets, CAN IDs and frame layouts, D-Bus names,
app IDs, micom opcodes, glibc symbol versions, SMACK/AT_SECURE behaviour, and iAP2 protocol
constants. Those need the car or a live process (§6).

---

## 6. Items only your car can confirm — read-only capture first

Nothing in this section writes to the car. Use `recovery/car_snapshot.sh` at the start of
every session (read-only tar to stdout, scratch folder removed on exit), and keep captures local.

| Item | Why it matters | Read-only capture |
|---|---|---|
| CAN IDs / frame layouts (`0x4CC`, `0x63E`, `0x640`, `0x4E8`, `0x4EA`, `0x4E9`; CAN-TP `0x6E7` `F0/F2/F4`, `0x6DF`) | Cluster/HUD ECU is the portable layer, but layouts vary by trim/region/AR and LHT (`inventory.md` D4/D5) | Passenger-driven **stock-nav** session: capture `can0`/micom traffic (e.g. `candump -L`) and compare with `docs/hudnav/HUD_NAV_REPORT.md`, `stock_nav_frames_au_2026-11-25.md`. Do not inject. |
| Icon table / clear sequence | Wrong code can latch a spinner or leave the checkered flag on | Parked stock-nav symbol observation + later replay of a whitelisted frame set; `docs/hudnav/HUD_NAV_REPORT.md` lists still-unswept codes. |
| AppManager ownership IDs (260319: 110=CarPlay, 111=AA, 30=native) | Output must go silent while native nav owns guidance | Session-bus `dbus-monitor` with the match rules in `scripts/hudnav-run.sh:71-74` (bus address from `/tmp/session_appmgr`). Never reuse `110` as an AA fallback (inventory A10). |
| D-Bus names/paths/methods | Micom write path and ownership signal | `dbus-send --system --dest=com.lge.car.micom /Can org.freedesktop.DBus.Introspectable.Introspect`; session-bus monitor for `com.lge.hyundai.avn.appmanager /Information` (`navi_route_guide_application`) and `NameOwnerChanged`. Record exact strings in your profile; do not assume `com.lge.*` on other brands. |
| HUD presence / AR vs non-AR / LHT coding | Affects arrow animation expectations and whether the cluster forwards frames to the HUD | `AskHUDAbsentStatus` / `NotifyHUDAbsentStatusEvent` (presence only), trim/option coding, service manual, and visual observation. Stock `notifyBasicGuidanceUpdated_HUD(string)` JSON exists in `AppNavi`, but no HUD-guidance consumer was found in the 260319 rootfs — **the HUD drive path is still an open question**. |
| Micom opcodes (`SetCANMicomRecv=0x0400`, `NavigationTxMessageRequest=0x018F`) | Diagnostic only — our code calls D-Bus methods by name, not opcode | Both match EU 260128; TP opcodes are branch-dependent. Re-derive with `tools/micom/extract_commands.py` if needed. |
| SMACK / AT_SECURE / glibc version | Controls which activation route works | Read-only: `getconf GNU_LIBC_VERSION`, `getfattr -d`, `/proc/self/attr/current`, and the VM rehearsals in `docs/hudnav/AA_PROTOCOL.md`. Env `LD_PRELOAD` with `/` was blocked by AT_SECURE on 260319; global `/etc/ld.so.preload` works but must never remain overnight without the full guard/soak. |
| Real phone link (`count=0x101d0`, Identification acceptance, param 30) | CarPlay patch only | A real iPhone session on the car; the VM has no phone link. Keep the trace local. |

**Privacy:** car captures may contain GPS coordinates, street names, phone identifiers and LAN
addresses. `a private car-capture folder` is explicitly marked private. Redact
before sharing; publish only frame bytes, hashes, offsets and redacted logs.

---

## 7. Safe test ladder — do not skip rungs

1. **Host, offline.** `sh scripts/test.sh` (all host checks), `python3
   scripts/smoke.py`, `python -I tools/port_check.py --report`, static build
   `sh scripts/make-car-kit.sh`. Any red is a stop.
2. **VM, read-only where possible.** SCOURGE `tools/vm` lab (`docs/14-vm-lab.md`, WIP, not packaged
   in-repo) plus HUD Nav's `tests/vm/` scripts. Prove loader binding A/B with
   `LD_DEBUG=bindings`; prove signature scans find unique matches; run *stock controls* alongside
   patched runs. The VM cannot prove a live phone event, HUD rendering, or SMACK behaviour on a
   foreign build.
3. **Car, no TX.** Snapshot → upload kit → firmware hash gate (`cd / && sha256sum -c
   firmware.sha256`) → `hudnavd-diag` (CAN TX not compiled in) and `dbus-monitor` → diag logs →
   `hudnav-run.sh start dry` (logs the frames it *would* send). Confirm ownership gating and
   clear-on-off. End the session by reverting and doing one full cold boot to the home screen.
4. **Parked TX, owner present.** Only after 1–3 are green: `hudnav-run.sh start tx --yes`, with the
   frame-ID whitelist, kill-switch file, 4 s stale timeout and boot guard armed. Sweep symbols one
   at a time and have the owner read out the arrow/distance from the HUD. Never send trap /
   speed-limit frames in v1.
5. **Short drive / soak.** Passenger-driven validation, then (for a permanent install) the
   Phase-4 week-long soak from `docs/hudnav/HUD_NAV_REPORT.md`, checking boot time, `--:--` clock, settings store,
   and `dmesg` daily. Nothing system-wide stays on the car overnight before this passes.
6. **Restore.** `recovery/ssh_revert.sh` (one-command, hash-verified), then the CRLF-proof
   recovery stick `recovery/run_stuff.sh`, boot guard (`bootguard/`) and store
   guard (`storeguard/`). A failed gate is a stop, not a workaround.

A hash mismatch, an `AMBIGUOUS` signature, or an unexplained boot anomaly means **stop and
report**, not "try a different offset".

---

## 8. Submitting a new profile and evidence

A useful port contribution contains:

1. **Identity:** firmware ID from `.ver`, brand/region/platform, vehicle/model year, HUD option
   (AR/non-AR), traffic side, and which files were hashed (sha256 only — no OEM bytes).
2. **Profile JSON:** generated by `port_check.py` from *your own* stock ELFs, schema
   `hudnav-firmware-profile/1`, with `--firmware-id` set. Include the `--report` text. JSON is
   exempt from SPDX headers (SCOURGE `AGENTS.md`, Repo conventions).
3. **Evidence by tier:** host test output; VM run IDs and A/B binding logs; read-only car captures
   with GPS/street names/phone IDs removed; stock-nav frame-byte comparisons; any new Ghidra
   derivation notes (anchor strings, call-convergence evidence, uniqueness counts). Label every
   item `car-proven`, `VM-proven`, `offline-only`, or `WIP` (rule 10).
4. **Code changes:** keep hash gates, fail-open wrappers, rollback and the diag→dry→tx ladder
   intact; only change *where values come from*. Preserve existing SPDX headers; add SCOURGE's
   header (`SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors` /
   `SPDX-License-Identifier: AGPL-3.0-only`) to newly added first-party code files. Markdown,
   JSON and binary assets are exempt. Unattended commits need the rule-11 independent
   Flash-Max review with a `Second-Opinion:` trailer.
5. **Where:** propose `packages/hudnav/profiles/<firmware-id>.json` (or the agreed location — see
   `docs/hudnav/PORTABILITY_INVENTORY.md`, open questions) plus a short compatibility-matrix
   row. State plainly that the new firmware is **car-unverified** unless you actually drove it.

Remember: a profile is metadata derived from a stock image. It is not a licence to ship that
image, a patch binary, or someone's private drive log.

---

## 9. Explicit status statement

| Scope | Status |
|---|---|
| AU Ioniq 5 `NE_E.AUS.S5W_L.001.001.260319`, HUD fitted, owner's car | **Proven** (2026-11-25/26/27 car sessions; see `docs/hudnav/HUD_NAV_REPORT.md`). Some fields remain INFERENCE; see §6. |
| EU `260128` / EV6 `260128` | **Pins/fixtures only**; no image locally; HUD Nav never car-tested there. |
| CarPlay roundabout mapping, imperial units, HUD JSON backend, lane/street-name features | **Offline-tested / car-unverified** (`docs/hudnav/HUD_NAV_REPORT.md`, `README.md`). |
| Any other firmware, region, trim, brand, or platform | **Unverified. No claims.** |

---

## 10. Credit (prior art — knowledge, not code)

Per `docs/hudnav/PORTABILITY_INVENTORY.md`:

- **Open-HMG / HMG-API and the `hmgapi-wireless-cp-aa` wireless mod** (maintainer VysseCodes,
  AGPL-3.0): per-car/per-version hook maps, hash-pinned in-memory recipes, and the Car Report
  workflow are the methodological model. Their `docs/cluster-turns.md` and `docs/ar-hud.md`
  document the near-turn `DisplayType` table, 45° angle steps, ~457 m AR window, side-street bits,
  LHT roundabout codes, "never icon 64", reroute behaviour and the trap-frame/speed-limit conflict.
- **AwsomeFox** (HMG-API PR #75, AA AR turns), **hwjang1** (§47, CarPlay AR-HUD owner check on a
  Korean EV6 260902), and **KSubedi / Evergreen** (GPL-3.0-only, independent CarPlay-AR build) —
  car evidence and notes, credited without importing their code.
- Important caveat: HMG-API is **ARM `S5W_M` only**; its code cannot run on our x86 `S5W_L` unit,
  and its core README still says CarPlay on HMG-API itself is untested. The verified CarPlay AR
  results are from owner/derivative builds. It also never sends raw CAN; our micom-helper approach
  is a deliberate design contrast, not a contradiction of their findings.
- Broader prior art used by the parent project: **Numen / JacobPantuso** (E-GMP cluster/HUD CAN
  frame set), **dragz/egmpdbc**, **tylerharvey/Ioniq5_CAN**, **KSubedi/ultimate-wireless-projection**
  (Java-side maneuver semantics/runtime state machine), **SCOURGE's battery package** (car-proven
  `SetCANMicomRecv` pattern), **CloudPapi/ioniq5-usb-toctou**, **hkmcavn-tools**, **nudl**, and
  **HMG NEMESIS** for inspiration. See `docs/hudnav/HUD_NAV_REPORT.md` §12 for the full reference list.

Do not vendor any of these projects' source or firmware bytes. Quote behaviour and tables with
attribution only.

---

## 11. Uncertainties to verify before publishing

- [UNSURE] `nudl -r` region literal for AU in the pinned local binary; verify with `nudl list`.
- [RESOLVED] `PROFILE=<id>` / `profiles/*.json` are shipped in this package; see
  `tools/PROFILE_FORMAT.md`.
- [RESOLVED on 260319] Our CAN frames (`0x4E8`, CAN-TP `F2` arrow) drive the HUD directly on the owner's
  car; the stock HUD JSON path has no identified consumer and is not used. Other cars must re-check.
- [UNSURE] Non-260319 AA/CarPlay app IDs, D-Bus match rules, SMACK policy, glibc version, and
  cluster/HUD frame layouts — all must be derived on the tester's own car/firmware.
- [RESOLVED] Publishing stock sha256 pins, firmware IDs and hook offsets/signatures in
  profiles is acceptable (owner decision); SCOURGE already pins hashes.
