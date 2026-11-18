# Wireless CarPlay validation status

**Snapshot:** 2026-11-18, firmware `NE_E.EUR.S5W_L.001.001.260128`
(Ioniq 5 Gen5W_L, EU). Branch `carplay-vbtd51-broker`.
This file records only evidence that exists. It is deliberately stricter than
the design/status documents.

## Legend

| Label | Meaning |
|---|---|
| `LIVE` | Observed on the owner's car or phone in this session |
| `OFFLINE` | Built, statically checked, or fixture-tested off-car |
| `NOT VALIDATED` | No evidence yet; do not describe it as working |
| `BLOCKED` | Needs a prerequisite or owner decision |

“Offline PASS” never promotes a feature to `LIVE`. A method returning `0` is
not proof that bytes reached the phone.

## Matrix

| Area | Offline | Live | Evidence / caveat |
|---|---|---|---|
| Firmware identity (`vbtd-51`) | — | `LIVE` | Running `/app/bin/vbtd-51`, size `14117064`, SHA-256 `55e21d…cec9e0`; matches the pin |
| P1 RX-hook builder gates | `OFFLINE` | `NOT VALIDATED` | Exact preimage/span/hash gates PASS; hook not deployed |
| P1 RX bytes arrive on car | `NOT VALIDATED` | `NOT VALIDATED` | No patched `vbtd`; no `/run/wifi1b/carplay/rx.sock` traffic observed |
| Broker host helper/fixtures | `OFFLINE` | `NOT VALIDATED` | Host build, config/identity/security gates PASS |
| Broker target build/link | `OFFLINE` | `NOT VALIDATED` | glibc 2.25 ELF, `170208` bytes, SHA `57728611…ae1e`; not executed on car |
| Broker runtime on car | `NOT VALIDATED` | `NOT VALIDATED` | Not deployed |
| MFi helper target build | `OFFLINE` | `NOT VALIDATED` | Built against target rootfs; not deployed |
| MFi cert/signature from real chip | `NOT VALIDATED` | `NOT VALIDATED` | No auth socket/chip exchange observed |
| Default-off units/config | `OFFLINE` | `NOT VALIDATED` | Units not installed/enabled; example config fails closed |
| `ListenSPP(CAFF, …)` orchestration | `OFFLINE` client | `LIVE` | Returned `i 0` on the live adapter |
| SerialPort object registration | `OFFLINE` static chain | `LIVE` | Object `.../serial_00000000_deca_fade_deca_deafdecacaff` appeared; `status="connected"` |
| SerialPort UUID mapping | — | `LIVE` | Local and remote UUID both CAFF |
| `SendData` method ACK | `OFFLINE` client | `LIVE` | `SendData(ay 1 0x00)` returned `i 0` |
| RFCOMM byte delivery to phone | `NOT VALIDATED` | `NOT VALIDATED` | No RFCOMM/btsnoop capture; `i 0` is a stub ACK |
| AP SSID/channel/security/link-local | `OFFLINE` parser | `LIVE` | Status showed `IONIQ5-CarPlay`, channel 6, WPA2-PSK, link-local |
| AP client join | `NOT VALIDATED` | `NOT VALIDATED` | `ap_clients:0` before and after the SPP probe |
| iAP2 identification/session | `OFFLINE` codec/session | `NOT VALIDATED` | No broker; stock `vbtd` does not forward RX |
| Wi-Fi bootstrap | `OFFLINE` codec | `NOT VALIDATED` | No AP client; `carplay_ready:false` |
| `_airplay._tcp` advertisement | `NOT VALIDATED` | `NOT VALIDATED` | TXT/SRV still uncaptured |
| Projection/AirPlay handoff | `NOT VALIDATED` | `NOT VALIDATED` | No receiver trigger or projection |
| Live Apple-USB gate | `OFFLINE` | `NOT VALIDATED` | Fixture tests block Apple/allow others/fail closed; car sysfs not probed |
| Wired CarPlay coexistence | `NOT VALIDATED` | `NOT VALIDATED` | Not exercised this session; gate is conservative |
| USB SSH preservation | `OFFLINE`/docs | `LIVE` | The session itself used USB SSH on port 2224 |
| BlueLink/modem routing | `OFFLINE` reviewed | `NOT VALIDATED` | Not exercised this session |
| Rollback/restore | `OFFLINE` docs | `NOT VALIDATED` | No deployment, so no rollback was exercised |
| Reboot persistence | `NOT VALIDATED` | `NOT VALIDATED` | No reboot test |
| End-to-end wireless CarPlay | `NOT VALIDATED` | `NOT VALIDATED` | **Do not claim it works** |

