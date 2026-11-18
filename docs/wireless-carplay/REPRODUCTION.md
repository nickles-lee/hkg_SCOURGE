# Wireless CarPlay: stock gaps, first-party fixes, and the exact patch map

**Status: offline/build-only. Nothing in this document is car-validated.**
This is the reproduction companion to
[`VBT-SPP-HOOK-PORTING.md`](VBT-SPP-HOOK-PORTING.md) and
[`DEFERRED-CAR-PROBES.md`](DEFERRED-CAR-PROBES.md). It records what stock
Hyundai/LGE firmware does, what the first-party path replaces or supplies, and
the exact binary/function/address facts needed to reproduce the work with
Ghidra and REA. OEM binaries are read only; nothing here executes them.

Firmware: **NE_E.EUR.S5W_L.001.001.260128** (Ioniq 5 Gen5W_L, EU).
All addresses are **ELF virtual addresses (vaddr)**. Do not mix them with
Ghidra image addresses, file offsets, or runtime ASLR addresses.

## 0. Status legend

| Label | Meaning |
|---|---|
| `VERIFIED-STATIC` | Address, symbol size and/or bytes checked against the owner's binary with `readelf`/`nm`/`objdump` |
| `BUILT-OFFLINE` | Source builds and offline tests pass; not executed on the car |
| `DESIGN` | Patch/sequence proposed but not implemented |
| `DEFERRED` | Needs a car, iPhone, live bus, or chip probe |
| `CAR-VALIDATED` | On-car evidence exists. **No wireless CarPlay item currently has this label** |

## 1. End-to-end path and where stock breaks

```text
 iPhone
   |  Bluetooth discovery + CAFF SPP open
   v
 vbtd-51  VBTSppReceivedDataCfm  --(stock: payload not forwarded)-->  X
   |
   |  P1 hook (implemented offline)
   v
 /run/wifi1b/carplay/rx.sock  (root-only AF_UNIX SOCK_DGRAM)
   |
   v
 carplay-broker  -- iAP2 link/session/bootstrap/auth (TX blocked by P2)
   |                    |
   |                    +--> /run/scourge-carplay-auth/auth.sock
   |                              |
   |                              v
   |                         MFi helper -> real libCoreUtils.so + chip
   |
   +--> OEM session bus: Adapter.ListenSPP / SerialPort.SendData
                              |
                              v
                    vendor SerialPort object
                    (stock: not observed after close; during-connection
                     snapshot still missing)
                              |
                              v
                    RFCOMM TX to phone  <-- P2 unverified

 After Wi-Fi bootstrap:
 iPhone --Wi-Fi--> Wifi1b AP --AirPlay--> vendor receiver (AppProjection)
                                           stock: receiver not advertised/triggered
                                           P3 unresolved
```

The first-party broker is built to capture inbound SPP payloads and run the
iAP2 state machine. It cannot complete a real pairing until the vendor TX path
(or an equivalent first-party RFCOMM TX path) exists, and it cannot project
until the vendor AirPlay receiver is started and advertised. Both are separate
gates; do not conflate them with the RX hook.

## 2. Stock gaps we are filling

**Gap/fix summary**

| # | Stock gap | First-party fix | Status |
|---|---|---|---|
| G1 | SPP RX callback does not forward CarPlay payload | P1 `vbtd-51` RX hook -> broker socket | `BUILT-OFFLINE` |
| G2 | Registration chain exists; orchestration missing | P2 call `ListenSPP` with CAFF, verify object/TX; no patch preferred | `DESIGN` / car probe |
| G3 | Wireless capability flags are empty/dead | Broker bypasses the gate; no flag patch | `BUILT-OFFLINE` |
| G4 | No first-party iAP2 control plane or MFi RPC | Broker + MFi helper | `BUILT-OFFLINE` |
| G5 | AP passphrase/port are config-supplied, not attested | Fresh readback enforced; keep PSK/port in sync | `BUILT-OFFLINE` / PSK `DESIGN` |
| G6 | AirPlay receiver not advertised/triggered | P3 minimal AppProjection trigger (only if needed) | `DEFERRED` |
| G7 | Wired CarPlay coexistence not live-checked | Conservative Apple-USB detection + consent | `DESIGN` |
| G8 | No default-off deploy/rollback plumbing | Units + owner checklist, not auto-installed | `BUILT-OFFLINE` |

### What stock already does

This is not a "Hyundai shipped nothing" story. The pinned firmware already has:

- Bluetooth discovery and RFCOMM/SPP connection: the iPhone finds the car and
  opens the CAFF channel.
- The Wi-Fi AP, CarPlay IE and mDNS service flags in Wifi1b; the vendor
  AirPlay receiver binary and a listener on port 7000 (`AirTunes/320.17.6`).
- The vendor SerialPort/CommonAPI classes, the MFi library and the real chip
  behind `/app/lib/libCoreUtils.so`.
- A wired CarPlay path that must remain untouched (baseline re-verification is
  deferred).

The gaps below are missing **wiring, state and control-plane** pieces, not a
missing Bluetooth stack or a missing receiver binary.

### G1. The SPP receive callback drops the CarPlay payload

- **Evidence:** `vbtd-51` `VBTSppManager::VBTSppReceivedDataCfm(T_VBT_EVENT_BASE*)`
  at ELF `0x490ec0`, ELF size `0x355` (853 bytes), preimage SHA-256
  `2f3ba61377b73547026a634511ed238899c37ca06fdb111e0f3e1919e3493f06`.
- **Observed:** the iPhone discovers the car and opens the CAFF iAP2 SPP
  channel; stock accepts the connection but no first-party consumer receives
  the iAP2 bytes. The CarPlay spinner never completes.
- **Consequence:** no identification, no authentication exchange, no Wi-Fi
  bootstrap.
- **Fix:** **P1**, replace the whole `VBTSppReceivedDataCfm` span with the
  freestanding RX hook. `BUILT-OFFLINE`, not car-validated.

### G2. SerialPort registration exists; the missing piece is orchestration

