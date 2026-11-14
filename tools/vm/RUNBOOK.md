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
storage volume evidence, power-loss/USB-removal evidence, checks, lock/pin and
related fields), requires a current-schema result's
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
out-of-range write, bulk retention, reset defaults, a hostile
`startbit=INT_MAX`/boundary range guard (ignored and logged, never written)
fail-closed rejection of an unknown D-Bus method (`UnknownMethod`), and the
16-distinct-canid retention limit (the overflow write is logged and dropped
without mutating decoded state).
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
