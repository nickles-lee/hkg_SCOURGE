# CarPlay iAP2 RouteGuidance — sanitized protocol notes (WIP)

Evidence labels: **CONFIRMED** = offline tool output or host test; **CAR-PROVEN** =
observed on the owner's 260319 car; **INFERENCE** = reasoned but not directly observed.
Every field marked PROVISIONAL must be re-derived on a foreign firmware. No OEM bytes
or private captures are included.

## 1. Message family (iAP2 message IDs, big-endian on wire)

| Msg | Name | Direction |
|---|---|---|
| `0x5200` | StartRouteGuidanceUpdates | HU → phone (we send it) |
| `0x5201` | RouteGuidanceUpdate | phone → HU (route state) |
| `0x5202` | RouteGuidanceManeuverUpdate | phone → HU (per-maneuver detail) |
| `0x5203` | StopRouteGuidanceUpdates | HU → phone |
| `0x5204` | RouteGuidanceLaneGuidanceInformation | phone → HU (lane arrows) |

Stock 260319 never asks for any of these. Two prerequisites are needed before iOS
emits guidance: add component `0x001E` / Identification param **30**
(RouteGuidanceDisplayComponent) to the outgoing `IdentificationInformation` (`0x1D01`),
and register message IDs `0x5200`/`0x5203` sent and `0x5201`/`0x5202`/`0x5204`
received in the SDK dispatch table. Only nav apps that set
`sourceSupportsRouteGuidance` (TLV `0x14`) send visual guidance; a `0` value means no
cluster TBT for that app.

## 2. Framing

- Link packet: `FF 5A` + len BE16 @+2, ctrl @+4, seq @+5, ack @+6, session @+7,
  header checksum @+8, payload, 1-byte trailing checksum (negated 8-bit byte sum).
- Session control message: `40 40` + len BE16 @+2 (whole message, min 6) + msgid BE16
  @+4 + parameters.
- Parameter TLV: 4-byte header (len BE16 including header, id BE16) + value.
- Always inject through the stock transport object, which owns sequence/ack/checksum.

`0x5201` TLVs used here: `0x01` state, `0x03` current road, `0x04` destination,
`0x07` distance to destination (metres), `0x0A` distance to next maneuver,
`0x0E` maneuver count, `0x13` source name, `0x14` source-supports-RG gate.
`0x5202` sub-TLVs: `0x01` index, `0x03` `EManeuverType` 0..53, `0x04` after-road,
`0x09` junction type (0 intersection, 1 roundabout), `0x0A` junction angle,
`0x0B` junction exit angle. `src/rgd_tlv.c` parses the whole message and fails open on
malformed input; integer widths remain PROVISIONAL pending a live capture.

## 3. Hook point and gates (260319 only)

`src/cp_rg.c` redirects two calls inside the Identification parameter-list builder to
a wrapper `P'` (identified by unique byte fingerprints and a rel32 target check):

- `cp.sig.site1` (messages sent by accessory): append BE16 `52 00` and `52 03`.
- `cp.sig.site2` (messages received from device): append `52 01`, `52 02`, `52 04`,
  then call the builder once more with paramId 30 and the route-guidance component
  group TLV (small conservative capacities).

`cp.sig.P` is a 64-byte fingerprint of the list-builder prologue. Every required
signature must match **exactly once**; `AMBIGUOUS` is a hard stop. The patch is applied
to a copy of the service binary at runtime, with an RX→RW→RX mprotect sequence and
rollback; no signed ELF is modified on disk. Activation is per-process and
session-only via a `DT_NEEDED` bind mount. All pins/signatures live in
`profiles/*.json` and are re-derived by `tools/port_check.py`.

## 4. Sending and receiving

Sending `0x5200` reuses the stock encoder/wrapper/sender functions and the same thread
context the vendor uses (the receive dispatcher handles `IdentificationAccepted`,
`0x1d02`). The caller frees the encoder buffer with `delete[]`; resolving `_ZdaPv` by
dynamic symbol (never a fixed PLT slot) is mandatory. Receiving uses the `read`
interposer in `src/cp_hook.c`: strip link headers with checksum verification,
reassemble `40 40 LEN` messages across packets, then hand whole messages to the TLV
parser. Unknown message IDs are logged and ignored by stock code (no session teardown).

## 5. Validation evidence

- Offline: full `EManeuverType` conversion table and TLV parser have host unit tests.
- VM-proven: real `iAP2Service-2.0` binds `read@GLIBC_2.2.5` to the proof shim with
  `LD_PRELOAD` and to libc without it; 128 real `read` calls were forwarded. The VM
  had **no phone link**, so no real iAP2 guidance bytes were observed and `write()` was
  not runtime-tested.
- Car-proven: parked CarPlay guidance on 260319 after the later fix; roundabout
  mapping, imperial units and param-30 encoding are still unverified.

The VM-only proof shim lives under `tests/vm/` and is never built into a shipped car
artifact. Public prior art is credited in `docs/CREDITS.md`.
