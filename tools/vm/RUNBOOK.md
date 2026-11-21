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
  it also validates every scenario JSON contract (`scenarios` check),
including boot/exit markers, fail/removal metadata and numeric bounds, and
both profiles (`profiles` check: workaround keys/booleans, conformance
no-bypass, limitations). Validate one scenario or profile while editing with
`launch.py check-scenario <name|path>` (or `--all`) and
`launch.py check-profile <name|path>` (or `--all`) (JSON report, exit 0/1).

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
python3 -B launch.py list --limit 10        # negative limits are rejected
python3 -B launch.py list --label battery
python3 -B launch.py show <run-id>              # prints a schema: line to stderr
python3 -B launch.py show <run-id> --strict     # same validate_file checks as check-run
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
timings, images/overlays, per-image provenance, `harness_sources` hashes,
storage volume evidence, power-loss/USB-removal evidence, screenshot/artifact
metadata, gdbstub patch, phase and audit summaries, tools-manifest and compat-pin
evidence, lock and failure-bundle metadata, runtime provenance (QEMU version,
cache/SMP/memory, mode/kernel, state/share/network/audio blocks), checks,
lock/pin and related fields), requires a current-schema result's
`run_dir` to match the directory it was loaded from, and includes
verdict/rc consistency: PASS/DRY-RUN require rc 0, BLOCKED requires rc 77 and
blockers, FAIL requires a nonzero rc.
`check-run --all` scans every run result and reports `valid`, `legacy`,
`invalid`, result-less dirs (`missing`) and benchmark-only dirs
(`bench_only`), exiting 1 only when a current-schema result is
invalid/unreadable. `list --json` includes `schema_state`/`schema_ok`/
`schema_problems`, and `list --schema ok|legacy|invalid|missing` filters runs
by state; current-schema invalid results get a `!schema` marker in text
output, while older schema versions are reported as `legacy`. `doctor`
reports the same bulk `result_schema` health plus `bench_schema` health for
every `runs/bench-*/bench.json`.

State reuse:

```sh
python3 -B launch.py run --kernel compat --state states/compat --cmd '...'
python3 -B launch.py run --kernel compat --state states/compat --reset --cmd '...'
```

Optional share/display/input/network/audio:

```sh
--share DIR --display vnc --vnc-display :7 --network --audio
--usb-host 058f:6366
python3 -B launch.py input --qmp <run>/qmp.sock --click 640,360   # X,Y integers
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
sound server is needed. An opt-in VM test asserts the HDA card is visible and
that a one-second 48 kHz stereo `hw:0,0` playback completes. The vendor rootfs default `dmix` slave is absent, so
use `-D hw:0,0` (or configure a simple PCM) for a test tone.

## Persistent VM daemon

Boot once and iterate without cold boots:

```sh
python3 -B launch.py daemon start --kernel compat --state states/daemon \
  --label daily
python3 -B launch.py daemon status
python3 -B launch.py daemon list          # all daemon dirs + liveness
python3 -B launch.py daemon restart
python3 -B launch.py daemon exec --cmd 'uname -r; ls /media/host'
python3 -B launch.py daemon screenshot --output /tmp/daemon.png
python3 -B launch.py daemon console --lines 80
python3 -B launch.py daemon console --follow        # Ctrl-C to detach
python3 -B launch.py daemon stop
```

The daemon keeps the QEMU process, serial reader, QMP socket and state lock
alive; `exec`/`screenshot` use a control socket in `runs/daemon/`.
`daemon start --rootfs PATH` selects a local base vendor rootfs image, the
same override as `run`.
`daemon start` rejects zero/negative `--command-log-max`,
`--screenshot-keep` and `--exec-output-max` before boot. `daemon exec` caps
returned output at `--exec-output-max` bytes (default 1 MiB), keeping the tail
and printing a truncation warning; the command log stores exactly the capped
text and rotates at `--command-log-max`. `daemon screenshot` keeps at most
`--screenshot-keep N` (default 10) newest
`screenshot-*.png` captures in the daemon directory; older captures are
pruned and reported in the response `pruned` list. `daemon list --root runs
[--json] [--state ready|stale|...]` reports every daemon directory with its
recorded state, daemon/QEMU PID liveness, kernel/state config and artifact
counters (`command_log_bytes`, `screenshot_count`) and stored caps
(`command_log_max`, `screenshot_keep`, `exec_output_max`) without contacting
the sockets; a ready/starting record whose daemon process is gone
is marked `STALE` (JSON `stale: true`), and `--fail-stale` makes the
command exit 1 when any entry is stale (useful in scripts/doctor-like checks).
`daemon status` includes the active `command_log_max`, `screenshot_keep` and
`exec_output_max` caps; artifact counters are recomputed from the daemon
directory on each call, and a restart starts them at zero. `daemon restart` validates the stored config (kernel/display/device/cache,
compat-only options, positive timeouts/limits, available pin CPU) before
stopping the daemon, so an incompatible config does not kill a healthy
daemon. It then stops and starts again from the stored `config.json` (kernel, state, devices, cache, lock,
`pin-cpu`, `command-log-max`, `exec-output-max` and `screenshot-keep`
settings); those caps and the pin still apply after restart. Add `--reset` to
discard persistent state overlays; it requires a daemon started with
`--state`. A failed
`daemon start` includes the last serial lines and QEMU return code in the
status/error shown by the CLI. Unknown control actions and malformed JSON
requests return an error without disturbing the daemon. State
overlays survive `stop` and are reused by the next `daemon start`; the
default daemon directory is `runs/daemon` (`--daemon-dir` overrides it).
Stop flushes the guest with `sync` before killing QEMU. A stale/dead daemon
is reported by `status` instead of silently reusing it. An opt-in integration
test (`IONIQ_RUN_VM_TESTS=1`) writes a marker through one daemon, stops it,
restarts with the same `--state`, and verifies the marker survives. Another
opt-in test checks `daemon-commands.log`, `daemon console` serial output,
`daemon console` reads only the tail bytes needed for `--lines` (not the
whole serial log). `daemon status` (`reachable`, `qemu_alive`, `serial_bytes`,
`command_log_bytes`, `screenshot_count`, held lock) and guest exit-code
propagation through `exec`. A third test captures a daemon
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
leaves status stopped. An opt-in test starts a compat daemon and exercises
both QMP key (`a`) and absolute tablet click input without disturbing the
guest; a right-click variant covers the alternate pointer button. A duplicate `daemon start` must fail with "daemon already running"
without disturbing the active daemon. A direct screenshot request with an
output path outside the daemon directory is rejected and leaves the VM
healthy; a symlink inside the directory that resolves outside is rejected
too. Two simultaneous `exec` requests are serialized by the guest lock;
both return their own output with no cross-talk. Out-of-bounds clicks and
invalid QMP qcodes fail with diagnostics and leave the daemon healthy.
Corrupting or deleting `status.json` does not crash the daemon: status
reports unreachable/missing state, exec still works, stop rewrites `stopped`,
and a restart succeeds. Stopping twice is idempotent; after stop, status reports
`stopped`/unreachable and exec/screenshot fail fast without artifacts. A
screenshot copy to an unwritable/missing destination or a read-only directory
fails with a clear error, removes the daemon-dir capture, and leaves the
daemon healthy; a subsequent stop/restart in the same directory succeeds. Deleting `serial.log`
makes `daemon console` fail with "serial log missing" while exec/status
remain usable. Deleting `config.json` does not block stop; restart recreates
it and exec works. A corrupt/unwritable command log no longer discards a
successful exec result; the response records `log_error`. The command log is
capped by `daemon start --command-log-max` (default 1 MiB) and rotates once to
`daemon-commands.log.1`. A persistent overlay whose backing does not match the expected
image is rejected as a "backing mismatch" before QEMU starts. A corrupted
overlay fails cleanly without QEMU, and `--reset` rebuilds it for a clean
start/exec/stop. A stale control socket is detected in about
0.5 s: status reports `reachable:false`, `exec` fails with a control-socket
diagnostic, and a clean restart succeeds after the stale daemon exits.

`--cache` selects the non-persistent qcow2 mode (`writeback` default;
`directsync` is pathological on this host). Persistent state always forces
`writethrough`; see `docs/perf-smp-memory.md`. `--pin-cpu N` pins all QEMU
threads to an allowed host CPU for experiments (`run` and `daemon start`);
it is not a default because the measured ~1.5% gain at n=3 is too small to
justify one. Avoid CPU 0 for isolation experiments and expect `--smp >1` to
slow down when all vCPUs share one host CPU.

Scenarios with `requires_mode`, `requires_share` or
`requires_visible_frame` are wired automatically: pass a matching `--kernel`,
`--share`, and `--qmp-screenshot` is added for visible-frame scenarios.

For repeatable numbers, use the benchmark driver:

```sh
python3 -B bench.py --scenario boot --repeats 3 [--kernel vendor|compat] \
  [--cache writeback] [--pin-cpu N] [--smp N] [--memory MiB]
