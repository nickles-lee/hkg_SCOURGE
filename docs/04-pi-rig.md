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

### Other boards (Pi 4 / Pi 5)

A Pi 4 or Pi 5 works as the rig — its USB-C port is a USB 2.0 **device-mode** port (the four USB-A ports are
host-only behind the VL805), so `dtoverlay=dwc2,dr_mode=peripheral` applies there just as it does on the Zero 2 W.
Three differences matter, and only the first is a script concern:

1. **The UDC name is different** — `fe980000.usb` instead of `3f980000.usb`. `scripts/12_verify_pi.sh` resolves it
   from `/sys/class/udc`, so nothing needs editing; if you are following older notes, that is why the address
   differs.
2. **Power.** On a Pi 4/5 the USB-C port is also the normal power input **and** the only UDC, so a car run — where
   that port carries the gadget data to the head unit — must take power another way: 5 V on the GPIO header, a PoE
   HAT, or a genuine power+data splitter that keeps the two 5 V rails apart. A USB-C wall charger in that port is a
   bench setup: no host on the other end, so there is nothing to serve. GPIO 5 V is the requirement, not an
   optimisation; a Zero 2 W separates power and data ports and has none of this problem.
   `toctou-gadget.service` is installed **disabled** on purpose — `11_deploy_pi.sh` leaves it that way and arming is
   deliberate (`systemctl start toctou-gadget`, or TOCTOU mode on the page). Measured on this rig (Pi 4, USB-C wall
   power): a boot-time start does bind the UDC and the board keeps power *and* network, so enabling it is not
   itself a fault — but while a board is not answering, a surprise bind is one less variable to rule out. Power the rig independently anyway (see above) — never from the head unit's USB port for a 1–1.5 h
   update. USB-C in peripheral mode does not negotiate PD: use a fixed 5 V/3 A source; a Pi 4 doing nothing but
   mass storage idles near 0.7–1.2 A with a boot peak around 1.5 A.
3. **Same storage rule.** The 27 GB image still wants a 64 GB microSD and a host card reader. The USB 3 ports buy
   nothing, because the peripheral link is USB 2.0 High-Speed either way: ~20–30 MB/s practical is the **link**
   ceiling, not a per-board measurement. What differs is whether the board can reach it. Both use the same `dwc2`
   IP, but the Zero 2 W is 4×A53 @1 GHz with less memory bandwidth, and the hot path is Python (`nbdkit_toctou.py`)
   behind NBD — so which of the two is the limit is a question to measure: `sudo dd if=<card> of=/dev/null bs=1M
   count=1024` for the medium, and the `toctou.log` pass timestamps for the sustained figure during a real update.
   **Measured here (Pi 4 B, Kingston 128 GB exFAT stick on USB 3, 2026-11-20):** cold 1 GiB reads at two offsets give
   97.8–98.1 MB/s both through `nbdkit_toctou.py` and on the raw file — identical, so the Python plugin is **not** the
   limit; the medium is, and it sits ~3–4× above the USB 2.0 link. `nbdkit` burns ~40 % of one core doing it, and
   neither `read_ahead_kb` (0 → 4096) nor `max_sectors_kb` (512 → 4096) nor a 64 KiB block size moved the number:
   there is no per-request overhead to tune. Two measurement traps, both hit here: a `sha256sum` sweep of the same
   stick in the background halves everything (that is where a bogus 27 MB/s came from), and a second read of an
   already-read offset measures the block-device page cache, not the plugin — drop caches (`echo 3 >
   /proc/sys/vm/drop_caches`) and use a fresh offset. Consequence for the Zero 2 W comparison: the same Python plugin
   on A53 cores is expected to land ~2× lower (~40–50 MB/s), which is *still* above the link, so both boards are
   probably link-limited and the Pi 4's advantage is smaller than its CPU suggests. Only a real run tells.

### A fresh card: back up, flash, fill

Replacing the card with a bigger one is three moves, and the order is not free —
flashing destroys whatever was on it, so the backup cannot come later.

1. **Back up** the old contents. `udisksctl mount -b /dev/sdb1` mounts a removable
   card as your own user, so the copy needs no root:

   ```sh
   udisksctl mount -b /dev/sdb1
   VOL=$(lsblk -no UUID /dev/sdb1)
   rsync -a "/run/media/$USER/$VOL/" ~/backups/sd-"$VOL"-$(date +%F)/card/
   ```

   A raw dump (`sudo dd if=/dev/sdb bs=8M | zstd -3 > card.img.zst`) keeps the
   partition table too, but reads all 62 GB and needs root; the file copy is what
   you actually want back.

