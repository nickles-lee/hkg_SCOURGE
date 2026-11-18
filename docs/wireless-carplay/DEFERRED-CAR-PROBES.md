# Deferred car probes and blockers

These require the car or a live iPhone. Nothing here is claimed as passed.
Record results before changing any status in V23/V24 or the release docs.

## 1. SerialPort D-Bus object during a live SPP connection

- Connect the iPhone to the CAFF service and keep the channel open.
- On the car session bus, list `org.bluez.SerialPort.SerialPort.v2_0_*`, get
  owners, introspect `SendData`/`onStatusAttributeChanged`, and check the
  object path `/dev_<peer>/serial_<uuid>`.
- Stock `vbtd-51` showed no SerialPort object after an earlier connection had
  closed. A snapshot **during** the connection is still missing.
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

## 2. SPP TX loopback

- With a live connection and a `SerialPort.v2_0_*` object, call `SendData`
  with a single `0x00` byte (not a CAFF PDU). Expect the method to return 0,
  `VBTSppSendDataCfm` to log, and one RFCOMM UIH frame in a capture.
- Static chain: `StackSerialPortStub::SendData` (`0x612e50`) ->
  `VBTSppManager::SendData` (`0x494c30`) -> `VBTSppSendDataReq` (`0x7294e0`)
  -> `BrcmSpp::BrcmSppSendCmd` (`0x7879f0`).
- A broker `SendData` int32 0 is a stub ACK, not delivery.

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

## 4. AirPlay receiver identity and advertisement

- Capture `_airplay._tcp` TXT (`deviceid`, `pi`, `srcvers`) and the SRV port.
- Port 7000 was listening (`AirTunes/320.17.6`), but `GET/RTSP /info` returned
  `455 Method Not Valid In This State`.
- Determine whether the receiver advertises while no vendor iAP2 USB session
  exists, and whether a first-party session-start makes it advertise.

## 5. MFi helper and chip

- Build/deploy the root MFi helper and its `/run/scourge-carplay-auth` socket.
  The default-off unit is `packages/wifi1b/artifacts/systemd/scourge-carplay-auth.service`;
  see `packages/wifi1b/artifacts/carplay/README.md`.
- Verify real certificate/signature responses; never fake AA00-AA05.

## 6. Vendor projection trigger

- Run `CAR_PLAY_CONNECT_REQ` with no phone; expect the vendor
  `0x2aff` "connect state" refusal.
- If the receiver never advertises after a first-party session, the only
  fallback is the AppProjection `CarPlayAppFrameworkStart` trigger patch;
  the full vendor iAP2 builder path is NO-GO.

## 7. Wired baseline

- Confirm USB CarPlay and the receiver work before evaluating any wireless
  projection handoff.

## 8. Persistent deployment

- Temporary patched `vbtd` must use a stock backup and a verified USB SSH
  path. Persistent replacement only after owner approval and review.