python3 -B bench.py --scenario boot --repeats 3 --matrix --matrix-cpu 0
```

A child row is recorded as `FAIL` if its result says PASS but the process rc
is nonzero, or if the child `result.json` fails the result schema (the first
five problems are retained as `schema_problems`). It holds the global boot lock, runs children with `--no-lock`, writes
`runs/bench-<id>/bench.json` with mean/median/min/max and keeps each child
log, and exits nonzero if any run does not PASS. `--warmup N` runs N
unrecorded boots per scenario/variant first; warmups are retained in
`bench.json` and must PASS. `bench.json` also records `source` (`git_head`
and `dirty`) for provenance. Validate one document with
`launch.py check-bench <bench-dir|bench.json>` or all of them with
`check-bench --all` (JSON report, exit 0/1). Schema validation requires every row/warmup to
name a benchmarked scenario, a declared cache/pin variant, and an iteration
within `repeats`/`warmup`, with unique variant/row/warmup keys (duplicates
are rejected). The opt-in VM suite runs a real warmup+measured
pair and asserts warmup/row PASS, source revision and metrics; it also runs a
compat-kernel benchmark and validates its row/metrics. `--matrix` compares
`writeback`/`writethrough` by default; `--matrix-cache` and `--matrix-cpu`
select other combinations, and the unpinned baseline is always included.
Matrix rows and summary keys are grouped by scenario, cache and pin.
`--matrix-cpu` values are checked against the process affinity mask before
the boot lock is taken, and duplicate cache/pin values are collapsed. The
opt-in VM suite runs a real four-cell `writeback`/`writethrough` ×
unpinned/pinned matrix and asserts variants, row count, summary keys and
per-cell boot/readiness/scenario metrics.

Compare two runs with a relative mean threshold (default 5%):

```sh
python3 -B bench.py --compare runs/bench-<base> runs/bench-<new> --threshold 0.05
```

The opt-in VM suite (`IONIQ_RUN_VM_TESTS=1`) runs two real boot benchmarks,
checks a self-comparison is `SAME`/rc=0, a permissive real comparison is rc=0,
and a mean-doubled candidate is `REGRESSED`/rc=1.

`compare` also reports `source_match`/`source_warning` from the recorded
`git_head`; add `--require-source-match` to fail a cross-revision comparison.
`shape_match`/`shape_warning` compare `repeats`, `warmup`, `scenarios` and
`variants` order-insensitively; `--require-same-shape` fails a cross-shape
comparison.
It prints per-key `SAME`/`IMPROVED`/`REGRESSED`/`MISSING` rows and exits 1 for
a regression or a missing key; malformed benchmark documents are
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
method returns, and requires an unknown CAN method (`Bogus`) to fail with a
D-Bus `UnknownMethod` error. It is an interface/contract test, not real CAN
timing or vehicle-state fidelity.

## Vehicle-state model (lab-defined plus captured vendor layouts)

```sh
python3 -B launch.py run --profile research --scenario vehicle-state \
  --payload payload/tweaks-vm --timeout 180
