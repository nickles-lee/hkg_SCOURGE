# 11 — Troubleshooting

Collected from every failure actually hit during development. TARGET:
Ioniq 5 Gen5W_L, 260128 (EU) unless noted.

## Build

| Symptom | Cause / fix |
|---|---|
| `integrity_valid: false` on a freshly built container | Inspected under the wrong basename. The TE2 IV derives from the basename — inspect/decrypt under a `mango-rootfs.tar.gz` symlink, never as `stage1.te2`. (docs/02) |
| `over the ceiling by N bytes` | The additions must compress under 879,109,759 B. Check what you added; libdeflate L10 is already the tightest known. |
| `unexpected AppUpgrade bytes at <off>` | The stock binary is not the 260128 build — the locks gate is working as designed. Do not patch; go to docs/09. |
| `QTranslator failed` / `translation not found` | PyQt5 missing (optional check) vs. a wrong stock tar or an already-patched `.qm`. Rebuild from the genuine plaintext. |
| `new message too long` | The boot message exceeds 206 chars (the shorter `.qm` key). Shorten it. |
| Patch script aborts on sha256 mismatch | The ELF-hash rule. Never bypass; re-derive (docs/10) or fix the input staging (docs/03). |
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
| Update fails at 0% | A signature gate ran before the install. Known gates: the four engine validators (baked into stage-1) and the button gate. Collect evidence (`tools/update-debug/both_copies.sh`) instead of retrying blind. |
| Update rejected as unrecognizable | `.lge.upgrade.xml` was modified — it must stay byte-identical to stock. |
| HU does not mount the stick | Try bare exFAT on the whole device (no partition table) — that matches the proven TOCTOU image layout. |
| Boot message unchanged after flash | The displayed `.qm` may be the `new_gui` partition copy (a separate, higher-risk variant, intentionally not built). The failsafe and stubs are unaffected. |
| Watcher log stops early | The watcher is a `nohup` process: a head-unit reboot kills it. Do not reboot between starting it and the update attempt. |

## Packages

| Symptom | Cause / fix |
|---|---|
| `run_stuff.sh` does not run | Not at the stick root; wrong filesystem; or the loader already ran this exact content (md5 gate) — change the file to re-run. Check `/rw_data/initial_root.log`. |
| A daemon/installer "silently" never finds the USB stick | **fstype mismatch:** the car's exFAT is Tuxera `texfat.ko` and `/proc/mounts` reports `texfat`, not `exfat`. Code matching only `vfat\|exfat\|ntfs` rejects the real mount — and the failure is silent on automatic paths. Accept `texfat` (docs/02), or match by content. Both shipped daemons were fixed for this; the package tests now assert it. |
| A UI button/action does nothing after an install | The running UI is the old in-memory QML until the owning process restarts — the installer's restart step (AppLauncher terminate/launch) must have actually run; check the installer's `restart=` result line before debugging the QML itself. |
| Installer aborts on a QML hash | Base detection: the car's QML is neither the stock reference nor a known install state. Register the new base (docs/09) or restore stock first. |
| A binary runs on the host but not on the head unit | Toolchain class mismatch — a dynamic binary on the vendor's glibc 2.25. Check the static gate (`file` must say statically linked; `tweaks_persist` is the deliberate exception). (docs/07) |
| Custom tile disappeared | A firmware update reverted `/app` (expected) or the user switched to the Basic home type — ship a Basic-home tile too (docs/ui, Mode H). |
| Overlay clicks "fall through" | Missing input shield / foreground gating — follow `docs/ui/APP_OVERLAY_CHECKLIST.md` exactly. |
| Steering-wheel Home does not close an overlay | Expected without the daemon `home_seq` watcher — QML cannot see the session bus. See the overlay checklist. |
| `tweaks_persist` fails to start | It links against vendor glibc 2.25 + PCL 7.1.1 — rebuild against the correct extracted rootfs (`SCOURGE_ROOTFS`), and check `pers_admin_svc` is running. |
| SSH connects but auth fails | Default is `car`/`electric` port 2222; if the port was busy the daemon moved to 2223+ (the UI shows the actual port). |

## Verification

| Symptom | Cause / fix |
|---|---|
| `PREFLIGHT: OK` but a hash differs from docs/03 | The known-good table covers the validated message/config; a deliberately changed `config/message.txt` changes the rootfs hashes by design — re-record them. |
| `PACKAGE VERIFY: FAIL` on extra files | Only `BUILD.txt` and `MANIFEST.sha256` may be extras in a package tree. |
| Manifest verifies locally, fails on the stick | Truncated copy — re-copy and re-verify on the stick. |