The stock binary contains the full SerialPort registration and TX chain. The
empty `SetSpp*` callbacks are state-notification stubs, not registration
gates; they are not on the D-Bus object path.

- **Registration chain (static, verified):**
  1. D-Bus adapter `ListenSPP` -> `StackAdapterStub::ListenSPP` ELF `0x5cdc10`
     -> `VBTSppManager::Listen` ELF `0x493a40` -> `AdapterManager::IsServiceEnabled(0xa)`
     gate at `0x493aef` -> `VBTSppHandler::VBTSppListenReq` ELF `0x729740` ->
     `BrcmSpp::BrcmSppListenCmd` ELF `0x784680`.
  2. Phone connects -> `VBTSppConnectCfm` ELF `0x498240`, size `0x18f9` ->
     `DeviceManager::SetSerialPortConnectionStatus(..., 3 /*CONNECTED*/)` at
     `0x498714` -> `generateSerialPortPath` -> `IsAvailableSerialPort` ->
     `RemoteDevice::AddSerialPortList` ELF `0x48d920` ->
     `getSerialPortStub` (hash lookup only) and then factory registration;
     the exact `getSerialPortStub` -> `GeniviCommonAPIFactory::registerSerialPortStub`
     link is inferred (virtual/indirect, no direct call in the cached export).
     `registerSerialPortStub` ELF `0x633480` -> `StackSerialPortStub` ctor
     ELF `0x611fb0` -> `CommonAPI::Runtime::registerStub` at `0x633743`.
  3. TX: `StackSerialPortStub::SendData` ELF `0x612e50` ->
     `VBTSppManager::SendData` ELF `0x494c30` (gate
     `GetSerialPortConnectionStatus(addr,uuid) == 3` at `0x494cca`) ->
     `VBTSppHandler::VBTSppSendDataReq` ELF `0x7294e0` ->
     `BrcmSpp::BrcmSppSendCmd` ELF `0x7879f0`.
- **State stubs:** `SetSppListen` `0x3b4860` (called from `VBTSppEnableCfm`
  at `0x495d1c`), `SetSppListenStatus` `0x3b4890` (called from
  `VBTSppListenCfm` at `0x49510e` and `VBTSppListenStopCfm` at `0x49554d`),
  and `SetSppDisconnectedStatus` `0x3b48c0` (called from
  `VBTSppDisconnectCfm` at `0x496fbe`) feed a
  `ConnectionStateManager` listener, not the D-Bus registration.
- **Observed:** no `org.bluez.SerialPort.SerialPort.v2_0_*` object after an
  earlier connection had closed; a snapshot **during** a live connection is
  still missing. Because the chain is present, the object is expected to
  appear while the connection is live and to disappear on disconnect.
- **Consequence:** the likely gap is orchestration: nothing calls `ListenSPP`
  with the CAFF UUID, and/or SPP service `0xa` is not enabled. If the object
  appears during the connection, **no registration binary patch is needed**:
  the broker's existing `ListenSPP`/`SendData` client can use it.
- **Important limit:** the `SerialPort.v2_0` interface is **TX-only**. The
  introspection XML has no data-received signal and `VBTSppReceivedDataCfm`
  is log-only, so P1 is still required for inbound iAP2 bytes.
- **Fix:** **P2**, call `ListenSPP` with the CAFF UUID and verify the object
  during a live connection (see `DEFERRED-CAR-PROBES.md` §1-2). Only if the
  service gate or device lookup is dead, patch the orchestration; do not fill
  the `SetSpp*` stubs for registration. `DESIGN` / car probe.

### G3. Wireless-capability flags are dead ends

- **Evidence:**
  - `PersistenceInterface::GetSupportWirelessCP()` ELF `0x6824c0`,
    size `0x2e` — returns the zero value; no callers in the cached export.
  - `PersistenceInterface::onSupportWirelessCPChanged()` ELF `0x6823d0`,
    size `0x2e` — empty.
  - `PowerStateManager::onSupportWirelessCPChanged(bool const&)` ELF
    `0x6ba4e0`, size `0x2e` — empty.
  - `ProjectionDeviceStateManager::onSupportWirelessCPChanged(bool const&)`
    ELF `0x3b4610`, size `0x2e` — empty. Several identical empty listeners
    (including `onSupportWirelessAAChanged`, `onLastPositionChanged`,
    `onUserProfileKeyChanged`) are folded to this same address by
    identical-code folding; the address is the stub, not a unique body.
  - `ENABLE_WIRELESS_CARPLAY` is parsed by the VBTConfig loader
    (`FUN_00439280`, ELF `0x339280`) into the config map, but no consumer was
    found in the analyzed image. `VBTConfig::getSIGTestMode()` ELF `0x5b8ae0`
    (ELF size `0x41f`; Ghidra analyzed only 65 bytes) has no SPP-manager
    caller in the cached caller list.
- **Consequence:** setting the JSON flag or forcing `GetSupportWirelessCP` is
  not an activation path. It does not create a listener, a SerialPort object,
  or an AirPlay receiver.
- **Fix:** the first-party broker bypasses the capability gate and owns the
  control plane. Do **not** patch `GetSupportWirelessCP` to `true` as a
  shortcut; it is not sufficient and can mislead vendor state machines.

### G4. No first-party control plane or MFi helper

- **Evidence:** stock has no process that speaks the first-party iAP2
  link/session/bootstrap path, and no `/run/scourge-carplay-auth/auth.sock`.
  The MFi library (`/app/lib/libCoreUtils.so`, SHA-256
  `9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae`) and
  chip exist, but nothing invokes them for a first-party session.
- **Consequence:** even with RX and TX, there is no bounded certificate or
  signature exchange, and no AP/identity/consent policy.