2. **Flash** Raspberry Pi OS Lite (arm64) plus the headless first boot in one step.
   The script verifies the image's sha256, refuses a non-removable or mounted
   target, and writes `firstrun.sh` + the `systemd.run` hook + the WiFi regulatory
   domain onto the boot partition:

   ```sh
   udisksctl unmount -b /dev/sdb1
   sudo ~/piimg/flash-and-provision.sh /dev/sdb --yes
   ```

3. **Fill** it: the first-boot values (16) and the 27 GB image (13) in one run,
   with the free-space gate from 13 in front of the copy:

   ```sh
   bash scripts/00_toctou.sh sd /dev/sdb
   ```

   Step 3 re-renders `firstrun.sh` from `scripts/firstrun.sh.tmpl` and
   `config/rig.local.env`, so it replaces whatever step 2 injected: the template is
   the source of truth, the flash-time copy is only there in case you boot the card
   before running 3. If you would rather flash with Raspberry Pi Imager, do that and
   run step 3 alone — the only thing `flash-and-provision.sh` adds on top is the
   checksum check and the `RM=1` guard.

Space: `out/usb.img` is 26.9 GiB and the OS takes ~3 GiB, so a 58 GiB card leaves
about 28 GiB free — that is the whole reason for the 64 GB card. A 29 GB card is
~3 GiB short, and 13 now says so before copying instead of leaving a half-written
`usb.img` behind.

### The image on a stick instead (Pi 4 / Pi 5)

On a Pi 4 or 5 the image does not have to fight the SD card for room: the four
USB-A ports stay free while the USB-C port does the gadget, so a stick can carry
`usb.img`. A headless image runs no automounter, which is why the page has its own
**Mount** button — it mounts the stick read-only under `/media/<label>`
(`mount_media`), and Images then lists what is on it. Format it exFAT: FAT32
cannot hold a file over 4 GiB and the 27 GB image is one file. Cheap sticks write
far faster than they read, and the read speed is what a running update is limited
by, so measure it (`dd if=<bigfile> of=/dev/null bs=1M count=64`).

`scripts/10_provision_pi.sh` mounts that stick at boot: `LABEL=RIGIMG` →
`/media/RIGIMG`, exFAT, `ro,nofail`. `nofail` keeps a rig without the stick
bootable; the mount is there so a `serve.img` symlink never dangles across a
reboot. A different label goes in `RIG_IMAGE_STICK_LABEL`.

## Headless first boot of the rig's own card (16)

The rig runs Raspberry Pi OS, flashed straight from the official image. Nothing
in that image knows the rig's hostname, user, SSH key or WiFi — `systemd.run`
gives us one root shell on the first boot to set them:

```sh
scripts/16_provision_sd_boot.sh --card /dev/sdb --dry-run   # render + show, write nothing
scripts/16_provision_sd_boot.sh --card /dev/sdb             # install on the card
```

It renders `scripts/firstrun.sh.tmpl` with the values from `config/rig.local.env`
(gitignored — copy `config/rig.local.env.example`), installs the result as
`firstrun.sh` on the card's boot partition, and appends

```
systemd.run=/boot/firstrun.sh systemd.run_success_action=reboot \
systemd.unit=kernel-command-line.target cfg80211.ieee80211_regdom=<country>
```

to `cmdline.txt`. The previous `cmdline.txt` is kept as `cmdline.txt.pre-provision`.
On the first boot the script creates the user through `userconf-pi`, installs the
public key, writes one NetworkManager profile per candidate SSID, asks
`imager_custom set_wlan` to install its own association-retry timer, drops a
`publish-wifi-ip.service` that writes the rig's address to `wifi-ip.txt` on every
boot, then strips the hook, deletes itself and reboots. From then on the rig is
reachable as `<hostname>.local` and via that file.

Two things it deliberately does: it sets `127.0.1.1 <hostname>` in `/etc/hosts`
(otherwise every `sudo` on the rig warns about an unresolvable hostname), and it
persists the rfkill unblock into `/var/lib/systemd/rfkill`, because a soft-block
survives a reboot even once the regulatory domain is set.

### When the rig cannot find WiFi (17)

A rig that never associates is unreachable, and driving to it with a keyboard is
the failure mode this avoids. `scripts/17_provision_wifi_fallback.sh` (run ON the
rig, once) installs two units:

