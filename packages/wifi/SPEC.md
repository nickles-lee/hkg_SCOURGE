# SPEC — WiFi / CarPlay / SSH app for Ioniq 5 Gen5W (260128)

## 1. Target

| Item | Value |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W (LG/Velix) |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Platform | x86-64 Linux, kernel `4.1.27-abl`, Qt 5 / QML |
| WLAN | Broadcom BCM4359 / CYW89359, PCIe, `wlan0` |
| UI process | `/app/bin/AppHomeScreen` (AppLauncher app ID 11) |
| QML root | `/app/share/AppHomeScreen/qml_eco3.0/` |
| Deployment | `initial_root` USB failsafe, `run_stuff.sh` as root |
| State | `/rw_data/wifi/` |
| Daemon | `/rw_data/wifi/wifi_daemon`, HTTP on `127.0.0.1:8090` |

## 2. Goals

1. Add a top-level **WiFi** tile/page to the home screen, All Menus and Setup.
2. Toggle the WLAN interface on/off.
3. Import SSID + key from `wifi.txt` on the USB root.
4. Client/STA mode: connect to a network using imported credentials.
5. CarPlay mode: start an AP with the CarPlay information element, start the
   CarPlay transport services, and enable the Bluetooth wireless flag.
6. SSH server: start/stop, key import, password `car` / `electric`.
7. Diagnose and restore entirely through the USB harness.
8. Coexist with the Hello and Battery QML patches.

## 3. Non-goals

- No firmware flash, updater, bootloader or partition changes.
- No new AppLauncher app ID or separate GUI process.
- No modification of AppNavi, AppProjection or the updater binaries.
- No automatic CarPlay connection at boot without user action.
- No cellular/modem configuration.

## 4. Architecture

```text
USB run_stuff.sh
  -> installs QML page/tile + daemon + dropbear + systemd unit
  -> starts wifi-daemon.service
       |
       +-- /rw_data/wifi/wifi_daemon (static C)
       |     - HTTP 127.0.0.1:8090
       |     - /rw_data/wifi/status.json
       |     - config / logs / backups
       |     - owns wlan0 via ConnMan / wpa_supplicant
       |     - starts dropbear, patches VBTConfig, starts CarPlay services
       |
       +-- WifiPage.qml inside AppHomeScreen
             - polls status.json / HTTP
             - WLAN toggle, mode selector, client scan, CarPlay config, SSH
```

### 4.1 Daemon responsibilities

- Load `cfg80211` / `bcmdhd` if `wlan0` is missing.
- Stop the OEM `supplicantmanager` path/service, remove `/tmp/no_unece`, and
  kill stale supplicant processes.
- Ensure `connman.service` is active.
- Apply one of three modes:
  - `off`: down interface, stop AP/client/SSH.
  - `client`: ConnMan station mode with imported SSID/key; DHCP via ConnMan.
    Services are matched by `md5(ssid)` inside the `wifi_` identifier, so
    spaces or a truncated name column cannot break the lookup.
    The daemon starts and owns `/usr/sbin/wpa_supplicant -u -Dnl80211 -iwlan0`
    after evicting `supplicantmanager`; scan uses `wpa_cli` and connection
    falls back to `wpa_cli add_network/set_network/select_network`.
  - `carplay`: ConnMan AP tether + CarPlay IE + iAP2/mDNS + VBT flag.
- Watchdog every 10 s:
  - stop respawned `supplicantmanager`,
  - re-apply CarPlay AP if down,
  - re-apply client if no IP,
  - restart SSH if enabled but down.
- Write status JSON once per second.
- Log to `/rw_data/wifi/daemon.log`.

### 4.2 State machine

Mode application is executed in a forked child (`apply_mode_work`), so the
HTTP loop and `status.json` polling stay responsive while `wpa_supplicant`,
ConnMan and the CarPlay stack are brought up. A new mode request cancels the
in-flight child and starts a fresh apply (`applying: true` in `/status`).
The child hands back its result and pairing/SSH state through small files in
`/rw_data/wifi/`; the watchdog only re-applies while no child is running and
backs off to 60 s after a failure.

