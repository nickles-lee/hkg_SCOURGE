# Status — WiFi app build

**Date:** 2026-11-06
**Target:** Ioniq 5 Gen5W, `NE_E.EUR.S5W_L.001.001.260128`

## VM validation (2026-11-06/07)

Validated in the firmware VM (vendor kernel 4.1.27-abl + decrypted 260128
rootfs, using per-run overlays):

- **Vendor Qt 5.4 qmllint**: all 21 shipped QML artifacts pass, including
  every patched host variant.
- **Daemon on the guest kernel**: one-shots, HTTP `/status`+`/logs`, an
  ~80 s no-wlan0 soak, and clean shutdown all healthy.  One real bug found
  and fixed: instant death when loopback was not up (now brings `lo` up
  and retries the bind for 30 s).
- **Real connmand 1.29**: the `--hidden/--frequency/--maxsta/--appinfo 1`
  tether form parses and issues the correct D-Bus calls; the positional
  form reproduces the car's `unknown option` errors; `config --wifi`
  reproduces the car's exact error; the agent registers.  Server-side
  technology calls need the car's wifi hardware.
- **Home/Star watcher**: fully validated against the real session bus
  (home method call and star signal both bump `home_seq` correctly).
- **texfat log dump**: validated end-to-end with the real Tuxera driver.
- **Installer/restore on the real QML tree**: status=OK, all variants
  detected, SMACK labels preserved, `car` persisted in all four account
  files, supplicantmanager units masked, the manual-start fallback branch
  exercised, and restore returns the exact stock md5.
- **Real AppHomeScreen** with `-gui:3`: loads the patched `qml_eco3.0`
  tree, draws the patched Advanced QML (WifiTile + WifiPage) with zero
  QML errors.  The later Wayland teardown crash is the documented
  headless-compositor limitation.

Details in `docs/FINDINGS.md` §5.2f.

## Offline hardening (2026-11-07)

- **Log-bundle prune hardened** (`rm -rf` injection fix): only the
  daemon's own `wifi_logs_<14-digit stamp>` names are prune candidates,
  and deletion is shell-free (`safe_rmtree`, `lstat`-first so symlinks are
  unlinked not followed, depth-capped). `tests/test_prune.sh` proves
  planted injection names do not execute and decoys are kept;
  `tests/test_wifi_stack.sh` guards against the shell form returning.
  **Status: offline-tested — VM retest and car validation still pending.**

- Firmware findings documented in `docs/FINDINGS.md`.
- Static C daemon `src/wifi_daemon` built and smoke-tested:
  - `/status`, `/health`, `/scan`, `/mode`, `/wlan`, `/import`, `/ssh`
  - import parser for key=value and two-line `wifi.txt`
  - state JSON and persistent config
- QML page `HomeScreen/WifiPage.qml` built and syntax-smoke-tested with
  Qt 6 `qmlscene6` using stubs (vendor Qt 5.4 lint still pending in VM).
- Home-screen integration generated for:
  - stock
  - Hello
  - Battery
  - Hello + Battery
- WiFi tile added to Advanced pages 1-3 with a global first-free-slot
  allocator (page 2/3 when page 1 is full), Wide home, All Menus and Setup.
- Install-time custom tile order: Hello=0, Battery=1, WiFi=2, Tweaks=3.
  Battery and Tweaks generators use the same allocator.
- The installer distinguishes the page-based Battery app from the later
  Battery-tile app and selects `hello_battery_wifi` vs
  `hello_battery_tile_wifi` accordingly, so reinstalling WiFi does not drop a
  Battery tile.
- Hello/Battery/WiFi/Tweaks tiles use the theme text colour and blue icons,
  not white-on-light. The car photo showed the old Hello tile was nearly
  invisible on the light theme.
- The installer copies the refreshed Hello/Battery tile components and icons
  so existing installs get the fix without a separate reinstall.
- Credentials are still copy/import-only: no auto-connect; UI enables WLAN,
  Client/CarPlay and SSH.
- `docs/UI-TILES.md` added as the general home-screen tile guide.
- Installer/restore offline tests pass for all four bases, plus:
  - SSH user survives a simulated `esinit` passwd rewrite;
  - port-conflict fallback selects 2223/2224;
  - `make test` → all PASS.
- USB tree assembled at `out/usb` (also `out/wifi_app_usb.zip`).
- Static `dropbearmulti` bundled.
- User `car` / password `electric` installer path, persisted in
  `/etc/passwd.legacy` and `/etc/passwd.secure` (the firmware rewrites
  `/tmp/passwd` on every boot), plus a daemon startup guard.
- SSH defaults to port **2222**; automatic fallback to 2223-2230 if busy.
  A vendor/debug dropbear on 22 is detected but never killed.
- Secure-console layout verified: `/etc/passwd -> /tmp/passwd`,
  `/etc/shadow -> /tmp/shadow -> /etc/shadow.Global`, and stock 260128 has
  no dropbear binary/socket. The app does not patch `esinit`.
