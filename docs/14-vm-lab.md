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
- power-loss recovery: kill QEMU mid-write, reboot the same state, require
  the synced marker to survive (ext4 journal recovery)
- repeat-update recovery (`recovery-repeat-update`): media-level verify,
  corruption detection, backup repair, re-verify and rediscovery; not the
  AppUpgrade engine's credential-gated repeat path
- offline update-media validation (`check-update`): package-map grammar,
  manifest coverage and confinement, optional hashes and declared sizes for a
  directory or raw ext4 stick; no decryption or updater execution
- update-state audit (`check-update-state`): version/FUMO timing/install
  params/dm_state consistency against the RDM tree, with URLs and credentials
  kept out of the report
- update-media preflight (`update-preflight`): read-only Mode V mount,
  package-map grammar/sizes, manifest hashes and opaque-payload checks without
  launching the updater
- synthetic update media (`make-update-media`): vendor-4.1-mountable ext4
  image with lab package map/manifest/payload, letting both preflight
  scenarios run without OEM media
- negative update preflight (`update-preflight-negative`): a disposable
  read-write overlay proves size/path/manifest corruption is rejected and the
  media is restored; the base stick is never modified
- DM/RDM tree validation and diff (`check-dm-tree`): FUMO state, account and
  auth structure, DTD/entity rejection and credential-redacted leaf diffs
- offline FUMO exchange model (`fumo-exchange`): strict Idle/Downloading/
  Downloaded/Updating transitions, package size/hash/version admission, mock
  DM endpoint, injected failures and a per-step `--script` runner; not
  dmengine or SyncML
- dmengine startup (`dmengine-startup`): real vendor DM engine with a lab
  DMCDEVINFO SysV segment, tree/config rewrite, FUMO/SCOMO plugin init and
  device-info acceptance; no DM session or production address
- dmengine OEM queue handler (`dmengine-oem-queue`): lab SysV client sends a
  type-0x36 command and receives event 66 after `recvDM_01`; no AppDMClient
  policy or HTTP FUMO update
- dmengine FUMO HTTP exchange (`dmengine-fumo-http`): OEM command 0/field1=1
  starts `dmc_startFumoSession`; the engine POSTs WBXML SyncML to a loopback
  mock endpoint, which returns a minimal WBXML response with Status 200 and
  Final. The engine completes the session handshake (state 1, lastError 0)
  without performing an update
- FUMO package download (`dmengine-fumo-download`): the mock sends a
  `syncml:auth-md5` server Cred and a FUMO Exec, then an OMA Download
  Descriptor. OEM confirmation code 10 (`DMC_REQ_DL_OK`) lets the engine
  fetch a 184-byte object (132-byte DP body with component/update tables and
  a 16-byte payload, plus a 52-byte lab HMAC trailer); `DL session
  complete` and FUMO state 30 -> 40 are asserted; the install-notification
  POST `900 Success` is also checked. `launch.py check-dp` validates the
  container layout. The DP is a structural fixture, so this is a download,
  not an install
- FUMO update request (`dmengine-fumo-update`): the mock answers the DM report
  session with a FUMO Update Exec. The engine queues
  `./ManagedObjects/FUMO/Update` and rejects the structural DP with
  `0x6011` / FUMO `24593`; no completed update state is asserted (negative)
- FUMO combined command (`dmengine-fumo-download-update`): Exec on
  `./ManagedObjects/FUMO/DownloadAndUpdate` downloads the DD/DP, advances
  state 30 -> 40 -> 60, hands event 77 with `/update//lab-fumo` to the OEM
  handler, then reports lab result 200, reaching state 90 and idle. The
  result is lab-supplied; no physical install is claimed
- FUMO failure result (`dmengine-fumo-update-failure`): the lab OEM reports
  result 500; the engine reports the failure through a DM session and returns
  to idle without state 90 (negative result path)
- FUMO invalid result (`dmengine-fumo-update-invalid-result`): result 0 is
  remapped to `UNDEFINED(409)`, reported through a DM session, and state 90 is
  never reached (negative validation path)
- FUMO missing object (`dmengine-fumo-download-missing`): the DD parses and
  confirmation is sent, but `/object.bin` returns 404; the engine aborts the
  DL session with `0x6306`, FUMO result 407 / error 25350 (negative)
- FUMO notification failure (`dmengine-fumo-notify-failure`): the DP download
  succeeds but `/notify` returns HTTP 500; the engine aborts with `0x6308`,
  FUMO result 503 / error 25352 and never reaches downloaded/update states
- FUMO update without result (`dmengine-fumo-update-no-result`): after the
  DownloadAndUpdate handoff the OEM sends no result; the engine stays in
  update progressing and never sets state 90 or reports success
- FUMO bad signature (`dmengine-fumo-download-badsig`): one trailer bit is
  flipped; the engine still downloads and advances to state 40, while
  `check-dp --key` rejects the object with a signature mismatch (negative
  engine-validation boundary)