```

The synthetic micom simulator packs fixed-frame writes into per-canid 128-bit
frames and decodes two lab-defined canids into a deterministic state:

- `0x0100`: speed (0..300, clamped), gear (P/R/N/D), ignition
  (OFF/ACC/ON/START)
- `0x0101`: battery SOC (0..100, clamped), doors bitmask, climate setpoint in
  half-degrees C (16..30, clamped), outside temperature as int8 C (-40..85,
  clamped)

It also decodes the captured CANService layouts into separate vendor fields:

- `0x1ab`: Gen3High 10-bit speed (`(raw + 1) * 0.5` km/h, values above
  `0x3fe` ignored) and gear from byte 8 bits 6..7
- `0x2fc`: Gen3High battery temperature, byte 7
- `0x2b0`: steering, bytes 5..6 big-endian in 0.1-degree units, byte 7 << 2
- `0x56e` / `0x55a`: Gen2 speed (`0x1ff` sentinel) and battery byte 9

The lab-only `org.scourge.vehicle.State` interface at `/Vehicle` exposes
`GetState`/`Reset`; the last `SetCANMicomRecv` payload is retained as
`bulk=<hex>`. The probe asserts decoded lab and vendor values, one clamp
marker per out-of-range write, bulk retention, reset defaults, a hostile
`startbit=INT_MAX`/boundary range guard (ignored and logged, never written),
fail-closed rejection of an unknown D-Bus method (`UnknownMethod`), and the
16-distinct-canid retention limit (the overflow write is logged and dropped
without mutating decoded state).
Rebuild the payload with `sh build_research_tools.sh` after changing the
simulator. The lab mapping is not a vendor DBC and does not claim stock
vehicle fidelity.

## Visual regression

```sh
python3 -B launch.py visual baseline <run-id> qmp-screenshot.png ui-smoke
python3 -B launch.py visual compare <run-id> qmp-screenshot.png ui-smoke
python3 -B launch.py visual compare <run-id> qmp-screenshot.png ui-smoke \
  --threshold 0.01 --tolerance 8 --diff /tmp/ui.diff.png
python3 -B launch.py visual list
```

Baselines live in `baselines/` (gitignored); names must be plain filenames
(path separators/traversal are rejected), and `compare` validates the
threshold (0..1) and per-channel tolerance (0..255), writes a diff PNG and
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
starting QEMU. With `--state`, it builds the persistent overlays, releases
the state lock and permits a subsequent real run. Dry runs bypass the global
boot lock, so they never queue behind a live VM.

## Car capture validation

Ground-truth car dumps live outside the tracked tree. Validate one without
extracting or executing target data:

```sh
python3 -B launch.py check-capture <capture-dir> [--verify-hashes]
```

The report checks the `archives/`, `partitions/`, `runtime/`, `meta/` and
`critical/` layout, required `rootfs.tar`/`critical.tar`/`SOURCE.txt`/
`SHA256SUMS`, every listed hash target and optional `SHA256SUMS.navi2`.
`--verify-hashes` hashes the listed files; a plain run only checks names and
presence. Absolute `/tmp/.../car-dump-*` paths in a sums file are normalized
to capture-relative paths; every hash target must resolve inside the capture
root (`../` and symlink escapes are rejected), and every archive, partition
and `critical.tar` must be listed in a sums file (`unlisted`). Map-looking
archives/images are warnings by default; `--reject-maps` turns them into
failures. Exit 0/1 with a JSON report.

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
`suite_lock` evidence, plus per-run `guest_rc`, `error`, bounded
`failures`/`failure_count` and `checks` for triage without the child log.
Daemon status records the lock it holds.
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
`POWER_LOSS_RECOVERED`, proving the ext4 journal recovered the marker. The
opt-in VM suite runs this helper end to end and asserts both phases plus the
`power_loss` marker/killed evidence. This is
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

## Vendor esinit partial boot (Mode V)

```sh
python3 -B launch.py run --profile conformance --scenario vendor-esinit \
  --rootfs /path/to/rootfs.img --timeout 180
```

Boots the vendor rootfs with its real `/lib/systemd/esinit` as PID 1. The
scenario passes only when the serial log proves the non-FDE decision
(`FDE SUPPORT: No`) and the selected non-FDE fstab
(`[FDE] ***** use fstab for non FDE`); `boot_markers` maps each evidence
string to a check. This exercises OEM/car/country detection, eMMC fsck and
the FDE gate without the research shell.

It stops there deliberately: without micom/thermal hardware, the lge-fan
driver attach enters uninterruptible sleep in `__driver_attach` during
`modprobe fan`, so systemd is never reached. The trace evidence is in
`NOTES.md`. This is a partial boot, not stock equivalence.

`--rootfs PATH` selects a local base image instead of the default
`vm/rootfs.img`; the image stays outside git and overlays remain disposable.
Use it when the default image lives on a slow network share. `doctor` and
`daemon start` accept the same option.

## Offline update media validation

Validate an update stick directory or raw ext4 image without decrypting,
extracting or running the updater:

```sh
python3 -B launch.py check-update <dir-or-ext4-image> \
  [--verify-hashes] [--verify-files]