- **Fix:** new first-party components, not OEM patches:
  - `carplay-broker` (`packages/wifi1b/src/carplay/cp_broker_main.c` and the
    broker modules) — supervised helper; iAP2 link/session/bootstrap, AP
    readback, SPP D-Bus client, auth RPC.
  - `carplay-native-auth` (`packages/wifi1b/src/carplay/cp_native_auth.c`) —
    root-only MFi RPC against the real pinned library/chip.
  - Default-off systemd units and owner config:
    `packages/wifi1b/artifacts/systemd/` and
    `packages/wifi1b/artifacts/carplay/README.md`.
  All are `BUILT-OFFLINE`; none are installed or activated.

### G5. AP readback freshness is enforced; the PSK is not attested

- **Evidence:** the Wifi1b daemon brings up the AP, CarPlay IE and mDNS
  service flags and writes `/rw_data/wifi1b/status.json`. The broker requires
  `observations_fresh`, `ap_active` and `ap_network_ready`, then refreshes
  `ap_ssid`, `ap_channel`, `ap_security` and `ap_linklocal` from the readback.
  `cp_bootstrap` refuses the WiFi offer until `AP_READY` and `readback_valid`
  are set, so a stale/off-air status cannot start the offer.
- **Residual gap:** the AP **passphrase** (`ap_psk`) and AirPlay port are
  config-supplied; `status.json` is not used as a trust anchor for them. If
  `ap_psk` does not match the daemon, the phone fails the join. Keep the
  owner config in sync; do not treat the readback as attesting the PSK.
- **Fix/status:** freshness gate `BUILT-OFFLINE`; PSK/port attestation remains
  `DESIGN`. The example config leaves identity/AP values empty and sets
  `usb_active=1`, so it fails closed until the owner fills it.

### G6. AirPlay receiver advertisement/projection is not wired

- **Evidence:** port 7000 listens (`AirTunes/320.17.6`), but `GET /info`
  returned `455 Method Not Valid In This State`; `_airplay._tcp` TXT/port was
  not captured. The vendor receiver is hosted by `AppProjection` and started
  through the vendor iAP2Service USB-session chain. `AppProjection` imports
  `CarPlayAppFrameworkStart` (PLT stub at ELF `0x17b00`; `JUMP_SLOT`
  relocation at `0x435d28`), but no caller was identified in the cached
  export. The export's `0x439f48` entry is outside every loadable segment and
  is an analysis artifact, not code.
- **Consequence:** a completed first-party pairing may still not make the
  receiver advertise or accept AirPlay; the projection handoff is a separate
  gate.
- **Fix:** **P3**, only if the first-party session does not make
  `_airplay._tcp` appear. The fallback is a minimal `AppProjection` trigger
  patch, not the vendor iAP2 builder. `DEFERRED`; no patch address is proven.

### G7. Wired CarPlay coexistence is not fail-closed yet

- **Evidence:** the broker policy callback currently returns a static
  `usb_active` config value. That is a bench override, not a live check.
- **Consequence:** if a wired CarPlay session is active and the phone does not
  advertise wired availability in the iAP2 exchange, a static `usb_active=0`
  could allow a wireless offer.
- **Fix:** detect Apple USB enumeration (conservative: any Apple USB device
  blocks wireless) and require `consent=1`; fail closed. `DESIGN`.

### G8. Deployment/rollback is manual and default-off

- **Evidence:** the patched `vbtd`, broker, MFi helper and units are not
  installed by `build_payload.sh`.
- **Consequence:** no accidental activation, but also no one-click deploy.
- **Fix:** default-off artifacts plus the owner checklist in
  `packages/wifi1b/artifacts/carplay/README.md`; temporary `vbtd` replacement
  only with a stock backup and verified USB SSH. `BUILT-OFFLINE` plumbing.

## 3. Patch inventory

### New first-party components (not OEM patches)

These implement the control plane that stock does not wire up. They are
ordinary first-party binaries, not patches to Hyundai/LGE code.

| Component | Responsibility | Status |
|---|---|---|
| `cp_wire` | Bounded iAP2 wire codec, identity fields, WiFi config messages | `BUILT-OFFLINE` |
| `cp_link` | Link layer: sequencing, ACK/retransmit, bounded reorder | `BUILT-OFFLINE` |
| `cp_bootstrap` | Identification, WiFi bootstrap, consent/USB policy, auth state machine | `BUILT-OFFLINE` |
| `cp_session` | Session lifecycle and wire/link/bootstrap integration | `BUILT-OFFLINE` |
| `cp_runtime_broker` | Supervised runtime: policy gates, SPP attach, RX/TX pump, restart | `BUILT-OFFLINE` |
| `cp_spp_dbus` | Calls OEM `Adapter.ListenSPP`, discovers `org.bluez.SerialPort.SerialPort.v2_0_*`, calls `SendData`; no fake ACK | `BUILT-OFFLINE`; blocked by G2 |
| `cp_spp_socket` | Root-only `SOCK_DGRAM` receiver for the P1 hook, `SO_PEERCRED` checked | `BUILT-OFFLINE` |
| `cp_oem_auth_async` / `cp_auth_broker` | Nonblocking MFi RPC client; one operation in flight | `BUILT-OFFLINE` |
| `cp_native_auth` | Root MFi helper; real pinned library/chip, bounded certificate/signature RPC | `BUILT-OFFLINE` |

The broker never runs `ip`, `route`, SSH, AP or firewall commands. It only
uses the explicit OEM session bus, the RX datagram socket, the read-only
daemon status file, and the root MFi socket. A broker failure must not take
down SSH, BlueLink, the AP, wired CarPlay or the stock receiver.

**Target build proof (offline):** `build_carplay_broker.py` now links the
helper against the target rootfs glibc 2.25. Current output:
`packages/wifi1b/out/carplay-broker-target/carplay-broker`, 170000 bytes,
SHA-256 `a3f8db92e729162c96bcfc5af2772a685bcc67abb9ea75249536318acc5c6d22`;
NEEDED is exactly the allowed seven libraries; GLIBC references are
`{2.2.5, 2.4, 2.14, 2.17}`; no RPATH/RUNPATH; no own `GLIBC_PRIVATE`
references; loadability requires the matching 2.25 `ld.so`, which supplies
the libraries' `GLIBC_PRIVATE` references. The helper was **not executed**;
this is static loadability evidence only. `cp_target_compat.c` supplies the target-2.25
`fstat`/`fstatat` and pre-C23 `strtol`/`strtoul` shims; it is a pure
pass-through and does not weaken the socket's symlink/inode/uid checks.