Command forms are firmware-verified (the car binaries were executed with
their own loader): tether uses
`--hidden/--frequency/--maxsta/--appinfo 1`; client connect uses the ConnMan
agent flow because this `connmanctl` has no `config --wifi`; every `wpa_cli`
call names `-p /var/run/wpa_supplicant`.  The OEM `supplicantmanager.path`
and `.service` are masked by the installer so they cannot respawn and take
the control socket.  The daemon and dropbear carry the SMACK floor label
`_`, not `Privileged`.

| State | WLAN | ConnMan | CarPlay services | SSH |
|---|---|---|---|---|
| `off` | down | disabled | left as-is | stopped |
| `client` | station | Wi-Fi enabled, service connected | left as-is | optional |
| `carplay` | AP | tether on + CarPlay IE | iAP2 + mDNS started | optional |

Mode changes are explicit user actions. The daemon starts in the last saved
mode, but the installer creates `mode=off`, `wlan_on=0`.

## 5. Daemon API

Bound to `127.0.0.1:8090` only.

| Method | Path | Meaning |
|---|---|---|
| GET | `/status` | full JSON state |
| GET | `/health` | liveness |
| GET | `/scan` | scan Wi-Fi networks (`wpa_cli`, ConnMan fallback) |
| GET | `/logs?source=daemon\|wpa\|ssh` | last 16 KB of the selected log |
| POST | `/mode?value=off\|client\|carplay` | switch mode |
| POST | `/wlan?on=0\|1` | WLAN toggle |
| POST | `/import` | import `wifi.txt` from USB |
| POST | `/client/connect` | connect using imported client credentials |
| POST | `/client/disconnect` | disconnect / off |
| POST | `/carplay/start` | start AP + CarPlay stack |
| POST | `/carplay/stop` | stop WiFi |
| POST | `/carplay/restart` | restart AP + CarPlay stack |
| POST | `/carplay/pair` | start AP + enable BT discoverable/pairable |
| POST | `/carplay/pair/stop` | turn BT discoverability off |
| POST | `/ssh?enabled=0\|1&port=2222` | start/stop SSH |
| POST | `/ssh/import` | import `authorized_keys` from USB |
| POST | `/logs/dump` | copy log bundle to the USB stick |

CLI equivalents:

```sh
wifi_daemon --status
wifi_daemon --mode client|carplay|off
wifi_daemon --import
wifi_daemon --import-file /path/to/wifi.txt
wifi_daemon --ssh on|off
wifi_daemon --dump-logs
```

## 6. Config and status

### 6.1 `/rw_data/wifi/config.conf` (mode 0600)

```ini
mode=off
wlan_on=0
ssid=
psk=
carplay_ssid=IONIQ5-CarPlay
carplay_psk=<random>
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
carplay_id=
carplay_btaddr=
ssh=1
ssh_port=2222
```

### 6.2 `/rw_data/wifi/status.json`

Fields: `ok`, `mode`, `wlan_on`, `driver`, `iface`, `ip`, `ssid`, `signal`,
`ap_active`, `ap_ssid`, `ap_clients`, `carplay_ready`, `carplay_iap2`,
`carplay_mdns`, `ssh_active`, `ssh_enabled`, `ssh_port`, `ssh_port_fallback`,
`vendor_ssh_22`, `secure_console`, `debug_console`, `imported`,
`imported_from`, `client_ssid`, `carplay_ssid`, `carplay_psk`,
`carplay_band`, `carplay_channel`, `carplay_btaddr`, `carplay_id`,
`bt_discoverable`, `bt_pairable`, `pair_message`, `applying`, `message`,
`home_seq`, `home_epoch`, `home_source`, `last_app_key`, `home_monitor`.
`home.json` is a tiny side file written immediately on a Home/Star event:
`{"seq":N,"epoch":E,"source":"home|star","last_app_key":K,"monitor":true}`.

