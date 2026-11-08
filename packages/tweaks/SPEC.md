# SPEC — Tweaks / night-mode fixes

## 1. Target

| Item | Value |
|---|---|
| Vehicle | Hyundai Ioniq 5 Gen5W |
| Firmware | `NE_E.EUR.S5W_L.001.001.260128` |
| Platform | x86-64, kernel `4.1.27-abl` |
| UI process | `/app/bin/AppHomeScreen` (AppLauncher ID 11) |
| QML root | `/app/share/AppHomeScreen/qml_eco3.0/` |
| Deployment | `initial_root` USB loader |
| State | `/rw_data/tweaks/` |
| Daemon | `/rw_data/tweaks/tweaks_daemon`, HTTP `127.0.0.1:8091` |

## 2. Goals

1. Keep the instrument cluster's amber night tint active while CarPlay or
   Android Auto is displayed.
2. Apply the same amber warmth to the CarPlay / Android Auto screen.
3. Expose both as independent switches in a new Tweaks home-screen app.
4. Follow the vehicle's existing blue-light filter setting and Hyundai gamma
   table.
5. Auto-accept the boot safety disclaimer after a configurable 1–10 seconds;
   any touch cancels it for that boot.
6. Keep the vehicle DST flag in sync with a selected IANA timezone using
   bundled, freshly compiled tzdata.
7. Optionally block Hyundai OTA via the vendor `AppDMClient` offline hook
   without disabling local USB updates.
8. Install and restore entirely through the USB sideload path.

## 3. Non-goals

- No firmware flash, updater, bootloader or partition changes.
- No CAN/body or cluster firmware modification.
- No new AppLauncher app ID or separate GUI process.
- No replacement of the vendor blue-light UI.

## 4. Architecture

```text
USB run_stuff.sh
  -> installs QML page/tile + static daemon + PCL helper + wrandr wrapper
  -> starts tweaks-daemon.service
       |
       +-- /rw_data/tweaks/tweaks_daemon (static C)
       |     - HTTP 127.0.0.1:8091
       |     - status.json / config.conf / cluster_gamma
       |     - reads vehicle state via tweaks_persist
       |     - applies DRM gamma via wrandr.real
       |     - listens to com.lge.avn.appmanager.info for projection state
       |     - auto-DST via APP_SETTING_CLOCK_DAYLIGHTSAVETIME
       |     - OTA offline via AppDMClient.setOFFlineMode
       |     - writes disclaimer runtime files for AppStandbyClock
       |
       +-- /usr/bin/wrandr (wrapper)
       |     - rewrites --crtc=1 --gamma=128:128:128 to the desired warmth
       |
       +-- /rw_data/tweaks/tweaks_persist (dynamic PCL helper)
             - pclKeyReadData(APP_SETTING_DISP_BLUE_LIGHT_*)
```

## 5. Vendor gamma table

```text
level 1   128:120:112
level 2   128:119:110
level 3   128:118:108
level 4   128:117:106
level 5   128:116:104
level 6   128:115:102
level 7   128:114:100
level 8   128:113:98
level 9   128:112:96
off       128:128:128
```

Brightness is kept at `128:128:128`.

## 6. Cluster fix

- Daemon reads `APP_SETTING_DISP_BLUE_LIGHT_STATUS` and
  `APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL`.
- When the filter is on, it writes the desired gamma to
  `/rw_data/tweaks/cluster_gamma`.
- `ApplicationManager` resets the cluster with
  `wrandr --crtc=1 --gamma=128:128:128 --brightness=128:128:128`.
- The wrapper detects exactly that neutral reset and substitutes the desired
  gamma.
- When the filter is off, or the Tweaks switch is off, the daemon writes
  `off` and the wrapper passes the vendor command through.

## 7. Infotainment fix