**Wire facts that must not drift**

- iAP2 accessory-server UUID ends `CAFF`; phone-client UUID ends `CAFE`.
- CarPlay service UUID is `EC884348-CD41-40A2-9727-575D50BF1FD3`.
- Identification, discovery and session-start identity fields must agree.
  Bluetooth MAC, AP MAC and pairing identifier are distinct roles.
- WPA2 in the WiFi-config message omits the security enum; do not copy the
  `0x5703` security value from a different message. WPA3 transition/only uses
  the negotiated values, not a hardcoded 2.
- The RX hook's UUID kind is evidence only; do not treat CAFE as an inbound
  listener.

### P1 — `vbtd-51` SPP RX hook (implemented offline)

**Binary identity**

| Item | Value |
|---|---|
| File | `/app/bin/vbtd-51` |
| Size | `14117064` bytes |
| SHA-256 | `55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0` |
| Symbol | `_ZN13VBTSppManager21VBTSppReceivedDataCfmEP16T_VBT_EVENT_BASE` |
| Demangled | `VBTSppManager::VBTSppReceivedDataCfm(T_VBT_EVENT_BASE*)` |
| ELF vaddr | `0x490ec0` |
| File offset | `0x490ec0` (verified through the executable `PT_LOAD`) |
| ELF size | `0x355` (853) |
| Preimage SHA-256 | `2f3ba61377b73547026a634511ed238899c37ca06fdb111e0f3e1919e3493f06` |

**What the patch does**

Replace the entire 853-byte function with a freestanding hook that:

1. Rejects a null event.
2. Reads the peer address (`std::string` data pointer at event `+0x08`, length
   at `+0x10`) and requires exactly 17 printable ASCII bytes.
3. Reads the UUID (`std::string` data pointer at `+0x30`, length at `+0x38`)
   and requires exactly 36 bytes matching one of:
   - CAFF `00000000-DECA-FADE-DECA-DEAFDECACAFF`
   - CAFE `00000000-DECA-FADE-DECA-DEAFDECACAFE`
   - CarPlay `EC884348-CD41-40A2-9727-575D50BF1FD3`
4. Reads the payload pointer at `+0x50` and length at `+0x58`; requires
   `1..16384` bytes.
5. Sends one `AF_UNIX`/`SOCK_DGRAM` datagram to
   `/run/wifi1b/carplay/rx.sock` with a 32-byte header and the opaque payload.
   The socket is nonblocking, `MSG_DONTWAIT | MSG_NOSIGNAL`; a missing receiver
   or full queue is a silent drop. The hook never blocks, allocates, logs,
   sleeps, takes a lock, or raises `SIGPIPE`.
6. Returns. The OEM event object is not modified.

The hook does **not** call any OEM function and does **not** perform iAP2,
authentication, pairing or projection. It is a capture/forward shim only.

**Known risk:** P1 replaces the entire `0x355` span, but the cached Ghidra
export only analyzed 200 bytes of it. Any stock side effects in the
unanalyzed remainder are intentionally not preserved. If the vendor state
machine depends on them, P2 must restore that state separately; do not assume
the hook is a drop-in behavioural replacement for the stock callback.

**Event ABI used (private, activation-time verification required)**

| Offset | Field |
|---|---|
| `+0x08` | `std::string` peer data pointer |
| `+0x10` | peer length (must be 17) |
| `+0x30` | `std::string` UUID data pointer |
| `+0x38` | UUID length (must be 36) |
| `+0x50` | payload pointer |
| `+0x58` | payload length (`uint32`) |

This layout was recovered from the `-63` event ABI and is assumed to hold for
`-51`. The hook validates lengths/pointers before use and drops anything that
does not match, so a wrong ABI fails closed. Verify it on the car before
relying on the forwarded payload.

**Datagram format (little-endian, 32-byte header)**

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic ASCII `W1BSPP01` |
| 8 | 4 | payload length, `uint32` |
| 12 | 17 | peer Bluetooth address, ASCII `AA:BB:CC:DD:EE:FF` |
| 29 | 1 | UUID kind: `1`=CAFF, `2`=CAFE, `3`=CarPlay service |
| 30 | 2 | reserved, zero |
| 32 | N | opaque iAP2 payload |

**Replacement identity (current builder/hook)**

| Item | Value |
|---|---|
| Blob size | `660` bytes |
| Blob SHA-256 | `7a436aa2d2d19cfea03ec6a2521df1e780d3374d27a34bbc8c26a519f81da485` |
| NOP fill | `193` bytes (`0x90`) |
| Output size | `14117064` bytes |
| Output SHA-256 | `8691ad1468122c5f9f8c577dedb0f2185bb126b2f8afe8b94ceb44bcc095a9a9` |
| Patched region SHA-256 | `b0d27780c8d3f7e831a110d94f5d2a28dab47175d18d9f78ab74a99150f32558` |
| Hook source SHA-256 | `c06dd1ac7fa5bf2db92b682741251efbe6cfbeee35f4e269cb80f6f409e231bb` |
| Hook header SHA-256 | `7a228f2f4ee0043ca08095facb407db733f6eda56dd3b2834ef091e00718c618` |
| Builder SHA-256 | `5f76d49e351512e6abc07468bebfff1d9a2ffd19de01ef712e1c22584e521fe5` |
| Manifest status | `EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED` |
| `car_validated` | `false` |
| `installer_activated` | `false` |
| `oem_binary_executed` | `false` |

The output SHA is toolchain-dependent in principle; the builder records the
compiler/linker identity in the manifest. The security-relevant gates are the
input SHA, exact preimage SHA, vaddr/offset mapping, function extent, blob fit,
and byte equality outside the span.

**Build**