```

The report checks the required `.lge.upgrade.xml`, `IONIQ5_EU.ver` and
`MANIFEST.sha256`. It parses the package-map header (7 fields) and entries
(6 fields), requires integer ids/sizes/flags, confines package paths to the
declared product, rejects unsafe filenames and duplicate entries, and checks
that the manifest covers `IONIQ5_EU.ver`, confines every path and has no
duplicates. Missing manifest targets fail the report.

`--verify-hashes` streams each manifest target through `debugfs` in image mode
or hashes it directly in directory mode. `--verify-files` compares
`IONIQ5_EU.ver` declared sizes for files that are actually present; absent
package payloads are not failures because a stick may carry only a subset.
Image mode requires simple paths without whitespace; directory mode supports
arbitrary names. Exit 0/1 with a JSON report.

The real `update-k.img` passes with 35 package entries, 2 manifest targets and
header `NE_E.EUR.S5W_L.001.001.260128` / `IONIQ5_EU`.

## Update-state audit

```sh
python3 -B launch.py check-update-state <update-dir|ext4-image>
```

Audits the vendor `/update` metadata without opening payloads or printing
signed URLs, server addresses or credentials. It validates:

- `updateVersionInfo` and `scheduledUpdateVersion` version shape
- `FumoUsbUpdateTimeVersion` timestamp/previous/current fields and agreement
  with `scheduledUpdateVersion`
- `fota/install_param.txt` required keys and integer counts
- `fota/dm_state.txt` as 0/1
- optional `scheduledUpdateDate` as `YYYY.MM.DD`
- the RDM tree's FUMO state against `dm_state` (Idle 10 ↔ 0)
- `.lge.upgrade.xml` presence, non-emptiness and opacity, and a non-empty
  `package.tar.sign`

The captured `/update` passes with version `5W.XXX.S5W_L.001.001.260128`,
FUMO current/scheduled `NE_E.EUR...260128`, FUMO state 10, dm_state 0 and a
next FUMO count of 1. A mismatch is reported as a warning where the vendor
state cannot prove a hard failure.

## Synthetic update-media image

```sh
python3 -B launch.py make-update-media --output /tmp/update.img --size-mb 24
python3 -B launch.py check-update /tmp/update.img --verify-hashes --verify-files
```

Builds a small lab update stick with the real filenames and package-map
grammar: `IONIQ5_EU.ver`, `MANIFEST.sha256`, `.lge.upgrade.xml` and an opaque
`HU/images/mango-rootfs.tar.gz`. It contains no OEM bytes or credentials.

The ext4 image uses 4K blocks, 256-byte inodes and disables
`metadata_csum`/`64bit`/`orphan_file` so the vendor 4.1 kernel can mount it.
It is accepted by both `update-preflight` and `update-preflight-negative`, so
the preflight scenarios can run in a clean checkout without `update-k.img`.

## Update-media preflight (Mode V)

```sh
python3 -B launch.py run --profile research --scenario update-preflight \
  --usb update-k.img --timeout 240
```

Mounts the real update stick read-only in the vendor rootfs and validates it
without launching `AppUpgrade`, decrypting the package or staging an update:

- required `.lge.upgrade.xml`, `IONIQ5_EU.ver` and `MANIFEST.sha256`
- the package blob is opaque (not plain XML), and the payload is not a plain
  gzip or tar despite the `.tar.gz` name
- the 7-field header, 6-field entries, product/path confinement, safe
  filenames and declared sizes for files present on the stick
- `MANIFEST.sha256` format and every listed hash
- no `AppUpgrade`/`dmengine` process and no `/tmp/AppUpgrade` staging

A real run with `update-k.img` passes all five checks in about six seconds.
This is a preflight only; it does not verify the encrypted envelope or run the
engine.

The negative variant proves the guest checks fail closed:

```sh
python3 -B launch.py run --profile research \
  --scenario update-preflight-negative --usb update-k.img --timeout 240
```

It sets `UPDATE_PREFLIGHT_NEGATIVE=1`, mounts the **disposable qcow2 overlay**
read-write, then proves a wrong declared size, a `..` package path and a
corrupted manifest hash are each rejected. It restores both files, re-verifies
the baseline and requires nine checks. The base stick is untouched; the
overlay is discarded after the run.

## Device-model regression (Mode V)

```sh
python3 -B launch.py run --profile research --scenario device-model \
  --timeout 120
```

Asserts the QEMU substrate the vendor harness relies on:

- `8086:29c0` Q35 host bridge
- `1b36:0007` SDHCI controller with `mmcblk0` as the mounted root disk
- `8086:2918` ICH9 LPC and `8086:2922` SATA
- no `1af4:` virtio devices in the vendor kernel

A real run passes all six checks. This is substrate regression coverage, not
equivalence with the captured BXT PCI topology.

## MOF notification codec

```sh
python3 -B launch.py mof-notify <notification-uri>
python3 -B launch.py mof-notify --encode [--vin ...] [--previous ...] \
  [--next ...] [--update-id ...] [--host ...]
```

Decodes the captured `/dm/message?msgtype=4&cor=<base64>` notification shape:
eight pipe-delimited device fields, a caret-delimited `VIN:<17>_AVN` record,
previous/next versions, update id and client type. It validates the msgtype,
path, base64, VIN, versions and numeric update id. Reports contain no server
host or signed query values.

`--encode` builds a lab notification with the default VIN
`VIN:LABVIN00000000000_AVN`, versions and update id. This is the input the MOF
client passes to `recvDM_01`; it does not provide the OEM callback that starts
the engine's FUMO session.

The captured notification decodes with msgtype 4, a 17-character VIN,
`NE_E.XXX...250226` -> `NE_E.XXX...250604` and update id 54728.

## dmengine OEM queue codec

```sh
python3 -B launch.py dmengine-queue encode --code 66 --field1 7 \
  --text 'msgtype=4&cor=LAB' [--mtype 0x36]
python3 -B launch.py dmengine-queue decode --mtype 0x36 --hex <payload>
python3 -B launch.py dmengine-queue trace --file /tmp/dm.strace
```

Codec for the vendor dmengine <-> OEM handler SysV queue recovered from a
traced run:

- queue key `0xd54`
- command mtype `0x36` (dmengine waits for this from the OEM handler)
- event mtype `0x48` (dmengine sends this to the OEM handler)
- 1048-byte payload: int32 code at offset 0, int64 fields at 8 and 16, UTF-8
  text at 32

The codec only encodes/decodes messages; it does not perform queue I/O.
`trace` parses `msgsnd`/`msgrcv` lines from an `strace -xx` log, including
partial 300-byte captures.

The traced dmengine emitted events 93, 92 and 66 (notification) and blocked
waiting for a type-0x36 command. A plain `dmclient-events` run sends no
type-0x36 commands; the fota callback below is the event source that calls
`startFumoUpdate`. AppDMClient also uses its own queue key 0xe9b.

## AppDMClient fota callback (research)

```sh
python3 -B launch.py run --profile research \
  --scenario dmclient-fota-events --usb update-k.img \
  --tools tools.dev.img --timeout 540
