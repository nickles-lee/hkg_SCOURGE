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

## 2. How the blue-light filter actually works (verified in decompiled code)

`ApplicationManager` owns the whole pipeline. Its `PersistenceHandler`
registers the PCL keys, and on change calls:

- **Instrument cluster**: `MicomInterfaceM::setBLUStatus(bool)` /
  `setBLULevel(int)` send `SetCANFixedMicomRecv` on the **system bus**
  (`com.lge.car.micom.can` → `/Can` → `com.lge.car.micom`):
  - status: CAN **0x063F**, start bit **26**, 2 bits, data **2 = on / 1 = off**
  - level: CAN **0x063F**, start bit **12**, 4 bits, data **1–9**
  The cluster ECU renders its own amber from these signals — the cluster is
  **not** a head-unit CRTC.
- **Head-unit front display** (where CarPlay/Android Auto render):
  `PlatformUtils::writeBLValue` runs
  `wrandr --crtc=1 --brightness=<r:g:b>` through `QProcess::startDetached`,
  and checks the command for `brightness=128:128:128` to decide the cluster
  status. `wrandr` is a **Wayland client** (`ias_crtc` protocol); the session
  runs with `XDG_RUNTIME_DIR=/var/run/user/root` (ApplicationLauncher.service).
  wrandr numbers CRTCs from **1**: CRTC 1 is the front display (HDMI1,
  1920x720).

So the earlier model ("cluster = CRTC 1, reset via --gamma") was wrong on
both counts: the cluster amber is CAN-driven, and the only wrandr target is
the front display via the **brightness** channel.

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

- Cluster: while the fix is on and projection is active while the vendor has
  flipped the filter off, re-assert the vendor's own blue-light CAN signals
  (0x063F status=on + level) through the micom D-Bus service, and restore the
  vendor state when the force ends. A marker file survives daemon restarts.
- Front display: apply the same warmth via `wrandr --crtc=1 --brightness=…`
  with the session's `XDG_RUNTIME_DIR`, re-applying every tick; the wrandr
  wrapper also rewrites the vendor's neutral `--brightness=128:128:128`
  reset instantly.
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

## 9. VM validation (2026-11-06)

The daemon was validated in a local QEMU lab booting the vendor kernel and
rootfs (a private research harness; not part of this repository) against a
real vendor `dbus-daemon` plus a signature-checking micom stand-in that owns
`com.lge.car.micom` and implements `SetCANFixedMicomRecv(ayiii)` at `/Can` on
`com.lge.car.micom.can` — 24/24 checks pass on the vendor kernel 4.1.27
rootfs:

- filter on + projection idle: no CAN call, front tint applied via wrandr;
- CarPlay scenario (projection on, vendor filter off): night forced, cluster
  amber re-asserted through exactly two CAN 0x063F writes (status bit 26 = 2,
  level bit 12 = 4), `blu_forced` marker written, method returns received;
- projection end: cluster restored (status = 1), marker cleared;
- session-bus monitor against a real session bus: `carplay_play_enable_status`
  and `front_drawing_application` counted, guid-stripped address works with
  the vendor `dbus-monitor --address`.

Bugs the VM run caught (all fixed):

1. **micom D-Bus destination swap** — the daemon sent to
   `--dest=com.lge.car.micom.can`, a bus name nobody owns; the vendor's
   ApplicationManager sends to `com.lge.car.micom` with interface
   `com.lge.car.micom.can` (verified at the `QDBusMessage::createMethodCall`
   call site). On the car every blue-light write would have failed with
   `ServiceUnknown`.
2. **CAN argument packing** — the four message arguments were passed to
   `dbus-send` as one argv string, so the call went out with signature `ay`
   instead of `ayiii` (dbus-send parses one argv word as one argument). The
   host suite missed it because its fake dbus-send does not validate
   signatures; the signature-checking VM sim rejected it with `UnknownMethod`.
3. **Swallowed micom errors** — dbus-send's stderr was redirected to
   /dev/null; a failed CAN write left an empty `blu_call.out` and no evidence.
   `blu_call.out`/`ota_call.out` now capture stderr, and exec failures log
   errno.
4. **Orphaned dbus-monitors** — `tick()` started dbus-monitors even in
   `--once`/`--status`/`--set` mode, orphaning them when the process exited
   one tick later. One-shot modes no longer fork monitors.

VM-only artifacts (not daemon bugs): the guest boots without loopback (the
status server correctly fail-fasts; the test raises `lo` with the vendor
busybox), `/etc/passwd` is a symlink to `/tmp/passwd`, and `/var` is a symlink
to `rw_data/var`.