## 7. QML integration

New components in `qml_eco3.0/component/`:

| File | Purpose |
|---|---|
| `WifiTile.qml` | home/menu tile with WiFi glyph |
| `WifiSwitch.qml` | toggle switch |
| `WifiButton.qml` | pill button |

New page in `qml_eco3.0/HomeScreen/`:

| File | Purpose |
|---|---|
| `WifiPage.qml` | full-screen WiFi/CarPlay/SSH UI |

Patched variants generated by `scripts/build_artifacts.py`:

| Variant | Base |
|---|---|
| `QHMI_HomeScreenMain_Advanced.wifi.qml` | stock |
| `.hello_wifi.qml` | Hello |
| `.battery_wifi.qml` | Battery page |
| `.hello_battery_wifi.qml` | Hello + Battery page |
| `.battery_tile_wifi.qml` | Battery tile |
| `.hello_battery_tile_wifi.qml` | Hello + Battery tile |
| `QHMI_HomeScreenMain_Wide.wifi.qml` | stock |
| `.hello_wifi.qml` | Hello |
| `Screen_HomeScreen_AllMenus.wifi.qml` | stock |
| `.hello_wifi.qml` | Hello |
| `Screen_HomeScreen_Submenu_Setup.wifi.qml` | stock |
| `.hello_wifi.qml` | Hello |
| `QCustomizedMenuListAllowEmpty.wifi.qml` | stock |
| `.hello_wifi.qml` | Hello |
| `Q_ThemeImage.qml` | stock + `file://` support |

The installer detects the current QML by payload markers and stock MD5, then
copies the matching variant. It backs up the current files first.
It stamps `wifi_daemon` and `dropbearmulti` with `chsmack -e _` (the SMACK
floor label proven by the Battery app) and masks
`supplicantmanager.path`/`.service`; `run_stuff_restore.sh` unmasks and
restarts the OEM path unit.

`WifiPage.qml` follows `APP_OVERLAY_CHECKLIST.md`: it owns its visibility
(`requested` + `open()`/`close()`), gates on `!AppHomeModel.isAtOtherApp`,
closes on the back affordance and every navigation/foreground change, covers
the page with a `preventStealing` all-buttons `MouseArea`, and places all
real content above it at `z: 2`.  Hosts declare it at
`y: ConstantModel.MENU_BAR_Y` with no `visible:` override and call
`wifiPage.open()` from tiles/menu signals.  Home/Star closes it through the
daemon's `home_seq`.

### 7.1 Tile placement

Custom tiles use `emptySlots(pageModel)` and `firstEmptySlot(customIndex)` in
the Advanced QML. `firstEmptySlot` scans `listPage1`, `listPage2`, then
`listPage3` and returns the `customIndex`-th free cell. Each page gets the
tile, but only the page returned by the allocator is visible. This places a
tile on page 2 or page 3 when page 1 is full, and guarantees exactly one tile.

The ordinal is fixed at install time by counting existing custom tile markers:
Hello=0, Battery=1, WiFi=2, Tweaks=3. Coordinates stay relative to
`menuPageN`, whose parent page already starts below the top menu bar
(`MENU_BAR_Y + MENU_BAR_Y / 2`). Never use absolute screen Y for a tile.

The same allocator is used by the Battery and Tweaks generators. The WiFi app
does not add a page, so `totalPageCount` remains `totalPagesNormalMode + 2`
(Battery page only). See `docs/UI-TILES.md` for the full guide.

### 7.2 Theme

Custom tile labels use `QText`'s default `HomeThemeHelper.normalTextColor`.
Normal icons are generated in blue (`#2F6FED`) and disabled icons in grey
(`#8A8A8A`), so they are visible on both the light and dark home themes. Do
not hardcode `color: "#FFFFFF"` in a custom tile.

## 8. Installer

`run_stuff.sh`:

