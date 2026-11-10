# Status — Tweaks app build

**Date:** 2026-11-10
**Target:** Ioniq 5 Gen5W, `NE_E.EUR.S5W_L.001.001.260128`

## Done

- Static `tweaks_daemon` built:
  - HTTP API on `127.0.0.1:8091`
  - vendor persistence read through `tweaks_persist`
  - Hyundai gamma table
  - cluster and infotainment fixes
  - deep red night mode toggle (front 128:32:16, cluster CAN level 9)
  - daemon/persist run on the SMACK floor label `_` (micom/system-bus access);
    live label exposed in /status and /health
  - `/reboot`: `systemctl reboot --no-block`, then `sync()` + micom
    `SetETCCPUWDTReset array:byte:0x00` if the daemon is still alive after
    20 s; update/process guard and CSRF header
  - `dbus-monitor` projection detection with manual override
  - session-bus Home/Star counter (`home_seq`) for the Reboot overlay
- `wrandr` wrapper:
  - intercepts `--crtc=1 --gamma=128:128:128`
  - preserves every other invocation
- `tweaks_persist` built against the vendor 2.25 glibc/PCL.
- QML:
  - `TweaksPage.qml`
  - `RebootPage.qml` (stock-style Yes/No modal at y 90/h 630/z 2000)
  - `TweaksTile.qml`, `RebootTile.qml`, `TweaksSwitch.qml`, `TweaksButton.qml`
  - generated variants for stock / Hello / Battery / WiFi combinations; the
    Reboot tile is ordinal 4 after Tweaks and hidden when the grid is full
  - Advanced tile searches pages 1–3 for the first free slot, so it appears
    on page 2 when page 1 is full
- Installer:
  - detects all known QML bases
  - backs up QML and `/usr/bin/wrandr`
  - installs daemon/helper/wrapper/service/QML
  - restore script reverses the changes
- Tests:
  - daemon CLI/HTTP/state machine
  - Reboot `/reboot` CSRF, duplicate, update guard, failed-clean fallback,
    micom WDT invocation, status fields and Home/Star counter
  - wrapper rewrite/passthrough
  - install/reinstall/restore for twelve QML bases, positive/negative SMACK gates
  - QML parse check with the vendor Qt 5.4 `qmllint`
- Safety disclaimer auto-accept:
  - AppStandbyClock `main.qml` + both confirm screens patched
  - on/off and 1–10 s timeout exposed in Tweaks
  - any touch cancels for the boot; last-used driver confirmed
- Auto-DST:
  - latest IANA tzdata downloaded/compiled at build time
  - selectable zone picker in Tweaks
  - daemon writes `APP_SETTING_CLOCK_DAYLIGHTSAVETIME`
- OTA block:
  - vendor `AppDMClient.setOFFlineMode` hook
  - persists via `/update/offline_status`
  - off by default; local USB path untouched
- USB tree and zip built:
  - `out/usb/`
  - `out/tweaks_app_usb.zip`
- USB copy note: the compiled tzdata tree contains hardlinks; copy it to
  FAT/exFAT with `cp -R`/`rsync -r`, not `cp -a` (see README).

## Needs the car

- Confirm `wrandr --crtc=1` is the cluster.
- Confirm `wrandr --crtc=0` covers the main display / CarPlay surface.
- Confirm `tweaks_persist read` returns the blue-light keys on the car.
- Confirm `dbus-monitor` sees `carplay_play_enable_status`,
  `android_auto_play_enable_status`, and `front_drawing_application`.
- Visual check that the cluster and projection tints match.
- Reboot persistence and restore check.
- Confirm disclaimer auto-accept after reboot and touch-cancel behaviour.
- Confirm the selected DST zone changes the vehicle DST switch correctly,
  including a car whose Setup DST value was `Custom` (`2`) or missing.
- Confirm the Tweaks page starts below the status bar, the steering-wheel
  Home/Star closes it, and taps on empty areas no longer reach the home tiles.
- Confirm `setOFFlineMode(1)` blocks OTA without affecting SSH/Bluelink and
  that local USB detection still works; confirm `setOFFlineMode(0)` restores.
- Reboot app: tile appears after Tweaks; Yes/No modal; No/outside/Home closes;
  Yes cleanly reboots the unit; driving restriction blocks the request; the
  daemon runs on `_` and `/status` shows `smack_expected`, `smack_ok`,
  `home_seq`, `reboot_state`.
- The 20 s micom hard-reset fallback is **not** to be forced on a car unless
  the test is deliberate; the host tests use a fake `dbus-send` and the
  command is derived from `/etc/weston-crash.sh`. It remains car-pending.

## First on-car test

1. Run `run_stuff_diag.sh` as the loader script. Review `tweaks_diag.txt`.
2. Install.
3. Open Tweaks; both switches should be on.
4. Tap Reboot; confirm No/outside closes it; then confirm Yes once and
   verify the clean reboot. Do not force the WDT fallback on this first run.
5. Start CarPlay/Android Auto; cluster should stay amber.
6. Use `Identify front` to confirm the front-display CRTC.
7. Toggle the infotainment fix and check the projection screen.
8. Restore and confirm stock behaviour.
