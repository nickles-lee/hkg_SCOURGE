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
- `python3 -B launch.py doctor --graphics` should be green before booting;
  it also validates every scenario JSON contract (`scenarios` check).

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
python3 -B launch.py show <run-id>              # prints a schema: line to stderr
python3 -B launch.py show <run-id> --strict     # exit 1 on schema problems
python3 -B launch.py check-run <run-id>         # JSON schema report, exit 0/1
python3 -B launch.py extract <run-id> qmp-screenshot.png /tmp/shot.png
python3 -B launch.py prune --keep 800        # dry-run
python3 -B launch.py gc --apply              # destructive, explicit
python3 -B launch.py gc --apply --allow-no-result   # also regression/interrupted dirs
```

`prune`/`gc` never delete `runs/daemon` (a live daemon owns sockets and
overlays there) and, by default, skip any directory without `result.json`
(regression bundles, interrupted runs). `--allow-no-result` opts those in.

`check-run` validates the result core schema (`schema_version`, verdict, rc,
timings, images/overlays, checks, lock/pin and related fields).
`check-run --all` scans every run result and reports `valid`, `legacy`,
`invalid` and result-less dirs, exiting 1 only when a current-schema result is
invalid/unreadable. `list --json` includes `schema_state`/`schema_ok`/
`schema_problems`; current-schema invalid results get a `!schema` marker in
text output, while older schema versions are reported as `legacy`. `doctor`
reports the same bulk `result_schema` health.

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
alive; `exec`/`screenshot` use a control socket in `runs/daemon/`. A failed
`daemon start` includes the last serial lines and QEMU return code in the
status/error shown by the CLI. State
overlays survive `stop` and are reused by the next `daemon start`; the
default daemon directory is `runs/daemon` (`--daemon-dir` overrides it).
Stop flushes the guest with `sync` before killing QEMU. A stale/dead daemon
is reported by `status` instead of silently reusing it. An opt-in integration
test (`IONIQ_RUN_VM_TESTS=1`) writes a marker through one daemon, stops it,
restarts with the same `--state`, and verifies the marker survives. Another
opt-in test checks `daemon-commands.log`, `daemon console` serial output,
`daemon status` (`reachable`, `qemu_alive`, `serial_bytes`, held lock) and
guest exit-code propagation through `exec`. A third test captures a daemon
screenshot, validates PNG dimensions/size/metadata, confirms the daemon-dir
capture was unlinked after the CLI copy, and stops the daemon. Another test
unlinks the daemon QMP socket, requires the screenshot command to fail with a
QMP diagnostic and no artifact, then stops the daemon and checks QEMU is gone.
Another starts a daemon, kills its QEMU, requires the status/daemon death to
be detected, then starts a fresh daemon in the same directory and exercises
an `exec` before stopping it. A SIGKILL of the daemon itself must reap QEMU
through the parent-death signal; status becomes unreachable and a fresh start
in the same directory succeeds. The persistent-state variant writes a marker,
SIGKILLs the daemon, restarts on the same `--state`, and verifies the marker
and state-lock release. A command that exceeds `daemon exec --timeout` must
return the timeout diagnostic while the daemon remains ready for a subsequent
exec. The `--reset` variant writes a marker, stops, restarts with `--reset`,
and verifies the marker is gone. A lock-contention test holds `state.lock`
externally: `daemon start` must fail with "state is already in use", leave no
QEMU, then succeed cleanly after the lock is released. Stop during a long
in-flight exec returns promptly (bounded guest-lock wait), kills QEMU and
leaves status stopped. A stale control socket is detected in about
0.5 s: status reports `reachable:false`, `exec` fails with a control-socket
diagnostic, and a clean restart succeeds after the stale daemon exits.

`--cache` selects the non-persistent qcow2 mode (`writeback` default;
`directsync` is pathological on this host). Persistent state always forces
`writethrough`; see `docs/perf-smp-memory.md`. `--pin-cpu N` pins all QEMU
threads to an allowed host CPU for experiments (`run` and `daemon start`);
it is not a default because the measured ~1.5% gain at n=3 is too small to
justify one. Avoid CPU 0 for isolation experiments and expect `--smp >1` to
slow down when all vCPUs share one host CPU.

For repeatable numbers, use the benchmark driver:

```sh
python3 -B bench.py --scenario boot --repeats 3 [--kernel vendor|compat] \
  [--cache writeback] [--pin-cpu N] [--smp N] [--memory MiB]