```

Runs the updater-launch path (USB, standby, updater exec) and then calls
`sendEventToDMClientFromAppUpgrade(int)` on `com.lge.hkmc.avn.upgrade.fota`
object `/fotaInterface` for events 0, 1 and 2. The running AppDMClient turns
those into real SysV type-0x36 commands `0x28`, `0x29` and `0x2a` on dmengine's
queue; the scenario asserts each command and `fota-callback`.

A real run passes all eight checks (`dmclient-usb`, `dmclient-standby`,
`updater-exec`, `updater-alive`, `fota-command-28/29/2a`, `fota-callback`).
This proves the AppDMClient → dmengine callback and queue commands. Feeding
those commands to dmengine alone still does not produce an HTTP request, so
the mock-DM HTTP exchange remains open. It is not a real update or a stock
manager test.

## dmengine OEM queue lab handler (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-oem-queue --payload payload/tweaks-vm --timeout 240
```

Runs the real dmengine with the lab device-info segment and lab tree/config,
plus `mock-oem-queue`. The lab handler:

- opens SysV key `0xd54`
- sends one mtype `0x36` command (code 1)
- reads mtype `0x48` events and logs codes/fields
- waits for dmengine's `com.lge.hkmc.avn.upgrade.fota.mofInterface` bus name,
  then sends `recvDM_01` with a lab notification

The scenario requires the mock command send, the MOF bus name, dmengine event
66 and no production address. A real run passes all six checks. It proves the
OEM side of the queue protocol; it does not implement AppDMClient policy or
complete an HTTP FUMO update.

## dmengine FUMO HTTP exchange (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-http --payload payload/tweaks-vm --timeout 240
```

Runs the real dmengine with a lab DMCDEVINFO segment, lab tree/config and a
lab `/update` state, brings loopback up and starts the loopback mock DM
server. It then sends OEM command code 0 with field1=1, the session-start
command recovered from dmengine's receive dispatch.

The engine must log `[dmc_startFumoSession] sesstionType = [0]`,
`Triggering DM session`, `Open connection to DM server,
http://127.0.0.1:8002/dm/message` and `Performing HTTP POST`. The mock server
records the request:

```text
MOCK_DM_REQUEST method=POST path=/dm/message
  content_type=application/vnd.syncml.dm+wbxml content_length=907
  body_prefix=02a4016a1d2d2f2f53594e434d4c...
MOCK_DM_RESPONSE status=200 syncml=1 session=4D bytes=234
```

The mock replies with a minimal **WBXML** SyncML DM response: `Status` 200 for
the Alert/Replace commands and a `Final` element.

After the download the engine posts the install notification to the DD's
`installNotifyURI`:

```text
MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success
```

The mock returns an empty 200. The download scenario asserts `fumo-notify`
alongside the DP GET. The engine parses three
statuses (debug: `cmdType=STATUS ... resultCode=0200`), finishes setup with
`result=0x600a`, and reports `SCOMO_Plugin_onSessionStateChange
inType=[1], inState=[1], inLastError=[0]` with no `DM session aborted`.

This is a completed DM session handshake, not an update: the mock performs no
management operations and the scenario makes no update-success claim. A real
run passes all eight checks.

## dmengine FUMO package download (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download --payload payload/tweaks-vm --timeout 220
```

After the status handshake the mock endpoint sends a FUMO `Exec` command on
`./ManagedObjects/FUMO/Download` together with a server `Cred`. The engine
accepts the server authentication (`Server authentication (by client), 401`)
and requests the package URL:

```text
MOCK_DM_REQUEST method=POST path=/dm/message content_length=907
MOCK_DM_RESPONSE status=200 syncml=1 session=4D bytes=341 exec=1
MOCK_DM_DD method=GET path=/package.bin bytes=402 object=/object.bin object_bytes=52
MOCK_DM_DOWNLOAD method=GET path=/object.bin bytes=52 preamble=RedBendEFDPackage version=4 components=1
```

`/package.bin` is an OMA Download Descriptor (`application/vnd.oma.dd+xml`),
not the object. After the engine parses it, the probe sends the OEM
confirmation command code 10 (`DMC_REQ_DL_OK`), which lets the engine fetch
the descriptor's `objectURI`. The mock object is a 184-byte RedBend EF
object: a 132-byte DP body (20-byte `RedBendEFDPackage` preamble, version 4,
one 28-byte component entry, one update-table entry, a NUL-terminated string
table and a 16-byte `LABUPDATE-DATA01` payload) plus a 52-byte lab
HMAC-SHA256 signature trailer.

The container can be checked independently:

```sh
python3 -B launch.py check-dp <package.dp> --key lab-dp-key
```

It validates the preamble, version, declared body size, component-table
range, update-table range, payload bounds, string offsets, installer type, NUL
termination and the optional lab HMAC-SHA256 trailer. Multiple component and
update entries are supported. The trailer is an
integrity fixture, not a vendor firmware signature.

The engine logs `DL session complete`, advances FUMO state `30` (downloading)
to `40` (downloaded), and in this run also to `50` (update progressing). The
DP is a structural fixture with one update payload but no signature, so no
install or update-success is claimed. A real run passes all fourteen checks.

## dmengine FUMO update request (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-update --payload payload/tweaks-vm --timeout 320
```

Runs the authenticated DD/DP download, then lets the mock answer the DM report
session with a FUMO `Update` Exec on `./ManagedObjects/FUMO/Update`. The
engine queues the command and reaches the update path:

```text
MOCK_DM_UPDATE_EXEC path=/dm/message
targetURI=./ManagedObjects/FUMO/Update
FUMO_Plugin_onSessionStateChange(DM): Error=[24593]
DM session aborted, 0x6011
```

This is a **negative** install-request test. The DP body has an update payload
but no vendor signature (the lab HMAC trailer is not used by the engine), so
the engine rejects it and never reaches a completed update
state (`Set state, 60` is absent). The scenario asserts the request, the
rejection, and no completion; it does not claim an install. It runs dmengine at
debug log level because the FUMO update target is logged at debug.

