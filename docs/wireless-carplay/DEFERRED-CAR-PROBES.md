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
- If absent, determine whether `SetSppListenStatus`/`AddSerialPortList` must be
  restored or whether the broker needs a direct TX hook.

## 2. SPP TX loopback

- With a live connection, call the vendor `SendData` path with a bounded test
  payload and confirm RFCOMM `SENT`/phone receipt.
- A broker `SendData` int32 0 is a stub ACK, not delivery.

## 3. SIG Test Mode listener

- `VBTConfig` `VBT.GAP."SIG Test Mode": true` makes `VBTSppEnableCfm` call
  `Listen("BTSPP", 00001101-...)`. That is the generic SPP UUID, not CAFF.
- Test whether it creates a SerialPort object, but do not assume it helps
  CarPlay; the phone's CarPlay discovery uses CAFF.

## 4. AirPlay receiver identity and advertisement

- Capture `_airplay._tcp` TXT (`deviceid`, `pi`, `srcvers`) and the SRV port.
- Port 7000 was listening (`AirTunes/320.17.6`), but `GET/RTSP /info` returned
  `455 Method Not Valid In This State`.
- Determine whether the receiver advertises while no vendor iAP2 USB session
  exists, and whether a first-party session-start makes it advertise.

## 5. MFi helper and chip

- Build/deploy the root MFi helper and its `/run/scourge-carplay-auth` socket.
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