python3 -B bench.py --scenario boot --repeats 3 --matrix --matrix-cpu 0
```

It holds the global boot lock, runs children with `--no-lock`, writes
`runs/bench-<id>/bench.json` with mean/median/min/max and keeps each child
log, and exits nonzero if any run does not PASS. `--warmup N` runs N
unrecorded boots per scenario/variant first; warmups are retained in
`bench.json` and must PASS. `bench.json` also records `source` (`git_head`
and `dirty`) for provenance. The opt-in VM suite runs a real warmup+measured
pair and asserts warmup/row PASS, source revision and metrics. `--matrix` compares
`writeback`/`writethrough` by default; `--matrix-cache` and `--matrix-cpu`
select other combinations, and the unpinned baseline is always included.
Matrix rows and summary keys are grouped by scenario, cache and pin. The
opt-in VM suite runs a real four-cell `writeback`/`writethrough` ×
unpinned/pinned matrix and asserts variants, row count, summary keys and
per-cell boot/readiness/scenario metrics.

Compare two runs with a relative mean threshold (default 5%):

```sh
python3 -B bench.py --compare runs/bench-<base>/bench.json \
  runs/bench-<new>/bench.json --threshold 0.05
```

The opt-in VM suite (`IONIQ_RUN_VM_TESTS=1`) runs two real boot benchmarks,
checks a self-comparison is `SAME`/rc=0, a permissive real comparison is rc=0,
and a mean-doubled candidate is `REGRESSED`/rc=1.

`compare` prints per-key `SAME`/`IMPROVED`/`REGRESSED`/`MISSING` rows and
exits 1 for a regression or a missing key; malformed benchmark documents are
rejected with exit 2 and per-document problems. New `bench.json` files carry
`schema_version: 1` and are validated before comparison and after writing.
`bench.json` records `metrics` for
`boot`, `readiness` and `scenario`; select one with `--metric boot|readiness|
scenario` (default boot). A single-run comparison is noise; use `--repeats`
for decisions.

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

## Vehicle-state model (lab-defined)

```sh
python3 -B launch.py run --profile research --scenario vehicle-state \
  --payload payload/tweaks-vm --timeout 180
```

The synthetic micom simulator packs fixed-frame writes into per-canid frames
and decodes two lab-defined canids into a deterministic state:

- `0x0100`: speed (0..300, clamped), gear (P/R/N/D), ignition
  (OFF/ACC/ON/START)
- `0x0101`: battery SOC (0..100, clamped), doors bitmask, climate setpoint in
  half-degrees C (16..30, clamped), outside temperature as int8 C (-40..85,
  clamped)

The lab-only `org.scourge.vehicle.State` interface at `/Vehicle` exposes
`GetState`/`Reset`; the last `SetCANMicomRecv` payload is retained as
`bulk=<hex>`. The probe asserts decoded values, one clamp marker per
out-of-range write, bulk retention, reset defaults and a hostile
`startbit=INT_MAX`/boundary range guard (ignored and logged, never written).
Rebuild the payload with `sh build_research_tools.sh` after changing the
simulator. The frame
mapping is lab-defined: it is not a vendor DBC and does not claim stock
vehicle fidelity.

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
workspace-local, so each agent can choose the frames it guards. The opt-in
VM suite (`IONIQ_RUN_VM_TESTS=1`) includes an end-to-end test that captures a
real `ui-smoke` QMP frame, baselines it, compares it (zero changed pixels),
then perturbs a copy and requires the diff to fail.

## Dry run

```sh
python3 -B launch.py run --dry-run --scenario boot
```

Builds the run metadata, overlays and `command.json`, prints the full QEMU
argv, and exits with verdict `DRY-RUN` without taking the boot lock or
starting QEMU. Useful for inspecting device/cache/scenario arguments.

## Parallel runs and suites

All heavy boots share an exclusive lock at `runs/.vm-lab.lock`:

- `launch.py run` and `recovery-power-loss` take it by default and wait up to
  `--lock-timeout` seconds (default 600) instead of racing on EHCI/QEMU
  resources; `--no-lock` is the explicit opt-out.
- The persistent daemon holds the same lock for its lifetime. `daemon start`
  waits up to `min(--lock-timeout, --ready-timeout)` queued behind another
  boot; direct runs queue behind a ready daemon. A direct run queued behind a
  live daemon will time out after its lock wait: `daemon stop` first (or use
  `--no-lock` only for a controlled experiment).
- `regress.py` holds the lock for the whole suite and runs its children with
  `--no-lock`, so there is no self-deadlock.

```sh
python3 -B regress.py --profile conformance --scenario boot
python3 -B launch.py run --cmd '...' --lock-timeout 900
python3 -B launch.py run --cmd '...' --no-lock   # controlled opt-out only
```

`result.json` records the lock (`held`, `path`, `pid`, `waited_seconds`);
inside a regression suite the child runs record `held: false` because the
suite holds the lock, and `summary.json` records both `locked: true` and the
`suite_lock` evidence. Daemon status records the lock it holds.
A blocked second run exits with a clear `another VM boot holds ...` error.

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

`recovery-usb-removal` mounts the update stick and has the host detach it via
QMP after a runtime marker:

```sh
python3 -B launch.py run --profile conformance \
  --scenario recovery-usb-removal --usb update-k.img --timeout 180