- Daemon watches `com.lge.avn.appmanager.info` on **both the system and the
  session bus** (AppNavi subscribes to it via `QDBusConnection::sessionBus()`,
  so the session bus is the one that carries the signals in practice):
  - `carplay_play_enable_status`
  - `android_auto_play_enable_status`
  - `front_drawing_application`
- `status.json` exposes `mon_system`, `mon_session`, `sig_carplay`,
  `sig_android`, `sig_front` and `sig_bus` so a test drive proves which bus
  delivered the signals, plus `deep_red` for the deep-red night mode toggle.
- The CarPlay and Android Auto app IDs are learned separately, each from the
  `front_drawing_application` value at the moment its play-enable flag turns
  on (0→1). A later front-app signal never overwrites the learned ID, so
  opening Setup during playback cannot poison it.
- While the playing source's app is foreground and the filter is on, the
  daemon applies the same gamma to `main_crtc` (default 0). If a source's app
  ID is still unknown (e.g. the daemon started mid-playback), playback alone
  enables the tint as a fallback.
- When projection leaves, the daemon restores neutral on the main CRTC.
- The feature is independent of the cluster switch.

## 8. Safety disclaimer auto-accept

- Patch `AppStandbyClock/qml_*` `main.qml` and both
  `Screen_AppStandbyClock_Confirm*.qml` screens.
- `main.qml` holds per-boot `disclaimerAutoCancelled`/`disclaimerAutoDone`
  state and a pass-through touch guard.
- The confirm screen starts a 1–10 second timer from
  `/rw_data/tweaks/disclaimer_auto` and
  `/rw_data/tweaks/disclaimer_timeout_ms`.
- On timeout it confirms the last-used driver, except when the vehicle is
  driving/calling and the current driver is not the first driver.
- Any touch cancels the timer for the rest of the boot.

## 9. Auto-DST

- `scripts/fetch_tzdata.py` downloads the latest IANA tzdata, compiles it with
  `zic -b fat`, and writes `zones.txt` and a version/hash record.
- The installer copies the compiled tree to `/rw_data/tweaks/zoneinfo/`.
- The daemon evaluates the selected IANA zone with
  `TZ=:/rw_data/tweaks/zoneinfo/<zone>` and `localtime_r()`.
- When `tm_isdst` differs from `APP_SETTING_CLOCK_DAYLIGHTSAVETIME`, it writes
  the new value through `tweaks_persist`.
- The selected zone only determines the DST rule; the normal clock timezone
  remains controlled by Setup.

## 10. OTA block

- `AppDMClient` exposes the session-bus method
  `com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode(int)` at
  `/fotaInterface`.
- Enable writes `/update/offline_status=1` and calls `setOFFlineMode(1)`;
  disable writes `0` and calls `setOFFlineMode(0)`.
- The daemon re-applies after `AppDMClient` restarts and retries if the
  session bus/client is not ready.
- The vendor offline flag persists across reboots. Some update/reset cleanup
  paths delete the file; the daemon restores it on boot when enabled.
- No process is stopped and `AppUpgrade`/`usb-automount` are not modified.

## 11. Daemon API

| Method | Path | Meaning |
|---|---|---|
| GET | `/status` | state JSON |
| GET | `/health` | liveness |
| GET | `/logs` | log tail |
| POST | `/set?cluster_fix=0\|1` | cluster toggle |
| POST | `/set?infotainment_fix=0\|1` | projection toggle |
| POST | `/set?deep_red=0\|1` | deep red night mode (front 128:32:16, cluster level 9) |
| POST | `/set?disclaimer_auto=0\|1` | disclaimer auto-accept |
| POST | `/set?disclaimer_timeout_ms=1000..10000` | auto-accept delay |
| POST | `/set?auto_dst=0\|1` | automatic DST |
| POST | `/set?dst_zone=<IANA>` | DST timezone |
| POST | `/set?ota_block=0\|1` | Hyundai OTA block |
| POST | `/apply` | apply immediately |
| POST | `/restore` | disable night fixes and restore neutral |
| POST | `/identify?crtc=N&seconds=3` | identify a display |

