# Wireless CarPlay validation status

**Snapshot:** 2026-11-19; live evidence is from the 2026-11-18 session.
Firmware `NE_E.EUR.S5W_L.001.001.260128` (Ioniq 5 Gen5W_L, EU). Branch
`carplay-vbtd51-broker`.
This file records only evidence that exists. It is deliberately stricter than
the design/status documents.

## Legend

| Label | Meaning |
|---|---|
| `LIVE` | Observed on the owner's car or phone in this session |
| `OFFLINE` | Built, statically checked, or fixture-tested off-car |
| `NOT VALIDATED` | No evidence yet; do not describe it as working |
| `BLOCKED` | Needs a prerequisite or owner decision |
| `WIP` | In progress or uncommitted; no evidence yet; never describe it as working |

“Offline PASS” never promotes a feature to `LIVE`. A method returning `0` is
not proof that bytes reached the phone. Transport evidence is recorded in
four separate steps (see [the ladder](#transport-evidence-ladder)); a higher
step is never inferred from a lower one.

## Matrix

| Area | Offline | Live | Evidence / caveat |
|---|---|---|---|
| Firmware identity (`vbtd-51`) | — | `LIVE` | Running `/app/bin/vbtd-51`, size `14117064`, SHA-256 `55e21d…cec9e0`; matches the pin |
| P1 RX-hook builder gates | `OFFLINE` | `NOT VALIDATED` | Exact preimage/span/hash gates PASS; hook not deployed |
| P1 RX bytes arrive on car | `NOT VALIDATED` | `NOT VALIDATED` | No patched `vbtd`; no `/run/wifi1b/carplay/rx.sock` traffic observed |
| Broker host helper/fixtures | `OFFLINE` | `NOT VALIDATED` | Host build, config/identity/security gates PASS |
| Broker target build/link | `OFFLINE` | `NOT VALIDATED` | glibc 2.25 ELF with XML validation enabled, `240960` bytes, SHA `bc89f525…cf92913`, GLIBC max `2.17`, NEEDED unchanged; not executed on car |
| Broker runtime on car | `NOT VALIDATED` | `NOT VALIDATED` | Not deployed |
| MFi helper target build | `OFFLINE` | `NOT VALIDATED` | Built against target rootfs; not deployed |
| MFi cert/signature from real chip | `NOT VALIDATED` | `NOT VALIDATED` | No auth socket/chip exchange observed |
| Default-off units/config | `OFFLINE` | `NOT VALIDATED` | Units not installed/enabled; example config fails closed |
| `ListenSPP(CAFF, …)` orchestration | `OFFLINE` client | `LIVE` | Returned `i 0` on the live adapter |
| SerialPort object registration | `OFFLINE` static chain | `LIVE` | Object `.../serial_00000000_deca_fade_deca_deafdecacaff` appeared during the connection; `status="connected"` |
| SerialPort UUID mapping | — | `LIVE` | Local and remote UUID both CAFF |
| TX ladder step 1: `SendData` method ACK | `OFFLINE` client | `LIVE` | `SendData(ay 1 0x00)` returned `i 0`; the stub ignores the vendor result at `0x613011` and builds the reply at `0x613038` |
| TX ladder step 2: vendor TX acceptance | `NOT VALIDATED` | `NOT VALIDATED` | No `VBTSppSendDataCfm` (or equivalent) log was recorded for the probe; step 2 is unobserved |
| TX ladder step 3: host RFCOMM emission | `NOT VALIDATED` | `NOT VALIDATED` | No btsnoop trace spans the probe; host writer is static-only (next row) |
| HCI snoop writer/ACL TX tap | `OFFLINE` | `NOT VALIDATED` | `BSA_Boot` `0x84ffe0` → `DispBtSnoopInit` `0x8a2260`; `DispHciAclData` `0x883ac0` runs inside the `bte_hcisu_send` `0x84f7b0` path before the queued send; inside the pinned `vbtd-51` (`55e21d…cec9e0`), no per-function pin |
| Snoop capture enabled on the unit | `NOT VALIDATED` | `NOT VALIDATED` | File is chosen at startup (`-b`/snoop config); opens `O_CREAT` + `O_TRUNC`; never toggled; see [capture constraints](DEFERRED-CAR-PROBES.md#2-spp-tx-evidence-ladder) |
| TX ladder step 4: byte-dependent peer reply | `NOT VALIDATED` | `NOT VALIDATED` | Needs the real iAP2 exchange (P1 RX) or an owned cooperative SPP peer; not attempted |
| RFCOMM byte delivery to phone | `NOT VALIDATED` | `NOT VALIDATED` | Ladder steps 2–4 unrecorded; a bare `0x00` need not answer |
| Receiver listener owner/scope on 7000 | `NOT VALIDATED` | `NOT VALIDATED` | `AirTunes/320.17.6` answered on 7000; owning PID and bind scope not recorded (D1) |
| Receiver DNS-SD preflight (Workstream A) | `OFFLINE` | `NOT VALIDATED` | Bounded lookup/policy/listener-owner/preflight code and ASan fixtures PASS; not run on the car or against a real AP advertisement |
| AP SSID/channel/security/link-local | `OFFLINE` parser | `LIVE` | Status showed `IONIQ5-CarPlay`, channel 6, WPA2-PSK, link-local |
| AP configuration agreement | `OFFLINE` | `NOT VALIDATED` | Root-private snapshot freshness, identity and PSK comparison fixture-tested; no proof of installed radio key |
| AP client join | `NOT VALIDATED` | `NOT VALIDATED` | `ap_clients:0` before and after the SPP probe |
| iAP2 identification/session | `OFFLINE` codec/session | `NOT VALIDATED` | No broker; stock `vbtd` does not forward RX |
| Wi-Fi bootstrap | `OFFLINE` codec | `NOT VALIDATED` | No AP client; `carplay_ready:false` |
| `_airplay._tcp` advertisement | `NOT VALIDATED` | `NOT VALIDATED` | No AP-side browse; TXT/SRV still uncaptured (D1) |
| Receiver identity corroboration | `NOT VALIDATED` | `NOT VALIDATED` | D1 requires exactly one candidate: `deviceid` = BT MAC+1 or pinned; `pi` hashed and pin-compared; nothing observed |
| Projection/AirPlay handoff | `NOT VALIDATED` | `NOT VALIDATED` | No receiver trigger or projection |
| Live Apple-USB gate | `OFFLINE` | `NOT VALIDATED` | Fixture tests block Apple/allow others/fail closed; car sysfs not probed |
| Wired CarPlay coexistence | `NOT VALIDATED` | `NOT VALIDATED` | Not exercised this session; gate is conservative |
| USB SSH preservation | `OFFLINE`/docs | `LIVE` | The session itself used USB SSH on port 2224 |
| BlueLink/modem routing | `OFFLINE` reviewed | `NOT VALIDATED` | Not exercised this session |
| Rollback/restore | `OFFLINE` docs | `NOT VALIDATED` | No deployment, so no rollback was exercised |
| Reboot persistence | `NOT VALIDATED` | `NOT VALIDATED` | No reboot test |
| End-to-end wireless CarPlay | `NOT VALIDATED` | `NOT VALIDATED` | **Do not claim it works** |

## Transport evidence ladder

`SendData` returning `0` is not delivery. Record four steps separately; each
has its own ceiling, and no step is inferred from the one below it.

| Step | Evidence | Ceiling | State |
|---|---|---|---|
| 1 | D-Bus `SendData` returns `i 0` | Stub reply only: `StackSerialPortStub::SendData` (`0x612e50`) ignores the vendor result at `0x613011` and builds the zero reply at `0x613038` | `LIVE` 2026-11-18 |
| 2 | Vendor request acceptance: `VBTSppSendDataCfm` (`0x490b60`) log or equivalent | The request reached the vendor layer; not proof that the phone received anything | `NOT VALIDATED` |
| 3 | Matching outbound RFCOMM UIH for the correct peer and DLCI in a stock HCI snoop trace | Host stack emission; the honest ceiling of a one-byte probe | `NOT VALIDATED` |
| 4 | A response that depends on the probe bytes | Peer receipt. With an iPhone this needs the real iAP2 exchange (P1 RX); an owned cooperative SPP peer proves the stack path only | `NOT VALIDATED` |

A bare `0x00` need not produce a reply, so step 3 never becomes step 4 by
itself. “RFCOMM delivery to phone” stays `NOT VALIDATED` until step 4 exists.

## What the 2026-11-18 live probe proves

- Stock `vbtd-51` contains a working SerialPort registration path:
  `VBTSppConnectCfm` → `SetSerialPortConnectionStatus(...,3)` →
  `AddSerialPortList` → CommonAPI registration.
- An external `ListenSPP` call with the CAFF UUID is sufficient to make the
  object appear when the phone is connected. The empty
  `SetSppListen`/`SetSppListenStatus` stubs are **not** registration gates.
- The object was observed **during** the connection, reached
  `status="connected"`, with local/remote UUID CAFF. An earlier absence was
  seen only after a connection had closed; it was not a registration failure.
- The `SendData` method is present and returns `i 0` for a one-byte payload:
  ladder step 1 only.

## What it does **not** prove

- That the vendor accepted the TX request (step 2) or that the host stack
  emitted a matching RFCOMM frame (step 3).
- That the one-byte `SendData` payload reached the phone over RFCOMM (step 4).
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
3. **RFCOMM delivery unproven:** the `SendData` ACK is ladder step 1; steps
   2–4 have no evidence and no snoop capture was taken.
4. **AP bootstrap not reached:** the phone has not joined `IONIQ5-CarPlay`;
   `ap_clients` stayed 0.
5. **Projection unresolved:** [static findings](STOCK-PROJECTION.md) identify
   stock receiver startup and the wireless control path; AP-side discovery,
   selection of the phone, authenticated setup, and first frame are unproven.
6. **Safety gates:** the Apple-USB scan is built and fixture-tested but not
   car-validated; the example keeps `usb_active=1` (manual block). A charging
   iPhone is intentionally treated as a blocker.
7. **PSK source:** the broker refuses a stale or mismatching daemon snapshot.
   The daemon reports its configured PSK, not an attestation of the running
   radio key. Successful association is still a separate gate.

## VM lab (2026-11-19)

Two Mode V runs passed on the same-firmware image (vendor `4.1.27-abl`,
260128 rootfs, read-only backing with a disposable overlay):

- `boot`: PASS, boot `6.913 s`, readiness `7.152 s`;
  `tools/vm/runs/1795045720012913559`.
- `vendor-systemd-dbus`: PASS, 7/7 checks (systemd PID 1, D-Bus socket and
  service before Multi-User, Multi-User reached); run
  `tools/vm/runs/1795046110569814948`. The expected `micommanager` failure
  remains, and `vbtd.service` did not start, so no SPP path ran.

The `tools/vm` unit suite ran 595 tests: 502 passed, 87 skipped, and exactly
the six reviewed environment-dependent reds — zero new failures. Scenario,
profile and `refs` contracts are clean.

Ceiling: VM substrate only. There is no radio, Bluetooth controller, `vbtd`,
RFCOMM peer, MFi chip, AP association or receiver DNS-SD in these runs. No
wireless-CarPlay gate is promoted. A fresh rootfs image build from the CIFS
staging tree is blocked because the share exposes files as mode 0664
(including `/bin/bash`); no improvised `chmod` was applied.

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
| `VBTSppSendDataCfm` (or equivalent) acceptance for the one-byte probe | TX ladder 2 | Phase 2 |
| Outbound RFCOMM UIH for the peer/DLCI in a btsnoop trace | TX ladder 3 | Phase 2 (capture prerequisite) |
| A reply that depends on the probe bytes | TX ladder 4 | Phase 3/4 |
| Listener owner/scope on 7000, AP-side `_airplay._tcp` SRV/TXT, one corroborated identity | Receiver (D1) | Phase 2b |
| Receiver preflight deterministic on stale/missing/fresh snapshots | Receiver (A8) | Phase 0/A8 |
| Broker `state:"running"`, no fake auth, AP readback fresh | Broker | Phase 4 |
| Phone joins AP; `ap_clients > 0`; broker session counters advance; daemon `carplay_session_observed` becomes true from a fresh root-owned broker status (`running`, receiver observed, AP on air, peer, `rx_packets > 0`) | Bootstrap | Phase 5 |
| Real MFi cert/signature bytes | Auth | Phase 4 |
| `_airplay._tcp` TXT/SRV and projection | P3 | Phase 2b/5 |
| Wired CarPlay/SSH/BlueLink after rollback | Regression | Phase 6 |

See [`TEST-PLAN.md`](TEST-PLAN.md) for the executable sequence and
[`REPRODUCTION.md`](REPRODUCTION.md) for the static/patch map. D1 is the
stock-only receiver identity probe in `TEST-PLAN.md` Phase 2b; the transport
ladder is defined above.
