# 05 — The car run (TOCTOU deployment)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU). Read docs/12 first.

## Preconditions

- **Your own vehicle.** Parked, off-road, battery supported for a 1–1.5 h
  update. Nothing in this repo should ever touch a vehicle in service.
- The stage-1 build passed `09_preflight.sh` and every artifact matched the
  known-good table (docs/03).
- The rig is deployed and armed (docs/04), watchdog disabled, `toctou.log`
  tailing.
- A recovery plan exists: the A/B slot is the built-in fallback (the update
  writes the inactive slot), and the stock package + rig can always
  re-run. Keep a stock 260128 package tree available.

## The run

1. Power the Pi independently; connect its **data** port to the head
   unit's USB data port.
2. On the head unit: `Settings > General > Software Update > Update via
   USB`. Confirm the same-version reinstall prompt (version stays
   `NE_E.EUR.S5W_L.001.001.260128`).
3. Watch `toctou.log`:
   - `PASS #1 ... served=GENUINE` (the md5/RSA check reads the genuine
     container)
   - `*** SWITCH at pass #2 ***`
   - `PASS #2 ... served=EVIL` (the install reads the modified container)
4. Let the update finish completely. **Do not unplug the Pi, do not power
   off, do not interrupt.** The head unit reboots into the new rootfs.

## Post-boot verification

- The startup safety-disclaimer screen shows the boot message
  ("Good news everyone! We're rooted!...").
- Plug a FAT/exFAT stick with `payload/run_stuff.sample.sh` (renamed to
  `run_stuff.sh`) at its root: within ~8 s the failsafe runs it as root and
  writes `RUN_STUFF_OK.txt` back to the stick (uid=0 proves the root
  channel).
- `/rw_data/initial_root.log` shows the loader start and the run.
- The updater stubs are in place: the sample's `updater_guard` line, or
  `scripts/15_car_preflight.sh` from an update drive, reports `31c0c3` at
  all six offsets (four in AppUpgrade, two in AppScomoDM).

## Failure handling

| Symptom | Meaning / next step |
|---|---|
| No `PASS #1` in the log | The head unit did not read from the gadget — check the cable (data, not charge-only), the UDC bind, and that the update was started from USB. |
| `PASS #1` but no switch | The update never reached the install read; re-arm and retry. Do not power off mid-update. |
| Update fails mid-install | Let the A/B slot recover on reboot; the active slot is untouched. Re-run after diagnosing. |
| Boot message unchanged | The displayed `.qm` may be the copy inside `new_gui.tar.gz` (a separate partition copy); that variant is intentionally not part of the initial build. |
| Failsafe does not run the stick | Check the stick is FAT/exFAT at the root, `run_stuff.sh` at the root, and `/rw_data/initial_root.log`. |

## After a successful run

- The car is now rooted with the failsafe (docs/06) and the stubbed
  updater (docs/08).
- Install the app packages: `packages/compose.sh` (docs/07).
- If you no longer want the rig connected, tear it down on the Pi
  (`99_teardown.sh`); it is not needed after the first flash.
- Keep the rig and a stock package for recovery.

## Evidence worth keeping

Photos of the update screen at each stage, `toctou.log`, the stick's
`RUN_STUFF_OK.txt`, and `/rw_data/initial_root.log` — they are the record
that the deployment matched the documented flow.