```sh
python3 packages/wifi1b/scripts/build_carplay_spp_patch.py \
  --input /path/to/vbtd-51 \
  --out /tmp/opencode/vbtd51-patch \
  --target 51
```

**Offline gates**

```sh
SCOURGE_ROOTFS=/path/to/rootfs \
  python3 packages/wifi1b/tests/test_carplay_vbtd51_patch.py
python3 packages/wifi1b/tests/test_carplay_spp_patch.py   # includes -63 regression
```

The `-51` test checks the target pins, exact preimage, blob fit,
outside-span preservation, and a read-only real-firmware readback. The shared
`test_carplay_spp_patch.py` covers wrong-binary and single-byte-corruption
refusal, preimage mismatch, mapping and output gates. The OEM binary is never
executed.

### P2 — call `ListenSPP` and verify stock registration (no patch preferred)

The stock chain is present; the likely gap is orchestration. Prefer an
external `ListenSPP` call with the CAFF UUID over any binary patch. The cached
Ghidra export is **truncated** for several handlers; its `size` field is the
analyzed prefix, not the ELF symbol size, so reanalyze full extents before
concluding a function is empty.

**Primary experiment (no binary patch)**

Call the vendor adapter's `ListenSPP(string uuid, string name)` with the CAFF
UUID. The broker already speaks this interface; the exact D-Bus service/path
must be confirmed with `busctl tree` on the car. Expected chain:

```text
Adapter.ListenSPP
  -> StackAdapterStub::ListenSPP 0x5cdc10
  -> VBTSppManager::Listen 0x493a40
  -> AdapterManager::IsServiceEnabled(0xa) gate at 0x493aef
  -> VBTSppHandler::VBTSppListenReq 0x729740
  -> BrcmSpp::BrcmSppListenCmd 0x784680
```

On a live CAFF connection, `VBTSppConnectCfm` (`0x498240`, size `0x18f9`)
calls `DeviceManager::SetSerialPortConnectionStatus(..., 3)` at `0x498714`.
That function generates `/dev_<peer>/serial_<uuid>`, calls
`RemoteDevice::AddSerialPortList` (`0x48d920`), constructs
`StackSerialPortStub` (`0x611fb0`), and registers the CommonAPI stub at
`0x633743`. The object should exist while the connection is live and disappear
on disconnect.

If the object is absent, check:

- SPP service `0xa` enabled (`AdapterManager::IsServiceEnabled` gate).
- The phone is a known/paired device; otherwise `SetSerialPortConnectionStatus`
  logs the not-found path at `0x478a20` (line `0x300`) and returns without
  creating the object.
- `VBTSppConnectCfm` actually received `VBT_SPP_CONNECT_CFM`; an RFCOMM connect
  alone is not enough.

**TX proof once the object exists**

Do not send a CAFF PDU. Call `SendData` with one `0x00` byte. Success means
the method returns 0, `VBTSppSendDataCfm` logs, and the capture shows one
RFCOMM UIH frame. A method ACK alone is not delivery. The broker's existing
`cp_spp_dbus` client can then be used for real iAP2 TX.

**Fallback patch (not preferred)**

If the service gate is dead and no external call can enable it, the smallest
orchestration edit is in `VBTSppEnableCfm`: NOP the `getSIGTestMode` branch at
`0x495b6c` (`0f 84 9e 01 00 00` -> six NOPs). Caveat: that path listens on the
standard SPP UUID `1101` (`BTSPP`), not CAFF; it proves the machinery but is
unlikely to satisfy the phone's CarPlay SDP expectation. Any edit trips the
whole-file SHA pin; re-pin through docs/09 and keep the patch WIP.

**Do not** fill `SetSppListen` / `SetSppListenStatus` for registration. They
are state-notification stubs for a `ConnectionStateManager` listener, not the
D-Bus object path, and their signatures lack the peer address/UUID.

**Address table (ELF vaddr, authoritative ELF size)**

| Function | ELF vaddr | ELF size | Role |
|---|---:|---:|---|
| `StackAdapterStub::ListenSPP` | `0x5cdc10` | `0x776` | D-Bus listen entry |
| `VBTSppManager::Listen` | `0x493a40` | `0x497` | SPP listen orchestration |
| `VBTSppHandler::VBTSppListenReq` | `0x729740` | `0x340` | Request layer |
| `BrcmSpp::BrcmSppListenCmd` | `0x784680` | `0xcc2` | Stack listen |
| `VBTSppManager::VBTSppConnectCfm` | `0x498240` | `0x18f9` | Connection mapping |
| `DeviceManager::SetSerialPortConnectionStatus` | `0x4784d0` | `0x8eb` | Registration driver |
| `RemoteDevice::AddSerialPortList` | `0x48d920` | `0x894` | Per-device records |
| `RemoteDevice::getSerialPortStub` | `0x48ba70` | `0x27d` | Stub lookup |
| `GeniviCommonAPIFactory::registerSerialPortStub` | `0x633480` | `0x5e6` | CommonAPI registration |
| `StackSerialPortStub` ctor | `0x611fb0` | — | Stub construction |
| `StackSerialPortStub::SendData` | `0x612e50` | `0x394` | Actual TX method |
| `VBTSppManager::SendData` | `0x494c30` | `0x482` | Vendor TX entry |
| `VBTSppHandler::VBTSppSendDataReq` | `0x7294e0` | `0x259` | Request layer |
| `BrcmSpp::BrcmSppSendCmd` | `0x7879f0` | `0x60e` | Stack TX |
| `VBTSppManager::VBTSppSendDataCfm` | `0x490b60` | `0x355` | TX completion |
| `VBTSppManager::VBTSppListenCfm` | `0x4950c0` | `0x40a` | Listener status |
| `VBTSppManager::VBTSppEnableCfm` | `0x4958f0` | `0xd5a` | Enable/fallback gate |
| `ProjectionDeviceStateManager::SetSppListen` | `0x3b4860` | `0x2e` | Empty state stub |
| `ProjectionDeviceStateManager::SetSppListenStatus` | `0x3b4890` | `0x2e` | Empty state stub |
| `ProjectionDeviceStateManager::SetSppDisconnectedStatus` | `0x3b48c0` | `0x3ec` | Log-only state stub |