- `rig-ap-fallback.service` — watches `wlan0`. After `AP_GRACE_SECONDS` (90)
  with no association it raises a WPA2 access point named by `RIG_AP_SSID`, using
  NetworkManager's own shared mode (no hostapd config to rot). The profile is
  never autoconnected and is deleted on the way out, so the next boot goes
  straight back to station mode.
- `rig-wifi-agent.service` — the page at `http://10.42.0.1/` on that AP: the
  networks the radio can see, a passphrase box, one button. It hands the pair to
  `nmcli` as an argv list (never a shell) after validating it, and refuses every
  request while the rig is not in AP mode.

Both values live in `/etc/rig/wifi.env`, mode 0600, written from
`config/rig.local.env` (`RIG_AP_SSID`, `RIG_AP_PASSPHRASE`, at least 8
characters). The POST is gated by that AP passphrase — whoever can join the AP
already has it, and a setup portal reachable only over its own link does not need
an invented second factor.

The dashboard does the same job without the AP when the rig is already on the
network: the WiFi panel lists what the rig can see and pushes a network to it
over ssh.

### Remote access from the car (USB-SSH and a reverse tunnel)

A rig in a car has no inbound route, so the page's **Remote access** line watches
the two ways out instead:

- **`usb0`** — the gadget's own network function: `ssh` over the same cable, no
  WiFi and no access point in the way. The page reports the address when the
  interface exists. The current gadget is mass-storage only, so this stays empty
  until a network function joins the deployment; do not add one casually, because
  every extra descriptor the head unit sees during an update is a risk it does not
  need.
- **`rig-tunnel.service`** — a `RemoteForward` the rig dials out repeatedly, so
  the build host reaches the rig's sshd as if it were local. That is how the
  upstream author debugs from the car. The target, the remote port and the key
  live in `/etc/rig/tunnel.env` on the board (0600, root), never in this repo —
  the file names a host, and the leak gate has no way to tell a friend from a
  secret:

  ```sh
  sudo tee /etc/rig/tunnel.env >/dev/null <<'EOF'
  RIG_TUNNEL_TARGET=you@build-host
  RIG_TUNNEL_REMOTE=2222
  RIG_TUNNEL_KEY=/root/.ssh/id_ed25519
  EOF
  sudo chmod 0600 /etc/rig/tunnel.env
  sudo systemctl enable --now rig-tunnel
  ```

  **Start tunnel** / **Stop tunnel** on the page run the same two `systemctl`
  calls, token-gated like every other action. The unit retries every 10 s, so a
  dropped link or a WiFi roam costs nothing; with the file missing the unit never
  starts at all (`ConditionPathExists`), and the page says so instead of showing
  a dead button.

### The cable as a debug link (23)

The USB-C port does one job at a time: the gadget owns the UDC. When the head
unit is not being updated, the same cable can carry a network instead of a drive:

```sh
sudo scripts/23_usb_debug_link.sh --on        # stop the gadget, bring up usb0 10.55.0.1/24
ssh -J pi@<rig> root@10.55.0.2 -p 2224        # from a laptop on the rig's WiFi
sudo scripts/23_usb_debug_link.sh --off       # link down; re-arm with systemctl start toctou-gadget
```

That is one half of the link. Nothing on the head unit configures its `usb0` or
listens on it until the Wifi1b package is installed (`packages/wifi1b`), which is
what puts `10.55.0.2` and a key-only dropbear on 2224 there. The rig's own sshd
carries the second hop, so no NAT and no IP forwarding are involved.

Keep the two apart on purpose: during an update the head unit should see mass
storage and nothing else, because every extra USB descriptor is a risk in the one
phase that cannot be retried casually.

### Where each file lives

One table, used by `scripts/18_deploy_rig_agent.sh` and by the *Files* section of
the page itself (so the page answers "is it there, and is it current"):

| From this host (`tools/rigdash/pi/…`) | To the rig |
| --- | --- |
| `rig_wifi_agent.py` | `/usr/local/lib/rig/rig_wifi_agent.py` (0755) |
| `rig-ap-fallback.sh` | `/usr/local/sbin/rig-ap-fallback` (0755) |
| `rig-ap-fallback.service` | `/etc/systemd/system/rig-ap-fallback.service` |
| `rig-wifi-agent.service` | `/etc/systemd/system/rig-wifi-agent.service` |

```sh
scripts/18_deploy_rig_agent.sh --print   # the mapping, nothing else
scripts/18_deploy_rig_agent.sh           # copy, install, restart if changed
```

