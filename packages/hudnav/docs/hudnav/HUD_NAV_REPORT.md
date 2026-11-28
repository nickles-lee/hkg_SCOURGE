# HUD Nav design report — essentials (sanitized, WIP)

Condensed from the working investigation report; superseded where the later car notes
differ. Evidence labels: **CONFIRMED**, **CAR-PROVEN**, **INFERENCE**, **PROVISIONAL**.
No captures, coordinates, road names or private evidence paths are reproduced.

## 1. Architecture

`hudnavd` is a source-agnostic core: interposers normalize their source enums into a
common `hn_turn` and feed a Unix-socket line protocol (`SPEC.md`). The core applies
motion smoothing, distance glide (4 Hz, gain 0.8, back-step hold), per-source rounding
and units, then renders frames through a pluggable output backend. The CAN backend
emits the micom wire format observed on the proven car with a hard transmit whitelist.

## 2. Stock delivery path (260319)

Stock navigation runs in a separate native process and sends turn guidance to the
cluster over CAN via `SetCANMicomRecv`; the cluster/HUD consumes it. A factory HUD is
a separate hardware prerequisite; presence, trim, AR/non-AR and traffic-side coding
are car facts, not firmware facts. Stock AppNavi also sends HUD guidance as JSON via
`notifyBasicGuidanceUpdated_HUD(string)` — the consumer is not identified, so no HUD
JSON backend is shipped.

## 3. Decoded cluster channel (partly car-proven)

- `0x4CC` turn: type, side-street bits, direction in 7.5° units, little-endian metre
  distance. Direction and distance for the confirmed turns are car-proven; trailing
  bytes are still PROVISIONAL.
- `0x63E`: lookahead/approach-bar companion; its prefix and distance matched stock for
  two tested maneuvers, remaining fields provisional.
- `0x4E8` destination distance (tenths/unit nibbles, little-endian integer, flag) from
  CarPlay TLV `0x07`; layout confirmed, one byte's semantics provisional.
- `0x4EA` / `0x4E9` guidance-route flags, CAN-TP `F2` arrow list and `F0` text channel
  are implemented at the proven minimum; other codes are unverified.
- The stock "Calculating" state (`09`) is **not** emitted by us; a clear (`03`) is not
  an arrival. The end-of-route clear sequence was derived from stock frames.

## 4. Guidance ownership

AppManager broadcasts the owner (`110` CP, `111` AA, `30` native, `0` none on 260319).
Earlier prototypes let each shim assign ownership themselves, which produced stray
guidance-on and clear frames on native takeover. The current design takes ownership
only from the AppManager feed and requires a fresh maneuver after a change.

## 5. Conversion / state machine

- LHT/RHT: only roundabout exit angle and U-turn angle are traffic-side mirrored
  (measurement-confirmed); plain turn/slight/sharp/on-ramp angles are physical and
  unmirrored.
- Full CarPlay `EManeuverType` 0..53 table and the Android table are in `src/convert.c`.
- "Never icon 64", far-vs-near threshold, and the approach-bar window come from prior
  art; the AU bar window (~341–350 m) differs from the Android default (457 m), so it
  stays a tunable.
- Motion: lease + passed-maneuver expiry with one-time clear, link-stale clear, and
  guidance-ownership gating. All host-tested.

## 6. Validation boundary

Car-proven on one vehicle/firmware only (`NE_E.AUS.S5W_L.001.001.260319`, HUD fitted,
2026-11-25…27). The host replay covers the two confirmed maneuvers, native takeover,
release, stale projection input and loss of the owner feed. It cannot establish when
the HUD renders guidance or whether iAP2 carried visual navigation data. Roundabout
mapping, imperial units, the HUD JSON backend and lane/street-name features remain
offline-tested / car-unverified. Every other firmware, region, trim and platform is
untested. See `docs/STATUS.md` and `PORTING.md`.
