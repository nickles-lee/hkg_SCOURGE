# Wifi1b

Wifi1b ("WiFi build B") is a standalone or side-by-side CarPlay AP/client app
for the Ioniq 5 Gen5W head unit. It replaces the ConnMan backend with the
field-proven public static recipe (static `wpa_supplicant` AP/client, own
DHCP server **and client**, key-only dropbear on 2224) and hardens the
installer to address the observed install failures.

This integrated version is **offline-tested, not car-validated**. It is work
in progress: do not present it as a verified fix and do not deploy it to a
vehicle without explicit owner authorization.

Field evidence (2026-11-17): the V23 car build demonstrated modem-owned default
routing alongside WiFi client mode and SSH through the Pi/`usb0` on port 2224.
BlueLink's remaining startup blocker was an unreadable vendor navigation cache,
not a reason to reset provisioning or keys. See the
[BlueLink guide](../../docs/bluelink/architecture-and-diagnostics.md) and
[postmortem](../../docs/bluelink/postmortem-2026-11-mof-cache.md).
This is partial feature validation, not validation of all modes or of the
combined tree with subsequent log-retention changes. Inbound remote commands,
explicit navigation search, and post-repair reboot persistence remain pending.

- Works standalone on the canonical Battery/Tweaks/Reboot base; no WiFi app
  reinstall and no migration stage
- Also works beside an existing WiFi app and does not modify WiFi's page,
  tile, service, config or status
- Separate paths: `/rw_data/wifi1b`
- Separate service: `wifi1b-daemon.service`
- Separate HTTP port: `8093`
- Separate SSH: `wifi1b@<ip> -p 2224` (UID 0, key-only, bind-all)
- Separate QML: `Wifi1bPage` / `Wifi1bTile` (additive patcher preserves WiFi,
  Battery, Tweaks and Reboot)
- Installer preflight stops only its own radio daemons; no manual WiFi Off
  step is needed
- Installer: no SMACK readback gate; verified inode-preserving I/O,
  non-fatal import, systemd preferred with single-instance fallback
- Daemon starts idle; importing credentials never activates anything
- Restore puts back the original shared QML and the original
  service file/enabled/active state from the install transaction
- V22+ BlueLink-safe client routing: while `rmnet_usb0`/`rmnet_usb1` has an
  IPv4 address, no WiFi default route is installed. WiFi remains an SSH-only
  link. Unverified legacy wlan0 defaults and ownership records are preserved;
  route changes fail closed rather than assuming ownership. A 30 s audit
  restores/removes the bounded proto-66 fallback as the
  modem data state changes; routes on other interfaces are never touched.
- V22+ Orange Pi USB-SSH: the ECM link is `usb0` at `10.55.0.2/24` with no
  default route. The generated udev rule and helper follow the configured
  Wifi1b SSH port, and dropbear stays available over `usb0` when WiFi is Off.
  The watchdog also ensures the listener whenever `usb0` is present, even
  while WiFi is unassociated or failed, so startup no longer depends on a
  manual key import. The SSH worker does not reserve `wlan0`, so a foreign or
  OEM radio owner cannot block the USB listener. ConnMan is kept off `usb0`
  so it cannot overwrite the static link.
- `client_route_policy=modem_safe` is the default; `fallback` is available in
  `config.conf` for owners who need a WiFi default while the modem is up.
- CarPlay AP identity: `carplay_id` comes from an explicit config value or the
  stock `USBSerial.txt`/`VBTConfig.json` identity, never the Bluetooth MAC;
  values with unsupported characters or one equal to the Bluetooth MAC are
  omitted, and AP start is refused if no valid distinct identity remains.
  The opt-in `carplay_vendor_ie=1` (default `0`)
  requests the candidate Apple
  Device IE via `ap_vendor_elements`; status remains `ie_unverified` until a
  readback or on-air capture proves it.
- The ConnMan drop-in accepts the exact known V21 file for an over-the-top
  upgrade; unknown owner overrides are still refused.

The `wifi1b` account is a deliberate product default with UID 0, not a hidden
credential. Owner keys already present in `/rw_data/wifi1b` are preserved
unless a replacement `authorized_keys` is supplied. Treat the stick and its
backups as private.

This is the canonical integration, not the legacy Wifi1b combo wrapper.
See [V8 integration](../wifi/docs/V8-WIFI1B.md) for validation and limitations.

Build and verify:

```sh
make test                 # offline tests; set SCOURGE_DROPBEAR first
sh ../build_flat.sh       # standalone Battery/Tweaks/Reboot/Wifi1b bundle
```

The default `build_flat.sh` bundle is standalone: it does **not** reinstall
WiFi and has **no** migration stage. Existing WiFi-bearing release bundles are
preserved separately.

Artifacts:

- `out/wifi1b_usb.zip`
- `out/usb/` (standalone payload; requires the current canonical
  Battery/Tweaks or Battery/Tweaks/WiFi shared QML)
- Repository `out/appcombo-flat/` (standalone two-file bundle; preferred)

## Updating a running daemon over SSH

For a daemon-only update, see
[docs/UPDATE-OVER-SSH.md](docs/UPDATE-OVER-SSH.md). The update is staged under
`/rw_data/wifi1b` and launched detached with `systemd-run` (or `setsid`), so
stopping the daemon and dropping the SSH session does not kill the swap. The
helper verifies the staged binary hash, backs up the live binary, replaces it,
waits for the listener, and rolls back on failure.
