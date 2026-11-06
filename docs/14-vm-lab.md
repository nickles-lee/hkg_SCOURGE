# 14 — The VM lab (bench-test before the car)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU). Boots your own
decrypted rootfs under QEMU/KVM so apps, installers and patches can be
exercised dozens of times an hour without touching the vehicle.

**Status: WIP.** The harness already runs on the maintainer bench — it is
what produces the "VM-validated" notes on the packages (docs/07) — but it is
not yet packaged inside this repo. Importing it under `tools/vm/`, adding
screenshot support, and adding a second (compat-kernel) graphics mode are
tracked in the master plan:
[`docs/14-vm-lab-plan.md`](14-vm-lab-plan.md). Until the import lands, this
page is the design record and roadmap.

## Why a VM

- The head unit is an x86-64 PC. The exact vendor kernel (`4.1.27-abl`) and
  the decrypted glibc 2.25 rootfs boot under KVM on commodity hardware.
- Iteration: a boot-to-shell run takes well under a minute; the same test
  on the car is an hour plus physical access.
- Agent-friendly: a serial console with framed commands, a structured
  `result.json` per run, and managed scenarios with PASS/FAIL/BLOCKED
  verdicts.
- Safety: untrusted firmware code only ever executes inside the guest; no
  NIC by default; every disk is a disposable qcow2 overlay over read-only
  backing images, so your staged inputs are never written.

## The one hard limit: the vendor kernel cannot show graphics

Read straight from the shipped kernel config (the IKCONFIG block inside
`vmlinux`):

- `CONFIG_FB` is not set; `CONFIG_VGA_CONSOLE` is not set
- `CONFIG_DRM_FBDEV_EMULATION` is not set
- the only DRM GPU driver is `CONFIG_DRM_I915=y`
- no bochs/QXL/virtio-gpu/vmwgfx, no virtio, no 9p

QEMU cannot emulate an i915 GPU, and AMD-only hosts have nothing to pass
through, so the exact vendor kernel is **headless-only in the VM — by
design, not for lack of work**. This constraint drives the two-mode design
below.

## Two modes

| | Mode V — vendor fidelity (works today) | Mode C — compat (roadmap) |
|---|---|---|
| Kernel | exact `4.1.27-abl` | pinned modern kernel + initramfs |
| Rootfs | your decrypted 260128 rootfs | the same rootfs |
| Display | none possible (see above); headless Weston | virtio-gpu/bochs → Weston fbdev/DRM |
| Input | none | virtio tablet/keyboard + QMP injection |
| Screenshots | vendor `weston-screenshooter` (plan P0-4) | QMP `screendump` (plan P0-5) |
| Good for | policy ground truth: SMACK, `.sign` gates, update policy, crypto/credential behavior | UI/QML/package behavior, layout, input, human debugging |

Division of truth: security and policy claims are only made from Mode V.
UI findings from Mode C are always labeled "compat kernel, not stock kernel
fidelity".

## What the lab can establish — and what it cannot

Offline-validated today (Mode V):

- vendor-kernel boot; USB update-tree discovery; texfat/exFAT mounts
- D-Bus service contracts; the real AppLauncher / ApplicationManager /
  AppDMClient lifecycle; the updater launch chain
- package install/restore round-trips against the real QML tree (docs/07)
- update-engine state machines up to the device-credential gate

Car-only (never VM claims):

- RF reality: WiFi/BT association, CarPlay over the air, iPhone behavior
- micom/CAN timing, audio/radio/GPS, the physical i965 display pipeline
- the device-credential path (the provisioned key is absent from the
  rootfs; code cannot substitute for missing provisioned state)

## Building it from your own firmware

Nothing OEM ships in this repo (AGENTS.md). The lab consumes artifacts you
already staged for the stage-1 build: after `scripts/01_stage_inputs.sh`
and `scripts/02_decrypt_rootfs.sh` you have the plaintext rootfs, and the
lab boots it read-only through qcow2 overlays. Concrete build/run steps
ship with the `tools/vm/` import (plan P1-13); until then, ask in the
Discord and a maintainer can walk you through the current bench setup.

## Screenshots before interactivity

Project decision: screenshot capture lands first (plan P0-4/P0-5) — one
PNG per run, agent-friendly — then interactive GTK/VNC windows for humans
(plan P1-5/P1-6).

## Roadmap and status labels

The full prioritized plan, with effort estimates and acceptance criteria,
is [`docs/14-vm-lab-plan.md`](14-vm-lab-plan.md). Everything in this repo
keeps its honesty labels: `car-validated` · `VM-validated (vendor kernel)` ·
`VM-validated (compat kernel)` · `offline-validated` · `WIP`.