A real run passes all seventeen checks. The positive download scenario remains
`dmengine-fumo-download` (notice level, fourteen checks).

## dmengine FUMO DownloadAndUpdate (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-update --payload payload/tweaks-vm --timeout 320
```

Runs the combined FUMO command. The mock sends an Exec on
`./ManagedObjects/FUMO/DownloadAndUpdate`; the lab tree's
`DownloadAndUpdate/PkgURL` leaf is empty in the captured tree, so the probe
inserts the loopback package URL before starting dmengine.

The engine performs the authenticated DD/DP download, then logs:

```text
Got EXEC on "DownloadAndUpdate" node.
FUMO_Plugin: FUMO DownloadAndUpdate executed in this session
DL session complete
calling VDM_FUMO_UA_executeUpdate
confirmUpdateCB (response=0)
Sent event to OEM handler = [77] ... strData = [/update//lab-fumo]
```

FUMO state advances `30 -> 40 -> 60` (update progressing). The engine hands
event `77` with the downloaded package path to the OEM handler.

The lab OEM handler then sends queue command `20` with the valid result code
`200`:

```text
MOCK_OEM_SENT code=20 field1=200
VDMC_FUMO_updateReport] result = [200]
VDM_FUMO_triggerReportSession(200)
Set state, 90
Set state, 10
DM session complete
```

The engine reports the result through a new DM session and returns to idle.
Result `200` is lab-supplied; the physical updater is not run, so no real
install or firmware-success is claimed. (Invalid codes such as `0` or `1200`
are remapped to `409` by the engine.)

The scenario runs dmengine at debug log level. A real run passes all twenty
checks. `dmengine-fumo-download` and `dmengine-fumo-update` remain separate
positive/negative scenarios.

## dmengine FUMO update failure result (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-update-failure --payload payload/tweaks-vm --timeout 340
```

Runs the same combined `DownloadAndUpdate` download/update handoff, but the lab
OEM handler reports result `500` instead of success:

```text
MOCK_OEM_SENT code=20 field1=500
VDMC_FUMO_updateReport] result = [500]
VDM_FUMO_triggerReportSession(500)
Set state, 10
DM session complete
```

The engine accepts and reports the failure through a DM session, returns to
idle and does **not** enter success state `90`. The result is lab-supplied; the
physical updater is not run, so no install claim is made. A real run passes all
twenty-one checks.

## dmengine FUMO invalid update result (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-update-invalid-result --payload payload/tweaks-vm --timeout 340
```

Runs the same combined download/update handoff, but the lab OEM handler reports
an invalid result code `0`:

```text
VDMC_FUMO_updateReport] result = [0]
validateResultCode - invalid result code (0). using UNDEFINED(409) instead.
VDM_FUMO_triggerReportSession(0)
Set state, 70
Set state, 10
DM session complete
```

The engine remaps the result to `409`, reports the failure through a DM
session, returns to idle and never enters success state `90`. The result is
lab-supplied; no physical install is claimed. A real run passes all
twenty-two checks.

## dmengine FUMO missing object (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-missing --payload payload/tweaks-vm --timeout 300
```

Parses the DD, authenticates and sends the OEM download confirmation, but the
mock returns 404 for `/object.bin`:

```text
MOCK_DM_MISSING method=GET path=/object.bin
DL session aborted, 0x6306
FUMO_Plugin_onSessionStateChange(DL) : fumo result : 407, last error : 25350
```

The engine retries and reports the failure; no successful object download or
update is claimed. A real run passes all fourteen checks.

## dmengine FUMO install-notification failure (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-notify-failure --payload payload/tweaks-vm --timeout 320
```

The DD/DP download succeeds, but the mock returns HTTP 500 for the
`installNotifyURI` callback:

```text
MOCK_DM_NOTIFY_FAIL path=/notify bytes=11 body=900 Success
DL session aborted, 0x6308
FUMO_Plugin_onSessionStateChange(DL) : fumo result : 503, last error : 25352
```

The engine retries the notification, reports the failure and never reaches
downloaded/update states (`Set state, 40`/`60` are absent). A real run passes
all fifteen checks.

## dmengine FUMO update without result (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-update-no-result --payload payload/tweaks-vm --timeout 340
```

Runs the combined download/update handoff but deliberately withholds the OEM
update result. The engine downloads the DD/DP, hands event 77 to the OEM
handler and remains in update progressing:

```text
Got EXEC on "DownloadAndUpdate" node.
event to OEM handler = [77] ... strData = [/update//lab-fumo]
Set state, 60
```

It never logs `VDMC_FUMO_updateReport`, never sets state 90 and issues no
success report. This shows the engine waits for an OEM result; no physical
install is claimed. A real run passes all nineteen checks.

## dmengine FUMO bad-signature boundary (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-badsig --payload payload/tweaks-vm --timeout 320
```

The mock flips one bit in the lab HMAC signature trailer of the 184-byte
object. The engine does not validate that trailer, so it still downloads the
object, posts `900 Success` and advances FUMO state 30 -> 40.

The host validator rejects the same object:

```sh
python3 -B launch.py check-dp <object.bin> --key lab-dp-key
# signature mismatch
```

This documents that the lab trailer is an integrity fixture, not an
engine-enforced vendor signature. No install is claimed. A real run passes all
sixteen checks.

## dmengine FUMO resume (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-resume --payload payload/tweaks-vm --timeout 300
```

The mock serves only the first 64 of 184 object bytes with `Accept-Ranges` and
an ETag. The engine records the resume offset and immediately retries:

```text
MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=184
MOCK_DM_OBJECT_HEADERS range=Range: bytes=64-
MOCK_DM_RESUME method=GET path=/object.bin offset=64 bytes=120 total=184
DL session complete
Set state, 30
Set state, 40
```

The mock returns the remaining 120 bytes with `206 Partial Content`; the
download completes and the install notification is posted. A real run passes
all seven checks. No install is claimed.

