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

Optional share/display/input/network:

```sh
--share DIR --display vnc --vnc-display :7 --network
python3 -B launch.py input --qmp <run>/qmp.sock --click 640,360
python3 -B launch.py input --qmp <run>/qmp.sock --key ret
```

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
