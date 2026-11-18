# Porting the Wifi1b CarPlay SPP hook

One-page field guide for porting the experimental Bluetooth SPP hook to a
different `vbtd` build. The OEM binary is read **only**; the builder never
executes it. This path is **not car-validated**, not activated by the
installer, and must never be presented as working CarPlay on its own.
The full stock-gap/fix/address map is in
[`REPRODUCTION.md`](REPRODUCTION.md).

## Symptom and target

An iPhone discovers the car and opens the CarPlay/iAP SPP channel, then the
CarPlay spinner never completes. In stock `vbtd` the receive callback drops
the event and the listener/projection state callbacks are empty. The
SerialPort registration chain itself is present: `VBTSppConnectCfm` ->
`SetSerialPortConnectionStatus` -> `AddSerialPortList` -> CommonAPI
registration. During the earlier connection window no vendor SerialPort
object was observed, but a snapshot **during** a live connection is still
missing (see `DEFERRED-CAR-PROBES.md` §1). The first-party broker needs the
RX hook for inbound bytes and a `ListenSPP` orchestration test for the
vendor TX object; it is not yet car-verified. Live 2026-11-18: the stock
SerialPort object appeared and `SendData` returned a stub ACK; see
[`VALIDATION.md`](VALIDATION.md) and [`TEST-PLAN.md`](TEST-PLAN.md).

Identify the exact running binary first:

```sh
readlink -f /proc/$(pidof vbtd)/exe
sha256sum /proc/$(pidof vbtd)/exe
```

Pin the whole-file SHA-256, size, `VBTSppReceivedDataCfm` virtual address,
file offset, true function length and preimage SHA-256 in
`config/firmware_locks.env`. Never reuse another hash's values.

## Functions

| Function | Role | Patch decision |
|---|---|---|
| `VBTSppReceivedDataCfm` | inbound SPP event | Replace with the first-party RX hook; stock drops payload |
| `VBTSppSendDataCfm` | TX completion | Verify; stock drops completion/errors |
| `ProjectionDeviceStateManager::SetSppListen` | state notification to `ConnectionStateManager` | Empty; **not** a registration gate. Do not patch for registration |
| `SetSppListenStatus(string,bool)` | listener interface/status notification | Empty; called from `VBTSppListenCfm`/`StopCfm`; **not** registration |
| `SetSppDisconnectedStatus(string)` | listener teardown notification | Log-only; called from `VBTSppDisconnectCfm` |
| `DeviceManager::SetSerialPortConnectionStatus` | registration driver | Reuse; calls `AddSerialPortList` when status is CONNECTED (3) |
| `DeviceManager::AddSerialPortInterface` | SerialPort object API | Exists; no static caller. The live path is `SetSerialPortConnectionStatus` -> `AddSerialPortList` |
| `RemoteDevice::AddSerialPortList` | per-device SerialPort records | Reuse; reached from `SetSerialPortConnectionStatus` |
| `StackSerialPortStub` | CommonAPI SerialPort (`SendData`, status) | Reuse; do not reimplement |
| `VBTSppConnectCfm` | connection mapping | Full ELF size `0x18f9`; calls `SetSerialPortConnectionStatus(...,3)` at `0x498714`. Cached text covers only 39 bytes of logger prefix; do not call it logger-only |
| `VBTSppListenCfm` / `VBTSppEnableCfm` | listener events | `ListenCfm` full `0x40a`, calls `SetSppListenStatus`. `EnableCfm` full `0xd5a`: SIG-test-true branch calls `Listen(1101,BTSPP)` at `0x495ccd`; the other branch calls `SetSppListen` at `0x495d1c`. Cached text is prefix-only |
| `PersistenceInterface::GetSupportWirelessCP` | wireless capability | Stock returns false; first-party path bypasses it |
| `ENABLE_WIRELESS_CARPLAY` | JSON key | Parsed into the `VBTConfig` map; **no consumer queries it in 260128**; not an activation switch |
| `iAP2Service` identification selector | vendor wireless builder (4/9) | Dormant; vendor fallback is NO-GO |
| `CarPlayAppFrameworkStart` / AppProjection | vendor AirPlay receiver | Only projection fallback; large live Qt binary |

## Finding functions in a different hash

Use symbols and semantics, never fixed offsets:

