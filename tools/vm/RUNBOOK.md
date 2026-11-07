# vm-lab runbook — Mode V and Mode C

Status: **VM-validated** where stated; this is a research harness, not a stock
head-unit replica. Start with [PLAN.md](../PLAN.md), read
[NOTES.md](../NOTES.md) for dated evidence, and use
[FIDELITY.md](../FIDELITY.md) plus [compat/FIDELITY.md](../compat/FIDELITY.md)
for the limits.

## Capability matrix

| Capability | Mode V (vendor kernel) | Mode C (compat kernel) |
|---|---|---|
| Exact `4.1.27-abl` kernel | yes | no (pinned host kernel) |
| Policy/security/crypto claims | **yes** | **no** |
| Root shell / USB / D-Bus scenarios | yes | yes |
| Display / screenshots | headless Weston artifact only | virtio-vga + QMP PNG |
| Interactive GTK/VNC + input | no (input-free by design) | yes |
| 9p host share | no | yes |
| Persistent state | yes | yes |
| User-mode network | no | opt-in, conformance-forbidden |

## Prerequisites

- `qemu-system-x86_64` >= 8.0, `/dev/kvm`, `qemu-img`, `debugfs`.
- Host kernel modules `virtio_gpu`, `bochs`, `virtio_input`, `virtio_net`,
  `9p`, `9pnet_virtio`, `usb_storage` for Mode C; `mkinitcpio`.
- `fakeroot`, `mkfs.ext4`, `e2fsck` for image/tools/storage builds.
- Your own decrypted rootfs at `rootfs.img` and the vendor `bzImage` symlink;
  see the SCOURGE stage-1 flow. Never commit those images.
- `python3 -B launch.py doctor --graphics` should be green before booting.

## Build basics

```sh
# Mode V backing images (symlinks into your staged firmware)
ls -l bzImage rootfs.img update-k.img

# Tools image (source-built helpers + embedded manifest)
sh build_tools.sh tools.$(date +%Y%m%d).img

# Mode C kernel pin + initramfs
sh compat/pin-kernel.sh
sh compat/build-initramfs.sh
```

`tools.dev.img` is a legacy unmanifested image and is intentionally refused;
`ALLOW_STALE_TOOLS=1` is a research-only override recorded in the result.

## Mode V quickstart (policy ground truth)

```sh
python3 -B launch.py run --profile conformance --scenario boot
python3 -B launch.py run --profile conformance --scenario usb-discovery \
  --tools tools.<date>.img --usb update-k.img
python3 -B launch.py run --profile conformance --scenario dbus-ready \
  --tools tools.<date>.img
python3 -B launch.py run --profile research --scenario updater-launch \
  --tools tools.<date>.img --usb update-k.img --timeout 600
```

Mode V is headless by kernel config. `run-phase4.sh` provides the phase4 /
AppUpgrade flow and requests a `wayland-screenshot.png` artifact by default:

```sh
TOOLS_IMAGE=tools.<date>.img TIMEOUT=600 sh run-phase4.sh
```

Mode V screenshots come from the vendor `weston-screenshooter` through the
second serial artifact channel. They prove capture, not visible UI content.

## Mode C quickstart (graphics/UI substrate)

```sh
python3 -B launch.py run --kernel compat --profile conformance --scenario boot
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario usb-discovery --tools tools.<date>.img --usb update-k.img
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario dbus-ready --tools tools.<date>.img
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario environment-audit
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario storage-isolation --storage split
```

Graphics smoke and screenshots:

```sh
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario ui-smoke --qmp-screenshot --timeout 180
python3 -B launch.py run --kernel compat --qmp-screenshot \
  --cmd 'sleep 5' --timeout 60
```

`ui-smoke` requires a non-black QMP frame and writes
`qmp-screenshot.png` plus `qmp_pixels` metadata into the run dir.

