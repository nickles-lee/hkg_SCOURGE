# Experimental CarPlay broker services

**Status: EXPERIMENTAL, build-only, not car-validated.** These units are
shipped as artifacts and are **not** copied or enabled by
`scripts/build_payload.sh`. Nothing here is a working wireless CarPlay
feature; it is the deployment plumbing for the checkpoints in
[`docs/wireless-carplay/VBT-SPP-HOOK-PORTING.md`](../../../../docs/wireless-carplay/VBT-SPP-HOOK-PORTING.md)
and [`DEFERRED-CAR-PROBES.md`](../../../../docs/wireless-carplay/DEFERRED-CAR-PROBES.md).

## Files

| File | Purpose |
|---|---|
| `../systemd/scourge-carplay-auth.service` | Root-only MFi helper; serves the real vendored certificate/signature library on `/run/scourge-carplay-auth/auth.sock` |
| `../systemd/carplay-broker.service` | Supervised first-party broker; reads the patched `vbtd` RX socket and the private/explicit OEM session bus |
| `carplay-broker.conf.example` | Owner-supplied identity template; empty values fail closed |

The units are intentionally **not** installed by the Wifi1b payload. An owner
who wants to run the experiment copies them over SSH after the broker, MFi
helper and patched `vbtd` have passed their offline gates.

## Prerequisites (all must be true before starting)

1. `vbtd` is the pinned build, patched with `build_carplay_spp_patch.py`, and
   the stock binary is backed up at a verified path with USB SSH still
   reachable.
2. `/rw_data/wifi1b/carplay-broker` is the target-rootfs build from
   `build_carplay_broker.py`, and its manifest says
   `EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED`.
3. `/rw_data/wifi1b/carplay-native-auth` is the target-rootfs MFi helper from
   `build_carplay_native.py`, and `/app/lib/libCoreUtils.so` matches its
   compiled-in SHA-256 pin. The helper talks to the real chip; no fake
   AA00-AA05 responses are permitted.
4. `/rw_data/wifi1b/carplay-broker.conf` is owner-supplied, root-owned `0600`,
   and `consent=1` is a deliberate owner choice. Empty identity fields keep
   the broker stopped.
5. The daemon reports fresh AP/readback state in `/rw_data/wifi1b/status.json`.
   The broker refreshes SSID/channel/security/link-local from that readback,
   and `cp_bootstrap` refuses an offer until the AP is ready and the readback
   is valid. The AP **passphrase** is still config-supplied and is not
   attested by status; keep `ap_psk` in sync with the daemon or the phone will
   fail the join. The AirPlay port is also config-supplied.
6. The example config sets `usb_active=1` (block). A static flag is not a
   substitute for detecting a connected iPhone; do not set `usb_active=0`
   where wired CarPlay may be active. Live Apple-USB detection is still WIP.

## Manual install (owner, over SSH)

```sh
# From the staged USB/SSH directory, not from the repo checkout:
install -o root -g root -m 0755 carplay-broker /rw_data/wifi1b/carplay-broker
install -o root -g root -m 0755 carplay-native-auth /rw_data/wifi1b/carplay-native-auth
install -o root -g root -m 0600 carplay-broker.conf /rw_data/wifi1b/carplay-broker.conf
install -o root -g root -m 0644 carplay-broker.service /etc/systemd/system/carplay-broker.service
install -o root -g root -m 0644 scourge-carplay-auth.service /etc/systemd/system/scourge-carplay-auth.service
systemctl daemon-reload
```

Do **not** run `systemctl enable` until the owner explicitly accepts the
experimental status and the prerequisites above. Start the Wifi1b daemon/AP
yourself first; the broker unit does **not** pull in `wifi1b-daemon.service`,
so starting the broker cannot change AP/network state. Start the helper first,
then the broker:

```sh
systemctl start scourge-carplay-auth.service
systemctl start carplay-broker.service
systemctl status --no-pager scourge-carplay-auth.service carplay-broker.service
cat /run/wifi1b/broker-status.json
```

A `state:"running"` line means the helper loop is alive, not that pairing,
authentication or projection succeeded. Check the session/auth counters and
the on-car observations separately.

## Stop and roll back

```sh
systemctl stop carplay-broker.service scourge-carplay-auth.service
systemctl disable carplay-broker.service scourge-carplay-auth.service 2>/dev/null || true
systemctl reset-failed carplay-broker.service scourge-carplay-auth.service 2>/dev/null || true
rm -f /etc/systemd/system/carplay-broker.service
rm -f /etc/systemd/system/scourge-carplay-auth.service
systemctl daemon-reload
# Optional: remove the helper binaries and the PSK-bearing owner config.
# rm -f /rw_data/wifi1b/carplay-broker /rw_data/wifi1b/carplay-native-auth
# rm -f /rw_data/wifi1b/carplay-broker.conf
# Restore the stock vbtd from its verified backup and confirm SSH/BlueLink.
```

The broker never runs `ip`, `route`, SSH, AP or firewall commands. It only
uses the explicit OEM session bus, the root-only RX datagram socket and the
read-only daemon status file. A broker failure must not take down SSH,
BlueLink, the AP, wired CarPlay or the stock receiver.
