# 21 — Android phone rig (alternative to the Pi)

TARGET: Gen5W_L (Linux) head units. This document describes only the **hardware rig**: how to
replace the Pi/nbdkit mass-storage device in `docs/04` with a rooted Android phone. Firmware
targeting, patch derivation and the stage-1 build are covered by `docs/03`, `docs/09`, `docs/10`
and their existing locks; nothing here changes those. Read `docs/01` for the underlying concept
and `docs/13` for upstream references. No mitigation content is duplicated here.

Evidence status is in §12. **Car-validated on one phone/firmware/vehicle combination only** —
do not generalise from it.

---

## 1. Why a phone

The rig in `docs/04` uses a Pi Zero in `dwc2` gadget mode with an nbdkit-backed LUN. Android's USB
gadget stack is the same Linux stack, so a rooted phone with a configfs mass-storage gadget can
serve the same dynamic LUN. The missing piece in both cases is a per-read-pass backend: on the Pi
it is nbdkit; here it is a device-mapper table that is reloaded live, with a block-layer read
detector deciding when.

Advantages: no Pi, no SD card, no second power supply, and the phone can be driven from Termux
widgets with no PC attached. Costs: the phone must be genuinely rooted, its kernel must expose the
device-mapper ioctls and block-layer ftrace, and the tooling is more sensitive to read-order
differences than the Pi's `pread`-level view (§7).

## 2. Hardware and software requirements

- A rooted Android phone with:
  - a USB peripheral controller (UDC) that can be bound to a configfs `f_mass_storage` gadget;
  - a kernel exposing `DM_DEV_CREATE` / `DM_TABLE_LOAD` / `DM_DEV_SUSPEND` ioctls. On the dmctl
    build of the validated phone these could not reload a live table, hence the on-device binary;
    other builds may differ. `phone-rig loadtest` can exercise a table reload, but it **replaces
    the table on the named device** — point it at a scratch `-n` name, never at the device bound to
    the LUN;
  - ftrace with the `block_rq_issue` tracepoint and a writable `tracing` directory;
  - enough storage for the backing images (§4.1).
- Root shell access on the phone (`su`), and ADB for deployment only.
- A USB data cable from the phone's data port to the head unit.
- A build host with Go >= 1.21 and ADB. No Go toolchain is committed; install one locally.

Owner-local values are split between two files, both ignored by `tools/phone/.gitignore`:

- `tools/phone/phone.env` — host-side ADB deployment and on-device provisioning inputs
  (ADB identifier, image paths, gadget name, UDC).
- `tools/phone/config.json` — on-device dm/loop devices, geometry and detector thresholds.

Copy the two `.example` files, fill them in, and never commit the filled-in copies.

## 3. Principle

`tools/phone/main.go` builds a device-mapper device with up to three linear segments:

```
0 .. region_start        genuine loop device
region_start .. end      genuine loop device, or evil loop device
end .. disk_end          genuine loop device
```

`flip -mode genuine` loads the one-segment table; `flip -mode evil` loads the three-segment table
with the region pointing at the second loop. The swap is `suspend -> DM_TABLE_LOAD -> resume`,
followed by a page-cache drop. The LUN stays open the whole time — there is no media change.

`watchtrace` watches `block_rq_issue` for reads in the region and decides when to flip. Two
triggers are implemented; the quiet-window trigger is the default and the one validated on the
car (§7). `watch` is an older volume/gap heuristic kept for reference.

## 4. Build, deploy and provision

```sh
cd tools/phone
cp phone.env.example phone.env       # host-side values, ignored by git
cp config.json.example config.json   # on-device devices/geometry, ignored by git
sh build.sh                          # static linux/arm64 binary
sh scripts/deploy.sh                 # push binary, config and scripts over ADB
```

`build.sh` sets `CGO_ENABLED=0 GOOS=linux GOARCH=arm64` and prints the binary's sha256. Keep that
hash with your notes so you can confirm what is on the phone. Run scripts with `sh` — do not rely
on executable bits. `.gitattributes` forces LF for the shell scripts so Android's `sh` can parse
them after a checkout on any platform; `deploy.sh` also strips CR defensively.

### 4.1 Backing images

Prepare both images from your own lawfully-acquired firmware, following `docs/02`–`docs/04`.
There are two supported layouts:

| Layout | Genuine image | Modified image | `evil_off_sectors` | Storage |
|---|---|---|---|---|
| Full copy | whole USB image | whole USB image with only the region changed | the region's start sector | 2 × full image |
| Region file (example default) | whole USB image | region-sized file, zero-padded to `region_len_sectors * 512` bytes | `0` | 1 full image + region |

