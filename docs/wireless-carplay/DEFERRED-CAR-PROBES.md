# Deferred car probes and blockers

These require the car or a live iPhone. §1 and §2 record the 2026-11-18
partial live result: CAFF SerialPort registration appeared and `SendData`
returned a stub ACK. §2 is an evidence ladder, not a delivery claim; RFCOMM
delivery, AP bootstrap and full wireless CarPlay remain unproven. Run §4 (D1)
as the first probe after the Phase 1 baseline: it is the highest-value
stock-only probe, needs no listener or phone association, and must precede
any projection work. Record results before changing any status in V23/V24 or
the release docs.

## 1. SerialPort D-Bus object during a live SPP connection (observed)

- Connect the iPhone to the CAFF service and keep the channel open.
- On the car session bus, list `org.bluez.SerialPort.SerialPort.v2_0_*`, get
  owners, introspect `SendData`/`onStatusAttributeChanged`, and check the
  object path `/dev_<peer>/serial_<uuid>`.
- The static chain is present: `VBTSppConnectCfm` -> `SetSerialPortConnectionStatus`
  -> `AddSerialPortList` -> `GeniviCommonAPIFactory::registerSerialPortStub`.
  `SetSppListen`/`SetSppListenStatus` are state-notification stubs, not
  registration gates.
- Before/while connecting, call the vendor adapter's `ListenSPP` with the CAFF
  UUID (interface `org.bluez.Adapter.Adapter.v2_0`; confirm the exact service
  and object path with `busctl tree`). Expect a `VBTSppListenCfm` log entry.
- If the object is still absent, check SPP service `0xa` enabled, a known/paired
  device, and a real `VBT_SPP_CONNECT_CFM`; a fallback binary patch is the last
  resort, not the first.
- **Live result 2026-11-18 (registration established):** with the phone
  connected, `ListenSPP(CAFF, SCOURGE-iAP)` returned `i 0`; the object
  `.../serial_00000000_deca_fade_deca_deafdecacaff` appeared **during the
  connection** with `status="connected"` and local/remote UUID CAFF. The
  earlier absence was seen only after a connection had closed, so it was not
  a missing-registration result. No registration patch was needed. The
  adapter stayed `discoverable=true`, `pairable=false`; the daemon
  `/carplay/pair` action reported Bluetooth control unavailable. The object
  persisted after `StopListenSPP` with `status="connected"`; it is expected to
  clear on phone disconnect. Delivery over RFCOMM is the open gate (§2).

## 2. SPP TX evidence ladder

`SendData` returning `i 0` proves only that the stub built a reply. Record the
ladder in order; each step has its own ceiling and none is inferred from the
step below.

1. **D-Bus ACK** — `SendData(ay 1 0x00)` returns `i 0`. Observed 2026-11-18.
   `StackSerialPortStub::SendData` (`0x612e50`) ignores the vendor result at
   `0x613011` and builds the zero reply at `0x613038`.
2. **Vendor request acceptance** — `VBTSppSendDataCfm` (`0x490b60`) log entry
   or equivalent. Not recorded for the probe. The stock log sink used for
   `[BSA]`/VBT lines in the 2026-11-18 session is readable.
3. **Host stack emission** — a matching outbound RFCOMM UIH for the correct
   peer and DLCI in the stock HCI snoop trace. This is the honest ceiling of a
   one-byte probe. Not recorded; no trace was taken.
4. **Peer receipt** — a response that depends on the bytes. An iPhone answers
   only in the real iAP2 exchange (needs P1 RX); an owned cooperative SPP peer
   can prove the stack path but not iPhone behavior. Not attempted.

Static chain: `StackSerialPortStub::SendData` (`0x612e50`) ->
`VBTSppManager::SendData` (`0x494c30`) -> `VBTSppSendDataReq` (`0x7294e0`) ->
`BrcmSpp::BrcmSppSendCmd` (`0x7879f0`).

**Capture constraints (offline static facts).**

- `BSA_Boot` ELF `0x84ffe0` calls `DispBtSnoopInit` ELF `0x8a2260`;
  `DispHciAclData` ELF `0x883ac0` runs inside the `bte_hcisu_send` ELF
  `0x84f7b0` path, before the queued send. These are inside `vbtd-51`, re-verified by `objdump` against the
  pinned whole-file SHA-256 `55e21d…cec9e0`; there is no separate per-function
  pin. No capture was taken and nothing was executed.
- `DispBtSnoopInit` opens its file with `O_CREAT|O_TRUNC` and exits the daemon
  if the open fails. The startup path can supply a filename (`-b`/snoop
  config); the in-binary pattern is `log_data/BT/vbt_snoop_<pid>.cfa`.
- Capture must start before the phone connects, or the RFCOMM SABM/PN
  negotiation is missing from the trace. Prefer an already-enabled trace;
  otherwise one controlled restart, with the Phase 1 stock backup in place and
  explicit owner approval. Never toggle snoop config silently on a live unit,
  and copy an existing trace before a restart truncates it.
- Read the btsnoop header's version and datalink fields before decoding; do
  not assume H4 vs USB. Then follow L2CAP signaling, RFCOMM SABM/PN, and UIH.
