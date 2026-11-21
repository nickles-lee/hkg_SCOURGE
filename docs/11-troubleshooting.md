# 11 — Troubleshooting

Collected from every failure actually hit during development. TARGET:
Ioniq 5 Gen5W_L, 260128 (EU) unless noted.

## Build

| Symptom | Cause / fix |
|---|---|
| `integrity_valid: false` on a freshly built container | Inspected under the wrong basename. The TE2 IV derives from the basename — inspect/decrypt under a `mango-rootfs.tar.gz` symlink, never as `stage1.te2`. (docs/02) |
| `over the ceiling by N bytes` | The additions must compress under 879,109,759 B. Check what you added; libdeflate L10 is already the tightest known. |
| `unexpected AppUpgrade bytes at <off>` | The stock binary is not the 260128 build — the locks gate is working as designed. Do not patch; go to docs/09. |
| `QTranslator failed` / `key not found in .qm` | PyQt5 missing (optional check) vs. a wrong stock tar or a `.qm` without the SAFETY_OBEY keys. Rebuild from the genuine plaintext. |
| `message too long for <key>` | One locale's text no longer fits its stock `.qm` UTF-16 slot. The error names the key and the slot size; shorten that locale in `config/messages.json` (or use a shorter wording). |
| Patch script aborts on sha256 mismatch | The ELF-hash rule. Never bypass; re-derive (docs/10) or fix the input staging (docs/03). |
| `firmware audit: ... lock pin is stale/wrong` | A published pin disagrees with a package whose row and file agree. Fix the pin, not the download. The 2026-11 fix corrected `VER_ROOTFS_ROW_STOCK_CRC` (it held the initial-root container's CRC, `1744474344`, instead of the stock `1243982759`). Re-run `scripts/01b_audit_inputs.py`; it names the value to use. |
| `firmware audit: ver.sha ... but header identity and rows verify` | This is an official server repack: HMG changed the `.ver` header's package-id counter (e.g. `1489` → `101489`), so the file hash/size move while every row is identical. Warning only; continue. Do not "fix" the package. |
| `ERROR: missing input/mango-rootfs.tar.gz` after `01_stage_inputs.sh --download` | Older staging bug: `--download` wrote the package tree into `input/` without copying the four staged inputs. Current `--download` downloads into `input/package_stock/` and stages from there; re-run it. |
| `gzip -t` fails on the decrypted plaintext | Trailing 8 bytes were stripped from the genuine stream, or a non-gzip input. Re-decrypt from the staged container. |
| Build output does not match the known-good table | Stop. Compare inputs first (locks gates), then each intermediate (`work/` artifacts) against the docs/03 table. Do not deploy a mismatched build. |
| `ERROR: TE2 content key not set` | Run `scripts/00_fetch_third_party.sh`, or fill `config/key.local` (see `config/key.local.example`). |
| `xz`/`venv` errors in `00_env.sh` | Install python3-venv/python3-virtualenv via your package manager first. |

## Rig

| Symptom | Cause / fix |
|---|---|
| Pi reboots mid-update | The BCM2835 hardware watchdog was armed. It must be disabled on a **clean boot** (`RuntimeWatchdogUSec=0`); `daemon-reexec` does not disarm a live watchdog. (docs/04) |
| Head unit sees nothing on USB | Charge-only cable; or `dwc2` overlay missing / UDC not bound (`ls /sys/class/udc` must show `3f980000.usb` with the dwc2 driver). |
| 27 GB transfer would take ~44 h | That is the Pi's WiFi. Use `scripts/13_copy_image_to_sd.sh` (SD card in a host reader). |
| `nbdkit` plugin errors on the Pi | Re-run provisioning (`scripts/10_provision_pi.sh`); check `toctou.log` for the plugin's stderr. |

## Car update flow

| Symptom | Cause / fix |
|---|---|
| Update button grayed out | The button gate chain (docs/08): AppDMClient's `checkUSBUpdate()` verifies the drive wrapper + signed XML against `teruten5_pub.key`, and the stock AppLauncher refuses to spawn a modified AppDMClient. Confirm the `dmclient_gate` deployer ran all stages (launcher + dmclient + installed stubs) and the unit was **rebooted** afterward. |
| Update fails at 0% | A signature gate ran before the install. Known gates: the five engine validators (SCOMO four + FUMO, baked into stage-1) and the button gate. Collect evidence (`tools/update-debug/both_copies.sh`) instead of retrying blind. |
| Update rejected as unrecognizable | `.lge.upgrade.xml` was modified — it must stay byte-identical to stock. |
| HU does not mount the stick | Try bare exFAT on the whole device (no partition table) — that matches the proven TOCTOU image layout. |
| Boot message unchanged after flash | The displayed `.qm` may be the `new_gui` partition copy (a separate, higher-risk variant, intentionally not built). An SSH `.qm` refresh also needs a head-unit reboot before AppStandbyClock reloads the translations. The failsafe and stubs are unaffected. |
| Watcher log stops early | The watcher is a `nohup` process: a head-unit reboot kills it. Do not reboot between starting it and the update attempt. |
| `update_watch.log` is enormous (~100 MB) | Pre-fix watchers appended a full `ps -ef` every 2 s. Current `tools/update-debug/` deployers log transitions only and cap the file at 2 MiB; re-run the deployer to replace the installed watcher. |

## Packages

For BlueLink failures with working WiFi/SSH, use the
[BlueLink architecture and diagnostic guide](bluelink/architecture-and-diagnostics.md).
The [2026-11 MOF cache postmortem](bluelink/postmortem-2026-11-mof-cache.md)
documents a car-confirmed startup blocker and a reversible manual repair;
public-internet probe failures alone do not diagnose a SIM/subscription fault.

| Symptom | Cause / fix |
|---|---|
| `run_stuff.sh` does not run | Not at the stick root; wrong filesystem; or the loader already ran this exact content (md5 gate) — change the file to re-run. Check `/rw_data/initial_root.log`. |
| `/rw_data` fills up (97% seen: 58 wifi + 23 wifi1b snapshots = 152 MB) | The loader's md5 gate lives in `/run` (tmpfs), so every power-on re-runs a left-in stick, and each install used to add a full ~2-3 MB transaction snapshot with no retention. Current installers prune to the restore point plus two newest snapshots (`prune:` line in the install log) before creating a new one, and cap `daemon.log`/`wpa_supplicant.log` at 1 MiB each. Navigation databases were ~650 MB on one car; usage varies, so measure rather than delete them. See the tentative [baseline and read-only report](18-rw-data-baseline.md). Retention fixes are offline-validated; car validation pending. |
| A daemon/installer "silently" never finds the USB stick | **fstype mismatch:** the car's exFAT is Tuxera `texfat.ko` and `/proc/mounts` reports `texfat`, not `exfat`. Code matching only `vfat\|exfat\|ntfs` rejects the real mount — and the failure is silent on automatic paths. Accept `texfat` (docs/02), or match by content. Both shipped daemons were fixed for this; the package tests now assert it. |
| A UI button/action does nothing after an install | The running UI is the old in-memory QML until the owning process restarts — the installer's restart step (AppLauncher terminate/launch) must have actually run; check the installer's `restart=` result line before debugging the QML itself. |
| Installer aborts on a QML hash | Base detection: the car's QML is neither the stock reference nor a known install state. Register the new base (docs/09) or restore stock first. |
| A binary runs on the host but not on the head unit | Toolchain class mismatch — a dynamic binary on the vendor's glibc 2.25. Check the static gate (`file` must say statically linked; `tweaks_persist` is the deliberate exception). (docs/07) |
| Custom tile disappeared | A firmware update reverted `/app` (expected) or the user switched to the Basic home type — ship a Basic-home tile too (docs/ui, Mode H). |
| Overlay clicks "fall through" | Missing input shield / foreground gating — follow `docs/ui/APP_OVERLAY_CHECKLIST.md` exactly. |
| Steering-wheel Home does not close an overlay | Expected without the daemon `home_seq` watcher — QML cannot see the session bus. See the overlay checklist. |
| `tweaks_persist` fails to start | It links against vendor glibc 2.25 + PCL 7.1.1 — rebuild against the correct extracted rootfs (`SCOURGE_ROOTFS`), and check `pers_admin_svc` is running. |
| Front tint works but the instrument cluster never changes | The daemon must run on the SMACK floor label `_` (`chsmack -e _`), not `Privileged`: only the floor label has rwx to `App::micommanager` and the system-bus socket. Check `smack` in `/status`/`/health` (must be `_`) and `smack=` in `run_stuff_result.txt`; the diagnostic includes a micom D-Bus probe that writes no CAN (Introspect may activate the service). The Battery daemon is the car-proven example. |
| Reboot app says “service unavailable” | The Tweaks daemon is not reachable on `127.0.0.1:8091`, or the install failed its SMACK runtime gate (`smack_check` in `run_stuff_result.txt`). Check `systemctl status tweaks-daemon.service` and `/rw_data/tweaks/daemon.log`. |
| Reboot clean path does not restart the unit | After 20 s the daemon sends `SetETCCPUWDTReset`; check `reboot_wdt_sent`/`reboot_wdt_ok` in `/status` and `wdt_call.out`. If neither path works, the pinhole is the remaining reset. Do not test the fallback on a car without a deliberate plan. |
| Reboot refused during a flash update | Expected: `AppUpgrade`/`AppScomoDM` is running and `/reboot` returns `409 update_in_progress`. |
| SSH connects but auth fails | Default is `car`/`electric` port 2222; if the port was busy the daemon moved to 2223+ (the UI shows the actual port). |
| WiFi tether logs `unknown option [?][no]` / `[?][5180]` | Positional tether args. This vendor `connmanctl` takes options: `--hidden/--frequency/--maxsta/--appinfo 1` (`packages/wifi/SPEC.md`). |
| WiFi client: `Error '--wifi': Invalid argument`, then connmand `Not registered` | This build has no `connmanctl config --wifi`. The passphrase must go through the ConnMan agent flow (`agent on` → `connect <service>` → passphrase on stdin); no agent means `Not registered`. |
| WiFi tether: `Permission denied`; wpa control socket dead | SMACK label mismatch. Keep the daemon and everything it execs on the floor label `_` (`chsmack -e _`); `Privileged` has no ConnMan/wpa rule. Also **mask** `supplicantmanager.path`/`.service` so the OEM manager cannot respawn and reclaim `/var/run/wpa_supplicant/wlan0`. |
| WiFi daemon exits at start: `bind(http): Cannot assign requested address` | Loopback was not up yet. Current builds bring `lo` up and retry the bind for 30 s; verify with `ip link set lo up` and check `/proc/net/dev`. |
| Client failure reported as `SSH deferred: no WLAN IP` | Older build: `ssh_start()` overwrote the real association/DHCP error. Current builds save the error first — read the `client mode failed:` line. |

## Verification

| Symptom | Cause / fix |
|---|---|
| `PREFLIGHT: OK` but a hash differs from docs/03 | The known-good table covers the validated message/config; a deliberately changed `config/messages.json` changes the rootfs hashes by design — re-record them and re-validate the image. |
| `PACKAGE VERIFY: FAIL` on extra files | Only `BUILD.txt` and `MANIFEST.sha256` may be extras in a package tree. |
| Manifest verifies locally, fails on the stick | Truncated copy — re-copy and re-verify on the stick. |
| QML loads on the host but not on the car | Host Qt vs vendor Qt mismatch. Run the firmware's own `/usr/bin/qt5/qmllint` over every shipped QML file before deploying (docs/07, "Validating without the car"). |
| A command works with `--help` but fails on the car | The help text and the parser disagree on this vendor build. Execute the car binary with its own loader or in the VM and compare parser output (docs/07). |