1. Set `PATH`, `USB_DIR`, payload/backup paths.
2. Remount USB rw.
3. Verify `manifest.md5`.
4. Detect QML base variants.
5. Back up current QML to `/rw_data/wifi/backup/<stamp>/` and USB
   `backup_current/`.
6. Remount `/` rw.
7. Create/update `car` user in `/etc/passwd` and `/etc/shadow`.
8. Install icons, daemon, dropbear, service.
9. Create default config; import `wifi.txt` / `authorized_keys` if present.
10. Install QML variants; preserve SMACK labels.
11. Clear QML caches.
12. Stop OEM `supplicantmanager`; remove `/tmp/no_unece`.
13. Enable/start `wifi-daemon.service`; fallback to manual start.
14. Restart `AppHomeScreen` via AppLauncher app ID 11.
15. Write `run_stuff_result.txt` and `/rw_data/wifi/result.txt`.

`run_stuff_restore.sh` reverses the QML, service, user and firewall changes.

`run_stuff_diag.sh` is read-only and writes `wifi_diag.txt`.

## 9. SSH

- Binary: static `dropbearmulti` (425 KB, musl) from the owner-access project.
- Installed to `/rw_data/wifi/bin/dropbearmulti`; symlink `dropbear`.
- Run: `dropbear -r <hostkeys> -p <wlan-ip>:<port> -P /rw_data/wifi/ssh/dropbear.pid`.
- User: `car`, UID 0, password `electric` (SHA-512 crypt in `/etc/shadow.Global`).
- Persisted in `/etc/passwd.legacy` and `/etc/passwd.secure` because `esinit` rewrites `/tmp/passwd` on every boot.
- Key-only also supported via `authorized_keys`.
- Firewall: explicit `INPUT -i wlan0 -p tcp --dport <port> -j ACCEPT`.
- Default port 2222; legacy port 22 is removed on restore. If 2222 is busy, the daemon falls back to 2223-2230 and reports the actual port.

## 10. Security and safety

- No PSK logging; config mode 0600.
- SSH password is a known default; user is told to change it.
- `car` is UID 0 and is added to both `passwd.legacy` and `passwd.secure`;
  this intentionally bypasses the secure-console `nologin` policy for the
  app's SSH account.
- SSH only on `wlan0`; firewall rule removed on stop.
- No writes outside `/app/share/AppHomeScreen`, `/rw_data/wifi`, `/etc`
  (user/service) and the temporary rootfs remount.
- Installer aborts on unknown QML base.
- Restore path provided.
- CarPlay/iAP2 services are only started, not modified.
- `VBTConfig.json` is backed up before patching.

## 11. Test plan

### Offline

- `src/wifi_daemon --status` → valid JSON.
- `--import-file wifi.txt.sample` → parses SSID/CarPlay SSID.
- HTTP `/health`, `/status`, `/scan`, `/mode` → valid JSON.
- QML brace/structure check (host) and vendor `qmllint` in the 260128 VM.
- Installer variant selection against stock/Hello/Battery copies (planned).

### On car

1. Diagnostic payload → confirm `pcb_version`, `optUNECE`, `wlan0`,
   `supplicantmanager`, `VBTConfig.json`.
2. Install app; verify WiFi tile and page.
3. Import `wifi.txt`; start Client; verify IP/internet.
4. Start CarPlay AP; verify AP SSID, DHCP, SSH from laptop.
5. Pair iPhone; verify wireless CarPlay; capture logs.
6. Reboot; verify persistence.
7. Restore; verify original QML/UI returns.

## 12. Known limitations

- Wireless CarPlay needs on-car iPhone validation.
- The app is a QML page, not a separate app ID/process.
- OEM `supplicantmanager` is masked by the installer; a firmware update can
  reset `/etc/systemd/system` and undo the mask.  The watchdog still stops a
  respawned manager and the diagnostic reports the mask state.
- Starting `carplay_iAP2.service` may reconfigure the front USB port.
- Password auth is enabled for convenience; key-only is recommended.