## What the 2026-11-18 live probe proves

- Stock `vbtd-51` contains a working SerialPort registration path:
  `VBTSppConnectCfm` → `SetSerialPortConnectionStatus(...,3)` →
  `AddSerialPortList` → CommonAPI registration.
- An external `ListenSPP` call with the CAFF UUID is sufficient to make the
  object appear when the phone is connected. The empty
  `SetSppListen`/`SetSppListenStatus` stubs are **not** registration gates.
- The object reached `status="connected"`, with local/remote UUID CAFF.
- The `SendData` method is present and returns `i 0` for a one-byte payload.

## What it does **not** prove

- That the one-byte `SendData` payload reached the phone over RFCOMM.
- That iAP2 identification, authentication, Wi-Fi bootstrap or projection
  works.
- That the broker/MFi helper can run on the car, or that the RX hook works.
- That wired CarPlay, BlueLink or rollback still behave after a deployment.
- That the setup survives a reboot or a phone reconnect.

## Current blockers

1. **P1 not deployed:** stock `VBTSppReceivedDataCfm` does not forward inbound
   iAP2 bytes; the SerialPort interface is TX-only.
2. **Broker/MFi not deployed:** no first-party iAP2 state machine or real
   certificate/signature exchange on the car.
3. **RFCOMM delivery unproven:** `SendData` ACK is not a delivery receipt.
4. **AP bootstrap not reached:** the phone has not joined `IONIQ5-CarPlay`;
   `ap_clients` stayed 0.
5. **Projection unresolved:** `_airplay._tcp` advertisement and the
   AppProjection trigger are still unprobed.
6. **Safety gates:** the Apple-USB scan is built and fixture-tested but not
   car-validated; the example keeps `usb_active=1` (manual block). A charging
   iPhone is intentionally treated as a blocker.
7. **PSK source:** the broker still takes `ap_psk` from owner config, not an
   attested readback; it must match the daemon.

## Forbidden claims until new evidence exists

- “Wireless CarPlay works.”
- “Pairing/authentication/projection succeeded.”
- “`SendData` delivered bytes to the phone.”
- “The broker or MFi helper is active on the car.”
- “Wired CarPlay/BlueLink/rollback are unaffected” after a deployment.
- “The P1 hook is validated” based only on the offline builder.

## Next evidence required

| Evidence | Gate | Test-plan phase |
|---|---|---|
| Patched `vbtd` runs with SSH/BlueLink intact, stock backup present | P1 deployment | Phase 3 |
| `rx_packets`/`W1BSPP01` datagrams on a live CAFF connection | P1 RX | Phase 3 |
| RFCOMM capture of a bounded `SendData` payload | TX delivery | Phase 2/4 |
| Broker `state:"running"`, no fake auth, AP readback fresh | Broker | Phase 4 |
| Phone joins AP; `ap_clients > 0`; broker session counters advance (daemon `carplay_session_observed` is hardcoded false) | Bootstrap | Phase 5 |
| Real MFi cert/signature bytes | Auth | Phase 4 |
| `_airplay._tcp` TXT/SRV and projection | P3 | Phase 5 |
| Wired CarPlay/SSH/BlueLink after rollback | Regression | Phase 6 |

See [`TEST-PLAN.md`](TEST-PLAN.md) for the executable sequence and
[`REPRODUCTION.md`](REPRODUCTION.md) for the static/patch map.