**Status:** primary experiment `DEFERRED` (needs car); fallback patch
`DESIGN`, not implemented. No registration patch is claimed.

### P3 — AirPlay/projection trigger (not identified)

- `AppProjection` SHA-256
  `8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3`,
  2,321,256 bytes, stripped.
- `CarPlayAppFrameworkStart` is an **imported** framework symbol: PLT stub at
  ELF `0x17b00`, `JUMP_SLOT` relocation at `0x435d28`. The cached export's
  `0x439f48` entry is outside the last `PT_LOAD` (which ends at `0x438720`)
  and is an analysis artifact, not code. The PLT stub is not the
  implementation; patching it is not the fallback.
- No caller of the thunk was identified in the cached export. The receiver is
  started through the vendor iAP2Service USB-session chain, and
  `CAR_PLAY_CONNECT_REQ` currently returns the `0x2aff` "connect state"
  refusal with no phone.
- **Do not** patch `AppProjection` yet. First determine whether a completed
  first-party iAP2 session makes `_airplay._tcp` appear. If it does not,
  recover the minimal `CarPlayAppFrameworkStart` caller and the required
  delegate/context setup. This remains `DEFERRED`.

### Not patched: vendor iAP2 builder fallback is NO-GO

The vendor wireless builder path was evaluated and rejected as a fallback: it
would require patching `vbtd`, `iAP2Service`, `AppProjection`, and adding a new
transport/AP provider; `ENABLE_WIRELESS_CARPLAY` is not consumed; and
`GetSupportWirelessCP` returns false with empty change handlers. It is more
surface area than the first-party broker and still does not guarantee the
receiver starts. Recorded as a decision, not a patch plan.

## 4. Full address map (vbtd-51, ELF vaddr)

**Binary/library pins**

| Artifact | Size | SHA-256 |
|---|---:|---|
| `vbtd-51` | `14117064` | `55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0` |
| `vbtd-63` | `14149896` | `c56b68fdd35b16b95688ab54a48ffb9f18034d925c057e86f8b9fe304f8b26b9` |
| `AppProjection` | `2321256` | `8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3` |
| `iAP2Service-2.0` | — | `38419131df82ec7e3c3b29a68ecc1a078c7f0b403051f137fe9f46f82b99f26e` |
| `libCoreUtils.so` | `1200368` | `9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae` |
| `libiAP2AppFramework-2.0.so` | — | `d4d0558c9c53f02ee14024c081ccd83a02ca29404ef400131e2951f68a388120` |
| `libAirPlay.so` | — | `ef9e69e3b3388d4a7a1d185827c710af25b8ee99d406c60c664a9609b00aada0` |

Use `readelf -sW`/`nm -D -S` for the authoritative size. The "Ghidra" column
is the analyzed prefix in the cached export and is often much smaller.

| Function | ELF vaddr | ELF size | Ghidra | Status |
|---|---:|---:|---:|---|
| `VBTSppManager::VBTSppReceivedDataCfm` | `0x490ec0` | `0x355` | 200 | **P1 patched offline** |
| `VBTSppManager::VBTSppSendDataCfm` | `0x490b60` | `0x355` | 200 | Stock; TX completion |
| `VBTSppManager::SendData` | `0x494c30` | `0x482` | — | Vendor TX entry; inspect |
| `VBTSppManager::VBTSppListenCfm` | `0x4950c0` | `0x40a` | 170 | Calls `SetSppListenStatus`; full body not cached |
| `VBTSppManager::VBTSppEnableCfm` | `0x4958f0` | `0xd5a` | 252 | Calls `Listen`/`SetSppListen`; SIG-test branch |
| `VBTSppManager::VBTSppConnectCfm` | `0x498240` | `0x18f9` | 39 | Calls `SetSerialPortConnectionStatus(...,3)` at `0x498714` |
| `ProjectionDeviceStateManager::SetSppListen` | `0x3b4860` | `0x2e` | 46 | Empty state stub; called from `EnableCfm` at `0x495d1c` |
| `ProjectionDeviceStateManager::SetSppListenStatus` | `0x3b4890` | `0x2e` | 46 | Empty state stub; called from `ListenCfm`/`StopCfm` |
| `ProjectionDeviceStateManager::SetSppDisconnectedStatus` | `0x3b48c0` | `0x3ec` | 192 | Log-only; called from `DisconnectCfm` at `0x496fbe` |
| `ProjectionDeviceStateManager::onSupportWirelessCPChanged` | `0x3b4610` | `0x2e` | 46 | Empty |
| `PersistenceInterface::GetSupportWirelessCP` | `0x6824c0` | `0x2e` | 46 | Zero-return stub |
| `PersistenceInterface::onSupportWirelessCPChanged` | `0x6823d0` | `0x2e` | 46 | Empty |
| `PowerStateManager::onSupportWirelessCPChanged` | `0x6ba4e0` | `0x2e` | 46 | Empty |
| `DeviceManager::SetSerialPortConnectionStatus` | `0x4784d0` | `0x8eb` | — | Registration driver |
| `DeviceManager::AddSerialPortInterface` | `0x4708b0` | `0x481` | 48 | Exists; not the live path; no cached caller |
| `RemoteDevice::AddSerialPortList` | `0x48d920` | `0x894` | 1271 | Reached from `SetSerialPortConnectionStatus` at `0x478885` |
| `RemoteDevice::getSerialPortStub` | `0x48ba70` | `0x27d` | 352 | Stub lookup only |
| `GeniviCommonAPIFactory::registerSerialPortStub` | `0x633480` | `0x5e6` | — | Registration factory (indirect/virtual) |
| `StackSerialPortStub::SendData` | `0x612e50` | `0x394` | — | Actual stub TX |
| `SerialPortStubDefault::SendData` (client) | `0xaad7e0` | `0x15f` | — | Default stub |
| `SerialPortStubDefault::SendData` (short) | `0xaacd80` | `0x2e` | — | Short overload |
| `initializeSerialPortDBusStubAdapter` | `0xaa4c60` | `0x124` | — | Registration |
| `createSerialPortDBusStubAdapter` | `0xaa4d90` | `0x32d` | — | Registration |
| `generateSerialPortPath` | `0x57d410` | `0x185` | — | Path helper |
| `VBTSppHandler::VBTSppSendDataReq` | `0x7294e0` | `0x259` | — | Request entry |
| VBTConfig loader (`FUN_00439280`) | `0x339280` | Ghidra-local | 3020 | Parses `ENABLE_WIRELESS_CARPLAY` |
| `VBTConfig::getSIGTestMode` | `0x5b8ae0` | `0x41f` | 65 | No SPP caller found |
| `AdapterManager::SetSIGTestMode` | `0x3e0dc0` | `0x2d3` | 244 | Generic SPP/test mode |

