# Porting the Wifi1b CarPlay SPP hook

One-page field guide for porting the experimental Bluetooth SPP hook to a
different `vbtd` build. The OEM binary is read **only**; the builder never
executes it. This path is **not car-validated**, not activated by the
installer, and must never be presented as working CarPlay on its own.

## Symptom and target

An iPhone discovers the car and opens the CarPlay/iAP SPP channel, then the
CarPlay spinner never completes. In stock `vbtd` the receive callback drops
the event, the listener/projection state callbacks are empty, and the vendor
SerialPort D-Bus object is not registered. The first-party broker therefore
has no transport and no TX object.

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
| `ProjectionDeviceStateManager::SetSppListen` | listener/projection state | Restore the intended state/registration; stock is empty |
| `SetSppListenStatus(string,bool)` | listener interface/status | Store the interface/status; stock is empty |
| `SetSppDisconnectedStatus(string)` | listener teardown | Clear stored state; stock only logs |
| `DeviceManager::AddSerialPortInterface` | SerialPort object creation | Reuse; stock implementation exists |
| `RemoteDevice::AddSerialPortList` | per-device SerialPort records | Reuse; stock implementation exists |
| `StackSerialPortStub` | CommonAPI SerialPort (`SendData`, status) | Reuse; do not reimplement |
| `VBTSppConnectCfm` / `VBTSppListenCfm` / `VBTSppEnableCfm` | connection/listener events | Verify the restored state is consumed; no blind patch |
| `ProjectionDeviceStateManager::GetSupportWirelessCP` | wireless capability | Stock returns false; first-party path bypasses it |
| `ENABLE_WIRELESS_CARPLAY` | JSON key | **No consumer in 260128**; not an activation switch |
| `iAP2Service` identification selector | vendor wireless builder (4/9) | Dormant; vendor fallback is NO-GO |
| `CarPlayAppFrameworkStart` / AppProjection | vendor AirPlay receiver | Only projection fallback; large live Qt binary |

## Finding functions in a different hash

Use symbols and semantics, never fixed offsets:

1. Load the binary in Ghidra/REA and match exported/demangled names.
2. Find callers/callees: `VBTSppEnableCfm` calls the listener path; `VBTSppListenCfm`
   calls the status setter; `VBTSppConnectCfm` consumes the connection mapping.
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

Deployment is separate: temporary replacement with a stock backup and a
pre-verified USB SSH path; persistent only after owner approval and
independent review. Restore the stock binary on any failure. Keep the
`SMACK64`/`SMACK64EXEC` labels and use an atomic replace.

## Do not

- Do not patch `GetSupportWirelessCP` or treat `ENABLE_WIRELESS_CARPLAY` as
  activation; it is unread in this firmware.
- Do not patch `SetSppListen` blindly; recover the vendor state/registration
  call sequence and reuse the existing SerialPort classes.
- Do not create a second listener or a second AirPlay receiver.
- Do not fake MFi, Bluetooth pairing, AirPlay pair-verify, AP credentials,
  projection, or receiver trust.
- Do not copy `vbtd-63` offsets, spans, or event ABI to `vbtd-51` without
  re-measurement and re-pinning.