## dmengine FUMO resume across restart (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-resume-restart --payload payload/tweaks-vm --timeout 360
```

Two-phase resume. Phase 1 uses the partial-only mock mode (64/184 bytes with
`Accept-Ranges`/ETag); after the engine writes `dlresume.dat` and the log shows
the partial object, the probe stops the engine. Phase 2 restarts dmengine with
a Range-capable mock and `dm_state=1`:

```text
phase 1: MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=184
phase 1: /update/fota/dme/dlresume.dat written
phase 2: MOCK_DM_OBJECT_HEADERS range=Range: bytes=64-
phase 2: MOCK_DM_RESUME method=GET path=/object.bin offset=64 bytes=120 total=184
phase 2: MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success
phase 2: DM session complete
phase 2: Set state, 50
```

The restarted engine truncates its log at startup, so phase-2 assertions use
the whole truncated log rather than a byte offset. State `50` is the
post-download value in this run; the check accepts a completed DL/DM session
plus state `50` and the notification. A real run passes all ten checks. No
install is claimed.

## dmengine FUMO resume without an ETag (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-resume-noetag --payload payload/tweaks-vm --timeout 320
```

The mock advertises `Accept-Ranges: bytes` on the first 64/184 response but
omits the ETag. The engine records the offset but refuses to resume:

```text
MOCK_DM_NOETAG_PARTIAL method=GET path=/object.bin bytes=64 total=184
DL resume: got offset=64, eTag=<[null]>, isIgnoreMissingETag=0
MOCK_DM_NOETAG_FULL method=GET path=/object.bin bytes=184 range=none
DL session complete
Set state, 40
MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success
```

No `Range: bytes=64-` is sent; the engine performs a full GET from zero and the
download completes. This proves resume requires an ETag, not just
`Accept-Ranges`. A real run passes all eight checks. No install is claimed.

## dmengine FUMO multi-component download (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-multi --payload payload/tweaks-vm --timeout 320
```

The mock serves a 291-byte object with a two-component, three-update DP body:

```text
MOCK_DM_DD method=GET path=/package.bin bytes=403 object=/object.bin object_bytes=291
MOCK_DM_DOWNLOAD method=GET path=/object.bin bytes=291
  preamble=RedBendEFDPackage version=4 components=2
MOCK_DM_MULTIDP method=GET path=/object.bin bytes=291 components=2 updates=3
MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success
DL session complete
Set state, 30
Set state, 40
```

Layout: 52-byte header, two 28-byte component entries
(`LABCOMP`/`LABID` and `LABCOMP2`/`LABID2`), three 8-byte update entries,
59 bytes of strings, three 16-byte payloads (239-byte body) and the 52-byte
lab HMAC trailer. The Python builder, parser and `check-dp --key` validate the
same layout; a loopback test compares the C fixture byte-for-byte. A real run
passes all sixteen checks. No install is claimed.

## dmengine FUMO multi-component DownloadAndUpdate (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-update-multi --payload payload/tweaks-vm --timeout 380
```

Combines the multi-component package with the combined FUMO command. The mock
sends `Exec ./ManagedObjects/FUMO/DownloadAndUpdate` and serves the 291-byte
two-component/three-update object:

```text
MOCK_DM_MULTIDP method=GET path=/object.bin bytes=291 components=2 updates=3
Got EXEC on "DownloadAndUpdate" node.
Set state, 30 -> 40 -> 60
event to OEM handler = [77] ... strData = [/update//lab-fumo]
VDMC_FUMO_updateReport] result = [200]
Set state, 90
DM session complete
```

The lab OEM result is supplied by the probe; the package is structural, so no
physical install or firmware-success is claimed. A real run passes all
twenty-two checks.

## dmengine FUMO multi-component resume (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-resume-multi --payload payload/tweaks-vm --timeout 340
```

The mock serves a 64/291 partial of the two-component/three-update DP with
`Accept-Ranges` and `ETag: "lab-dp-291"`. The engine retries with the Range
header and completes the download:

```text
MOCK_DM_DD method=GET path=/package.bin object=/object.bin object_bytes=291
MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=291
MOCK_DM_RESUME method=GET path=/object.bin offset=64 bytes=227 total=291
DL resume: got offset=64, eTag=<"lab-dp-291">, isIgnoreMissingETag=0
Object: size=291 len=227, offset=64, eTag=<"lab-dp-291">
DL session complete
Set state, 40
MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success
```

The mock returns the remaining 227 bytes with 206. A real run passes all seven
checks. No install is claimed.

## dmengine FUMO multi-component failure result (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-update-multi-failure --payload payload/tweaks-vm --timeout 380
```

Same multi-component `DownloadAndUpdate` flow, but the lab OEM reports result
`500`:

```text
MOCK_DM_MULTIDP bytes=291 components=2 updates=3
Set state, 30 -> 40 -> 60
event to OEM handler = [77] strData = [/update//lab-fumo]
MOCK_OEM_SENT code=20 field1=500
VDMC_FUMO_updateReport] result = [500]
VDM_FUMO_triggerReportSession(500)
Set state, 10
DM session complete
```

The engine reports the failure through a DM session, returns to idle and never
enters success state `90`. The result is lab-supplied and the package is
structural; no physical install is claimed. A real run passes all twenty-three
checks.

## dmengine FUMO multi-component invalid result (Mode C, negative)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-fumo-download-update-multi-invalid-result --payload payload/tweaks-vm --timeout 380
```

Same multi-component `DownloadAndUpdate` flow, but the lab OEM reports the
invalid result `0`:

```text
MOCK_DM_MULTIDP bytes=291 components=2 updates=3
event to OEM handler = [77] strData = [/update//lab-fumo]
VDMC_FUMO_updateReport] result = [0]
validateResultCode - invalid result code (0). using UNDEFINED(409) instead.
VDM_FUMO_triggerReportSession(0)
Set state, 70
Set state, 10
DM session complete
```

The engine remaps the result to `409`, reports the failure and returns to idle
without success state `90`. The result is lab-supplied and the package is
structural; no physical install is claimed. A real run passes all twenty-four
checks.

## dmengine startup (Mode C)

```sh
python3 -B launch.py run --kernel compat --profile research \
  --scenario dmengine-startup --payload payload/tweaks-vm --timeout 200
