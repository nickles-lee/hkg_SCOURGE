# On-car test guide — WiFi app 260128

## Before install

1. Build the USB tree:
   ```sh
   cd packages/wifi
   sh scripts/build_payload.sh
   ```
2. Copy `out/usb/*` to the USB root.
3. Optional: add `wifi.txt` and `authorized_keys` to the USB root.
4. Keep a second stick with `run_stuff_diag.sh` copied as `run_stuff.sh`.

## First run — diagnostic only

Use the diagnostic stick first. Check `wifi_diag.txt` for:

- `pcb_version`
- `optUNECE`
- `wlan0` present?
- `bcmdhd` loaded?
- `supplicantmanager` active?
- `VBTConfig.json` and `ENABLE_WIRELESS_CARPLAY`
- `carplay_iAP2` / `carplay_mdnsd` active?
- `iptables` policy

## Install

1. Plug in the WiFi app stick.
2. Wait ~10 s.
3. Check `run_stuff_result.txt`:
   - `status=OK`
   - `variant=...`
   - `daemon=systemd-ok`
   - `oem_mask=ok`
   - `restart=dbus-ok`
4. Open the WiFi tile.
5. If the result says `REBOOT_REQUIRED`, reboot the head unit before
   testing the UI; the old QML stays in memory until the UI process
   restarts.

## AP + SSH test

1. Status tab → CarPlay AP.
2. Check the WiFi stack in a shell:
   ```sh
   ps | grep wpa_supplicant
   wpa_cli -p /var/run/wpa_supplicant -i wlan0 ping
   ls -la /var/run/wpa_supplicant
   getfattr -n security.SMACK64 /rw_data/wifi/wifi_daemon \
            /rw_data/wifi/bin/dropbearmulti
   systemctl is-enabled supplicantmanager.path supplicantmanager.service
   connmanctl technologies | grep -A20 wifi
   ```
   Expect `PONG`, both units `masked`, and the daemon/dropbear label `_`.
3. On laptop, join `IONIQ5-CarPlay` with the key from `/rw_data/wifi/config.conf`.
3. Ping `10.42.0.1`.
4. SSH:
   ```sh
   ssh car@10.42.0.1 -p 2222
   # password: electric
   ```
5. Leave it up 10+ minutes; check AP stays.
6. Reboot the head unit; after it returns, verify SSH still works on 2222:
   ```sh
   ssh car@10.42.0.1 -p 2222
   ```
7. Check `/rw_data/wifi/daemon.log`.

## Client test

1. Put `wifi.txt` on the stick with your home/hotspot SSID/key.
2. In the app, press **Import USB**.
3. Status → Client.
4. Verify IP and internet from the head unit shell:
   ```sh
   ping -c 3 1.1.1.1
   ```
5. SSH from a laptop on the same network using the IP shown on the Status tab.
6. If it fails, the daemon log shows the ConnMan agent transcript
   (`connmanctl agent rc=...`); the passphrase is redacted.

## Wireless CarPlay test

1. In the app, open CarPlay AP tab; note SSID/key.
2. Press **Start CarPlay AP**.
3. Confirm in shell:
   ```sh
   connmanctl technologies | grep -A20 wifi
   systemctl is-active carplay_iAP2 carplay_mdnsd
   grep -n ENABLE_WIRELESS_CARPLAY /app/data/vbt/VBTConfig.json
   wpa_cli -i wlan0 ping
   ```
4. On iPhone: Settings → Bluetooth → select `IONIQ5-CarPlay` (or
   Settings → General → CarPlay → Available Cars).  The daemon log should
   show the option-form tether (`hidden [0]`, `frequency [...]`,
   `maxsta [...]`, `app_info [1]`) and no `unknown option`.
5. Confirm the app shows the BT address and `discoverable: yes`.
6. Select wireless CarPlay / connect.
6. If it fails, capture:
   ```sh
   tail -200 /rw_data/wifi/daemon.log
   dmesg | tail -100
   connmanctl technologies
   connmanctl services
   ```
7. Try 2.4 GHz channel 6 if 5 GHz fails:
   ```ini
   carplay_band=2.4
   carplay_channel=6
   ```
   Import again, restart AP.

## Overlay / Home key

- The page starts at screen `y=90`; the Exit button and console tabs sit
  ~24 px inside it, not on the bar's edge.
- Press the steering-wheel Home or Star key while the page is open: it
  closes within ~1 s (the daemon tails the session bus and writes
  `home.json`; the QML polls it at 500 ms).  Verify in a shell:
  ```sh
  cat /rw_data/wifi/home.json
  # {"seq":N,"epoch":E,"source":"home|star","last_app_key":K,"monitor":true}
  ```
  `monitor` must be `true`; if it is false, `dbus-monitor` or the session
  bus address is missing (the diagnostic prints both).
- Tap empty areas and drag on the page: nothing behind it should launch or
  scroll (full input shield).
- Switch to another app from the top bar: the overlay closes instead of
  staying painted over the other app.

## Live console

On the 1920 layout the right side of the WiFi page shows a live console:
**Daemon** and **Supplicant** tabs tail the logs, **Dump** writes a bundle
to the stick. The left-hand UI keeps working while a mode is applied (the
apply runs in a background child).

## Log dumps

- After install, look for `wifi_logs_<timestamp>/` on the stick — the
  daemon writes one automatically ~15 s after start.
- Status tab → **Save logs** for an on-demand bundle.
- While a mode keeps failing, a fresh bundle lands on the stick every
  10 minutes; attach the stick and wait.

## What the VM already proved

Before driving to the car, these are already validated against the real
firmware in the QEMU/KVM harness (see `docs/FINDINGS.md` 5.2f):

- every shipped QML file passes the car's own Qt 5.4 qmllint, and the real
  AppHomeScreen loads the patched `qml_eco3.0` tree and draws the patched
  Advanced page with zero QML errors;
- the tether option form (`--hidden/--frequency/--maxsta/--appinfo 1`)
  issues the correct D-Bus calls against real connmand, and the positional
  form reproduces `unknown option`;
- the Home/Star watcher bumps `home_seq` for both key types on the real
  session bus;
- log dumps work on a real Tuxera `texfat` stick;
- install/restore round-trips on the real QML tree, including the
  manual-start fallback and supplicantmanager masking.

Still car-only: RF (tether bring-up with the wifi technology present,
association, DHCP), wireless CarPlay over the air, vbtd Bluetooth pairing,
and the connmand technology SetProperty acceptance.

## Restore

1. Copy `run_stuff_restore.sh` over `run_stuff.sh`.
2. Plug in; check `run_stuff_result.txt` (`WIFI_RESTORE`, `status=OK`).
3. Confirm the original home screen returns and the WiFi tile is gone.
4. Confirm `car` user removed:
   ```sh
   grep '^car:' /etc/passwd
   ```