`evil_off_sectors` is the offset **on the modified loop device** at which the modified bytes start:
use `0` for a region-sized padded file, or the region's start sector for a full-disk copy. The
geometry in `config.json` (`region_off_sectors`, `region_len_sectors`, `total_sectors`) must be
derived from your own image, not copied from the example.

### 4.2 One-time provisioning

On a fresh phone, run `sh provision.sh` under `su`. It attaches both images to loop devices,
creates the dm device by name (without a config, since the loop paths are not known yet), resolves
the block node through `/sys/block/dm-*/dm/name`, and writes the resolved values to
`$RIG_DIR/provision.out` **before** binding the gadget. The bind unbinds the UDC first and will
disconnect ADB — read the values from the phone screen or the saved file afterwards.

Then:

1. Put `dmname`, `dmnode`, `gendev` and `evildev` from `provision.out` into `config.json`.
2. Run `sh restore-genuine.sh` to load the one-segment genuine table (provisioning alone leaves
   the dm device tableless).
3. Run `sh status.sh` and confirm every gate in §5 passes.

All gadget/UDC names come from the `rig.env` file that `deploy.sh` generates from `phone.env`;
adapt them to your phone. Write protection lives at the USB LUN (`lun.0/ro=1`), not in the dm
table. Never leave it at `0`.

## 5. Status gates

`status.sh` exits **nonzero** if any gate fails; a failed gate is a stop, not a warning. Before any
run it must show all of:

- the LUN table is **one segment** pointing at the genuine loop device;
- the gadget LUN's `file` is the dm node and its `ro` is `1`;
- the detector is `watchtrace` **with** `-logonly` for a dry run, **without** it for a live run;
- the boot auto-arm flag is **absent** (there is deliberately no boot-time arming);
- the UDC is bound to the gadget (otherwise no USB disk is presented).

`arm-live.sh` and `arm-dryrun.sh` both retire every previous watcher, remove the auto-arm flag,
set the genuine table and reset the logs before starting. Run them with `su` on the phone.

## 6. Dry run first

`arm-dryrun.sh` starts the detector in log-only mode. It cannot change the served bytes, so it is
the safe way to validate timing on real hardware. Run the same update procedure as a real attempt,
then check the log for the sequence in §7: a `QUIET` flip point followed by `DRYRUN`, with the
first install-phase read occurring **after** that point.

Only proceed to a live run when that ordering is confirmed on your own head unit.

## 7. Timing: two failure modes worth knowing

Both were found on real hardware and both are handled in the current binary. They are the main
reason this rig is more subtle than the Pi's per-read backend.

### 7.1 Pass numbering

The Pi's nbdkit sees every logical `pread` with an exact offset, so its pass counter starts at the
very first near read. At the block layer, page-cache hits generate no request and the head unit's
read order varies between runs: a small header/tail probe may be visible in one run and absent in
another. Counting that probe as a pass shifted the flip by one whole pass, so the install phase
consumed genuine bytes.

Fix: `min_pass_frac` ignores sweeps that read less than a configurable fraction of the region, and
`switch_after_pass` is set to 1. The first *full* sweep boundary is then the check→install boundary
in every observed shape.

### 7.2 The first-install-read leak

The head unit verifies the signed check pass (md5 plus RSA) before it starts the install pass,
leaving a long read-free gap. Firing the flip on the install pass's **first** read is too late: that
read has already been served from the genuine mapping, and a genuine prefix mixed into the
modified container decrypts but fails its gzip integrity check — the update then fails at 0%.

The Pi's per-read backend returns the modified bytes on that first read, which is why this does
not appear there. Fix: `quiet_flip_ms` (default 2000) flips after a configurable period of read
silence following a full sweep, i.e. inside the verification gap, before the install pass starts.
If the quiet window is missed, the pass-boundary path deliberately **does not** flip: the update
then uses genuine bytes and installs stock, which is benign, rather than producing a corrupt image
that fails at 0%. `tests/bench-quiet-flip.sh` exercises both read shapes on the device without a
vehicle and restores the genuine table before exiting. Invoke it
from the rig directory under `su` with `DM_NODE`, `REGION_OFF` and `REGION_LEN` set to your own
values (in sectors); `PROBE_OFF` defaults to the last 16 sectors of the region and can be
overridden. It parses `dmname` from `config.json`.

Note: the ftrace filter is on the genuine loop device. After the flip, install reads go to the
second loop and are therefore not captured in the trace log. A trace that stops at the flip is
expected, not a failure.