The `vbtd-63` equivalent RX target is:

| Item | Value |
|---|---|
| Size | `14149896` |
| SHA-256 | `c56b68fdd35b16b95688ab54a48ffb9f18034d925c057e86f8b9fe304f8b26b9` |
| `VBTSppReceivedDataCfm` vaddr/offset | `0x491c90` |
| Extent | `0x355` |
| Preimage SHA-256 | `ce4499ebe7786606b94c58809283f2306aacd77914c38f620807e396d19ca5c7` |

`build_carplay_spp_patch.py --target 63` remains the default regression path;
its output was byte-identical to the pre-`-51` builder in the checkpoint
review.

## 5. Reproduction workflow (Ghidra + REA)

### 5.1 Verify the binary before touching it

```sh
BIN=/path/to/vbtd-51
sha256sum "$BIN"
stat -c '%s' "$BIN"
```

Expected for this firmware:

```text
55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0
14117064
```

If either differs, stop. Do not port offsets by hand; use
[`VBT-SPP-HOOK-PORTING.md`](VBT-SPP-HOOK-PORTING.md) to re-pin.

### 5.2 Get exact extents from the ELF, not the decompiler

```sh
readelf -sW "$BIN" | grep VBTSppReceivedDataCfm
nm -D -S -C "$BIN" | grep 'VBTSppManager::VBTSppReceivedDataCfm'
```

`readelf` gives the symbol value and `st_size`. `nm -D -S` gives the same
demangled, with size. The cached Ghidra `functions.jsonl` `size` is only the
analyzed prefix; it is **not** authoritative. For example, the export records
39 bytes for `VBTSppConnectCfm`, while `readelf` records `0x18f9`.

### 5.3 Ghidra image base

In this cached project, Ghidra addresses are ELF vaddr plus `0x100000`
(`VBTSppReceivedDataCfm`: Ghidra `0x590ec0`, ELF `0x490ec0`). Confirm the base
from the first `PT_LOAD` before using any address:

```sh
readelf -lW "$BIN" | grep LOAD
```

The first executable `PT_LOAD` in this binary has `VirtAddr 0x0`; the Ghidra
project was imported at `0x100000`. Do not assume another project uses the
same base. In REA, use the ELF vaddr from `readelf`, then cross-check the
Ghidra address.

### 5.4 Map vaddr to file offset

```python
import struct
from pathlib import Path
data = Path("vbtd-51").read_bytes()
phoff, = struct.unpack_from("<Q", data, 0x20)
phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
for i in range(phnum):
    off = phoff + i * phentsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
        struct.unpack_from("<IIQQQQQQ", data, off)
    if p_type == 1 and (p_flags & 1) and p_vaddr <= 0x490EC0 < p_vaddr + p_filesz:
        print(hex(p_offset + (0x490EC0 - p_vaddr)))
```

For this build the result is `0x490ec0`, but always let the `PT_LOAD` mapping
produce it. Never assume offset == vaddr.

### 5.5 Confirm the exact preimage

```python
import hashlib
from pathlib import Path
data = Path("vbtd-51").read_bytes()
start, length = 0x490EC0, 0x355
print(hashlib.sha256(data[start:start+length]).hexdigest())
```

Expected:
`2f3ba61377b73547026a634511ed238899c37ca06fdb111e0f3e1919e3493f06`.

If the bytes differ, the binary is not the pinned build. Stop.

### 5.6 Build and verify P1

```sh
rm -rf /tmp/opencode/vbtd51-patch
python3 packages/wifi1b/scripts/build_carplay_spp_patch.py \
  --input /path/to/vbtd-51 \
  --out /tmp/opencode/vbtd51-patch \
  --target 51
```

The builder must refuse on:

- whole-file SHA/size mismatch;
- vaddr not covered by exactly one executable `PT_LOAD`;
- vaddr/offset mismatch against `config/firmware_locks.env`;
- function preimage SHA mismatch;
- hook blob larger than the function extent;
- relocations, undefined symbols, oversized sections, non-`ret` terminal text;
- missing expected hook strings;
- any byte difference outside the replaced span.

It writes `vbtd-51.spp-rx.patched` and
`vbtd-51.spp-rx.manifest.json`. Check the manifest says
`car_validated:false`, `installer_activated:false`,
`oem_binary_executed:false`.

### 5.7 Inspect the patched span

```python
from pathlib import Path
p = Path("/tmp/opencode/vbtd51-patch/vbtd-51.spp-rx.patched").read_bytes()
off, length, blob = 0x490EC0, 0x355, 660
print(p[off:off+16].hex(" "))          # hook entry
print(p[off+blob-16:off+blob].hex(" "))# last blob bytes
print(p[off+blob:off+length].hex(" ")) # must be 0x90 * 193
```

