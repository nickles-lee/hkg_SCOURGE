# Findings — Ioniq 5 Gen5W WLAN / Wireless CarPlay / SSH

Target: `NE_E.EUR.S5W_L.001.001.260128`, x86-64, kernel `4.1.27-abl`.
See `SPEC.md` and `README.md` for the app implementation.

## 1. Executive summary

| Question | Answer | Confidence |
|---|---|---|
| WLAN hardware present? | Yes — Broadcom BCM4359 / CYW89359, PCIe, `wlan0` | Proven on car + firmware |
| Can WLAN be enabled? | Yes — stock stack can load it; UI hidden by variant gate | Firmware-confirmed |
| Client/STA? | Yes — ConnMan + wpa_supplicant | Firmware-confirmed |
| AP/hotspot? | Yes — ConnMan tether + built-in DHCP | Proven on car |
| Wireless CarPlay? | Very likely — AP + CarPlay IE + iAP2/mDNS + VBT flag all exist | Needs iPhone test |
| WiFi app? | Yes — QML page + static daemon + service | Built |
| SSH? | Yes — static dropbear, password/key auth | Built + prior field test |

## 2. Hardware / driver

| Item | Evidence |
|---|---|
| Module | BCM4359 / CYW89359 |
| Bus | PCIe; `bcmdhd.ko` alias `pci:v000014E4...` |
| Driver | `lib/modules/4.1.27-abl/.../bcmdhd/bcmdhd.ko`, v1.363.68 |
| Firmware | `lib/firmware/bcmdhd/rtecdc.bin`, `nvram.txt`, `4359b1.clm_blob`, FW 9.40.112.10 |
| Interface | `wlan0` |
| Module blacklist | `/etc/modprobe.d/wifi_disable.conf`; explicit modprobe/insmod works |

Prior field test on this unit confirmed `bcmdhd` loaded, firmware open
succeeded, and `dhd_bus_start` ran.

## 3. Why Wi-Fi looks disabled

Two gates:

1. **UI gate** — `AppSetup` has full Wi-Fi client UI, gated by
   `isWifiChipsetSupport()`. `AppEngineerMode/qml/System/Global.qml`
   `getIsWifiSupport()` returns false for `pcb_version` 11/12 or UNECE.
2. **Runtime gate** — `supplicantmanager.path` watches `/tmp/no_unece`.
   `AppLauncher` creates it when launched with `-un:0`; the NE/Ioniq 5 table
   entry uses `-un:0`. The path unit starts `supplicantmanager.service`, which
   loads `cfg80211`, inserts `bcmdhd`, and starts `wpa_supplicant`.

So the radio can be running while the Settings menu is hidden. The app owns
the stack directly and bypasses the gate.

## 4. Modes

### Client / STA

- ConnMan + `wpa_supplicant`.
- `AppSetup` Decompiled classes: `WifiMonitor`, `WifiController`,
  `WifiListModel`, `WifiStateMachine`, `ConnmanIface`.
- Persisted setting: `wifi.wifiOnOff`.

### AP / hotspot

- `connmanctl tether wifi on <ssid> <psk> --hidden <yes|no>
  --frequency <MHz> --maxsta <n> --appinfo 1` (verified by executing the
  car binary; the positional form is rejected).
- ConnMan has `enable_wifi_tethering`, `dhcp_server_start`,
  `TetheringIdentifier`, `TetheringPassphrase`, `TetheringFrequency`,
  `TetheringMaxSta`.
