# Findings — night mode / blue-light filter around projection

Target: `NE_E.EUR.S5W_L.001.001.260128`, x86-64, kernel `4.1.27-abl`.

## 1. What the feature is

The amber “night mode” screen offset is the vehicle’s **Blue Light Filter**.
It is separate from the dark/day-night theme.

`AppSetup` persists:

- `APP_SETTING_DISP_BLUE_LIGHT_STATUS`
- `APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL`
- `APP_SETTING_DISP_GAMMA_R_G_B`
- `APP_SETTING_DISP_BRIGHTNESS_R_G_B`

Hyundai’s gamma table:

```text
level 1   128:120:112
...
level 9   128:112:96
off       128:128:128
```

## 2. Cluster tint is a DRM gamma reset

`ApplicationManager` applies the cluster tint with:

```sh
wrandr --crtc=1 --gamma=<r:g:b> --brightness=128:128:128
```

When the filter is disabled it issues:

```sh
wrandr --crtc=1 --gamma=128:128:128 --brightness=128:128:128
```

This is the “remove amber tint” command. It is not a CAN/body command.

## 3. Why CarPlay/Android Auto triggers it

`AppProjection` renders projection with a separate GStreamer/IVI surface:

```text
... ! mfxdecode live-mode=true ! mfxsink display=2 ivi-surface-id=4010 ...
```

The native UI tint does not cover that surface, and the vendor disables the
blue-light filter globally when projection takes over — including the
persistent `APP_SETTING_DISP_BLUE_LIGHT_STATUS` key. The cluster gamma reset
is collateral damage. Consequence for the fix: the daemon must not follow the
filter flag while projection is active, or it would clear the amber itself;
it re-applies the last known warmth every tick instead.

## 4. Display map

`weston_1920.ini`:

```text
HDMI1 1920x720   main infotainment
HDMI2 1280x720   second display / cluster
```

`wrandr --crtc=1` is the second output (cluster) in the vendor code. The
diagnostic script and `Identify display` buttons confirm this on the car.

## 5. Fix strategy

- Cluster: while the fix is on and night mode is *effective* (filter on OR
  projection active), re-apply the last known Hyundai warmth to the cluster
  CRTC every tick, and keep `cluster_gamma` armed so the wrandr wrapper also
  rewrites the vendor's neutral reset instantly.
- Infotainment: apply the same warmth to the main CRTC while projection is
  foreground, regardless of the filter flag the vendor flips off.
- Both are independent switches in the Tweaks app.

## 6. Disclaimer and DST

- The boot disclaimer is `AppStandbyClock`'s
  `Screen_AppStandbyClock_Confirm*.qml`; the Confirm button sends
  `KEY_EVT_CONFIRMSCREEN_BUTTON_CONFIRM_CLICKED`.
- DST is `APP_SETTING_CLOCK_DAYLIGHTSAVETIME` (0/1) through the vendor PCL.
  `TimeManager` treats `1` as DST on and `0` as DST off and adds/removes one
  hour when it broadcasts time. Value `2` (stock Setup "Custom" mode) is
  rejected by TimeManager as invalid, so auto-DST replaces it with the
  expected 0/1 flag.
- `pclKeyWriteData` enforces a per-resource owner check (client-side, against
  the Resource Configuration Table): writes from an app name that is not the
  key's "responsible application" fail with -44. The persistence helper
  retries the write under each candidate app name, caches the one that works
  in `/rw_data/tweaks/pcl_write_app`, and reports the PCL return codes; the
  daemon logs them, surfaces them on the DST card, and appends failures to
  `failures.log` plus `tweaks_failures.log` on a mounted USB stick.
  The RCT (`resource-table-cfg.itz`) lives on the persistence partition and
  can be collected with the diagnostic script if all candidates are rejected.
- The latest IANA tzdata is compiled at build time and installed under
  `/rw_data/tweaks/zoneinfo`; `TZ=:/rw_data/tweaks/zoneinfo/<zone>` gives the
  exact DST rule without changing the vehicle timezone offset.

## 7. OTA offline hook

`AppDMClient` exposes on its session-bus FOTA interface:

```text
com.lge.hkmc.avn.upgrade.fota
/fotaInterface
com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode(int)
```

- `1` sets `m_bOfflineMode`, writes `/update/offline_status=1`, and sends a
  DM-engine message (type `0x36`, args `0x14`, `0x199`).
- `0` clears it, writes `0`, and emits `onModeChangeCompleted(0)`.
- The constructor reads `/update/offline_status` on startup, so the state
  persists across reboots. Some update/reset cleanup paths delete the file.
- The hook does not touch SSH, interfaces, routes, DNS or firewall rules. It is
  DM-client-wide, so it may pause other DM/Bluelink sessions; validate on car.

## 8. Useful on-car checks

```sh
wrandr
cat /sys/class/drm/card0-*/status
cat /rw_data/tweaks/status.json
/rw_data/tweaks/tweaks_persist read
cat /rw_data/tweaks/disclaimer_auto /rw_data/tweaks/disclaimer_timeout_ms
cat /rw_data/tweaks/tzdata_version
TZ=:/rw_data/tweaks/zoneinfo/Europe/Amsterdam date
cat /update/offline_status
dbus-send --session --print-reply --dest=com.lge.hkmc.avn.upgrade.fota \
  /fotaInterface com.lge.hkmc.avn.upgrade.fota.fotaInterface.GetOTAEnableStatus
# com.lge.avn.appmanager.info (carplay_play_enable_status,
# front_drawing_application) is on the SESSION bus - AppNavi subscribes via
# QDBusConnection::sessionBus().  The address is in /tmp/session_appmgr:
addr=$(cut -d, -f1 /tmp/session_appmgr)
dbus-monitor --address "$addr" "type='signal',interface='com.lge.avn.appmanager.info'"
```