WiFi app QML-error capture (first-party payload on the host):

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario wifi-ui --share /path/to/wifi_app/out/usb --timeout 240
```

## Daily workflow

```sh
python3 -B launch.py list --limit 10
python3 -B launch.py list --label battery
python3 -B launch.py show <run-id>
python3 -B launch.py extract <run-id> qmp-screenshot.png /tmp/shot.png
python3 -B launch.py prune --keep 800        # dry-run
python3 -B launch.py gc --apply              # destructive, explicit
```

State reuse:

```sh
python3 -B launch.py run --kernel compat --state states/compat --cmd '...'
python3 -B launch.py run --kernel compat --state states/compat --reset --cmd '...'
```

Optional share/display/input/network/audio:

```sh
--share DIR --display vnc --vnc-display :7 --network --audio
--usb-host 058f:6366
python3 -B launch.py input --qmp <run>/qmp.sock --click 640,360
python3 -B launch.py input --qmp <run>/qmp.sock --key ret
```

`--usb-host VENDOR:PRODUCT` is compat-only and opt-in. QEMU must be able to
open the host device (typically a udev ACL or root); the device is handed to
the guest exclusively. This is a security boundary: only pass through devices
you intend the guest to own.

## Audio (Mode C)

```sh
python3 -B launch.py run --kernel compat --audio \
  --cmd 'cat /proc/asound/cards; aplay -l; \
         aplay -D hw:0,0 -d 1 -f S16_LE -r 48000 -c 2 /dev/zero'
```

`--audio` adds `intel-hda` + `hda-duplex` with a null host backend; no host
sound server is needed. The vendor rootfs default `dmix` slave is absent, so
use `-D hw:0,0` (or configure a simple PCM) for a test tone.

## Persistent VM daemon

Boot once and iterate without cold boots:

```sh
python3 -B launch.py daemon start --kernel compat --state states/daemon \
  --label daily
python3 -B launch.py daemon status
python3 -B launch.py daemon exec --cmd 'uname -r; ls /media/host'
python3 -B launch.py daemon screenshot --output /tmp/daemon.png
python3 -B launch.py daemon console --lines 80
python3 -B launch.py daemon console --follow        # Ctrl-C to detach
python3 -B launch.py daemon stop
```

The daemon keeps the QEMU process, serial reader, QMP socket and state lock
alive; `exec`/`screenshot` use a control socket in `runs/daemon/`. State
overlays survive `stop` and are reused by the next `daemon start`; the
default daemon directory is `runs/daemon` (`--daemon-dir` overrides it).
Stop flushes the guest with `sync` before killing QEMU. A stale/dead daemon
is reported by `status` instead of silently reusing it.

`--cache` selects the non-persistent qcow2 mode (`writeback` default;
`directsync` is pathological on this host). Persistent state always forces
`writethrough`; see `docs/perf-smp-memory.md`.

## CAN/micom smoke

```sh
python3 -B launch.py run --profile research --scenario can-smoke \
  --payload payload/tweaks-vm --timeout 180
```

Starts the system bus and the synthetic `tweaks-micom-can-sim`, then calls
`SetCANFixedMicomRecv` (canid 06 3f, start 26, size 2, data 2) and
`SetCANMicomRecv` (06 3f). Pass requires READY, both CALL/ACK markers and
method returns. It is an interface/contract test, not real CAN timing or
vehicle-state fidelity.

## Visual regression

```sh
python3 -B launch.py visual baseline <run-id> qmp-screenshot.png ui-smoke
python3 -B launch.py visual compare <run-id> qmp-screenshot.png ui-smoke
python3 -B launch.py visual compare <run-id> qmp-screenshot.png ui-smoke \
  --threshold 0.01 --tolerance 8 --diff /tmp/ui.diff.png
