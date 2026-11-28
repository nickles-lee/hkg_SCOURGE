# HUD Nav (`packages/hudnav`) — WIP

**Do not treat this as a finished, multi-car release.** This is the backend for
**Projection+** (`packages/projection`): it forwards Android Auto and CarPlay turn-by-turn
events to the factory cluster / HUD. It ships first-party C sources, host/VM tooling,
guard scripts, tests and metadata profiles — **no firmware, no OEM binaries, no car captures**.

## What it is

- `hudnavd` — the daemon. Reads the switch files and `settings.conf` under
  `/rw_data/hudnav`, normalizes turns/distances, applies rounding and units per source,
  and publishes status to `/tmp/hudnav-status.json`. It owns `/rw_data/hudnav`; the
  Projection+ UI only writes the switches and settings through `projection_daemon`.
- `src/aa_shim.c` — Android Auto `NavigationStatusCallback` interposer (gated to
  `AppProjection`, no `dlsym` dependency, no extra `NEEDED`, self-checking).
- `src/cp_shim.c`, `src/cp_hook.c` — CarPlay iAP2 logic model and production
  `read`/`write`/`open` hooks (self-gated on `iAP2Service-2.0`).
- `src/cp_rg.c` and friends — CarPlay Identification / RouteGuidance interceptor with
  hash gate, unique-signature gate, rel32 target check and rollback. This is the only
  code that patches call-site bytes; the gate is fail-closed.
- `src/output_can.c` — the micom/CAN output backend with a hard frame-ID write whitelist.
  Normal transmit is via a whitelisted helper; no raw CAN socket is opened.
- `tools/port_check.py` / `tools/profile_to_header.py` — host-only, stdlib-only
  portability tooling: derive a profile from *your own* stock files and generate the
  build-time header consumed by the C sources. See `tools/PROFILE_FORMAT.md`.
- `bootguard/`, `storeguard/`, `persist/`, `recovery/` — the safety ladder scripts
  (boot rollback, vendor-settings-store guard, optional persistent boot path, and
  one-command revert). **The persistent path is DISABLED by default** and is WIP.

## Validation status (honest)

Car-proven on **one** vehicle and firmware only: the owner's AU Hyundai Ioniq 5,
`NE_E.AUS.S5W_L.001.001.260319`, HUD fitted (2026-11-25…27 sessions). Host tests and
VM loader/replay proofs pass; several fields are still marked `PROVISIONAL` or
`INFERENCE` in the docs. Every other firmware, region, model, trim, AR/non-AR variant
and platform is **untested**. EU `260128` / EV6 `260128` exist only as pins/fixtures;
`S5W_M`, `ccNC` and `ccIC` are out of scope. See [docs/STATUS.md](docs/STATUS.md) and
[PORTING.md](PORTING.md).

## Safety ladder (never skip a rung)

1. **Host, offline:** `make -C packages/hudnav test` (C + Python + shell tests; tests
   needing stock firmware SKIP cleanly).
2. **VM, read-only where possible:** loader binding A/B with `LD_DEBUG=bindings`;
   stock controls alongside patched runs. The VM cannot prove a live phone event,
   HUD rendering or SMACK behaviour.
3. **Car, no TX:** read-only snapshot → firmware hash gate → `hudnavd-diag`
   (CAN TX not compiled in) → `hudnav-run.sh start dry`.
4. **Parked TX, owner present:** `hudnav-run.sh start tx --yes`, frame whitelist,
   kill-switch file, 4 s stale timeout, boot guard armed. Sweep one symbol at a time.
5. **Short drive / soak:** passenger-driven validation; then, and only then, consider
   the optional persistent boot path. Nothing system-wide stays on the car overnight
   before this passes.
6. **Restore:** `recovery/ssh_revert.sh` (hash-verified) and the CRLF-proof recovery
   stick. A failed gate, an `AMBIGUOUS` signature or an unexplained boot anomaly is
   a **stop**, not a workaround.

The global `/etc/ld.so.preload` route is documented but **banned as a persistent
install**. The default is a session-only, per-process `DT_NEEDED` bind mount. A
failed hash gate is always a stop.

## Build / test / port

From the repo root (WSL or any Linux host with `gcc`, `python3`, binutils):

```sh
make -C packages/hudnav test
python3 packages/hudnav/scripts/smoke.py
python3 -I packages/hudnav/tools/port_check.py --input <your-stock-rootfs> --report
```

Build outputs go to `packages/hudnav/build/` and `packages/hudnav/out/`, both
gitignored. Firmware-specific literals come from a profile selected with
`PROFILE=packages/hudnav/profiles/<id>.json`; the generator writes
`build/fw_profile.h`, which the two interposer sources include. Profiles contain
only hashes, offsets, strings and symbol names — no OEM bytes.

To port, follow [PORTING.md](PORTING.md), then the worked example in
[docs/hudnav/EU_260820_COMPARE.md](docs/hudnav/EU_260820_COMPARE.md). Design notes:
[CP_PROTOCOL.md](docs/hudnav/CP_PROTOCOL.md),
[AA_PROTOCOL.md](docs/hudnav/AA_PROTOCOL.md),
[HUD_NAV_REPORT.md](docs/hudnav/HUD_NAV_REPORT.md) and
[PORTABILITY_INVENTORY.md](docs/hudnav/PORTABILITY_INVENTORY.md).

## Relationship to Projection+

The single source of truth for the UI ↔ daemon ↔ backend contract is
[`../projection/design/BACKEND_CONTRACT.md`](../projection/design/BACKEND_CONTRACT.md),
unchanged by this package. Installation order with Tweaks / Wi-Fi / Projection+ and
restore interoperability are documented in
[docs/SCOURGE_INTEGRATION.md](docs/SCOURGE_INTEGRATION.md). This package does **not**
satisfy Projection+'s own owner requirement to build its tile patch from each car's
own files and integrate with `packages/compose.sh` — that is a separate gate.

Prior art is attributed (knowledge, not code) in [docs/CREDITS.md](docs/CREDITS.md).
No third-party source, firmware bytes or private captures are vendored.