- CarPlay AP + CarPlay IE + iAP2/mDNS + `ENABLE_WIRELESS_CARPLAY` patch.
- WiFi stack ownership: start `/usr/sbin/wpa_supplicant -u -Dnl80211` after
  evicting `supplicantmanager`; scan via `wpa_cli`, client connect with
  `wpa_cli add_network/set_network/select_network` and ConnMan DHCP;
  AP start keeps the supplicant alive before `connmanctl tether`.
- CarPlay AP button (labelled "Start CarPlay AP"; it also opens the Bluetooth
  pairing window): power/discoverable/pairable via the vendor `vbtd`
  CommonAPI adapter on the session bus, real BT address for the CarPlay IE,
  and AP/iAP2/mDNS start in one action.
- Diagnostic script expanded with wpa_supplicant, `iw list`, `vbtd`,
  session-bus names and adapter introspection.
- Log dumps to the stick: automatic ~15 s after daemon start and every
  10 min while applies fail, plus a **Save logs** button (POST
  `/logs/dump`, `wifi_daemon --dump-logs`). Bundles contain daemon,
  wpa_supplicant, ssh and install logs, status.json, a redacted config and
  a live state snapshot; the 5 newest are kept.
- wpa_supplicant start fixed for the car build: no `-f` (not compiled in),
  no `-B` (would silence the log), driver list `-Dnl80211,wext`, no
  `-e entropy.bin` (would be self-killed by stop_oem_wifi), and the real
  error line from wpa_supplicant.log is shown in the UI.
- Exit/home-key follow-up: prominent "✕ Exit" button with clearance below
  the 90px bar; the page now also closes on app switches
  (`onSwitchToOtherApp`); the installer verifies the AppHomeScreen restart
  and reports `REBOOT_REQUIRED` (the daemon updates via systemd but the
  UI keeps old QML in memory until it restarts).
- Daemon responsiveness: failed applies back the watchdog off to 60 s
  instead of hammering the stack every 15-20 s; status.json keeps updating
  during long connect waits; worst-case connect waits shortened.
- Round-4 fixes (firmware-verified by executing the car binaries):
  - tether switched to `--hidden/--frequency/--maxsta/--appinfo 1`; the
    positional form was the source of `cmd_tether_wifi: unknown option`;
  - client connect uses the ConnMan agent flow (`agent on` + `connect` +
    passphrase) because this build has no `config --wifi`; output is
    redacted;
  - `wpa_cli` always uses `-p /var/run/wpa_supplicant`;
  - `wifi_daemon`/`dropbearmulti` use the SMACK floor label `_` (the
    Battery-proven label); `supplicantmanager.path`/`.service` are masked
    by the installer and unmasked by restore;
  - overlay conforms to `APP_OVERLAY_CHECKLIST.md` (requested/open/close,
    foreground gating, full input shield, close on navigation);
  - header/exit/console moved ~24 px inside the 90 px menu bar;
  - daemon tails the session bus for Home/Star, writes `home.json`
    immediately and exposes `home_seq`/`home_epoch`; QML closes within
    ~0.5-1 s while open;
  - the real client-connect error is saved before `ssh_start()`, so the
    UI no longer reports "SSH deferred: no WLAN IP" instead of the actual
    association failure;
  - HTTP/apply sockets are `FD_CLOEXEC`, so a hung `systemctl`/`connmanctl`
    child cannot keep the daemon's port bound.
- Self-review fixes: session-bus address off-by-2 (pairing was dead on the
  car), own wpa_supplicant config (no open-network auto-associate), ConnMan
  service token lookup, scan no longer changes mode/auto-connects, SSH
  rebinds on IP change, 2s status polling with file-mirror reads, pairing
  stop no longer powers Bluetooth on, scan progress messages.
- UX fixes after the first on-car run: the WiFi page sits below the 90px
  status bar (`y: MENU_BAR_Y`, 630px layout), consumes clicks, and closes on
  the steering-wheel Home/Back key in Advanced, Wide, All Menus and Setup.

## Not yet done (needs the car / iPhone)

- On-car confirmation that `wlan0` appears and ConnMan tether AP works with
  the OEM stack evicted.
- SSH password login using the static dropbear on the car.
- Wireless CarPlay iPhone connection.
- `SupportWirelessCP` persistence check if CarPlay does not advertise.
- Vendor Qt 5.4 `qmllint` inside the 260128 VM.
- On-car reboot test: `ssh car@<ip> -p 2222` after a full power cycle.
- Final screenshots/UX adjustments.
- On-car verification that the WiFi tile lands on page 2 next to Hello and
  survives a page swipe/reboot.
- On-car confirmation that Scan lists nearby SSIDs and Connect imported
  obtains an IP.
- On-car confirmation that Start CarPlay AP enables discoverability and
  the AP comes up at 10.42.0.1.

## USB tree

```text
out/usb/
  payload/
  backup/
  manifest.md5
  run_stuff.sh
  run_stuff_restore.sh
  run_stuff_diag.sh
  README.txt
  README-wifi.txt
```

Copy the contents to a FAT/exFAT stick.

## Recommended first test

1. Diagnostic stick first (`run_stuff_diag.sh` as `run_stuff.sh`).
2. Install app.
3. CarPlay AP + SSH from laptop.
4. Client mode with `wifi.txt`.
5. iPhone wireless CarPlay.
6. Restore.