1. Load the binary in Ghidra/REA and match exported/demangled names.
2. Find callers/callees: `VBTSppListenCfm` calls `SetSppListenStatus`;
   `VBTSppEnableCfm` has mutually exclusive branches (`Listen(1101,BTSPP)`
   vs `SetSppListen`); `VBTSppConnectCfm` calls
   `DeviceManager::SetSerialPortConnectionStatus(...,3)`. Do not call
   `ConnectCfm` logger-only: the cached export decompiled only 39 of
   `0x18f9` bytes for `ConnectCfm` and 252 of `0xd5a` for `EnableCfm`.
   Reanalyze the full ELF extents before patching.
3. Corroborate with strings (`BTSPP`, CAFF/CAFE UUIDs, `/serial_`,
   `SerialPort`, `GetSupportWirelessCP`) and the `T_VBT_EVENT_BASE` layout.
4. Recover the true function extent from the next symbol/alignment, not the
   Ghidra boundary. Re-measure; do not copy `0x355` or another build's span.
5. In cached Ghidra exports, `vaddr` is ELF virtual address `+ 0x100000`.
   Map ELF vaddr to file offset through an executable `PT_LOAD`; require the
   pinned offset and exact preimage.

The RX event ABI used by the current hook is: peer `std::string` at `+0x08`
(pointer `+0x08`, length `+0x10`), UUID `std::string` at `+0x30` (pointer
`+0x30`, length `+0x38`), payload pointer `+0x50`, payload length `+0x58`.
Verify it on the target before relying on it. The hook validates the event,
the two strings, the known UUIDs (CAFF/CAFE/CarPlay), and the payload bounds.

## Build and verify

```sh
python3 packages/wifi1b/scripts/build_carplay_spp_patch.py \
  --input /path/to/vbtd-XX --out /tmp/opencode/vbtd-xx-patch --target XX
```

The builder must enforce the whole-file SHA/size, executable `PT_LOAD`
mapping, exact function preimage, target vaddr/length, hook blob fit,
no relocations/undefined symbols/oversized sections, a terminating `ret`,
and byte equality outside the replaced span. It writes a manifest with
`car_validated:false` and `installer_activated:false`.

Build the target broker and MFi helper separately; both are ignored outputs,
not installer payloads. The broker target build now links against the target
glibc 2.25 using `cp_target_compat.c` (target `__fxstat`/`__fxstatat` and
pre-C23 `strtol`/`strtoul` shims) and `-Wl,--allow-shlib-undefined` for
libpthread's loader-private reference; it is statically verified, not executed:

```sh
python3 packages/wifi1b/scripts/build_carplay_broker.py \
  --rootfs /path/to/rootfs --out packages/wifi1b/out/carplay-broker-target
python3 packages/wifi1b/scripts/build_carplay_native.py \
  --rootfs /path/to/rootfs --out packages/wifi1b/out/carplay-native
```

The default-off systemd units and owner checklist live in
`packages/wifi1b/artifacts/systemd/` and
`packages/wifi1b/artifacts/carplay/README.md`. They are not copied by the
Wifi1b payload builder and must not be enabled until the broker has a fresh
AP readback (never a stale credential), a live USB/wired-CarPlay gate, real
MFi responses, and the vendor TX path.

Deployment is separate: temporary replacement with a stock backup and a
pre-verified USB SSH path; persistent only after owner approval and
independent review. Restore the stock binary on any failure. Keep the
`SMACK64`/`SMACK64EXEC` labels and use an atomic replace.

## Do not

- Do not patch `GetSupportWirelessCP` or treat `ENABLE_WIRELESS_CARPLAY` as
  activation. The key is read into the config store but no consumer was found
  in the analyzed image, and the `GetSupportWirelessCP`/`onSupportWirelessCPChanged`
  stubs have no callers in the cached export. This supersedes the looser
  "reads the flag" wording in `packages/wifi/docs/FINDINGS.md`; the daemon
  flag patch there is also ineffective on 260128 (pre-existing, separate
  from this hook).
- Do not patch `SetSppListen` blindly; recover the vendor state/registration
  call sequence and reuse the existing SerialPort classes.
- Do not create a second listener or a second AirPlay receiver.
- Do not fake MFi, Bluetooth pairing, AirPlay pair-verify, AP credentials,
  projection, or receiver trust.
- Do not copy `vbtd-63` offsets, spans, or event ABI to `vbtd-51` without
  re-measurement and re-pinning.
