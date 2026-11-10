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
8. Add a fourth home-screen app, **Reboot**, with a stock-style Yes/No
   confirmation modal.
9. Reboot cleanly through systemd, with a vendor micom watchdog-reset fallback
   if the clean path does not take effect.
10. Install and restore entirely through the USB sideload path.

## 3. Non-goals

- No firmware flash, updater, bootloader or partition changes.
- No CAN/body or cluster firmware modification.
- No new AppLauncher app ID or separate GUI process.
- No replacement of the vendor blue-light UI.
- No immediate hard reset in the normal path: Reboot asks systemd first and
  only falls back to the micom watchdog when the clean reboot does not start.
- No reboot while driving or while a firmware update process is running.

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
       |     - POST /reboot: systemctl reboot --no-block, then the micom
       |       SetETCCPUWDTReset fallback after 20 s
       |     - session-bus home/star counter for the Reboot overlay
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
- The instrument cluster is **not** a head-unit CRTC. Amber is rendered by
  the cluster ECU from micom CAN **0x063F**: status bit 26 / 2 bits
  (2=on, 1=off), level bit 12 / 4 bits (1–9), sent with the vendor's own
  `SetCANFixedMicomRecv` call (system bus, `com.lge.car.micom`, `/Can`,
  `com.lge.car.micom.can`).
- While the cluster fix is on and night mode is effective, the daemon asserts
  the vehicle's level; deep-red night mode asserts level 9. Disabling the
  override restores the vehicle's status and level.
- The daemon (and the persist helper it execs) runs on the SMACK floor label
  `_`, never `Privileged`: the floor label is the vendor's IPC context with
  access to `App::micommanager` and the system-bus socket, while `Privileged`
  is the label-administration (onlycap) label with no such rules. The
  installer stamps the binaries and reports `smack=` in its result;
  `/status` and `/health` expose the running label.
- Ownership markers (`blu_forced`) survive daemon crashes. A restore is only
  considered complete when both writes succeed; otherwise it is retried.
- `cluster_gamma` remains the wrapper's desired front-display value; the
  wrapper rewrites only the vendor's neutral `--brightness=128:128:128` form.

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
- The CarPlay and Android Auto app IDs are learned separately from the first
  `front_drawing_application` event after the play-enable flag turns on; a
  stale foreground sample from before play-enable is never adopted.
- While the playing source's app is foreground and night mode is effective,
  the daemon applies the same warmth to `main_crtc` (default **1**, the
  vendor front display). If a source's app ID is still unknown (e.g. the
  daemon started mid-playback), playback alone enables the tint as a fallback.
- When projection leaves, the daemon restores the front display using the
  brightness recorded in `front_override`; the marker also covers daemon
  crashes and uninstall.
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

## 11. Reboot app

The Reboot app is a fourth tile and an overlay/modal shipped by the same
package; it has no separate daemon or AppLauncher ID.

- **UI:** `RebootTile.qml` opens `RebootPage.qml`, which immediately shows a
  stock-style Yes/No modal. Yes → `POST /reboot`; No/outside → close. The
  page is `y: 90`, height 630, `z: 2000`, and owns `visible`.
- **Clean path:** the daemon forks `/bin/systemctl reboot --no-block`. The
  HTTP 202 is sent before the fork so the QML can render “Restarting”.
- **Hard fallback:** after `reboot_wdt_ms` (default **20000**), if the daemon
  is still alive, it kills a hung clean child, calls `sync()`, and sends
  `SetETCCPUWDTReset array:byte:0x00` to `com.lge.car.micom` `/Ihu`
  `com.lge.car.micom.ihu` — the exact call `/etc/weston-crash.sh` uses. A
  failed clean command triggers the fallback immediately. The fallback is
  attempted once.
- **Guards:** the QML refuses on `AppHomeModel.drivingRestriction`; the daemon
  refuses with `409 update_in_progress` while `AppUpgrade`/`AppScomoDM` runs.
  The modal is disabled while `AppHomeModel.sysPopupShowing`.
- **CSRF:** `/reboot` requires `X-Reboot-App: 1` and rejects requests with an
  `Origin` header. It binds to `127.0.0.1` only.
- **Home/Star:** the daemon watches the session bus for
  `NotifyRequestHomeScreen` and `custom_key_value` key 7 and exposes
  `home_seq`; the page closes when the counter changes (seeded on open).
- **SMACK:** the daemon must run on the installer’s floor label `_`. The
  hard-reset path uses the same `/usr/bin/dbus-send` helper as the proven
  cluster CAN writes. The installer hard-gates `SMACK64EXEC` and the running
  `/proc/<pid>/attr/current`; `/status` and `/health` expose `smack`,
  `smack_expected`, `smack_ok`, and `smack_dbus`.
- **State:** `reboot_state` = `idle` / `rebooting` / `error`;
  `reboot_wdt_in_ms`, `reboot_wdt_sent`, `reboot_wdt_ok`,
  `reboot_message`, `reboot_error`, `home_seq`, `home_source`.
- **Localization:** button labels use the stock `TranslationsModel`
  Yes/No/OK keys; the Reboot-specific body copy is English-only, matching the
  rest of the Tweaks package.