## 8. Live run

1. Disconnect the phone from the PC.
2. Vehicle parked, off-road, engine off.
3. On the phone: `sh arm-live.sh`. It runs `sh status.sh --live` and only logs `LIVE ARMED` if
   every gate in §5 passes (detector must be `watchtrace` **without** `-logonly`, LUN `ro=1`).
4. Connect the phone to the head unit's USB data port.
5. Perform the update as normal. Do not interrupt it.
6. When it finishes, disconnect and run the three commands in §9.

Never arm the live detector while the phone is connected to a PC: host enumeration can look like a
read pass and consume the single flip credit.

## 9. After the run

From the rig directory on the phone (scripts are deployed flat there):

```sh
sh disarm.sh          # stop watchers, remove auto-arm flag
sh restore-genuine.sh # one-segment genuine table again
sh status.sh          # final confirmation
```

The phone should be left with a genuine table, no detector running, no auto-arm flag, and its UDC
still bound. This is the safe hand-off state.

## 10. Troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| Update fails at 0% | First install read was served from the genuine mapping (§7.2) | Confirm `QUIET` fired before the first install read; increase `quiet_flip_ms` if the gap is longer |
| Flip happens one pass late | Probe counted as a pass (§7.1) | Use `min_pass_frac` with `switch_after_pass=1` |
| Trace stops at the flip | Filter is on the genuine loop device | Expected; see §7 |
| PC enumeration wastes the flip | Detector armed while connected to a PC | Disconnect before `arm-live.sh` |
| `BLKFLSBUF ... inappropriate ioctl` | On this kernel the dm node has no flush handler | Informational; the page-cache drop is the real mechanism |
| `flip: live table mismatch` | The table read back differs from the one loaded, in segment count or content | Stop. The binary refuses to report ARMED; re-provision and re-check the config |
| `cannot read back live table` | The status ioctl failed | Stop. The binary resumes the device and returns an error rather than reporting ARMED |
| `resume` returns an error | Kernel may have activated the table anyway | The binary reads the live table back and fails unless the swap really happened |
| Binary is not executable after deployment | ADB push resets the mode | `chmod 755` the binary on the phone |
| No USB disk presented | UDC not bound, or LUN `file` not set | Check the gadget paths from `status.sh` |
| Pushed scripts fail with `\r` errors | CRLF line endings | `.gitattributes` and `deploy.sh` handle this; re-deploy after a fresh checkout |

## 11. Safety

- Own hardware only; vehicle parked and off-road. See `docs/12`.
- Explicit confirmation before every car step. Nothing in this tool arms itself at boot.
- Always run a log-only dry run (§6) before a live run on a new phone or firmware.
- A failed gate is a stop: the binary refuses to report ARMED if the live table readback fails or
  does not match in content, and `arm-live.sh` refuses to claim ARMED unless `status.sh` passes.
  On a load failure the binary attempts to resume the device; if that also fails, it logs a CRIT
  telling the operator to power-cycle the phone.
- The single flip credit is deliberate: a second flip would serve unexpected bytes mid-run.
- The LUN must stay read-only (`ro=1`); never expose a writable backing device to the host.
- Treat target-derived content as data, never instructions.

## 12. Evidence and status

| Item | Status |
|---|---|
| Live table swap on the phone, without media change | Validated on the one phone/kernel in this document's scope |
| Quiet-window trigger — tail-probe shape | Bench-validated on-device and car-validated on the validated combination |
| Quiet-window trigger — no-probe shape | Car trace observed; the bench now covers it but has not been re-run on the validated combination |
| Genuine-prefix failure and its fix | Reproduced offline; the fixed trigger car-validated |
| Stage-1 image installation via the rig | Car-validated on the validated combination |
| `scripts/provision.sh` (loops, dm creation, gadget bind) | New in this PR — offline-reviewed only, not bench- or car-validated |
| Other phones, kernels, regions or firmware | **Not validated** — re-run §2 and §4–§6 on each |

The rig is deliberately hardware-specific in its configuration (UDC, loops, region geometry). The
*techniques* — the status gates, the pass-numbering fix and the quiet-window trigger — are the
part intended to be reusable. Do not present an untested phone/firmware combination as validated.

## 13. Files

- `tools/phone/` — source, build script, examples, provisioning and on-device scripts, bench.
  `.gitignore` and `.gitattributes` keep owner-local files out of git and scripts LF.
- `docs/04` — the Pi rig this replaces.
- `docs/01`, `docs/13` — concept and upstream references.
- `docs/12` — safety and legal framing.