python3 -B launch.py visual list
```

Baselines live in `baselines/` (gitignored); `compare` writes a diff PNG and
reports changed pixels/ratio. A deliberate UI change is rebaselined by
running the `baseline` command against the reviewed frame. Baselines are
workspace-local, so each agent can choose the frames it guards.

## Parallel suites

`regress.py` takes an exclusive lock at `runs/.vm-lab.lock` for the whole
suite. A second suite waits for `--lock-timeout` (default 600 s) instead of
racing the first on EHCI/QEMU resources:

```sh
python3 -B regress.py --profile conformance --scenario boot
python3 -B regress.py --no-lock ...      # only for controlled, non-booting work
```

`summary.json` records `locked: true`. `--no-lock` is an explicit opt-out.

## Virgl experiment

```sh
python3 -B launch.py run --kernel compat --display egl-headless \
  --display-device virtio-vga-gl --scenario boot
```

This boots and the guest accepts the virtio-vga-gl device, but QEMU's
`egl-headless` display exposes no surface to QMP (`screendump: no surface`).
It is a documented no-win for the screenshot-first workflow; keep
`--display-device virtio-vga` for QMP captures. GTK with GL may work on a
desktop session but cannot be validated on this headless host.

## Recovery smoke

```sh
python3 -B launch.py run --profile conformance --scenario recovery-reboot \
  --timeout 120
```

The guest prints `HARNESS_CHECK reboot-marker PASS`, syncs, and calls
`reboot -f`; QEMU runs with `-no-reboot`, so the harness accepts the marker
plus a terminated QEMU as PASS (`reboot_exit: true`). This is a first
recovery slice, not power-loss, corrupt-media, no-space or repeated-update
coverage.

`recovery-no-space` fills a 16 MiB synthetic update volume until ENOSPC,
asserts the error, and removes the filler:

```sh
python3 -B launch.py run --profile conformance --scenario recovery-no-space \
  --storage split --timeout 120
```

`recovery-corrupt-media` corrupts `IONIQ5_EU.ver` on the update-stick overlay
and asserts the manifest gate fails cleanly:

```sh
python3 -B launch.py run --profile conformance \
  --scenario recovery-corrupt-media --usb update-k.img --timeout 300
```

The expected marker is `IONIQ5_EU.ver: FAILED` plus the `sha256sum` warning;
the backing update image is never modified. This is the manifest-level media
gate, not the AppUpgrade signature/credential path.

## Troubleshooting

| Symptom | Action |
|---|---|
| `tools ... unmanifested/stale` | rebuild with `sh build_tools.sh tools.<date>.img`; update `--tools` |
| `compat initramfs missing` | `sh compat/pin-kernel.sh && sh compat/build-initramfs.sh` |
| `compat kernel pin mismatch` | re-pin intentionally with `compat/pin-kernel.sh`; host kernel changed |
| `no_running_vm` failure | stop the listed `vm-lab` QEMU PIDs before another managed run |
| flat `entropy-ready` FAIL | known issue; run with `--storage split` or read `known_issue` |
| `updater-launch` timeout | use `--timeout 540` or `600`; the scenario waits for a log line |
| `update-graphics` FAIL | known Qt shutdown hang; see `probes/qt-shutdown.md` |
| `wifi-ui` missing imports | expected current evidence (`HMIEVENTS`, `AppHome_Enum`); AppHomeScreen runtime not wired |
| blank QMP frame | ensure a display device is attached; compat uses `virtio-vga` |
| GTK window fails headless | use `--display vnc --vnc-display :N`; GTK needs a desktop session |

Every FAIL/timeout writes `failure/summary.md` with dmesg, processes, mounts,
Weston/QML/updater logs and the serial tail. `--no-failure-bundle` skips it.

## Safety rules

- Default is no NIC; network is opt-in, compat-only and forbidden in
  conformance.
- The vendor rootfs/tool images are read-only backings; every run uses qcow2
  overlays. `--share` is a writable host directory; use a disposable one.
- Do not treat Mode C QML/graphics results as policy/security evidence.
- Do not publish OEM images, extracted binaries or run logs with vendor
  content; the SCOURGE sync script enforces this.