```

A real-boot startup probe for the vendor `/app/bin/dmengine`. The committed
`mock-dm-shm` helper creates the 932-byte DMCDEVINFO SysV segment that the
engine attaches to via its fixed `qipc_sharedmemory_DMCDEVINFO...` ftok path,
and populates the `VIN:..._AVN` and software-version fields. The probe then
copies the captured tree/config, rewrites the DM address, account names and
auth to lab/localhost values, replaces the MOF host, starts a session bus and
runs dmengine.

The scenario requires FUMO plugin initialization, `DevID len` device-info
acceptance, SCOMO plugin initialization and no production address in the lab
inputs or log. A real run passes all six checks.

This covers startup and device-info integration only. `session=fumo` on the
command line still produced no HTTP request; the next step is the MOF/SyncML
session message that triggers an exchange. Rebuild the payload with
`sh build_research_tools.sh`.

## Offline FUMO exchange model

```sh
python3 -B launch.py fumo-exchange [--installed VERSION] \
  [--package-version VERSION] [--fail download|install] [--chunk-size N]
```

A deterministic model of the OMA FUMO states (Idle 10, Downloading 20,
Downloaded 30, Updating 40), not the vendor dmengine and not SyncML. It
validates package names/versions, `http(s)` URLs without credentials or
fragments, declared size and SHA-256, rejects downgrades, and drives a mock
DM endpoint that checks a lab token and installed version.

The canonical run downloads in chunks and installs, ending at Idle with the
new version. `--fail download` returns to Idle with the package cleared;
`--fail install` stays Downloaded so the install can be retried. Invalid
transitions, oversized chunks and digest/size mismatches fail closed and are
recorded as error events. No sockets, credentials, OEM code or VM are used.

`--script FILE` runs a JSON step list instead of the canonical exchange. Ops
are `download`, `begin-download`, `append` (hex `data`), `complete-download`,
`fail-download`, `install`, `begin-install`, `complete-install`,
`fail-install`, `reset` and `status`. Each step records its before/after state,
ok flag and error; the first failing step stops the run. Example:

```json
[{"op": "begin-download"},
 {"op": "fail-download", "reason": "network"},
 {"op": "download"},
 {"op": "install"}]
```

## DM/RDM tree validation and diff

```sh
python3 -B launch.py check-dm-tree <tree.xml> \
  [--compare <other-tree.xml>] [--fail-on-diff]
```

Checks the `rdmtree` root and version, exactly one FUMO managed object
(`urn:oma:mo:oma-fumo:1.0`) with integer `State`, `PkgVersion` and `PkgURL`,
the DM account address/port, and `APPSRV`/`CLIENT` auth fields for every
account. DTD/entity declarations and trees over 2 MiB are rejected.

The report contains structural facts, FUMO state/version and auth types/levels
only. It never prints addresses, account names, `AAuthName`, `AAuthSecret` or
`AAuthData`. `--compare` reports changed/added/removed leaf paths with account
segments replaced by `<accountN>`; `--fail-on-diff` turns a difference into
exit 1. The captured stock and update trees are identical at leaf level
(30 nodes, 63 leaves, FUMO state 10).

## Vendor esinit fixture to Multi-User (Mode V)

```sh
python3 -B launch.py run --profile research --scenario vendor-esinit-fixture \
  --rootfs /path/to/rootfs.img --timeout 240
```

A lab fixture that lets the real vendor `esinit` finish. It makes three
explicit substitutions before handing PID 1 to `/lib/systemd/esinit`:

- creates the `/var` skeleton, `/tmp/passwd` messagebus entry and D-Bus
  tmpfiles rule that the missing vendor initramfs normally provides
- replaces the `/etc/fstab -> /tmp/fstab` symlink with an empty regular file,
  because esinit points `/tmp/fstab` at the vendor eMMC fstab and the missing
  partitions otherwise force `systemd-remount-fs` into emergency mode
- preserves `/sbin/modprobe` as `/sbin/modprobe.real` and installs a wrapper
  that skips only `modprobe fan`; every other call is delegated

The scenario requires `OEM Type:`, `running in system mode` and `Reached
target Multi-User System`, in order, from PID 1. A real run reaches
Multi-User in about 16 seconds with `micommanager` and `nua.service` still
failing. This is **not stock boot**: the fan/micom gate is bypassed, not
emulated, and the fstab/runtime layout is a lab fixture.

## Rust app package (Mode V)

Host-built Rust apps/daemons are static-musl binaries packaged with the
generic SMACK-aware installer (`docs/19-rust-apps.md`). The unit never gets
`rustc` or `cargo`.

```sh
bash scripts/00_fetch_rust.sh
bash tools/rust/scripts/package_app.sh \
    tools/rust/examples/scourge-statusd --fetch \
    --out third_party/rust/out/scourge-statusd-usb
cd tools/vm
python3 -B launch.py run --scenario rust-app \
    --payload ../../third_party/rust/out/scourge-statusd-usb \
    --timeout 300 --memory 1024 --smp 1 --label rust-app
```

The scenario installs the package from the payload stick, checks static
linking, starts `scourge-statusd`, verifies `GET /status` and the SMACK
exec/process labels, sends SIGTERM, re-runs the installer (idempotent) and
restores (data preserved). The research boot has no systemd, so the daemon
is started directly; the unit file is installed/checked but the systemd
start path is not exercised here.

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
| `daemon exec`/`screenshot` failure | prints a single `ERROR:` line (no argparse usage dump) and exits 1 |
| `daemon exec --timeout` invalid | rejected up front (`finite and positive`) before contacting the daemon |
| `daemon did not become ready` | the spawned daemon is terminated and reaped before the error is raised, so QEMU/lock ownership are not orphaned |
| failed run bundle seems slow | bundle collection is capped at 45 s total; guest-output and serial tails are written even if the shell is wedged; `--no-failure-bundle` skips collection entirely |
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
