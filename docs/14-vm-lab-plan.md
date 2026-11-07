# Ioniq 5 Gen5W — VM Lab plan (final, decisions applied)

**Date:** 2026-11-07 · **Workspace:** `~/ioniq/vm` (→ dedicated repo) · **Sync target:** this repository (SCOURGE)

Prioritized TODO + project plan for the Ioniq 5 Gen5W VM harness. This file is
the master plan; it becomes the VM repo's `PLAN.md` and seeds
`docs/14-vm-lab.md` in SCOURGE (adapted, policy-compliant).

---

## 0. Decisions (recorded 2026-11-07)

1. **Two modes, both first-class and documented:**
   - **Mode V (vendor fidelity):** exact `4.1.27-abl` kernel + decrypted rootfs.
     Ground truth for *policy* testing: SMACK, signature gates, update policy,
     crypto/credential paths, security gotchas.
   - **Mode C (compat):** pinned modern kernel + same decrypted rootfs. For
     graphical UI work: QML, packages, AppHomeScreen, interactive debugging.
2. **Screenshots AND interactivity — screenshots first.** Screenshot capture is
   P0 and the primary artifact for agents; interactive windows (GTK/VNC) come
   after the screenshot path is solid.
3. **"Good enough" compat is fine.** No i915 passthrough, no vendor-kernel DRM
   backport. Fidelity deltas are documented, not solved.
4. **Network/passthrough policy change accepted.** Opt-in user-mode networking
   and USB passthrough are allowed features; default stays air-gapped
   (`-nic none`), conformance profile forbids them.
5. **Compat kernel is the priority.** Risky work (offline `vmlinux` patch,
   gdbstub determinism hardening, vendor-kernel graphics) is demoted to P3.
   The gdbstub boot stays as-is (works today, known-fragile).
6. **Dedicated VM repo + periodic SCOURGE sync.** The VM work gets its own git
   repo; notes, source, docs and findings are periodically cross-committed into
   an appropriate SCOURGE section so others can build & use VMs for testing.
   Cross-commits must respect SCOURGE repo policy (§5 below).

---

## 1. Current state (verified 2026-11-06)

### What works
- KVM boot of the exact vendor kernel + decrypted EU 260128 rootfs (gdbstub
  SDHCI patch, `-smp 1`, serial-only).
- USB mass storage, `texfat`, update-tree discovery, split synthetic storage,
  environment audits.
- D-Bus session/system buses + micom/appmanager/applauncher/automount/credman
  fixtures; real AppLauncher/ApplicationManager/AppDMClient lifecycle
  scenarios; sigstub A/B (research profile).
- Managed research/conformance profiles, 13 scenarios, ~122 unit/VM tests,
  explicit qcow2 overlays, per-run `result.json` (schema 3).
- Mesa 11.2 swrast + LLVM 3.8 bundle: unmodified AppDMClient initializes Qt/EGL
  (headless Weston + no-op IVI compositor).

### Hard constraints found (drive the two-mode design)
- **The vendor kernel cannot produce graphics under QEMU.** Embedded config:
  `# CONFIG_FB is not set`, `# CONFIG_VGA_CONSOLE is not set`,
  `# CONFIG_DRM_FBDEV_EMULATION is not set`, `CONFIG_DRM_I915=y` only; no
  bochs/QXL/virtio-gpu/vmwgfx; no virtio PCI/MMIO; no 9p. QEMU cannot emulate
  i915 and this host is AMD-only → Mode V is headless-only, by design.