- Vendor `wpa_supplicant` contains hostapd/AP-mode code.
- Prior field test brought up `10.42.0.1` and DHCP; OEM stack reclaimed it
  after ~90 s (the app's watchdog/eviction addresses this).

### Wireless CarPlay

Evidence:

| Component | Evidence |
|---|---|
| ConnMan CarPlay IE | `connmanctl tether_carplay_ie wifi on <manufacturer> <model> <name> <id> <btaddr>` |
| ConnMan properties | `SetTetherCarPlayIE`, `GetTetherCarPlayIE`, `AddCarPlayIE`, `DelCarPlayIE`, `CarPlayManufacturer`, `CarPlayModelName`, `CarPlayDeviceName`, `CarPlayDeviceId`, `CarPlayBluetoothAddr` |
| Bluetooth flag | `vbtd-51`/`vbtd-63` read `ENABLE_WIRELESS_CARPLAY` from `/app/data/vbt/VBTConfig.json` |
| Wireless projection | `GetSupportWirelessCP`, `ProjectionDeviceStateManager`, `PowerStateManager::onSupportWirelessCPChanged` |
| Wireless transport | `iAP2Service-2.0`: `createWirelessCarPlayTransportComponentParamList`, `handleRequestAccessoryWiFiConfigInfo`, `WiFiInformation` |
| mDNS | `app/bin/mdnsd`, `carplay_mdnsd.service` |
| CarPlay service | `carplay_iAP2.service`, `iAP2Service-2.0` |
| UI strings | BluetoothHMI WPP strings; AppSetup `APP_SETTING_WIFI_DISABLE_WPP` |
| Home screen | `AppHomeScreen` logs `WIRELESS_CP SYSTEM LOAD FALSE` |

Missing: a stock caller of `tether_carplay_ie`; the trigger is likely
variant-coded or gated by `SupportWirelessCP`. The app performs the AP + IE +
services explicitly. Needs iPhone test.

### P2P

Driver/ConnMan support P2P; not used.

## 5. SSH

- No stock SSH server.
- Static `dropbearmulti` built by `packages/wifi/scripts/build_dropbear.sh`
  (pinned Alpine container / musl-gcc; static musl x86-64, ~425 KB, with the
  static-link gate in the same script).
- Prior field test started it over the AP and connected.
- Password auth now enabled for user `car` / `electric`; key import supported.
- Firewall default `DROP`; app adds a `wlan0` rule for the chosen port.

### 5.1 Secure-console gate and passwd layout

`lib/systemd/esinit` creates `/tmp/secure_console_disable` at boot, then
removes it if `micom.secure_console=1`; it also removes `/dev/ttyS2` and copies
either `/etc/passwd.legacy` (root `/bin/sh`) or `/etc/passwd.secure` (root
`/usr/sbin/nologin`) to `/tmp/passwd`.

On the real 260128 image:

```text
/etc/passwd  -> /tmp/passwd
/etc/shadow  -> /tmp/shadow -> /etc/shadow.Global
/etc/shells  -> /tmp/shells
```

Consequences:

- The SSH `car` user must be added to **both** `passwd.legacy` and
  `passwd.secure`, otherwise it disappears on the next boot.
- `/etc/shadow.Global` is the persistent shadow file.
- The stock rootfs has `dropbear@.service` but **no dropbear binary and no
  dropbear socket**. “Dropbear SSH on port 22” is not true for stock 260128;
  the earlier project had to bundle a static dropbear.
- `iptable_debug` is a separate micom parameter; the 260128 update rootfs has
  no consumer that makes the firewall permissive. The app still adds its own
  `wlan0` rules.
- Patching `esinit` line 478 alone may not restore serial: line 479 still
  removes `/dev/ttyS2`.

The app does not patch `esinit`. It persists `car` in both passwd templates and
re-checks at daemon startup.

### 5.2b Self-review fixes (2026-11-06)

A full review of the WiFi/CarPlay/SSH implementation found and fixed:

1. **Session-bus address off-by-2 (critical).** The parser advanced
   `p += 23` past `DBUS_SESSION_BUS_ADDRESS=`, but the needle is 25 bytes,
   so every Bluetooth D-Bus call used the address `S=unix:...` and failed.
   This alone made the whole pairing button dead on the car.
2. **OEM wpa_supplicant config.** The daemon ran the supplicant with
   `/etc/wpa_supplicant.conf`, which contains `network={key_mgmt=NONE}`:
   the supplicant auto-associates to any open network and fights ConnMan
   tethering. The daemon now writes its own
   `/rw_data/wifi/wpa_supplicant.conf` with no network blocks.
3. **ConnMan service lookup.** `find_service` took the *last* token of a
   `connmanctl services` line; connmanctl prints a trailing state word, so
   the ConnMan connect path never matched. It now scans tokens for the
   `wifi_` one.
4. **Scan changed the mode.** Scan silently flipped the mode to client, so
   the watchdog auto-connected within 20 s without user consent, and it tore
   down an active CarPlay AP. Scan now never changes the mode and only
   refuses while the CarPlay AP is running.
5. **SSH did not rebind on IP change.** dropbear binds to a specific wlan
   address; after a DHCP change it kept listening on the old IP. The
   watchdog now detects the change and restarts SSH.
6. **Status polling cost.** `build_status()` spawns ~6 subprocesses
   (connmanctl, ifconfig, iw, 2x systemctl) per call; it ran every second
   and again on every QML HTTP poll. The daemon now writes status every
   2 s and the QML reads the `status.json` file mirror first.
7. **Pairing-off powered Bluetooth on.** `bt_set_pairing(false)` also set
   Powered=true. Power is now only forced when enabling pairing.
8. Scan shows progress ("Scanning...", "scan: N networks") so the UI does
   not look frozen during the 10-20 s first-time stack bring-up, and
   retries the scan once if the first results are empty.

### 5.2c wpa_supplicant start failure on the car (2026-11-06)

The first on-car run of the supplicant start failed with
"wpa_supplicant failed to start". Root cause, from the firmware image:

- The car's `/usr/sbin/wpa_supplicant` is built **without
  `CONFIG_DEBUG_FILE`**: its usage text has no `-f`, so our
  `-f/rw_data/wifi/wpa_supplicant.log` made it print usage and exit on
  both attempts.
- `-B` would have silenced the log anyway: `daemon(0,0)` redirects
  stdout to /dev/null, overriding the `>>log` redirect.

The fixed invocation:

```sh
nohup /usr/sbin/wpa_supplicant -u -Dnl80211,wext -iwlan0 \
  -c/rw_data/wifi/wpa_supplicant.conf -P/rw_data/wifi/wpa_supplicant.pid \
  >>/rw_data/wifi/wpa_supplicant.log 2>&1 &
```

- no `-f` (unsupported), no `-B` (background via the shell so the log
  captures the real output);
- `-Dnl80211,wext` — the car build documents the comma list and falls back
  between drivers internally; a second attempt uses the compiled-in default
  order;
- **no `-e /rw_data/wifi/entropy.bin`**: the OEM supplicant is identified
  by exactly that cmdline argument in `stop_oem_wifi`, so passing it would
  make the daemon kill its own supplicant;
- on failure the last log line is surfaced in the UI message instead of a
  generic "see log";
- `tests/test_wifi_stack.sh` now cross-checks every passed option against
  the usage text compiled into the car's binary.

### 5.2d On-car round 3 fixes (2026-11-06)

- **Scan showed 0 dBm**: `scan_results` has five tab-separated fields; the
  splitter leaves `nf` at the last field index (4) but the guard was
  `nf < 5`, so every line was discarded and the ConnMan fallback returned
  its hardcoded `signal=0`. Fixed to `nf < 4`.
- **ConnMan service matching**: IDs are `wifi_<bssid>_<md5(ssid)>_...`.
  Matching is now done on the SSID's md5 inside the `wifi_` token, which
  is immune to spaces and to a truncated/padded name column. The
  `connmanctl config/connect` output is captured to the log.
- **Synchronous apply**: every mode change ran the whole 20-60 s network
  bring-up inside the HTTP handler, so the UI froze and a CarPlay press
  queued behind a client connect. Apply now runs in a forked child; a new
  request cancels it and starts fresh. The child hands back its result and
  the BT/SSH state via small files.
- **CarPlay tether**: the "simple" retry was invalid (connmanctl requires
  all six tether arguments). It is replaced by a 2.4 GHz channel-6 retry
  when the 5 GHz channel-36 tether fails; a successful fallback is saved
  to the config.
- **Log dumps never appeared**: the car's exFAT is Tuxera **texfat**
  (`kernel/fs/texfat.ko`), which the vfat/exfat/ntfs whitelist rejected.
  Mount detection now accepts any real mount point under
  `/media/usb`, `/media` or `/run/media` (preferring removable devices)
  and logs every failure. `state.txt` includes `/proc/mounts`.
- **Live console**: `GET /logs?source=...` returns the last 16 KB of a log;
  the wide layout shows a right-hand console with daemon/supplicant tabs
  and a Dump button.

### 5.2e On-car round 4 fixes (2026-11-06)

The on-car log showed `cmd_tether_wifi: unknown args/options`, "Error
enabling wifi tethering: Permission denied", a dead wpa_supplicant control
socket, and ConnMan "Not registered".  The car binaries were executed with
the firmware loader (`ld-2.25.so` + the firmware libraries) to verify each
command instead of trusting the help text:

- **Tether args** — the vendor parser accepts only
  `--hidden/--frequency/--maxsta/--appinfo`:
  `tether wifi on <ssid> <psk> --hidden no --frequency 5180 --maxsta 2
  --appinfo 1` reports `hidden [0] frequency [5180] maxsta [2]
  app_info [1]`.  The old positional form reports
  `unknown option [?][no]` / `unknown option [?][5180]`, exactly the car
  log.  The daemon now uses the option form for both 5 GHz and the 2.4 GHz
  fallback.
- **No `config --wifi`** — this build's `connmanctl config` only covers
  nameservers/timeservers/domains/ipv4/ipv6/proxy/autoconnect/remove.
  `config <service> --wifi passphrase ...` prints
  `Error '--wifi': Invalid argument`, then `connect` fails with
  `Not registered` because no agent is registered.  Client mode now runs
  the agent flow (`agent on` → `connect <service>` → passphrase → `quit`)
  with a paced stdin script, and redacts the key from the captured output.
- **SMACK** — the installer used `chsmack -e Privileged`.  That label is
  inherited by wpa_supplicant, wpa_cli and connmanctl, and has no rule for
  the ConnMan/supplicant sockets.  The Battery daemon deliberately uses the
  floor label `_`; WiFi now does the same for `wifi_daemon` and
  `dropbearmulti`.  This addresses the tethering "Permission denied" and
  the dead control socket.
- **OEM respawn** — `supplicantmanager.service` is `Restart=always` behind
  `supplicantmanager.path`; stopping it only makes it respawn and reclaim
  `/var/run/wpa_supplicant/wlan0`.  The installer now masks both units;
  the restore script unmasks and restarts the path unit.
- **Control socket** — every `wpa_cli` call now passes
  `-p /var/run/wpa_supplicant` explicitly, and a failed ping logs the
  directory listing (rate-limited).
- **Overlay geometry and input** — the page is at `y = MENU_BAR_Y` with
  header content starting ~24 px inside it, so the exit button and console
  tabs are no longer at the bar's edge.  It follows
  `APP_OVERLAY_CHECKLIST.md`: `requested`/`open()`/`close()`,
  `visible: requested && !isAtOtherApp`, navigation/foreground close
  handlers, and a full `preventStealing` all-buttons shield with content at
  `z: 2`.
- **Session-bus address quoting** — Dbuslaunch has written both
  `DBUS_SESSION_BUS_ADDRESS=unix:...` and the shell-quoted
  `DBUS_SESSION_BUS_ADDRESS='unix:...';` form.  `session_bus_address()`
  now strips the quotes/semicolon and a stale `guid=` before handing the
  address to `dbus-send`/`dbus-monitor`; this also hardens BT pairing.
- **Misleading client error** — `ssh_start()` sets "SSH deferred: no WLAN
  IP" when there is no address, and it ran *before* the failure message was
  copied.  Every client failure therefore looked like an SSH problem.  The
  real association/DHCP error is now captured first.
- **Socket inheritance** — `popen` children inherited the HTTP listener;
  a hung `systemctl` kept the port bound after the daemon was killed.  The
  listener and accepted sockets are now `FD_CLOEXEC`.
- **Home/Star** — the daemon now tails the session bus with `dbus-monitor`
  (`NotifyRequestHomeScreen` / `custom_key_value`), writes a tiny
  `home.json` immediately and exposes `home_seq`/`home_epoch` in
  `/status`.  The QML polls `home.json` at 500 ms while open and closes on a
  new event; `home_epoch` prevents a daemon restart (counter reset) from
  being mistaken for a key press.

### 5.2f VM validation on the real firmware (2026-11-06/07)

The app was validated in a local QEMU/KVM harness booting the exact vendor
kernel 4.1.27-abl + decrypted 260128 rootfs (SMACK active, vendor Qt 5.4,
Tuxera texfat).  Runs used per-run qcow2 overlays; the shared images were
never modified.

**Tier 1 — vendor QML parser + daemon**

- All 21 shipped QML artifacts (WifiPage, tiles, all 12 patched host
  variants, Q_ThemeImage) pass the car's own `/usr/bin/qt5/qmllint`
  (Qt 5.4), plus a stock control file.
- **Bug found and fixed**: the daemon died instantly with
  `bind(http): Cannot assign requested address` when loopback was not up
  yet.  It now brings `lo` up and retries the bind for 30 s.
- In-guest soak (~80 s, no wlan0): daemon alive, `status.json`/`home.json`
  written, HTTP `/status` and `/logs?source=...` return 200, clean SIGTERM.
- `md5sum` resolves on the firmware image (both the busybox applet and
  `/usr/bin/md5sum.coreutils` exist); the daemon/installer fallback is
  harmless insurance.
- Guest facts: busybox has **no** `wget` applet (use `bash /dev/tcp`).

**Tier 2 — real firmware services**

- Real `connmand` 1.29 + system bus started in-guest.  The option-form
  tether produces `cmd_tether_wifi arg num [13] hidden [0] frequency [5180]
  maxsta [2] app_info [1]` and issues the real D-Bus calls; the positional
  control reproduces `unknown option [?][no]/[?][5180]`.  `config --wifi`
  reproduces the exact car error.  Server-side "Method ... doesn't exist"
  errors are environmental: the VM has no network interfaces, so connmand
  registers no technology objects.  The actual SetProperty/tether acceptance
  still needs the car's wifi technology.
- `connmanctl` **exits 0 even on server errors** — validates the daemon's
  design of checking output text for "Error", not just the exit code.
- Agent flow: `agent on` registers against real connmand; the paced stdin
  script behaves (no premature passphrase consumption).
- SMACK A/B: inconclusive in the VM (default policy, not the car's rule
  set).  Learned: `/usr/sbin/connmand` itself carries `SMACK64="_"`, and
  `chsmack -e` (SMACK64EXEC) works for both `Privileged` and `_`.
- wpa_supplicant v2.5 `-u -g` global ctrl socket + `wpa_cli -g ... ping`
  works (both labels).
- **Home/Star watcher fully validated** against the real session bus:
  a dispatched `NotifyRequestHomeScreen` method call bumps `seq` with
  `source:"home"`; a `custom_key_value` signal bumps it with
  `source:"star"` and the right `last_app_key`.  Method calls to *unowned*
  names are rejected by the bus pre-dispatch and invisible to
  dbus-monitor — on the car the name is owned, so this is VM-only
  behaviour.
- **texfat log dump validated end-to-end**: the real Tuxera driver mounts
  the exFAT stick as `texfat` (exact car fstype string) and
  `--dump-logs` writes a complete bundle (daemon/wpa/ssh logs, redacted
  config, home.json, state.txt with SMACK labels and mask state).

**Tier 3 — installer end-to-end on the real QML tree**

- `run_stuff.sh` against the real `/app/share/AppHomeScreen`:
  `status=OK`, `variant=wifi/wifi/wifi/wifi/wifi` (the VM rootfs matches
  our reference md5s — no firmware drift), `oem_mask=ok`,
  `daemon=systemd-restart-failed+manual` — **the manual-start fallback
  branch was exercised for the first time and works**.
- Installed QML keeps the stock SMACK label (`SMACK64="_"`); the daemon
  and dropbearmulti get `SMACK64EXEC="_"`; `car` lands in passwd,
  passwd.legacy, passwd.secure and shadow.Global; both supplicantmanager
  units report `masked`.
- `run_stuff_restore.sh` returns the Advanced QML to the exact stock md5,
  removes the user, daemon and WiFi QML, and unmask succeeds.

**Tier 4 — real AppHomeScreen with the patched QML**

- The real launcher verifies AppHomeScreen's stock `.sign` (`CheckSign
  ret = 1`) and spawns it with the full vendor argv; the app registers
  (`[REGISTERED] AppId[11]`) and creates its IVI surface.
- Direct launch with `-gui:3` (the car's eco3.0 selector) confirms:
  `m_qmlBasePath ... "/app/share/AppHomeScreen/qml_eco3.0/"` — the
  **patched tree** — then `screenNext:
  "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"`, `SCREEN_STATE: START
  DRAW` → `DRAW FINISHED` with **zero QML errors**.  The patched Advanced
  (WifiTile + WifiPage embedded) loads cleanly in the real engine with the
  real ConstantModel/defineModel.
- The process later dies on the headless-compositor teardown
  (`wl_display error: invalid object`) — the documented harness graphics
  limitation, not our QML.
- VM facts worth remembering: the launcher passes `-gui:1` by default
  (bootinfo-dependent), and the launcher redirects child stdout to
  /dev/null — capture requires a direct launch.

**VM environment gotchas (for future runs)**

- `/var` is a symlink into `/rw_data`: `mkdir -p /var/run` fails on the
  dangling link — create `/rw_data/var/...` first.
- `dbus-daemon --system` needs `/tmp/passwd` (esinit normally provides
  it): `cp /etc/passwd.legacy /tmp/passwd`.
- `/mnt` is a symlink; use `/media` or `/tmp` for mounts.
- The 4.1 kernel refuses a second mount of the same ext4 with different
  options (rw then ro); bind-mount instead.
- `timeout(1)` does not exist in the guest.

### 5.3 WiFi supplicant ownership

`supplicantmanager` is the OEM "Wifi ACC Mode Manager" and its job is to start
`/usr/sbin/wpa_supplicant`. The first WiFi build stopped `supplicantmanager`
(and killed its process) to stop the OEM reclaiming `wlan0`, but then called
`wpa_cli`/`connmanctl` without ever starting a supplicant. Result on the car:
Scan returned no networks and Connect imported never associated.

The image has:

```text
/usr/sbin/wpa_supplicant
/usr/sbin/wpa_cli
/etc/wpa_supplicant.conf   (ctrl_interface=/var/run/wpa_supplicant)
```

The fix is to start our own after evicting the manager:

```sh
/usr/sbin/wpa_supplicant -B -u -Dnl80211 -iwlan0 \
  -c/etc/wpa_supplicant.conf -P/rw_data/wifi/wpa_supplicant.pid \
  -f/rw_data/wifi/wpa_supplicant.log
```

`-u` enables the `fi.w1.wpa_supplicant1` D-Bus interface ConnMan needs.
Scan/connect use `wpa_cli`; ConnMan remains the DHCP client. For AP mode the
same supplicant must stay alive before `connmanctl tether` is called.

### 5.4 Bluetooth is vendor vbtd, session bus

There is no BlueZ `bluetoothctl`/`hciconfig`/`hcitool` on the car. Bluetooth
is owned by `/app/bin/vbtd-63` (`vbtd.service`), which runs with
`EnvironmentFile=/tmp/session_appmgr.env`, so its control API is on the
**session bus**.

`vbtd` exposes a CommonAPI BlueZ-like adapter:

```text
org.bluez.Adapter.Adapter:v2_0
setPoweredAttribute
setDiscoverableAttribute
setPairableAttribute
getAddressAttribute
```

The stock BluetoothHMI app calls `BluetoothManagerQML.setDiscoverable(true)`
when the Add New screen opens (and sets pairable). Our first build only read
`/sys/class/bluetooth/hci0/address`/`hcitool dev`, so it reported “not
detected” and never made the car discoverable.

CarPlay pairing now reads `/tmp/session_appmgr.env`, calls the adapter over
`dbus-send --session`, sets powered/discoverable/pairable, reads the real
Bluetooth address, then starts AP + CarPlay IE + iAP2/mDNS.

### 5.2 Home-screen tile placement

The first 260128 WiFi build inserted `WifiTile` only into `listPage1` and used
`firstEmptyIndex(..., 1)`. On the real 1920 car, page 1 is full; the free
cells are on page 2. The photo of the home screen showed the Hello tile in the
first free cell of page 2 but no WiFi tile, because there was no
`wifiTile2`. The tile was also white-on-light and nearly invisible.

The fix is a global allocator: `firstEmptySlot(customIndex)` scans
`listPage1`..`listPage3` and returns the customIndex-th free cell. Each page
gets a tile, but only the page returned by the allocator is visible. The
ordinal is fixed at install time by counting existing custom tiles (Hello,
Battery, WiFi, Tweaks). Coordinates stay relative to `menuPageN`, which is
already below the top menu bar. See `docs/UI-TILES.md`.

## 6. Prior on-car evidence

From the TOCTOU / owner-access project:

- BCM4359/CYW89359, `bcmdhd`, `wlan0` confirmed.
- AP `10.42.0.1` + dropbear worked initially.
- OEM `connmand` + `supplicantmanager` + entropy `wpa_supplicant` reclaimed
  `wlan0` after ~90 s.
- `SmackProcessLabel=Privileged` fails with status 237; `/bin/sh script`
  works; existing daemons inherit PID1's label.
- The app stops `supplicantmanager.path`, removes `/tmp/no_unece`, and runs a
  watchdog.

## 7. Decompiled references

| Binary | Export ID | Content |
|---|---|---|
| `AppSetup` | `bin_5221ed11a7945eee` | Wi-Fi client UI, ConnMan, `wifi.wifiOnOff` |
| `AppEngineerMode` | `bin_6ecfb7d1716d2b4f` | `getIsWifiSupport`, WiFi debug |
| `supplicantmanager` | `bin_331e1e8a4a2d24ac` | driver load, wpa_supplicant, ACC |
| `AppProjection` | `bin_8f0096bcc0734d4c` | CarPlay control, WPP |
| `iAP2Service-2.0` | `bin_38419131df82ec7e` | wireless CarPlay transport |
| `vbtd-63` | `bin_c56b68fdd35b16b9` | `ENABLE_WIRELESS_CARPLAY` |
| `vbtd-51` | `bin_55e21d077082e97f` | same support symbols |
| `BluetoothHMI` | `bin_37f10926af48265c` | WPP UI |
| `AppHomeScreen` | `bin_3208391a859693e9` | wireless CP load state |
| `connmand` | `bin_d82d4018bfa9d8f8` | tether, DHCP, CarPlay IE |
| `connmanctl` | `bin_9db674081f35390f` | CLI commands |
| `wpa_supplicant` | `bin_6ad96a34c09b66c2` | AP/hostapd support |

## 8. Open questions

1. `pcb_version` / `optUNECE` on this car.
2. Does `connmanctl tether_carplay_ie` accept the generated identity/BT addr?
3. Is a `SupportWirelessCP` persistence key also required?
4. Does starting `carplay_iAP2.service` disturb USB media?
5. Does the iPhone connect after AP + IE + iAP2/mDNS + VBT flag?
6. Best way to hold `wlan0` against OEM respawn (current: stop path + remove
   `/tmp/no_unece` + watchdog).
