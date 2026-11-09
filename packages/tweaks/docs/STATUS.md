# Status — Tweaks app build

**Date:** 2026-11-06
**Target:** Ioniq 5 Gen5W, `NE_E.EUR.S5W_L.001.001.260128`

## Done

- Static `tweaks_daemon` built:
  - HTTP API on `127.0.0.1:8091`
  - vendor persistence read through `tweaks_persist`
  - Hyundai gamma table
  - cluster and infotainment fixes
  - deep red night mode toggle (front 128:32:16, cluster CAN level 9)
  - `dbus-monitor` projection detection with manual override
- `wrandr` wrapper:
  - intercepts `--crtc=1 --gamma=128:128:128`
  - preserves every other invocation
- `tweaks_persist` built against the vendor 2.25 glibc/PCL.
- QML:
  - `TweaksPage.qml`
  - `TweaksTile.qml`, `TweaksSwitch.qml`, `TweaksButton.qml`
  - generated variants for stock / Hello / Battery / WiFi combinations
  - Advanced tile searches pages 1–3 for the first free slot, so it appears
    on page 2 when page 1 is full
- Installer:
  - detects all known QML bases
  - backs up QML and `/usr/bin/wrandr`
  - installs daemon/helper/wrapper/service/QML
  - restore script reverses the changes
- Tests:
  - daemon CLI/HTTP/state machine
  - wrapper rewrite/passthrough
  - install/reinstall/restore for eight QML bases
  - QML parse check with host `qmllint`
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

## First on-car test

1. Run `run_stuff_diag.sh` as the loader script. Review `tweaks_diag.txt`.
2. Install.
3. Open Tweaks; both switches should be on.
4. Start CarPlay/Android Auto; cluster should stay amber.
5. Use `Identify front` to confirm the front-display CRTC.
6. Toggle the infotainment fix and check the projection screen.
7. Restore and confirm stock behaviour.