## 12. QML integration

New components:

- `HomeScreen/TweaksPage.qml`
- `component/TweaksTile.qml`
- `component/TweaksSwitch.qml`
- `component/TweaksButton.qml`
- `component/TweaksZonePicker.qml`
- `AppStandbyClock` main and confirm screens in `qml_eco3.0`, `new_qml`
  and `qml_genesis`

Patched variants are generated for:

| Layout | Variants |
|---|---|
| Advanced | stock, hello, battery, battery_hello, wifi, hello_wifi, battery_wifi, hello_battery_wifi |
| Wide | stock, hello, wifi, hello_wifi |
| All Menus | stock, hello, wifi, hello_wifi |
| Setup | stock, hello, wifi, hello_wifi |
| shared menu list | stock, hello, wifi, hello_wifi |

In the Advanced layout the tile is attached to all three page components and a
`tweaksTargetPage()` helper selects the first page with a free slot after the
existing Hello/WiFi tiles.

`TweaksPage.qml` starts at `y: ConstantModel.MENU_BAR_Y` with
`height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y`, so it never draws under the
90 px status bar. A full-page no-op `MouseArea` and the zone-picker overlay
consume stray taps so they cannot reach the home screen below. Every generated
screen that hosts the page also connects `AppHomeModel.onButtonBackClicked` to
close it, which handles the steering-wheel Home/Star and Back events.

## 13. Installation

1. Verify `manifest.md5`.
2. Detect the current QML base by markers and MD5.
3. Back up AppHomeScreen QML, AppStandbyClock QML and `/usr/bin/wrandr`.
4. Remount `/` rw.
5. Install assets, daemon, PCL helper, wrapper, service, `zoneinfo`, zones and
   disclaimer runtime files.
6. Install matching AppHomeScreen and AppStandbyClock QML variants.
7. Clear QML caches.
8. Start `tweaks-daemon.service`.
9. Restart `AppHomeScreen` (the disclaimer patch applies next boot).
10. Remount `/` ro.

Restore reverses the QML and `wrandr` changes, removes the daemon/service and
tzdata, and releases the OTA offline flag.

## 14. Testing

- `tests/test_daemon.sh` — CLI, HTTP, state machine, gamma application,
  disclaimer clamping/files, DST winter/summer evaluation and OTA calls.
- `tests/test_artifacts.sh` — generated AppHomeScreen and AppStandbyClock QML
  structure/markers.
- `tests/test_wrapper.sh` — neutral-reset rewrite and passthrough.
- `tests/test_installer.sh` — install/reinstall/restore for all QML bases,
  including AppStandbyClock backup, tzdata and disclaimer files.
- QML parse check with host `qmllint` when available.

## 15. Risks

1. `tweaks_persist` depends on the vendor GENIVI PCL and D-Bus environment.
   If it fails, the daemon reports the error and does not guess the filter
   state; `force_filter_on`/`warmth_override` can be used for testing.
2. The main-display gamma path may not cover the `mfxsink` IVI surface on
   every firmware; that is what the `Identify main` diagnostic and the
   independent toggle are for.
3. Firmware updates overwrite `/usr/bin/wrandr` and the QML; rerun the
   sideload installer.
4. Gamma applies to the whole cluster panel; the vehicle's own warmth levels
   are deliberately conservative.
5. `setOFFlineMode` is a vendor DM-client-wide offline hook. It does not touch
   SSH or the OS network stack, but it may pause other DM/Bluelink sessions
   handled by `AppDMClient`; validate connected services on the car. A narrower
   OTA-endpoint firewall is the fallback.
6. The OTA hook blocks new OTA sessions; an update already downloaded/prepared
   may still install until cancelled.
7. The disclaimer patch is QML-based per boot and applies on the next boot.
8. Auto-DST only changes the DST flag. If the selected IANA zone does not match
   the vehicle's region, transition dates will be wrong; the zone is selectable
   in the UI.
