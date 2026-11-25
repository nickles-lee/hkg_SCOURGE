# 04 — The Pi rig (provision, deploy, arm)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU). Rig: Raspberry Pi
Zero 2 W in USB-gadget mode. All Pi host/user values live in
`config/paths.local.env` (`PI_HOST`, `PI_USER`, `PI_DIR`).

## Why a Pi

The TOCTOU (docs/01) needs USB media whose bytes differ between the check
read and the install read. A Pi Zero 2 W in `dwc2` peripheral mode, serving
an nbdkit-backed image, is the proven lowest-cost rig (~US$15).

## Hardware

- Raspberry Pi Zero 2 W (or any Pi with a UDC that can run `dwc2`)
- 64 GB+ microSD (32 GB may work — the 27 GB image + OS is tight)
- A USB data cable (the Pi's data port goes to the head unit's USB port)
- Power for the Pi (do **not** rely on the head unit's USB power for the
  full 1–1.5 h update; the 12 V supply state during update shutdown is not
  characterized)
- An SD card reader on the build host (the Pi's WiFi is ~0.17 MB/s — a 27 GB
  image over WiFi is ~44 h; copy it to the SD card directly instead)

## Provisioning (one time, ON the Pi)

```sh
# copy the fetched pizero tree to the Pi first (11_deploy_pi.sh does this)
ssh $PI_USER@$PI_HOST
sudo bash scripts/10_provision_pi.sh
sudo reboot
```

What it does: installs `nbdkit`, `nbdkit-plugin-python`, `nbd-client`,
`exfatprogs`; installs the no-watchdog and persistent-journal drop-ins;
appends `dtoverlay=dwc2,dr_mode=peripheral` under `[all]`.

**The hardware watchdog must be disabled on a clean boot**
(`RuntimeWatchdogUSec=0`). A live `daemon-reexec` does not disarm an
already-armed BCM2835 watchdog — a watchdog reboot mid-update kills the
rig at the worst possible moment.

After reboot, verify from the host:

```sh
scripts/12_verify_pi.sh
#   /sys/class/udc                    -> 3f980000.usb
#   dwc2 driver bound
#   RuntimeWatchdogUSec=0
#   nbdkit/nbd-client/mkfs.exfat present
```

## Deploy

```sh
scripts/13_copy_image_to_sd.sh      # 27 GB image -> SD card (host reader)
scripts/11_deploy_pi.sh --skip-image --start
```

`11_deploy_pi.sh` rsyncs the pizero scripts, writes the Pi-local
`regions.json` (evil path under the Pi's home), installs
`toctou-gadget.service`, and can enable+start it. Verify checksums on the
Pi (the script prints them).

## Arming and watching

With `toctou-gadget.service` running, the gadget presents `usb.img` to the
head unit. During the update:

```sh
ssh $PI_USER@$PI_HOST 'tail -f /var/log/toctou.log'
```

Expected sequence (docs/05):

```
PASS #1 ... served=GENUINE
*** SWITCH at pass #2 ***
PASS #2 ... served=EVIL
```

Manual control if needed:

```sh
ssh $PI_USER@$PI_HOST
cd ~/$PI_DIR/pizero
sudo ./03_deploy_gadget.sh ~/$PI_DIR/build/usb.img ~/$PI_DIR/build/regions.json
sudo ./99_teardown.sh              # unwind the gadget
```

## Collecting the flash logs (22)

A run is judged on the numbers its logs carry, so they are pulled and parsed
rather than read over ssh:

```sh
scripts/22_collect_rig_logs.sh              # -> out/rig-logs/<stamp>/
scripts/22_collect_rig_logs.sh --watch 300  # re-pull whenever the log changes
```

It rsyncs `/var/log/toctou.log` (the plugin's log), `/var/log/toctou-reads.log`
(the per-read timeline) and the archived `toctou-reads.log.bench`, and writes the
board's own state (`uname`, `uptime`, the UDC, `regions.json`, the `serve.img`
pointer) and the `toctou-gadget` journal next to them, so a session can be read
without the board. `--host`/`--user` override `config/paths.local.env` for a rig
that answers on another address.

`values.json` in the bundle is the log parsed into the pass table: session id,
what was served, image size, the region, and per pass the served side, bytes,
duration and MB/s. A last pass closed by shutdown rather than by the next sweep is
flagged `mbps_lower_bound_only` — its duration includes idle time, so its rate is
not a measurement.

What those numbers looked like on the car run (`docs/17`): 4.7 MB/s on the
engine's md5 sweep, ~0.37 MB/s on the install pass, and 113 s on the head unit for
the 879.1 MB rootfs (7.8 MB/s) — the same 113 s a stock stick takes, so the rig did
not slow the update: the head unit's `tar` throttles the read, not the link. Two
traps when measuring the medium: a `sha256sum` sweep of the same stick in the
background halves every number, and a second read of an offset measures the page
cache instead of the medium.

## regions.json

Emitted by `scripts/08_build_usb_image.sh`:

```json
[{
  "off": 5505024,
  "len": 879126179,
  "evil": "/home/<user>/<dir>/build/stage1.te2",
  "switch_after_pass": 2,
  "mode": "benign_then_evil"
}]
```

`off/len` is the rootfs extent in the exFAT image (one contiguous extent;
`08_build_usb_image.sh` asserts the extent maps to the genuine rootfs).

## Reliability notes from field testing

- Keep the Pi on a clean boot with the watchdog disarmed before the run.
- Do not run anything else heavy on the Pi during the update.
- The update takes ~1–1.5 h served from the Pi (I/O is slower than a real
  stick); do not use the "install after shutdown" option — the Pi's power
  state during the car's shutdown is unknown.
- If a pass is interrupted, re-arm (`99_teardown.sh` +
`03_deploy_gadget.sh`) and restart the update from the head unit.

See docs/13 for the upstream field journal (every bring-up bug and fix:
swap trigger, watchdog reboot, offline logging) — it is fetched locally
with the pinned clone.