- Vendor rootfs ships Weston 1.10 (headless/fbdev/drm/ivi backends, pixman
  renderer), `/usr/libexec/weston-screenshooter` (Wayland client → PNG),
  `wcap-decode`, Qt 5.4.1, systemd + `systemd-udevd`, `esinit`, socat,
  busybox. Mesa bundle adds `swrast`/`kms_swrast`/`virtio_gpu`/`vmwgfx` DRI
  drivers (usable by a compat kernel's stack).
- Host: Arch/omarchy kernel 7.2.15 with `virtio_gpu`, `bochs`, `virtio_input`,
  9p, `virtio_net` modules + `CONFIG_SECURITY_SMACK=y` (Mode C can boot with
  `lsm=smack` for closer policy behavior); QEMU 11.1.2 with GTK/VNC/SPICE/dbus
  displays, virglrenderer present; KVM on a 16-thread AMD-only host.
- Vendor kernel `CONFIG_SMP=y`, `NR_CPUS=4` → `-smp 2/4` is legitimate to try.

### Gaps (non-graphics)
- Tools images are stale vs `toolsroot/` (`tools.dev.img` — the default — has
  the oldest `phase4.sh`); no manifest/version gate.
- No screenshot/QMP/display/input support in `launch.py` (hardcoded
  `-display none -vga none -monitor none`).
- No state reuse (`--keep-overlay` exists, no `--state/--reuse`), no run
  management (`list/show/extract/prune`), ~730 run dirs / 2.7 GB accumulate.
- No file share (9p) or network; every script change needs an image rebuild.
- No failure bundles; no known-issue/expected-fail metadata
  (`update-graphics` FAIL is a diagnosed Qt shutdown hang, not a regression).
- `doctor` doesn't check tools version, QEMU capabilities, or stale processes.
- Helper build only rebuilds 3 of ~8 helpers; others are untracked binaries.
- `probes/`, `toolsroot/`, `guest/` sources are not hashed into
  `harness_sources` provenance.
- No git repo; `vm/` is ahead of `project/vm` with no history.

---

## 2. Target architecture

```
                     ┌────────────────────────────┐
                     │  VM repo (git)             │
                     │  harness/ guest/ probes/   │
                     │  toolsroot/ compat/ docs/  │
                     │  PLAN.md NOTES.md          │
                     └──────────┬─────────────────┘
                                │ sync-to-scourge.sh (curated, policy-checked)
                                ▼
                     ┌────────────────────────────┐
                     │  SCOURGE (AGPL)            │
                     │  docs/14-vm-lab.md         │
                     │  tools/vm/ (selected src)  │
                     │  findings in docs/ui, etc. │
                     └────────────────────────────┘

Mode V (policy ground truth)      Mode C (UI / graphics)
  vendor 4.1.27-abl kernel          pinned compat kernel + initramfs
  decrypted vendor rootfs           same decrypted vendor rootfs
  headless Weston (pixman)          virtio-vga/bochs + Weston fbdev|drm
  serial + gdbstub SDHCI patch      QEMU GTK/VNC window (opt), QMP
  weston-screenshooter PNGs         QMP screendump PNGs (primary)
  no input (by design)              virtio-tablet/kbd + QMP input inject
  policy/crypto/update research     QML/package/AppHomeScreen work
```

**Shared:** same harness (`launch.py` + `harness/`), same scenarios/profiles,
same overlay hygiene, same tools/payload images where possible. Every run
records `mode: vendor|compat` and a fidelity note.

**Division of truth:**
- Security/policy findings (SMACK labels, signature gates, `.sign` checks,
  update policy, credential handling) are only claimed from **Mode V**.
- UI/QML/layout/input findings and screenshots for humans come from
  **Mode C**, labeled "compat kernel, not stock kernel fidelity".
- `docs/ui/*` claims stay sourced from decompilation + car/VM-vendor evidence;
  Mode C screenshots are supporting evidence, not policy proof.

---

## 3. Repo & SCOURGE sync design

### 3.1 VM repo

- **Location:** `git init` in `~/ioniq/vm/` (keeps all paths and
  symlinks working; the ioniq workspace itself stays a non-repo). Repo name:
  `vm-lab` (remote optional).
- **License:** AGPL-3.0-only + SPDX headers on all first-party files from day
  one — makes SCOURGE cross-commits mechanically clean.
- **.gitignore:** `runs/`, `*.img`, `*.qcow2`, `.cache/`, `__pycache__/`,
  `build/`, `graphics-*/packages/`, `payload/` build outputs, `*.log`,
  `vmlinux*`, `bzImage`, `r2.img`, symlinks into the share. Nothing OEM-derived
  is ever committed; images are built from the user's own staged firmware
  (SCOURGE docs/03 flow) or referenced read-only.
- **Layout (mostly existing):** `launch.py`, `harness/`, `profiles/`,
  `scenarios/`, `probes/`, `guest/`, `toolsroot/`, `tests/`, `regress.py`;
  new `compat/` (kernel pins, config, initramfs recipe), `docs/` (runbooks,
  findings), `PLAN.md` (this file, maintained), `NOTES.md` (research log),
  `sync-to-scourge.sh`.
- **Update `ioniq/AGENTS.md` + `vm/DEVELOPMENT.md`** source-of-truth wording:
  the repo replaces the old "reconcile into `project/vm`" step.

### 3.2 SCOURGE sync (periodic, curated)

- **Cadence — standing rule for any agent executing this plan:** when a
  milestone in §6 has its acceptance criteria met, run `sync-to-scourge.sh`
  and land a "VM lab update" commit in SCOURGE **before starting the next
  milestone**. Not per-run, not mid-milestone, never hand-copied (§7 risk
  5). The script is built in P0-1; if it slipped, a milestone-end sync may
  be assembled manually but must still pass every §5 check.
- **Target sections:**
  - `docs/14-vm-lab.md` — public runbook: what the lab is, Mode V vs Mode C,
    what each can/cannot prove, build-from-your-own-firmware instructions
    (hook into existing `scripts/01_stage_inputs.sh` / `02_decrypt_rootfs.sh`),
    how to run scenarios, how to screenshot, how to validate a package in the
    VM before the car.
  - `tools/vm/` — selected, sanitized source (harness, probes, guest helpers)
    with SPDX headers.
  - Findings cross-posted into existing docs where relevant
    (`docs/07-packages.md` validation notes, `docs/ui/`, `docs/11`).
- **sync-to-scourge.sh checks (SCOURGE policy, enforced):**
  1. SPDX header present on every copied first-party file (docs/ exempt).
  2. No OEM-derived bytes: refuse any image, extracted binary, firmware,
     `*.tar.gz`, run artifacts containing vendor content; only source/docs/
     sanitized findings cross.
  3. No secrets/personal data (CIFS paths, hostnames, keys, LAN details).
  4. Status labels attached: `car-validated` / `VM-validated (mode)` / `WIP`.
  5. Fetch-not-vendor: compat kernel + Mesa deps are pinned-by-hash and
     fetched at build time, never committed.
- **This PLAN/TODO** goes in as `docs/14` appendix or `tools/vm/PLAN.md`
  (policy-compliant markdown).

### 3.3 Initial cross-commit (completed 2026-11-07)

Done before new VM work started — SCOURGE commit `da6b72d` ("VM lab:
docs/14 runbook seed + master plan (WIP)", pushed to
`github.com/nickles-lee/hkg_SCOURGE`):

1. Plan copied into the local ioniq workspace for posterity
   (`~/ioniq/vm/PLAN.md`, untracked until P0-1 gitignores it).
2. SCOURGE gained `docs/14-vm-lab.md` (runbook seed) and
   `docs/14-vm-lab-plan.md` (this plan, sanitized: local paths and host
   details genericized, mirror note attached), plus an AGENTS.md routing
   row and a README bullet.
3. No source import yet — `tools/vm/` source sync happens at P1-13 once
   SPDX headers exist in the VM repo.

---

## 4. Prioritized TODO

Effort: **S** ≤ 0.5 d · **M** 1–2 d · **L** 3–5 d · **XL** 1–2 wk

### P0 — Foundation: trustworthy baseline + screenshots + compat spike

| # | Task | Effort | Acceptance |
|---|---|---|---|
| P0-1 | **VM repo setup**: `git init`, `.gitignore`, SPDX headers, `PLAN.md`, `NOTES.md`, README, `sync-to-scourge.sh` (enforces the §5 checks), initial commit | S | Clean clone reproduces a managed scenario; `git status` clean; no OEM bytes in history; sync script self-test refuses an OEM/secret-bearing file |
| P0-2 | **Tools manifest + staleness gate**: `toolsroot/manifest.json` (file→sha256), embed build ID in image, `launch.py` refuses/`doctor` warns on mismatch; `run-phase4.sh` refuses stale tools | S | Stale `tools.dev.img` is detected and reported; rebuild path documented |
| P0-3 | **Rebuild tools from source**: extend `build_research_tools.sh` to compile all helpers (automount, credman, micom, usb-signal, CAN sim), byte-compare vs image | S–M | All helpers rebuilt from `guest/*.c`; one tools image passes all scenarios |
| P0-4 | **Screenshot infra — Mode V**: `weston-screenshooter` probe (headless Weston socket), PNG extracted via tools/payload dump; `vm extract` copies artifacts out of a retained overlay | S–M | phase4/AppUpgrade runs produce `wayland-screenshot.png` in the run dir |
| P0-5 | **Screenshot infra — Mode C / generic**: QMP socket (`-qmp`), `screendump` helper, `--screenshot` flag on `launch.py run`, PNG stored in run dir | S–M | Any run with a display device produces a valid PNG without human interaction |
| P0-6 | **Compat kernel spike** (host kernel first): mkinitcpio initramfs with virtio/9p/virtio-gpu/bochs modules, boot vendor rootfs via virtio-blk, `virtio-vga` + GTK window, Weston fbdev + IVI, `qmlscene` probe rect, QMP screenshot | M–L | Boot < 15 s; `/dev/fb0` or `/dev/dri`; Weston 1.10 up; screenshot shows the probe rect at 1280×720; no gdbstub involved |
| P0-7 | **Deterministic QEMU flags**: `-no-user-config -nodefaults -name … -uuid … -rtc base=utc`, record in `command.json` | S | Identical argv across runs; no host QEMU config leakage |
| P0-8 | **SMP/memory experiments (Mode V)**: `-smp 2/4`, `noapic` on/off, `-m` sizing; measure boot + llvmpipe/Qt times; pick safe defaults | S–M | Matrix documented; default updated if wins are safe; regressions still pass |
| P0-9 | **Failure bundles**: on FAIL/timeout, collect dmesg tail, `ps`, mounts, weston.log, QML logs, serial tail, screenshot (if any) into run dir + summary | M | One command explains most failures without a re-run |
| P0-10 | **Doctor 2.0**: tools version, QEMU version + needed devices, KVM, disk, stale QEMU/overlays, compat-kernel artifacts, capability report (`--graphics`) | M | `doctor` fails fast with actionable output before boot |
| P0-11 | **Known-issue metadata**: scenario `expected_fail`/known-issue fields; `update-graphics` marked known-FAIL (Qt shutdown hang) with probe link | S | Regressions distinguish known issues from new breaks |

### P1 — Two-mode harness + daily workflow

| # | Task | Effort | Acceptance |
|---|---|---|---|
| P1-1 | **Mode C harness support**: `--kernel vendor|compat` (compat = pinned kernel+initramfs+virtio disks, `lsm=smack`, no gdbstub), mode profiles, `result.json` records mode + fidelity notes | M | Both modes run managed scenarios from one CLI; mode visible in every result |
| P1-2 | **Pin the compat kernel**: fetch-not-vendor pin (preferred: pinned 6.12 LTS or pinned Arch kernel version) + config/initramfs recipe + hashes; document fidelity deltas vs 4.1.27 | L | Reproducible from pins on a clean host; `doctor` verifies artifacts |
| P1-3 | **ui-smoke scenario** (Mode C): Weston + IVI + `qmlscene`/AppHomeScreen; assertions = process alive + QML log lines + screenshot artifact | M | PASS from clean invocation; PNG + logs in run dir; works for agents |
| P1-4 | **9p share** (Mode C): `--share DIR`, guest mount helper; host-editable scripts/QML without image rebuilds | M | Edit file on host → visible in guest next command; tools rebuild no longer needed for script iteration |
| P1-5 | **Interactive display**: `--display gtk|vnc[:n]` (default none), `virtio-tablet-pci` + `virtio-keyboard-pci`; document VNC for remote devs | S–M | Dev can click the probe rect in a window; input events observed in guest logs |
| P1-6 | **Input injection**: QMP `input-send-event` wrapper (`vm click x y`, `vm key <qcode>`); multi-touch later | M | Scripted click changes QML state; screenshot confirms; usable by agents |
| P1-7 | **State reuse**: `--state DIR` + `--reuse`/`--reset` (persistent named overlays for rootfs/tools) | M | Install once → reboot with state; `--reset` returns to pristine backing; backing hashes unchanged |
| P1-8 | **Run management**: `--label`, `vm list/show/extract/prune/gc` | M | Runs searchable by label; prune safely reclaims the 2.7 GB backlog |
| P1-9 | **Opt-in networking** (Mode C first): `-netdev user,restrict=on` + hostfwd SSH; default still `-nic none`; conformance forbids | M | Guest reaches a controlled endpoint; policy change documented + audited |
| P1-10 | **WiFi-app UI scenario**: install payload → real AppHomeScreen → patched QML page screenshot + QML-error capture (feeds the wifi validation plan, Phase 4) | M | `WifiPage` visible in PNG or its QML errors captured; findings appended to app docs |
| P1-11 | **Provenance extension**: hash `probes/`, `toolsroot/`, `guest/` sources into `harness_sources` | S | Any probe/script change is visible in `result.json` |
| P1-12 | **Docs consolidation + two-mode runbook** (VM repo): one quickstart per mode, capability matrix, graphics runbook, troubleshooting; supersede stale claims in `VM-HARNESS.md`/`README.local.md` | M | A new dev reaches a screenshot in each mode following only the runbook |
| P1-13 | **First full SCOURGE sync**: `tools/vm/` source import with SPDX headers + updated `docs/14`, run through the P0-1 sync script | M | SCOURGE lint clean; SPDX everywhere; no OEM bytes; status labels present |

### P2 — Comfort, coverage, fidelity

| # | Task | Effort | Acceptance |
|---|---|---|---|
| P2-1 | Visual regression: baseline PNGs + pixel-diff threshold + rebaseline flow | M | UI change produces reviewable diff |
| P2-2 | Persistent VM daemon: `vm start/console/exec/stop/status/screenshot` without cold boots | L | Start once, iterate all day; state survives |
| P2-3 | `wcap` build (Weston 1.10) + recording probe; `wcap-decode` extraction | M | Short vendor-UI recording decodable to frames |
| P2-4 | Virgl: `virtio-vga-gl` + GTK GL; measure vs llvmpipe; keep software fallback | M | Faster UI or documented no-win |
| P2-5 | Audio (Mode C): `intel-hda`/`hda-duplex`; ALSA probe | S–M | Guest sees a card; test tone works |
| P2-6 | USB host passthrough (opt-in): WiFi/BT/CAN dongles to guest | M | Dongle visible in guest; security boundary documented |
| P2-7 | Recovery scenarios (Mode V): reboot, power-loss, corrupt media, no space, USB removal, repeat update | L | Expected abort/retry/cleanup observed, not just survival |
| P2-8 | Vendor systemd/esinit partial boot (Mode V): minimal units, service ordering, D-Bus activation | L | Documented partial stock-boot; no false equivalence claims |
| P2-9 | CAN/micom expansion: broader vehicle-state simulation | L | Deterministic vehicle-state tests beyond current stubs |
| P2-10 | Parallel-run governor: lock/queue heavy boots, cap concurrency | M | Repeated parallel suites don't hit EHCI timeouts |
| P2-11 | Performance: qcow2 cache modes, CPU pinning, boot benchmarks | M | Repeatable numbers; defaults tuned for 16-thread host |
| P2-12 | Milestone-checkpoint SCOURGE syncs (§3.2 standing rule); findings into `docs/ui`, `docs/07`, `docs/11` | S each | Cross-commits pass policy checks each time |

### P3 — Risky / blocked (deliberately deprioritized)

| # | Task | Effort | Verdict |
|---|---|---|---|
| P3-1 | Offline `vmlinux`/PVH boot fix (kill gdbstub dependency) | L | Deprioritized per decision 5; gdbstub works, retry-later |
| P3-2 | gdbstub hardening (bounded retry, stop-reason logging) | M | Only if P3-1 fails to land first |
| P3-3 | QEMU DRM driver backport to vendor 4.1.27 kernel | XL/blocked | Needs vendor kernel source; not planned |
| P3-4 | Intel i915 passthrough | XL/hardware | Host is AMD-only; not planned |
| P3-5 | Custom RDP/VNC/input bridge on vendor headless compositor | L–XL | Only if Mode C is ever unacceptable |
| P3-6 | Real GPU/radio/CAN-timing fidelity | out of scope | Traces + car tests only |

---

## 5. SCOURGE cross-commit policy checklist (enforced by sync script)

- [ ] File is first-party and carries `SPDX-FileCopyrightText: 2026 The SCOURGE
      Project contributors` + `SPDX-License-Identifier: AGPL-3.0-only`
      (after shebang; C form for C sources; docs/ exempt).
- [ ] No firmware images, OEM binaries, OEM-derived artifacts, `*.tar.gz`,
      run logs with vendor content, or anything from `input/ work/ out/ runs/`.
- [ ] No TE2 key material, credentials, pubkeys, LAN/host details.
- [ ] Third-party material (compat kernel, Mesa) referenced by pinned URL+hash,
      fetched at build time — never committed.
- [ ] Every claim labeled: `car-validated` / `VM-validated (vendor kernel)` /
      `VM-validated (compat kernel)` / `offline-validated` / `WIP`.
- [ ] Information minimization: no unused-vulnerability writeups; platform
      gates documented, not weaponized.
- [ ] Target header (`TARGET: Ioniq 5 Gen5W_L, 260128 (EU)`) on firmware-specific
      scripts; scripts runnable via `sh`/`python3` (no +x dependence).

---

## 6. Milestones

- **M1 (week 1):** P0-1..P0-5, P0-7..P0-11 — repo live, tools trustworthy,
  screenshots working in both modes, failure bundles, doctor 2.0.
  - **SCOURGE sync checkpoint:** refresh `docs/14` status + carry M1
    findings across (§3.2 standing rule).
- **M2 (week 2):** P0-6 + P1-1..P1-3 — compat spike → `--kernel compat` +
  pinned kernel + `ui-smoke` PASS with screenshot.
  - **SCOURGE sync checkpoint:** compat-mode + `ui-smoke` evidence into
    `docs/14`.
- **M3 (week 3):** P1-4..P1-12 — 9p share, interactivity (window + input),
  state reuse, run mgmt, WiFi-app UI scenario, docs; **ends with the first
  full SCOURGE sync (P1-13)**.
- **M4 (weeks 4+):** P2 items in order; P3 only as capacity allows.
  - **SCOURGE sync checkpoint:** after each completed P2 batch (P2-12).

**Rough totals:** screenshots-everywhere ~1 wk; two-mode harness + pinned
compat ~1.5–2 wk; daily workflow + interactivity + sync ~1–1.5 wk; comfort
items beyond that.

---

## 7. Risks / open questions

1. **Vendor Weston on compat kernel** (udev/systemd age vs 7.x kernel) is the
   main M2 unknown — spike answers it early; fallbacks: manual `/dev` setup,
   `init=/init`, or older pinned kernel (6.1/5.15).
2. **Compat kernel pinning source** (Arch rolling vs kernel.org LTS build) —
   decide after spike; requirements: virtio-gpu/bochs/9p/virtio-input/SMACK.
3. **Mode C SMACK** (`lsm=smack`) approximates but does not reproduce 4.1.27
   policy semantics — policy claims stay Mode-V-only.
4. **EHCI concurrency flakiness** observed once — governor (P2-10) is the fix;
   don't parallelize boots before then.
5. **SCOURGE sync discipline** — the sync script must be used, not hand-copied;
   it enforces headers and byte rules so nothing OEM leaks.

## 8. Explicitly not planned

- Making the vendor kernel display graphics (blocked by kernel config).
- Intel GPU passthrough on this host.
- Treating `update-graphics` FAIL as a display problem (it's the known
  QtWayland shutdown hang).
- Solving `verifyKeyFile`/credential provisioning (unchanged, Mode V research).

---

## 9. Progress log

- **2026-11-07 — P0-1 complete** (`9f2c71f`): `vm-lab` repo initialized with
  `.gitignore`, SPDX headers (59 code files), `LICENSE`, `PLAN.md`,
  `NOTES.md`, README/dev-doc source-of-truth updates, local `docs/14-vm-lab.md`,
  and `sync-to-scourge.sh` with a passing refusal-rule self-test. 100 files /
  506 KB committed; no images or OEM bytes. Clean clone + managed `boot`
  scenario → `HARNESS_CHECK boot PASS`, `PASS rc=0`.
- **2026-11-07 — P0-2 complete**: deterministic `toolsroot/manifest.json`
  embedded in the image; `harness/tools.py` compares it via read-only
  `debugfs`. `build_tools.sh` writes and self-checks the manifest and sizes
  the image from the tree. `launch.py` refuses stale tools for managed
  scenarios, `doctor` reports `tools_manifest`, and `run-phase4.sh` refuses
  with rebuild instructions. Legacy `tools.dev.img` detected as
  `unmanifested`; rebuilt `tools.p0-2.img` is `fresh` and
  `usb-discovery` PASSes. 126 unit tests OK.
- **2026-11-07 — P0-3 complete**: one shared builder compiles all seven
  `guest/*.c` helpers (entropy, applauncher/appmanager/automount/credman,
  micom, usb-signal, tweaks CAN sim) into `build/helpers/`, `toolsroot/`,
  `guest/`, and the tweaks payload. `build_tools.sh` byte-compares the five
  toolsroot helpers against the image; rebuilt helpers are byte-identical
  (manifest build id unchanged). Scenario sweep with `tools.p0-3.img`:
  flat boot/usb-discovery/dbus-ready/dmclient-events/environment-audit PASS,
  updater-launch PASS at `--timeout 600`; split entropy-ready,
  storage-isolation, vendor-lifecycle, vendor-lifecycle-sigstub,
  applauncher-sigstub PASS. Known pre-existing issue for P0-11: flat
  `entropy-ready` fails nonblocking getrandom (legacy image too; split passes).
- **2026-11-07 — P0-4 complete**: raw artifact channel over a second QEMU
  serial port (`/dev/ttyS1` → `artifact-serial.bin`), with `--screenshot` on
  `launch.py`, automatic extraction of `wayland-screenshot.png` into the run
  dir, sha256 in `result.json`, and `run-phase4.sh` requesting a screenshot by
  default. The guest trap disables tty `-opost` to keep PNG bytes intact.
  `phase4.sh` now creates the missing `rw_data/var` target so Weston starts in
  flat runs. Evidence: phase4/AppUpgrade `PASS`, valid 1920×720 PNG; the image
  is black because Mode V Qt is offscreen (content is Mode C's job). 132 unit
  tests pass.
- **2026-11-07 — P0-5 complete**: `harness/qmp.py` QMP client
  (`qmp_capabilities` + `screendump format=png`, PNG validation) and
  `launch.py --qmp-screenshot` with `--display-device none|std|virtio-vga`.
  Capture runs before QEMU is killed, including on failures. Evidence:
  managed boot with `std` and with `virtio-vga` produced valid 720×400 PNGs
  with sha256/dimensions in `result.json`. 136 unit tests pass.
- **2026-11-07 — P0-6 complete**: `compat/mkinitcpio.conf`,
  `compat/build-initramfs.sh`, and `compat/spike.py` boot the vendor rootfs
  with host kernel 7.2.15 over virtio-blk, no gdbstub. Chain:
  virtio-vga 1280×720 → `/dev/dri/card0` + `/dev/fb0` → Weston 1.10
  `drm-backend --use-pixman` → `fullscreen-shell` → `weston-simple-shm` →
  QMP PNG. Evidence: boot 2.85 s, probe bbox `515,235–764,484`, image
  `docs/screenshots/mode-c-probe.png`. Fidelity delta: fullscreen-shell, not
  the vendor IVI window manager (IVI surfaces need placement; P1-3).
- **2026-11-07 — P0-7 complete**: QEMU argv is deterministic:
  `-no-user-config -nodefaults -name vm-lab -uuid <fixed> -rtc base=utc`;
  repeated argv is identical, and managed boot + usb-discovery pass under
  `-nodefaults`. 138 unit tests.
- **2026-11-07 — P0-8 complete**: `--smp 1|2|4`, `--memory MiB` and `--apic`
  are recorded per run. Boot matrix in `docs/perf-smp-memory.md`: SMP 1 is
  fastest, SMP 4 had a 26 s outlier, APIC removal and 8 GiB memory showed no
  win. Defaults stay `-smp 1`, `-m 4096`, `noapic`; no regressions.
- **2026-11-07 — P0-9 complete**: `harness/failures.py` writes a
  `failure/summary.md` plus dmesg/process/mount/storage/Weston/QML/updater/
  tools/serial logs on FAIL or timeout, while QEMU is still alive;
  `--no-failure-bundle` opts out. Verified with a real `exit 7` run
  (`FAIL rc=7`, complete bundle). 142 unit tests.
- **2026-11-07 — P0-10 complete**: `launch.py doctor --graphics` checks KVM,
  QEMU >= 8.0, required devices/display backends, `debugfs`, tools
  contents/manifest, input/update images, free disk, run count, stale
  `vm-lab` QEMUs, and (graphics mode) host kernel/compat initramfs/modules/
  spike. Failures print `next_actions`. Fresh tools + graphics → ok; legacy
  tools → only `tools_manifest` fails with the rebuild command. 144 tests.
- **2026-11-07 — P0-11 complete**: scenarios support `expected_fail`
  (bool or storage-scoped) and `known_issue`; runs record
  `expected_fail`/`known_failure`/`known_issue`; `regress.py` separates
  `known_failures` from `new_failures`. `update-graphics` is expected-fail
  (Qt shutdown hang, probe link); flat `entropy-ready` is expected-fail;
  `updater-launch` carries the timeout-540 note. Real flat entropy run:
  `FAIL (known)`, summary `ok=true`, no new failures. 147 tests.
- **2026-11-07 — P1-1 complete**: `launch.py --kernel vendor|compat`
  (legacy vendor sub-modes still accepted); compat uses pinned-or-host kernel
  + initramfs, virtio-blk root, `lsm=smack`, no gdbstub; mode/fidelity/kernel
  paths recorded in `result.json`. Mode-aware bootstrap avoids vendor
  `modprobe`. Compat PASS: boot, usb-discovery, dbus-ready, environment-audit,
  storage-isolation; vendor boot/usb-discovery still PASS. 152 tests.
- **2026-11-07 — P1-2 complete**: `compat/kernel.lock.json` pins the
  installed kernel version, `vmlinuz`/config hashes, required module hashes
  and the initramfs recipe; `compat/pin-kernel.sh` records it.
  `build-initramfs.sh`, `launch.py --kernel compat`, and `doctor --graphics`
  verify it and refuse mismatches (`--allow-kernel-mismatch` is research-only).
  `compat/FIDELITY.md` documents deltas vs `4.1.27-abl`. Pinned build + boot
  PASS; doctor ok. 157 tests.
- **2026-11-07 — P1-3 complete**: `ui-smoke` is Mode C only and asserts
  Weston alive, a visible Wayland probe, a real QML `qmlscene` marker, and a
  non-black 1280×720 QMP frame (`requires_visible_frame`). Evidence:
  all checks PASS, bbox `515,235–764,484`,
  `docs/screenshots/mode-c-ui-smoke.png`. QML runs offscreen because the
  vendor QtWayland stack only has IVI integration; real QML on the compositor
  is P1-10. 162 tests.
- **2026-11-07 — P1-4 complete**: compat-only opt-in `--share DIR` mounts a
  host directory at `/media/host` over 9p (`security_model=none`); setup
  timing and path are recorded, mount failure stops the run, vendor mode
  rejects it. Host file readable in-guest; docs warn to use a disposable
  directory. 166 tests.
- **2026-11-07 — P1-5/P1-6 complete (protocol level)**: `--display gtk|vnc`
  plus `--vnc-display`; compat adds virtio tablet/keyboard. `launch.py input`
  (`--click`/`--key`) injects QMP `input-send-event`. Evidence: VNC :7
  listened; live VM accepted click 640,360 and `ret`, both ok. A
  UI-state-changing click still needs the P1-10 real QML/IVI surface; current
  ui-smoke QML is offscreen. 170 tests.
- **2026-11-07 — P1-8 complete**: `--label` in results; `list/show/extract/
  prune/gc` CLI. Prune defaults to dry-run, protects retained overlays and
  unsafe paths; `--apply` is explicit. Evidence: labeled run listed; dry-run
  reported 62 candidates with 800 protected; extraction produced a valid PNG.
  174 tests. The 2.7 GB backlog was deliberately not deleted.
- **2026-11-07 — P1-7 complete**: `--state DIR` persistent qcow2 overlays
  with a state lock; `--reset` returns to pristine backing. Drives use
  writethrough plus guest `sync`; backings unchanged. Evidence: write →
  reboot read → reset absent; backing size/mtime unchanged. Flat storage
  only. 178 tests.
- **2026-11-07 — P1-11 complete**: `harness_sources` hashes guest, probes,
  toolsroot scripts and compat sources (manifest excluded); a compat result
  records 73 source hashes. 179 tests.
- **2026-11-07 — P1-9 complete**: compat-only opt-in `--network` adds
  user-mode net with `restrict=on`, hostfwd 2222→22 and `virtio-net-pci`;
  vendor/conformance reject it, default remains `-nic none`. Guest eth0 at
  10.0.2.15 pinged the controlled gateway 10.0.2.2. Pin includes virtio_net.
  182 tests.
- **2026-11-07 — P1-10 complete (QML-error branch)**: `wifi-ui` installs the
  first-party WiFi payload from `--share` and evaluates patched
  `WifiPage.qml`; install/QML logs are emitted. Evidence: install PASS and
  captured errors `HMIEVENTS`/`AppHome_Enum` not installed. This is not a
  rendered page; visual content needs the AppHomeScreen runtime. 186 tests.
- **2026-11-07 — P1-12 complete**: `docs/RUNBOOK.md` consolidates both
  quickstarts, capability matrix, screenshots, daily workflow, troubleshooting
  and safety; README/DEVELOPMENT/README.local route to it and stale wording is
  removed. 186 tests unchanged.
- **2026-11-07 — P1-13 complete**: first full SCOURGE sync via
  `sync-to-scourge.sh --apply`; 113 files (700 KB) in `tools/vm/`, updated
  `docs/14-vm-lab.md` and the progress mirror. Policy audit clean (no
  OEM/binaries/exec bits/local paths/credentials; SPDX on code; pins for
  third-party). SCOURGE commit `a9d9351` pushed to `main`.
- **2026-11-07 — P2-1 complete**: `launch.py visual baseline|compare|list`
  writes/uses workspace-local baselines, reports changed pixels/ratio with a
  threshold and tolerance, and writes a diff PNG. Verified identical → 0;
  different frame → 22,115 pixels (2.4%) over 1%. 189 tests.
- **2026-11-07 — P2-5 complete**: compat-only `--audio` adds
  `intel-hda` + `hda-duplex` with a null host backend. Guest sees card 0 /
  Generic Analog; `aplay -D hw:0,0` playback returns 0. `dmix` default is
  absent; direct device documented. Pin includes HDA modules. 191 tests.
- **2026-11-07 — P2 milestone sync**: incremental SCOURGE commit `e7c45a3`
  pushed (116 files in `tools/vm/`) with visual regression, audio,
  state/runs/share, network/display/input/failure/provenance updates and the
  P2 runbook. Policy audit clean.
- **2026-11-07 — P2-6 plumbing complete, hardware blocked**: compat-only
  repeatable `--usb-host VENDOR:PRODUCT` with EHCI pinning and result
  metadata; security boundary documented. Host USB nodes are root-only, so
  in-guest visibility is not validated; argv/refusal tests pass. 193 tests.
- **2026-11-07 — P2-6 sync**: SCOURGE commit `1f09222` pushed with USB
  passthrough plumbing, test, RUNBOOK/security note and docs/14 update.
  Policy audit clean.
- **2026-11-07 — P2-2 complete**: persistent daemon
  `start/status/exec/console/screenshot/stop`, control/QMP/serial sockets,
  state lock and guest `sync` on stop. Evidence: boot 3.53 s, exec,
  1280×720 screenshot, console, state surviving stop/restart. 196 tests.
- **2026-11-07 — P2-10 complete**: `harness/governor.py` serializes whole
  regression suites on `runs/.vm-lab.lock` with a 600 s default wait and
  `--no-lock` opt-out; summary records `locked`. Two parallel boot suites
  both passed without EHCI contention. 198 tests.
- **2026-11-07 — P2-11 complete**: `--cache` on runs/daemon; persistent
  state forces `writethrough`. Boot benchmark: writeback 5.92/6.15 s,
  writethrough 7.23/5.77 s, directsync 46.26/44.93 s; keep writeback.
  199 tests.
- **2026-11-07 — P2-2/P2-10/P2-11 sync**: SCOURGE commit `e079e20`
  pushed (121 files in `tools/vm/`) with daemon, governor, cache modes and
  tests. Policy audit clean.
- **2026-11-07 — P2-4 documented no-win**: `egl-headless` + `virtio-vga-gl`
  boots the compat guest, but QMP screendump has no surface. Keep
  `virtio-vga` for screenshots; GTK+GL remains untested headless. 200 tests.
- **2026-11-07 — P2-4 sync**: SCOURGE commit `6fd8e2f` pushed with the
  egl-headless/virtio-vga-gl options, no-win documentation and test.
- **2026-11-07 — P2-9 complete (contract test)**: `can-smoke` mounts the
  existing CAN-sim payload, starts the system bus, and asserts READY/CALL/
  ACK for `SetCANFixedMicomRecv` and `SetCANMicomRecv` with argument values.
  Vendor run PASS; serial contains the full simulator log. 203 tests. This
  is a deterministic interface test, not real CAN timing/vehicle fidelity.
- **2026-11-07 — P2-7 slices complete**: `recovery-reboot`,
  `recovery-no-space`, `recovery-corrupt-media`, and
  `recovery-usb-removal` (QMP `device_del`; guest sees I/O error and
  lazy-unmounts). All real runs PASS. Power-loss and repeat-update remain.
- **Remaining:** P2-3 wcap (client absent), P2-7 power-loss/repeat-update,
  P2-8 stock boot. P3 deferred.
- **2026-11-07 — P2-9/P2-7 sync**: SCOURGE commit `253bf08` pushed with
  CAN smoke, forced-reboot recovery, tests and docs (127 files in
  `tools/vm/`). Policy audit clean. The no-space addition is pending the next
  sync.
- **2026-11-07 — no-space sync**: SCOURGE commit `34e17a1` pushed with
  `recovery-no-space` and scenario-provided storage sizing (128 files in
  `tools/vm/`). Policy audit clean.
- **2026-11-07 — corrupt-media sync**: SCOURGE commit `0d81299` pushed with
  `recovery-corrupt-media`, tests and docs (130 files in `tools/vm/`).
- **Remaining:** P2-3 wcap (client absent), P2-7 power-loss/repeat-update,
  P2-8 stock boot. P3 deferred.