Expected entry starts with `48 85 ff 0f 84 ...` (the hook's null check), the
last blob bytes end with the socket path string, and the remaining 193 bytes
are `90`.

### 5.8 Run the offline suites

```sh
SCOURGE_ROOTFS=/path/to/rootfs \
  python3 packages/wifi1b/tests/test_carplay_vbtd51_patch.py
python3 packages/wifi1b/tests/test_carplay_spp_patch.py
python3 packages/wifi1b/tests/test_carplay_broker_main.py
python3 packages/wifi1b/tests/test_carplay_services.py
```

These are build/fixture tests, not car evidence.

### 5.9 Deploy/rollback

Use the owner checklist in
`packages/wifi1b/artifacts/carplay/README.md`. Temporary replacement only:
stock `vbtd` backup at a verified path, USB SSH reachable, atomic replace,
labels preserved, restore on failure. Persistent replacement waits for owner
approval and independent review.

### 5.10 Search anchors for a new build

Use these strings/xrefs in Ghidra or REA before guessing at function names:

| Anchor | Where it leads |
|---|---|
| `W1BSPP01`, `/run/wifi1b/carplay/rx.sock` | P1 hook artifact / broker RX path |
| `ListenSPP`, `StopListenSPP` | OEM adapter interface; broker D-Bus call |
| `org.bluez.SerialPort.SerialPort.v2_0_` | SerialPort service/object prefix |
| `SendData`, `onStatusAttributeChanged` | SerialPort stub methods/signals |
| `ENABLE_WIRELESS_CARPLAY` | VBTConfig loader; not an activation switch |
| `ENABLE_SERIAL_PORT_PROFILE` | VBTConfig loader; profile gate candidate |
| `BTSPP`, `00001101-0000-1000-8000-00805F9B34FB` | generic SPP service table; not CAFF |
| `00000000-DECA-FADE-DECA-DEAFDECACAFF` / `...CAFE` | CAFF/CAFE UUIDs |
| `EC884348-CD41-40A2-9727-575D50BF1FD3` | CarPlay service UUID |
| `_airplay._tcp`, `_carplay-ctrl._tcp` | AirPlay/receiver advertisement |
| `CAR_PLAY_CONNECT_REQ`, `0x2aff` | vendor projection request/refusal |

Cross-check every hit with the ELF symbol table; do not patch on a string hit
alone.

## 6. Deferred probes and what each would change

| Probe | Question | If negative | If positive |
|---|---|---|---|
| Live SerialPort D-Bus snapshot | Does a `SerialPort.v2_0_*` object appear **during** a CAFF connection? | P2 must restore registration or add direct TX | The broker's existing `SendData` client may be usable; prove delivery |
| SPP TX loopback | Does `SendData` int32 0 actually put bytes on RFCOMM? | P2/P3 blocked; do not treat stub ACK as delivery | iAP2 TX can proceed |
| SIG Test Mode listener | Does `VBT.GAP."SIG Test Mode"` create any listener? | No change; generic SPP is not CAFF | Still not CAFF; do not rely on it |
| `_airplay._tcp` TXT/SRV | Does the receiver advertise without a vendor USB iAP2 session? | P3 trigger needed | First-party session may be enough |
| MFi helper/chip | Do real certificate/signature bytes return? | Auth gate blocks; never fake AA00-AA05 | First-party auth can proceed |
| `CAR_PLAY_CONNECT_REQ` | Does the vendor accept a projection request with no phone? | Expected `0x2aff` refusal; P3 needed | Vendor trigger may be usable |
| Wired baseline | Does stock wired CarPlay work before/after? | Stop; preserve wired path first | Wireless coexistence can be evaluated |

## 7. Explicit non-goals

- Do not fake MFi authentication, Bluetooth pairing, AirPlay pair-verify, AP
  credentials, receiver attestation or projection.
- Do not patch `GetSupportWirelessCP` to `true` and call it activation.
- Do not patch `SetSppListen` alone; it is an empty state-notification stub,
  not a registration gate.
- Do not patch the `CarPlayAppFrameworkStart` PLT stub at `0x17b00`; it is not
  the implementation. The export's `0x439f48` entry is not mapped code.
- Do not treat a broker `SendData` int32 `0` as delivery; it is only the OEM
  stub's ACK.
- Do not copy `vbtd-63` offsets, spans or event ABI to `-51` without
  re-measuring and re-pinning.
- Do not execute an OEM binary on the host. The builder reads and writes
  bytes only.

## 8. File and evidence map

| Subject | File |
|---|---|
| Porting a hook to another build | `docs/wireless-carplay/VBT-SPP-HOOK-PORTING.md` |
| Car-only probes and blockers | `docs/wireless-carplay/DEFERRED-CAR-PROBES.md` |
| Current checkpoint/status | `docs/wireless-carplay/PROGRESS.md` |
| Hook source/wire contract | `packages/wifi1b/src/carplay/cp_spp_rx_hook.c`, `cp_spp_rx.h` |
| Patch builder/gates | `packages/wifi1b/scripts/build_carplay_spp_patch.py` |
| Firmware pins | `config/firmware_locks.env` |
| Broker helper | `packages/wifi1b/src/carplay/cp_broker_main.c` and sibling modules |
| Broker target build | `packages/wifi1b/scripts/build_carplay_broker.py` |
| Target-2.25 link shims | `packages/wifi1b/src/carplay/cp_target_compat.c` |
| Target build regression | `packages/wifi1b/tests/test_carplay_broker_target.py` |
| MFi helper | `packages/wifi1b/src/carplay/cp_native_auth.c` |
| MFi target build | `packages/wifi1b/scripts/build_carplay_native.py` |
| Default-off units/checklist | `packages/wifi1b/artifacts/systemd/`, `packages/wifi1b/artifacts/carplay/README.md` |
| Hook/broker tests | `packages/wifi1b/tests/test_carplay_vbtd51_patch.py`, `test_carplay_spp_patch.py`, `test_carplay_broker_main.py`, `test_carplay_broker_target.py`, `test_carplay_services.py` |

**Bottom line:** one OEM binary patch is implemented and hash-gated (the
`vbtd-51` RX hook). The vendor SerialPort TX/registration patch and the
AirPlay/projection trigger are **not** implemented; their candidate addresses
and the reason each is not yet a minimal patch are recorded above so the next
agent can continue from verified ELF extents instead of redoing the search.