`/etc/rig/wifi.env` is deliberately **not** in that list: it holds the rig's
secrets and is written on the board once by
`scripts/17_provision_wifi_fallback.sh`. The build artifacts are separate again —
`out/usb.img`, `out/stage1.te2`, `out/regions.json` travel to
`~/scourge/build/` with `scripts/11_deploy_pi.sh`.

### One entry point

The numbered scripts still are the steps, but there is no need to remember the
order:

```sh
scripts/00_toctou.sh doctor   # what is missing, and the command that fixes each line
scripts/00_toctou.sh prep     # venv -> pinned deps -> package -> decrypt -> image
scripts/00_toctou.sh rig      # provision + deploy + verify over ssh (--agent too)
scripts/00_toctou.sh sd /dev/sdb   # flash a card and install the first boot
```

`prep` skips whatever is already built (the 27 GB `usb.img` is stock firmware and
is only made when missing; `--force` rebuilds), so running it again after a small
patch costs only what that patch invalidates.

### Run book — the order matters

1. **Power the rig** on its own 5 V supply. Never from the head unit's USB port:
   a 1–1.5 h update drawn from the car is how a bricked unit happens.
2. **TOCTOU mode** on the rig (`/usb/role` on its page, or "TOCTOU mode"): the
   board switches to the peripheral role and the link comes up, so the head unit
   sees a USB mass-storage device.
3. **Plug the rig's USB-C into the head unit.**
4. **Trigger the update on the car:** `Settings > General > Software Update >
   Update via USB`, then accept the same-version confirmation (docs/17).
5. **Let it run to the reboot.** Do not unplug while the update is writing.
6. **Normal mode**, then unplug.

Steps 1–2 before step 3 is the part that gets done in the wrong order: a rig
that is plugged in while still in host mode enumerates as nothing, and the
update menu stays greyed out.

### Images — swapping what the rig serves

`~/scourge/build/` is the depot. Every `*.img` in it shows up in the page's
**Images** list with its size, age and hash, and **Serve** moves the
`serve.img` symlink onto that file — a pointer move, not a 27 GB copy. The
gadget's `ExecStart` points at `serve.img`, not at a concrete file, so the
symlink is the single answer to "what is on the wire".

A pointer move alone would change nothing: the plugin opens the baseline once at
config time and keeps that fd. **Serve therefore re-arms the gadget** as its last
step, which re-binds the UDC — the head unit sees the drive disappear and come
back as the newly chosen image. Serving needs the page's actions token, and the
link down first (`Normal mode`), so a running update cannot lose its device.

- A file whose name contains `stock` is *flagged* **recovery** in the list. The
  badge is a label: what actually decides is the region config. `regions.json`
  carries the swap (`off`, `len`, `evil`, `switch_after_pass`), and it is
  offset-based — serving any image with it in place still injects the envelope on
  the second read. A genuine stock reinstall is an **empty** region list:
  `~/scourge/build/regions-stock.json` holds `[]`, so recovery is
  `cp regions-stock.json regions.json` plus a USB role cycle (the gadget reads the
  file at start). `11_deploy_pi.sh` writes both files.
- **The link must be down** (`Normal mode`) to swap; with the link up the page
  refuses, because a half-written update is how a unit gets bricked.
- Hashes are computed on demand (a 27 GB `sha256sum` takes minutes on the Pi)
  and cached in `.hashes.json`, keyed by size + mtime — an edited file
  invalidates its own hash.
### Building stage-1 on the board (`19`)

`usb.img` is 100 % genuine stock firmware and never changes; what changes per
patch is the ~880 MB envelope `out/stage1.te2`. So the chain can run on the rig:

```sh
scripts/19_build_on_rig.sh --dry-run        # preflight: free space, venv, plan
scripts/19_build_on_rig.sh --install-deps   # create the rig venv, then build
scripts/19_build_on_rig.sh --with-stock     # copy work/stock-plain.tar.gz once
```

It checks for ~3 GB free (input + stock-plain + stage1.te2), rsyncs
`input/ scripts/ payload/ config/ third_party/ioniq5-usb-toctou`, runs
`03_patch_payload.py` → `04_repack_rootfs.sh` → `05_verify_rootfs.py` over ssh,
and installs the result into `build/` where the Images section serves from. It
refuses to build `usb.img`: that needs 27 GB and stays a PC job. There is no
page button for this on purpose — the build needs inputs and a venv that only
this script deploys, and a button that cannot build is worse than none.