- Captures stay outside the repo; record lengths, frame types, DLCIs and
  hashes only.
- **Live result 2026-11-18:** `SendData(ay 1 0x00)` returned `i 0` (step 1
  only). No vendor-acceptance log was recorded for the probe and no RFCOMM
  capture was available, so steps 2-4 are unproven. AP clients remained 0;
  `carplay_ready` and `carplay_session_observed` stayed false; the Wi-Fi
  bootstrap was not reached.

## 3. SIG Test Mode listener (unverified on `vbtd-51`)

- The earlier expectation was that `VBTConfig` `VBT.GAP."SIG Test Mode": true`
  makes `VBTSppEnableCfm` call `Listen("BTSPP", 00001101-...)`. That is **not
  established** on `-51`. The cached Ghidra export only analyzed 252 of the
  real 0xd5a bytes (`readelf -sW`), and that prefix is logger-only; the rest
  of the function has not been decompiled. Do not conclude either "it calls
  Listen" or "it is log-only" from the cached text.
- Reanalyze the full `VBTSppEnableCfm` extent before running the probe. If a
  listener appears, it is generic SPP, not CAFF; the phone's CarPlay discovery
  uses CAFF. Do not expect this probe to create the CAFF transport.

## 4. D1 — stock receiver identity on the AP (run first)

Run before any projection work and before any persistent change. D1 is
read-only and deploys nothing: no receiver restart, no `AppProjection`
change, no config write.

1. **Listener owner and scope on 7000.** `/proc/net/tcp{,6}` must show a
   LISTEN socket on port 7000 (`0x1B58`). The bind address must be the
   wildcard or the receiver interface address; loopback-only is
   `listener-scope` and a stop. Resolve the inode to an owning PID (once, not
   per poll) and require an allowlisted stock owner (`AppProjection` by
   default). Unknown owner is `listener-owner` and a stop.
2. **AP-side `_airplay._tcp` SRV/TXT.** Browse from the AP side with a
   read-only client already present on the unit: the target-built passive
   probe if an approved earlier step left one there, or an existing browse
   tool. D1 installs nothing. If neither client exists, record
   `DNS-SD: unobserved` and stop before projection work; do not install a
   browser just for the probe. Record the instance count, SRV port,
   `srcvers`, `deviceid_match`, and `pi_sha256` + length.
3. **Beacon/IE readback.** Record how it was read (AP configuration readback
   or an on-air scan from an owner device). If neither is available, record
   `IE: unverified`; `app_info=1` is Interworking, not the Apple Device IE,
   and no IE is present without a readback.
4. **Exactly one corroborated identity.** `deviceid` must equal the head-unit
   Bluetooth address with the last byte incremented, or the pinned
   `device_id`; anything else is `deviceid_match=no` and a stop.
   `pi` must match `receiver_pi_sha256` when pinned. Hash `pi` in place and
   record only the hash, its length and `deviceid_match=yes|no`. Never print
   the raw `deviceid`, `pi`, BSSID or Bluetooth address.
5. **Stop conditions.** No advertisement, two different identities, unknown
   listener owner, loopback-only listener, `deviceid`/`pi` mismatch, or an IE
   claim without readback. A stop is a report, not a patch trigger; do not
   restart `AppProjection` or change DNS-SD scope on the basis of D1 alone.

Already observed on 2026-11-18: port 7000 answered as `AirTunes/320.17.6`
and `GET /info` returned `455 Method Not Valid In This State`; the listener
owner and the advertisement were not recorded. A port answer is not identity.

## 5. AirPlay receiver advertisement (background)

- `_airplay._tcp` TXT carries `deviceid`, `pi` and `srcvers` (`320.17.6`), but
  no `pk`; `pi` is the pairing identifier, not the public key. TXT alone is
  not proof of receiver identity.
- Determine whether the receiver advertises while no vendor iAP2 USB session
  exists, and whether a first-party session-start makes it advertise. That is
  the D1 probe in §4; do not repeat it without recording the §4 fields.
- The [static trace](STOCK-PROJECTION.md) shows registration using the
  listener's actual port and an optional mDNS interface index, with no USB
  session check in the traced segment. It does not prove DNS-SD delivery on
  the candidate AP.

## 6. MFi helper and chip

- Build/deploy the root MFi helper and its `/run/scourge-carplay-auth` socket.
  The default-off unit is `packages/wifi1b/artifacts/systemd/scourge-carplay-auth.service`;
  see `packages/wifi1b/artifacts/carplay/README.md`.
- Verify real certificate/signature responses; never fake AA00-AA05.

## 7. Stock wireless projection handoff

- Compare stock receiver advertisement on the AP before and after
  association, then observe controller discovery/selection, authenticated
  AirPlay setup and first frame separately.
- The [static trace](STOCK-PROJECTION.md) shows that the stock startup
  caller exists. Missing advertisement alone does not justify a patch.
  The full vendor iAP2 builder path remains NO-GO.

## 8. Wired baseline

- Confirm USB CarPlay and the receiver work before evaluating any wireless
  projection handoff.

## 9. Persistent deployment

- Temporary patched `vbtd` must use a stock backup and a verified USB SSH
  path. Persistent replacement only after owner approval and review.