- FUMO resume (`dmengine-fumo-resume`): a 64/184 partial response with
  `Accept-Ranges`/ETag makes the engine retry `Range: bytes=64-`; the mock
  returns the remaining 120 bytes with 206 and the download completes
- FUMO resume across restart (`dmengine-fumo-resume-restart`): phase 1 writes
  `dlresume.dat` after a partial 64/184 object; after a dmengine restart the
  engine sends `Range: bytes=64-` and the Range-capable mock completes the
  download (state 50, install notification)
- FUMO resume without ETag (`dmengine-fumo-resume-noetag`): the mock advertises
  `Accept-Ranges` but omits the ETag; the engine logs offset 64 with a null
  ETag, refuses to send `Range`, re-downloads the full object and completes
  (state 40)
- FUMO multi-component download (`dmengine-fumo-download-multi`): the mock
  serves a 291-byte object with two components and three update payloads; the
  engine downloads it and completes state 30 -> 40
- FUMO multi-component combined command
  (`dmengine-fumo-download-update-multi`): the same 291-byte object plus
  `DownloadAndUpdate`, update handoff (event 77) and lab result 200 leading to
  state 90 -> idle
- FUMO multi-component resume (`dmengine-fumo-resume-multi`): a 64/291 partial
  with ETag `lab-dp-291` makes the engine send `Range: bytes=64-`; the mock
  returns the remaining 227 bytes with 206 and the download completes
  (state 40, install notification)
- FUMO multi-component failure result
  (`dmengine-fumo-download-update-multi-failure`): the same 291-byte package
  plus `DownloadAndUpdate`, with OEM result 500; the engine reports the failure
  and never enters success state 90
- FUMO multi-component invalid result
  (`dmengine-fumo-download-update-multi-invalid-result`): OEM result 0 is
  remapped to `UNDEFINED(409)`; the engine reports the failure and never sets
  state 90
- device-model regression (`device-model`): asserts Q35 host bridge, SDHCI
  root, ICH9 LPC/SATA and no virtio; substrate only, not captured BXT hardware
- AppDMClient fota callback (`dmclient-fota-events`): `/fotaInterface`
  `sendEventToDMClientFromAppUpgrade(0/1/2)` produces real dmengine queue
  commands `0x28/0x29/0x2a`; callback only, no HTTP update or credentials
- dmengine OEM queue codec (`dmengine-queue`): encode/decode the 1048-byte
  SysV command/event message (key 0xd54, mtype 0x36/0x48) and parse
  `strace -xx` queue logs; the AppDMClient event source remains the session
  gate
- MOF notification codec (`mof-notify`): decode/encode the
  `msgtype=4&cor=...` device/version notification, with host redaction;
  supplies the `recvDM_01` input but not the OEM session callback
- vendor systemd partial boot (`vendor-systemd`): real systemd reaches
  Multi-User; SMACK units fail, consistent with the missing vendor initramfs (documented)
- vendor systemd D-Bus service/ordering (`vendor-systemd-dbus`): the research shell
  prepares the missing runtime layout and hands PID 1 to systemd; the D-Bus
  socket/service start before Multi-User, with micommanager still failing
  without vehicle hardware (lab fixture, not stock/esinit)
- vendor esinit partial boot (`vendor-esinit`): the real vendor
  `/lib/systemd/esinit` runs as PID 1 through OEM/car/country detection, eMMC
  fsck and the non-FDE fstab decision; the lge-fan driver attach blocks
  without micom/thermal hardware, so systemd is not reached (partial boot,
  documented hardware gate)
- vendor esinit fixture (`vendor-esinit-fixture`): a lab runtime layout, an
  empty `/etc/fstab` and a modprobe wrapper that skips only `fan` let the real
  esinit reach real systemd and Multi-User; micommanager/nua still fail and
  the hardware gate is bypassed, not emulated
- synthetic micom CAN interface smoke (`can-smoke`): READY/CALL/ACK for
  fixed and bulk calls; contract only, not real CAN timing
- vehicle-state model (`vehicle-state`): lab-defined fixed-frame decode for
  speed/gear/ignition/battery/doors/climate/outside plus the captured
  CANService Gen3High layouts (`0x1ab` speed/gear, `0x2fc` battery,
  `0x2b0` steering) and Gen2 fallbacks (`0x56e`, `0x55a`), clamps, range
  guards, bulk retention and reset; not a vendor DBC or stock vehicle
  fidelity
- host-built static-musl Rust app package (`rust-app`): install, static
  linking, HTTP `/status`, SIGTERM shutdown, SMACK exec/process labels,
  idempotent reinstall and data-preserving restore; the sample daemon is
  started directly because the research boot has no systemd (unit install
  is checked, systemd start is not exercised)

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
- a shared boot lock for direct runs, the daemon, regression suites and
  benchmarks, plus explicit qcow2 cache modes (writeback default; persistent
  state forces writethrough)
- repeatable boot benchmarks (`bench.py`) with mean/median/min/max and
  per-run logs under `runs/bench-*`
- gdbstub handshake validation with connect retries and fast-fail on a boot
  kernel panic
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
  regression (see the scenario's `known_issue`).
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