- Deployed by `scripts/11_deploy_pi.sh`, or **uploaded from the page**: the
  browser sends the `File` as the request body (no multipart), the agent streams
  it to `<name>.part` and renames it when complete, so an interrupted upload is
  never mistaken for a usable image. Percent/GB progress shows in place.
- **A stick counts too.** Anything mounted under `/media`, `/run/media` or `/mnt`
  is scanned two levels deep, so an image on an exFAT stick can be served without
  copying 23 GB over the WiFi. Those rows are marked with their mount target and
  cannot be deleted from the page — manage them on the stick.
- **Removable devices** that are not mounted yet are listed with a **Mount ro**
  button (read-only: serving only reads, and a web page should not write to a
  card you pulled out of a camera).
- **Delete** removes one image from the depot; the one currently served is
  refused until another is served, and its cached hash goes with it.
- **No API token.** The page is reachable over the owner's LAN or the setup AP
  the owner raised in their own car, and a token in front of that only bought a
  field to retype on a phone. `/connect` still asks for the AP passphrase — that
  is a network credential, not a gate on this API. A `RIG_AGENT_TOKEN` left in
  `/etc/rig/wifi.env` from an older provisioning is ignored.

## Following the steps in a browser

```sh
scripts/rig_ui.sh          # http://127.0.0.1:8765
```

A local dashboard (FastAPI, `tools/rigdash/`) that reads the same files this
doc's steps produce: every step of the pipeline is one row with its script, what
it needs on disk, what it writes, and a status — `done`, `ready`, `blocked`
(missing inputs named) or `manual`. The rig steps are probed over ssh in one
round trip, so "deployed" means present on the board, not merely built here.

Rows can be run from the page, output streamed live. Device-writing steps
(`copy_to_sd`, `format_usb`, the SD boot step) refuse unless the request carries
`confirm=<step id>` as well. The page never shows a secret: it reports whether
`config/rig.local.env` is complete, not what is in it.

On the rig itself, `http://<rig>/` carries a **TOCTOU passes** block that reads
`/var/log/toctou.log`: the region (`off`, `len`, mode, `switch_after_pass`), the
image being served, whether the switch has happened, and one row per completed
pass with the bytes, the plugin's own duration and the derived MB/s. It polls
`/api/toctou` every 5 s while the page is open, so a run in the car is watchable
from a phone without ssh. The floor to check against: the region is
879 126 179 B. The rig side needs ≥9 s for a pass at the measured 98 MB/s; the
real floor is the USB 2.0 link the head unit sits on.

The first car run (2026-11-21, a **2021 Ioniq 5 RWD**, Pi 4 through its USB-C
device port) landed well below that floor, so budget from the measurement, not
from the link:

| pass | reads | bytes | duration | rate |
|---|---:|---:|---:|---:|
| #1 genuine check (`md5sum`) | 214 626 | 879 108 096 | 186.7 s | 4.7 MB/s |
| #2 evil install (`DecryptToPIPE \| tar`) | 210 413 | 861 851 648 | closed at session end | lower bound only |

Pass #1 is the engine's own md5 sweep and the slowest single read of the run. Pass
#2 is not a rate at all: the plugin keeps a pass open until the next sweep or
shutdown, so its duration carries the idle time between reads (the run above stayed
open from the first install read until the board was powered off). The page and
`22_collect_rig_logs.sh` now label that case instead of dividing by it.

The number that matters is the head unit's own, from
`/log_data/AppUpgrade/upgrade_log`: the rootfs install took **113 s for 879.1 MB
— 7.8 MB/s**, and the whole update **34 min 21 s**. A stock stick spent the same
113 s on that step (2026-07-26, volume `9C14-E5DD`), so the cost is the head
unit's decrypt → `zcat` → `tar` → ext4 pipeline, not the wire and not the rig:
moving the image elsewhere does not make a retry faster.

## Provisioning (one time, ON the Pi)

```sh
# copy the fetched pizero tree to the Pi first (11_deploy_pi.sh does this)
ssh $PI_USER@$PI_HOST
sudo bash scripts/10_provision_pi.sh
sudo reboot
```

What it does: installs `nbdkit`, `nbdkit-plugin-python`, `nbd-client`,
`exfatprogs`; installs the no-watchdog and persistent-journal drop-ins;
appends `dtoverlay=dwc2,dr_mode=peripheral` under `[all]`; and then runs
`17_provision_wifi_fallback.sh` in place, so the setup AP and its page come up
with the same pass. It skips that last part (with a note) when `RIG_AP_SSID` /
`RIG_AP_PASSPHRASE` are not in the environment or in `config/rig.local.env`.

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
