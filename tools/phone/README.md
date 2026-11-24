# Phone rig

An alternative to the Pi rig in `docs/04`: a rooted Android phone used as the USB mass-storage
device, with the byte region served through a device-mapper splice whose table is swapped between
read passes. The concept is the one in `docs/01`; see `docs/21` for the full runbook, hardware
requirements, provisioning, arming procedure and evidence status.

## Contents

| Path | Purpose |
|---|---|
| `main.go`, `dm.go` | On-device detector and device-mapper control (static arm64 binary) |
| `go.mod`, `build.sh` | Build from source with a locally installed Go toolchain |
| `config.json.example` | On-device devices/geometry — all values are placeholders |
| `phone.env.example` | Host deploy / provisioning inputs — copy to `phone.env` |
| `.gitignore`, `.gitattributes` | Ignore owner-local files; force LF for shell scripts |
| `scripts/provision.sh` | One-time on-device setup: loops, dm device, gadget LUN with `ro=1` |
| `scripts/deploy.sh` | Host-side: push binary, config and scripts over ADB |
| `scripts/arm-live.sh` | On-device: arm the real flipping detector |
| `scripts/arm-dryrun.sh` | On-device: log-only validation — cannot change served bytes |
| `scripts/status.sh` | On-device: read-only status gates |
| `scripts/disarm.sh` | On-device: stop watchers, remove auto-arm flag |
| `scripts/restore-genuine.sh` | On-device: return to the single-segment genuine table |
| `scripts/common.sh` | Shared paths, sourced by the on-device scripts |
| `tests/bench-quiet-flip.sh` | On-device bench for both read shapes; no vehicle involved |

## Quick start

```sh
cp phone.env.example phone.env       # ignored by git; fill in your own hardware values
cp config.json.example config.json   # ignored by git; derive geometry from your own image
sh build.sh
sh scripts/deploy.sh
```

Then, on the phone under `su`: `sh provision.sh`. It writes resolved device values to
`provision.out` before binding the gadget (ADB will disconnect). Fill those into `config.json`,
run `sh restore-genuine.sh`, then `sh status.sh`. After that: `sh arm-dryrun.sh`; only after a
successful dry run use `sh arm-live.sh`. Finish with `sh disarm.sh` and `sh restore-genuine.sh`.

`phone.env` holds host deploy inputs (ADB identifier, image paths, gadget name, UDC); `config.json`
holds on-device device names, geometry and detector thresholds. See `docs/21` §2 and §4.

## Safety

- This tool changes what a USB mass-storage LUN serves. Use only on your own hardware, parked and
  off-road. See `docs/12`.
- `arm-live.sh` must be run with the phone disconnected from the PC: host enumeration can look
  like a read pass and consume the single flip credit.
- There is deliberately no boot-time auto-arm. A reboot mid-run must not restart a detector.
- The LUN must stay read-only (`ro=1`); `status.sh` checks it.
- The log-only mode is the safe way to validate timing on real hardware before any live run.
- A failed live-table readback is a stop: the binary refuses to report ARMED, and `arm-live.sh`
  only claims ARMED when `status.sh --live` passes all gates.
- `scripts/provision.sh` is new and not yet bench- or car-validated; see `docs/21` §12.

## Status

Car-validated on one phone/firmware/vehicle combination. See the evidence table in `docs/21`.
Do not treat it as portable across phones or regions without re-validating the hardware gates.