```

The guest must see an I/O error/removed device (not a hang), then lazy-unmount
and pass. QEMU `device_del` models USB detachment, not a physical hot-unplug.

`recovery-repeat-update` is the media-level repeat attempt: verify the stick,
inject corruption into a manifest-covered file, detect the manifest failure,
restore the file from a backup, re-verify, then unmount and rediscover the
media:

```sh
python3 -B launch.py run --profile conformance \
  --scenario recovery-repeat-update --usb update-k.img --timeout 300
```

It passes only when all five checks pass (`repeat-baseline`,
`repeat-corrupt-detected`, `repeat-after-repair`, `repeat-rediscover` and
`recovery-repeat-update`). The repair is a local backup restore, not a
re-download; the AppUpgrade engine's own repeat behaviour needs provisioned
credentials and is not covered.

Power-loss recovery is a two-phase command:

```sh
python3 -B launch.py recovery-power-loss \
  --state states/power-loss --reset --timeout 180
```

Phase 1 writes a marker, syncs it, signals `POWER_LOSS_READY`, then starts a
write loop; the host kills QEMU. Phase 2 boots the same state and must print
`POWER_LOSS_RECOVERED`, proving the ext4 journal recovered the marker. This is
one crash-consistency slice, not update-engine crash safety.

## Vendor systemd partial boot (Mode V)

```sh
python3 -B launch.py run --profile conformance --scenario vendor-systemd \
  --timeout 120
```

Boots the vendor rootfs with real systemd instead of the research `/init` and
passes when `Reached target Multi-User System` appears. `result.json` records
`failed_unit_descriptions` observed in that boot. This is a **partial boot**:
SMACK setup units fail without the vendor initramfs, and nothing here is stock
equivalence or an update-conformance claim. The deeper
`vendor-systemd-dbus` scenario below prepares the missing runtime layout and
exercises the D-Bus socket/service path.

## Vendor systemd D-Bus service and ordering (Mode V)

```sh
python3 -B launch.py run --profile conformance --scenario vendor-systemd-dbus \
  --timeout 120
```

Two-phase: the research shell first creates the runtime layout that the
missing vendor initramfs normally provides (`/var` skeleton, `/var/run ->
/run`, `/tmp/passwd` from `passwd.legacy` with the `messagebus` user, and a
tmpfiles rule for `/run/dbus`), then `exec`s real systemd as PID 1. The
scenario passes only
when `result.json` records all of:

- the handoff shell was PID 1 (`research-shell-pid1`)
- systemd entered system mode (`systemd-pid1`)
- the D-Bus socket and service started (`dbus-socket`, `dbus-service`)
- D-Bus started before Multi-User (`dbus-before-multi-user`)
- `Reached target Multi-User System` (`multi-user-target`)

It also records `started_units`, `reached_targets` and
`failed_unit_descriptions`. A real run starts D-Bus and reaches Multi-User
with one remaining failure, `System service for micommanager`, which needs
the vehicle/micom hardware. This proves socket/service startup in order,
not on-demand client-triggered activation. The preparation is a **lab
fixture**, not the vendor initramfs and not esinit; this is not stock boot
equivalence.

## Troubleshooting

| Symptom | Action |
|---|---|
| `tools ... unmanifested/stale` | rebuild with `sh build_tools.sh tools.<date>.img`; update `--tools` |
| `compat initramfs missing` | `sh compat/pin-kernel.sh && sh compat/build-initramfs.sh` |
| `compat kernel pin mismatch` | re-pin intentionally with `compat/pin-kernel.sh`; host kernel changed |
| `no_running_vm` failure | stop the listed `vm-lab` QEMU PIDs before another managed run |
| `gdbstub connect failed after N attempts` | the stub never serviced `runs/<id>/gdb.sock`; inspect the QEMU process/serial log and retry |
| `guest kernel panic while waiting for ...` | boot readiness fails fast; inspect `serial.log` rather than waiting for the run timeout |
| QMP screendump failure | result is FAIL with `screenshot.qmp_error`; QEMU/overlays are cleaned and the CLI exit code follows the final verdict |
| failed run bundle seems slow | bundle collection is capped at 45 s total; serial tails are written even if the shell is wedged |
| flat `entropy-ready` FAIL | known issue; run with `--storage split` or read `known_issue` |
| `updater-launch` timeout | use `--timeout 540` or `600`; the scenario waits for a log line |
| `update-graphics` FAIL | known Qt shutdown hang; see the scenario `known_issue` |
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
