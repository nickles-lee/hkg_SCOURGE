# Plan — WiFi / CarPlay / SSH app

Status: implementation built; on-car validation remains.

## Phase 0 — Diagnostic (read-only)

Use `scripts/run_stuff_diag.sh` renamed to `run_stuff.sh` on a USB stick.

Collect:

- `pcb_version`, `optUNECE`, `/tmp/no_unece`
- `lspci`, `lsmod | grep bcmdhd`, `dmesg` Wi-Fi lines
- `ifconfig -a`, `iw dev`, `wpa_cli status`
- `systemctl status supplicantmanager.path/service connman`
- `systemctl status carplay_iAP2 carplay_mdnsd`
- `connmanctl technologies` / `services`
- `/app/data/vbt/VBTConfig.json`, `USBSerial.txt`, BT address
- `iptables -S`, `ip6tables -S`
- QML hashes/markers

Acceptance: report written to USB; `wlan0` presence, PCB/UNECE, current VBT
flag, and OEM service state known.

## Phase 1 — Headless AP + SSH proof

Use the daemon CLI or the installed app:

```sh
/rw_data/wifi/wifi_daemon --mode carplay
```

or via UI: WiFi → CarPlay AP → Start.

Verify:

- SSID appears (default `IONIQ5-CarPlay`; key in `/rw_data/wifi/config.conf`)
- laptop gets DHCP (normally `10.42.0.1`)
- `ssh car@10.42.0.1` works with `electric`
- AP stays up > 10 minutes despite OEM stack
- `/rw_data/wifi/daemon.log` shows no repeated failures

Acceptance: AP + SSH stable; no OEM reclaim.

## Phase 1b — Reboot-persistent SSH user + non-22 port

The firmware regenerates `/tmp/passwd` from `/etc/passwd.legacy` or
`/etc/passwd.secure` on every boot. The installer now patches both templates
and `/etc/shadow.Global`; the daemon also calls `ensure_car_user()` at startup.

SSH defaults to port 2222. The daemon checks `/proc/net/tcp`/`tcp6` and falls
back to 2223-2230 if the port is busy, without killing a vendor/debug dropbear.
The actual port is shown in the UI and `/status`.

Acceptance:

- after a simulated esinit passwd rewrite, `car` still exists;
- `ssh car@<ip> -p 2222` works after reboot;
- a busy 2222 selects 2223 and updates status;
- restore removes `car` from both templates and `shadow.Global`.

## Phase 1c — Home-screen tile placement

The first build put `WifiTile` only on `listPage1`; the car photo showed the
free cells were on page 2. The tile also used white text/icon on the light
theme. The fix:

- `firstEmptySlot(customIndex)` scans pages 1–3 and returns the Nth free cell;
- the tile is inserted on every app page but visible only on the selected page;
- install-time ordinal: Hello=0, Battery=1, WiFi=2, Tweaks=3;
- coordinates stay relative to `menuPageN` below the top menu bar;
- tile labels use `HomeThemeHelper.normalTextColor`; icons are blue;
- the WiFi installer refreshes the Hello/Battery tile components and icons;
- Battery/Tweaks generators use the same allocator.

See `docs/UI-TILES.md`.

## Phase 1d — WiFi stack and CarPlay pairing

- start `/usr/sbin/wpa_supplicant -u -Dnl80211 -iwlan0` after stopping
  `supplicantmanager`; keep it alive for client and AP;
- scan via `wpa_cli`, client connect via ConnMan plus a direct `wpa_cli`
  fallback; ConnMan handles DHCP;
- AP start retries full/simple tether syntax and logs `connmanctl
  technologies` on failure;
- `Start CarPlay AP` sets `vbtd` powered/discoverable/pairable on the
  session bus, reads the BT address for the CarPlay IE, then starts AP,
  iAP2/mDNS and the VBT flag.

Acceptance: Scan lists nearby SSIDs; Connect imported joins the network and
shows an IP; Start CarPlay AP makes the car discoverable and brings the
AP up at 10.42.0.1.

## Phase 2 — Client proof

Put `wifi.txt` at USB root with `ssid=`/`psk=`; press Import USB; press
Client.

Verify:

- `wlan0` connects via ConnMan
- IP, gateway, DNS; internet reachability
- SSH over client address
- no OEM reclaim

Acceptance: client mode stable across reboot.

## Phase 3 — Wireless CarPlay proof

1. In the app, set CarPlay identity fields (`carplay_id`, `carplay_btaddr`)
   if auto-detection is empty.
2. Start CarPlay AP.
3. Confirm `connmanctl technologies` shows `Tethering = True` and
   `TetheringIdentifier = IONIQ5-CarPlay`.
4. Confirm `carplay_iAP2` and `carplay_mdnsd` active.
5. Confirm `ENABLE_WIRELESS_CARPLAY=true` in VBTConfig.
6. Pair iPhone over Bluetooth; select wireless CarPlay.
7. Capture daemon log + DLT.

If it fails:

- Check `connmanctl tether_carplay_ie` return/log.
- Check `SupportWirelessCP` persistence key (use persistence tooling).
- Check whether `vbtd` restarted and advertises wireless CarPlay.
- Check iAP2 logs for WiFi info/auth errors.
- Try 2.4 GHz channel 6 vs 5 GHz 36.

Acceptance: iPhone shows CarPlay UI over Wi-Fi.

## Phase 4 — App/UI validation

- WiFi tile visible in the first free Advanced app-grid slot (page 2 or 3
  when page 1 is full), Wide home, All Menus, Setup.
- WiFi page tabs work; status matches daemon.
- Import button fills SSID/CarPlay SSID.
- WLAN toggle and mode buttons work.
- SSH toggle/key import/status work.
- Coexists with Hello, the Battery page, and the later Battery tile.

## Phase 5 — Reboot / restore

- Reboot; service starts; saved mode re-applies.
- Restore script returns original QML and removes user/service.
- Diagnostic after restore shows stock state.

## Phase 6 — Documentation

- Update `README.md`, `SPEC.md`, `docs/FINDINGS.md` with car results.
- Record CarPlay identity/persistence findings.
- Add DLT log excerpts.

## Test matrix

| Test | Offline | On car |
|---|---|---|
| Daemon status JSON | yes | yes |
| Import `wifi.txt` | yes | yes |
| Client connect | no | yes |
| AP + DHCP | no | yes |
| SSH password/key | no | yes |
| Wireless CarPlay | no | yes |
| OEM reclaim watchdog | no | yes |
| QML variants stock/Hello/Battery | yes | yes |
| Install/restore | partial | yes |
| Reboot persistence | no | yes |

## Risks

1. Wireless CarPlay gating/auth — unproven.
2. OEM `supplicantmanager` respawn — watchdog/mask.
3. SMACK/iptables — `/bin/sh -c` service, explicit rule.
4. USB gadget side effect from `carplay_iAP2.service`.
5. 5 GHz regulatory channels; fallback 2.4 GHz ch 6.
6. Password auth default `car/electric`; change it.