## 12. Daemon API

| Method | Path | Meaning |
|---|---|---|
| GET | `/status` | state JSON |
| GET | `/health` | liveness + build id/config version/SMACK label |
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
| POST | `/restore` | disable night fixes and release front/cluster overrides |
| POST | `/identify?crtc=N&seconds=3` | identify the front display (only `main_crtc`) |
| POST | `/reboot` | clean reboot + micom WDT fallback; requires `X-Reboot-App: 1` |

## 13. QML integration

New components:

- `HomeScreen/TweaksPage.qml`
- `HomeScreen/RebootPage.qml`
- `component/TweaksTile.qml`
- `component/RebootTile.qml`
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

In the Advanced layout both tiles are attached to all three page components.
Tweaks uses its existing per-page helper; Reboot uses the shared
`firstEmptySlot` allocator with ordinal 4 (after Hello/Battery/WiFi/Tweaks).
Wide, All Menus and Setup get both tiles; the shared menu list gets both
`tweaksClicked` and `rebootClicked` signals.

`TweaksPage.qml` starts at `y: ConstantModel.MENU_BAR_Y` with
`height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y`, so it never draws under the
90 px status bar. A full-page no-op `MouseArea` and the zone-picker overlay
consume stray taps so they cannot reach the home screen below. Every generated
screen that hosts the page also connects `AppHomeModel.onButtonBackClicked` to
close it, which handles the steering-wheel Home/Star and Back events.

`RebootPage.qml` is a full-page overlay at the same geometry (`z: 2000`) with
a 70 % dim and a stock `QPopupAnimationItem`/`QVerticalButton`/
`QHMI_ToggleButton` modal. It owns `visible: requested && !atOtherApp`, uses
the full-page input shield, and closes on Back, app switch, screen/page change,
`requestHomeByPP`, `AppHomeEngine.onNotifyHomeMainChanged`, and the daemon
`home_seq`. The Yes button is disabled while the daemon status is not known;
No/outside closes without a request.

## 14. Installation

1. Verify `manifest.md5`.
2. Detect the current QML base by markers and MD5.
3. Back up AppHomeScreen QML, AppStandbyClock QML and `/usr/bin/wrandr`.
4. Remount `/` rw.
5. Install assets, daemon, PCL helper, wrapper, service, `zoneinfo`, zones and
   disclaimer runtime files. Stamp the daemon/persist helper with the
   installer's `SMACK_SELF`, read `SMACK64EXEC` back, and fail on a mismatch.
6. Install matching AppHomeScreen and AppStandbyClock QML variants, including
   `RebootPage.qml`/`RebootTile.qml`; preserve the target `SMACK64` label.
7. Clear QML caches.
8. Start `tweaks-daemon.service`.
9. Restart `AppHomeScreen` (the disclaimer patch applies next boot).
10. Remount `/` ro.

Restore reverses the QML and `wrandr` changes, removes the daemon/service and
tzdata, and releases the OTA offline flag.

## 15. Testing

- `tests/test_daemon.sh` — CLI, HTTP, state machine, gamma application,
  disclaimer clamping/files, DST winter/summer evaluation and OTA calls.
- `tests/test_reboot.sh` — `/reboot` CSRF header, duplicate request, update
  guard, failed-clean-command fallback, micom `SetETCCPUWDTReset` invocation,
  status fields and the Home/Star session-bus counter.
- `tests/test_artifacts.sh` — generated AppHomeScreen and AppStandbyClock QML
  structure/markers, including the Reboot tile/modal geometry and close hooks.
- `tests/test_wrapper.sh` — neutral-reset rewrite and passthrough.
- `tests/test_installer.sh` — install/reinstall/restore for all QML bases,
  including AppStandbyClock backup, tzdata, disclaimer files and positive/
  negative SMACK label gates.
- QML parse check with the vendor Qt 5.4 `qmllint` in the 260128 rootfs.

## 16. Risks

1. `tweaks_persist` depends on the vendor GENIVI PCL and D-Bus environment.
   If it fails, the daemon reports the error and does not guess the filter
   state; `force_filter_on`/`warmth_override` can be used for testing.
2. The front-display brightness path may not cover the `mfxsink` IVI surface
   on every firmware; that is what the `Identify front` diagnostic and the
   independent toggle are for. The instrument cluster is not a CRTC; it is
   driven by micom CAN 0x063F.
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
9. The Reboot hard fallback is an **abrupt micom watchdog reset** of the ETC
   CPU. It runs only after the clean systemd path does not take effect
   (20 s), calls `sync()` first, and is suppressed while an update process is
   running. It is still a reset, not a graceful shutdown.
10. The Reboot app cannot help when AppHomeScreen or the daemon is already
    dead; the hardware reset pinhole remains the last resort.
11. The Reboot app ships inside Tweaks, so it cannot be uninstalled
    independently without restoring the pre-Tweaks backup.
12. The car validation of the Reboot path is still pending. The clean command,
    `/status` schema, installer gates and fallback invocation are host-tested
    with a fake `dbus-send`; the actual micom reset is derived from the vendor
    `weston-crash.sh` and remains unverified on the car.
