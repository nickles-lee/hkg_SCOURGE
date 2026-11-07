# 14 — The VM lab (bench-test before the car)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU). Boots your own
decrypted rootfs under QEMU/KVM so apps, installers and patches can be
exercised dozens of times an hour without touching the vehicle.

**Status: VM-validated substrate, WIP for full stock equivalence.** Mode V
(vendor kernel) is the policy/security test bench. Mode C (compat kernel) is
the graphics/UI substrate. Both are packaged in the VM lab source tree; the
full stock boot, genuine device credentials and a rendered AppHomeScreen page
remain WIP.

## Why a VM

- The head unit is an x86-64 PC. The exact vendor kernel (`4.1.27-abl`) and
  the decrypted glibc 2.25 rootfs boot under KVM.
- A managed boot takes seconds; the same test on the car takes an hour plus
  physical access.
- Every run writes a structured `result.json`, a serial log and (when asked)
  a screenshot. Scenarios have PASS/FAIL/BLOCKED verdicts and known-issue
  metadata.
- Untrusted firmware only executes in the guest. The default is no NIC; every
  disk is a disposable qcow2 overlay over read-only backing images.

## Two modes

| | Mode V — vendor fidelity | Mode C — compat |
|---|---|---|
| Kernel | exact `4.1.27-abl` | pinned host kernel (hash lock) + initramfs |
| Rootfs | your decrypted 260128 rootfs | the same rootfs |
| Boot | vendor bzImage + SDHCI gdbstub patch | virtio-blk, `init=/init`, `lsm=smack`, no gdbstub |
| Display | headless by kernel config; vendor `weston-screenshooter` artifact | virtio-vga → Weston 1.10 DRM + QMP PNG |
| Input | none by design | virtio tablet/keyboard + QMP injection |
| Share/network | no 9p; no NIC | opt-in 9p share; opt-in restricted user net |
| Good for | policy: SMACK, `.sign` gates, update policy, crypto/credential behavior | QML/UI, packages, layout, interactive debugging |

Division of truth: security and policy claims are only made from Mode V.
UI findings from Mode C are labeled "compat kernel, not stock kernel
fidelity". Mode C is never policy evidence.

## What the lab can establish — and what it cannot

VM-validated (vendor kernel):

- vendor-kernel boot; USB update-tree discovery; ext4/texfat mounts
- D-Bus service contracts; real AppLauncher/ApplicationManager/AppDMClient
  lifecycle; updater launch chain
- package install/restore round-trips against the real QML tree
- update-engine state machines up to the device-credential gate
- forced-reboot recovery smoke (marker + QEMU `-no-reboot` exit; one slice,
  not power-loss/corrupt-media coverage)
- no-space recovery on a synthetic 16 MiB update volume (ENOSPC assertion)
- corrupt-media recovery: overlay-only `IONIQ5_EU.ver` corruption is detected
  by `MANIFEST.sha256` with an actionable failure marker
- USB-removal recovery: QMP detaches the mounted update stick; the guest sees
  an I/O error and lazy-unmounts without hanging
- synthetic micom CAN interface smoke (`can-smoke`): READY/CALL/ACK for
  fixed and bulk calls; contract only, not real CAN timing

VM-validated (compat kernel):

- vendor rootfs boot on a pinned modern kernel; `/dev/fb0` + `/dev/dri/card0`
- Weston 1.10 DRM/pixman; visible Wayland probe; 1280×720 QMP screenshots
- QML engine evaluation (`qmlscene`) and WiFi-payload QML-error capture
- 9p host share, persistent state, VNC display, QMP click/key injection
- HDA audio card + direct `hw:0,0` playback; opt-in restricted user net
- visual-regression baselines/diffs and run management (list/show/extract/
  prune/gc)
- persistent VM daemon (`daemon start/exec/screenshot/console/status/stop`)
  with state surviving restarts
- a whole-suite lock for parallel regression runs and explicit qcow2 cache
  modes (writeback default; persistent state forces writethrough)
- opt-in USB passthrough plumbing (`--usb-host VENDOR:PRODUCT`); the host
  USB node must grant QEMU access, so hardware verification is host-gated

Car-only (never VM claims):

- RF reality: WiFi/BT association, CarPlay over the air, iPhone behavior
- micom/CAN timing, audio/radio/GPS, the physical i965 display pipeline
- the device-credential path (the provisioned key is absent from the rootfs)

## Building it from your own firmware

Nothing OEM ships in this repo (AGENTS.md). The lab consumes artifacts you
already staged for the stage-1 build. After `scripts/01_stage_inputs.sh` and
`scripts/02_decrypt_rootfs.sh`:

```sh
# Mode V
sh build_tools.sh tools.$(date +%Y%m%d).img
python3 -B launch.py doctor --graphics

# Mode C
sh compat/pin-kernel.sh
sh compat/build-initramfs.sh
python3 -B launch.py doctor --graphics
```

The tools image embeds a generated manifest; stale images are refused. The
compat kernel lock pins the installed kernel version, `vmlinuz`/config
hashes, required module hashes and the mkinitcpio recipe. Kernel bytes are
never committed.

Quickstarts:

```sh
# Mode V
python3 -B launch.py run --profile conformance --scenario boot

# Mode C
python3 -B launch.py run --kernel compat --profile conformance --scenario boot
python3 -B launch.py run --kernel compat --profile conformance \
  --scenario ui-smoke --qmp-screenshot
```

`tools/vm/RUNBOOK.md` in this repo (mirrored from the VM lab) has the full
two-mode workflow, capability matrix and troubleshooting.

## Screenshots before interactivity

Mode V captures `wayland-screenshot.png` through a second serial artifact
channel (vendor `weston-screenshooter`); it proves capture, not visible UI.
Mode C captures `qmp-screenshot.png` via QMP and can require a non-black
1280×720 frame (`ui-smoke`). Interactive GTK/VNC windows and QMP input are
available in Mode C; the vendor kernel stays input-free.

## Known issues

- Flat-mode `entropy-ready` fails the nonblocking getrandom check; split
  storage passes. It is marked expected-fail only for flat storage.
- `updater-launch` waits up to 240 s for an updater log line; use
  `--timeout 540` or `600`.
- `update-graphics` is expected-fail: the Qt shutdown hang is not a display
  regression (`probes/qt-shutdown.md`).
- `wifi-ui` currently captures missing AppHomeScreen imports
  (`HMIEVENTS`, `AppHome_Enum`); rendering the page needs the runtime.
- Mode C SMACK (`lsm=smack`) approximates but does not reproduce 4.1.27
  policy semantics.

Every FAIL/timeout writes a failure bundle with dmesg, processes, mounts,
Weston/QML/updater logs and the serial tail. Run management
(`list/show/extract/prune/gc`) defaults to dry-run; `--apply` is required to
delete runs.

## Status labels

`car-validated` · `VM-validated (vendor kernel)` · `VM-validated (compat
kernel)` · `offline-validated` · `WIP`. The prioritized plan and acceptance
criteria live in the VM lab master plan (`docs/14-vm-lab-plan.md`).
