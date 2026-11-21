# 05 — The car run (TOCTOU deployment)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU). Read docs/12 first.

**Car-validated 2026-11-21 on a 2021 Ioniq 5 RWD** — the whole flow below, one
take: 34 min 21 s from the first read to `[UPDATE_DONE]`, the swap landed at
pass #2, and the installed rootfs was the patched one (docs/17 has the numbers).

## Preconditions

- **Your own vehicle.** Parked, off-road, battery supported for a 1–1.5 h
  update. Nothing in this repo should ever touch a vehicle in service.
- The stage-1 build passed `09_preflight.sh` and every artifact matched the
  known-good table (docs/03).
- The rig is deployed and armed (docs/04), watchdog disabled, `toctou.log`
  tailing. Expect the engine's md5 sweep to run at ~4.7 MB/s and the install at
  ~7.8 MB/s — the head unit's own pipeline, not the rig, sets both.
- A recovery plan exists: the A/B slot is the built-in fallback (the update
  writes the inactive slot), and the stock package + rig can always
  re-run. Keep a stock 260128 package tree available. The stock *reinstall*
  over the rig is `~/scourge/build/regions-stock.json` (`[]`) copied over
  `regions.json` — with a swap region in place even a stock image installs the
  envelope (docs/04, Images).

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

- The startup safety-disclaimer screen shows the boot message. Build 5
  patches only `en-gb`/`en-au` with the old text ("Good news everyone!...");
  the other 24 locales keep the OEM disclaimer. After the optional
  all-language refresh (docs/08) every locale shows "We're rooted." /
  "run_stuff.sh auto-runs from USB." / "Updater signature checks are
  disabled." / "No warranties provided."
- Plug a FAT/exFAT stick with `payload/run_stuff.sample.sh` (renamed to
  `run_stuff.sh`) at its root: within ~8 s the failsafe runs it as root and
  writes `RUN_STUFF_OK.txt` back to the stick (uid=0 proves the root
  channel).
- `/rw_data/initial_root.log` shows the loader start and the run.
- The updater stubs are in place: the sample's `updater_guard` line, or
  `scripts/15_car_preflight.sh` from an update drive, reports `31c0c3` at
  all seven offsets: five in `AppUpgrade` (2 086 500, 2 086 852, 2 086 916,
  2 087 076, 7 346 024) and two in `AppScomoDM` (930 864, 930 928). The
  preflight's own header still says "five stubs expected" for the engine —
  the 2026-11-21 car run matched that, not the older "four" in this list.

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

Everything the rig produced is on its SD card, so collecting it is one command on
the build host:

```sh
scripts/22_collect_rig_logs.sh                 # rig already on the known WiFi
scripts/22_collect_rig_logs.sh --host 10.42.0.1  # joined the rig's fallback AP
```

The head unit's half of the record is on the head unit. The updater's own log is
**`/log_data/AppUpgrade`**, on the 1.9 GB persistent ext4 `/log_data` partition
(703 MB used, 1.1 GB free as of the 2026-11-21 capture) — it survives power-off
and is superseded when the engine runs another update. The perishable one is
`/log_data/dltlog/*.dlt`: it rotates fast, so the boot window has to be taken
early (`docs/bluelink/architecture-and-diagnostics.md:140`).

`tools/update-debug/collect_logs.sh` is the collector for exactly that set:
copied to the drive it tars `/log_data/{AppUpgrade,dltlog,standarddlt,kernel}`,
`/rw_data/{second_stage_preflight.txt,initial_root.log,dlt}` and `/update` into
`scourge_logs.tar.gz` with a `scourge_logs_result.txt` beside it. Call it from
`run_stuff.sh` (`sh "$USB_DIR/collect_logs.sh"`) rather than installing it *as*
`run_stuff.sh`, or the restore and `updater_guard` steps of the failsafe never
run.

That drops `toctou.log`, `toctou-reads.log`, the gadget journal, a `rig-state.txt`
(uname, service state, UDC, the region config that was actually in force), the
page's own `/api/toctou` view, and prints the pass table — `served`, bytes,
duration and MB/s per pass — into `out/rig-logs/<UTC stamp>/`. The MB/s there is
the head unit's real read rate, the one number a dry run cannot give you.
