# 19 — Rust apps and daemons (host-built, static musl)

TARGET: Ioniq 5 Gen5W_L (Linux), firmware 260128 (EU).

**Status: WIP.** The host build/package path is offline-validated; the
sample daemon is VM-validated on the vendor kernel. There is no on-car
validation yet, and the systemd start path is not exercised by the research
boot. Do not present this as a finished app platform.

## The model

Rust is a **builder-host** toolchain, not a head-unit runtime:

- `rustc`, `cargo` and a pinned std live under gitignored
  `third_party/rust/` on the builder.
- Cargo resolves and vendors crates on the builder.
- The unit receives a **self-contained static-musl binary** plus a systemd
  unit through the normal SCOURGE USB installer (docs/06).
- No Rust toolchain, no Cargo, and no shared Rust runtime are installed on
  the unit.

This matches the existing static-daemon policy (docs/07) and avoids a large
compiler on `/rw_data`. It also keeps crate `build.rs` code off the car.

## Why static musl

Rust has no stable ABI. A shared `libstd-<hash>.so` is tied to one exact
Rust build, so shared-libstd apps would need per-version runtime packages,
ABI checks and resolution on the unit. Static musl makes each app one file
with no runtime dependency. It also survives firmware/glibc changes.

The default build is **Cargo release with symbols kept** (the owner chooses
debuggability over a smaller file). Measured with the pinned 1.100.0
toolchain:

| Binary | Size |
|---|---:|
| `scourge-statusd` (Tokio + serde_json + anyhow), release default | ~1.2 MiB |
| same, stripped (not the default) | ~0.85 MiB |
| static C `battery_daemon`, stripped | ~1.1 MiB |
| pinned toolchain (builder only) | ~825 MiB installed |

A handful of Rust daemons is a few MiB on the unit, not a storage problem.
The toolchain stays on the builder.

Shared **libstd** is not supported. A per-app **dynamic glibc** build is a
future option for an app that must link vendor shared libraries or use
`dlopen`; it needs a glibc 2.25 sysroot and a GLIBC symbol gate, and is not
part of this workflow.

## Toolchain pin

`config/third_party.lock` `[rust-toolchain]` pins Rust 1.100.0
(2026-11-12), its commit, the host/target triples and the sha256 of the
`rustc`, `cargo`, `rust-std` (gnu + musl) dist tarballs. A hash mismatch is
a hard stop. `scripts/00_fetch_rust.sh` installs the toolchain; it never
uses rustup and never writes into the repo outside gitignored
`third_party/`.

The official host toolchain targets glibc 2.17+ / kernel 3.2+; the unit is
glibc 2.25 / kernel 4.1.27. The musl target is static and self-contained.

## Quick start

```sh
# once per builder checkout (network; ~160 MiB download)
bash scripts/00_fetch_rust.sh

# build + package the sample
bash tools/rust/scripts/package_app.sh \
    tools/rust/examples/scourge-statusd --fetch \
    --out third_party/rust/out/scourge-statusd-usb
```

Copy `third_party/rust/out/scourge-statusd-usb/` to a FAT/exFAT stick. The
initial-root failsafe runs `run_stuff.sh` as root; `run_stuff_result.txt`
reports the result. Restore by copying `run_stuff_restore.sh` over
`run_stuff.sh`.

To start a new project:

```sh
cp -R tools/rust/template ~/src/my-daemon
# edit Cargo.toml, scourge-app.env, src/main.rs, service/my-daemon.service
bash tools/rust/scripts/package_app.sh ~/src/my-daemon --fetch \
    --out third_party/rust/out/my-daemon-usb
```

## What `package_app.sh` does

1. Reads `scourge-app.env` (`APP_NAME`, `APP_VERSION`, `APP_BIN`,
   `APP_SERVICE`, `APP_DATA_DIR`, description).
2. Builds with the pinned Cargo:
   `--release --locked --offline --target x86_64-unknown-linux-musl`,
   using the toolchain's `rust-lld`; host `cc` is used only for build
   scripts and proc macros.
3. Requires the result to have no `PT_INTERP` and no `DT_NEEDED`; a dynamic
   binary is rejected.
4. Emits:
   - `run_stuff.sh` / `run_stuff_restore.sh`
   - `payload/bin/<app>` (static)
   - `payload/systemd/<app>.service`
   - `payload/app.env`, `payload/app.sha256`
   - `payload/lib/install_io.sh`
   - `manifest.md5`
   - `README.txt`

The generic installer verifies the manifest before writing, copies with a
verified read, installs the unit, labels the binary with the installer's
SMACK exec label (readback gate), is idempotent, and preserves app data on
restore unless `APP_PURGE=1`.

## Offline by default

`package_app.sh` builds with `--offline` from
`third_party/rust/vendor/<app>/`. `--fetch` is the explicit network step:
it runs `cargo vendor` and writes `Cargo.lock` if missing. Commit the
lockfile. Later builds need no network. The unit never fetches crates.

## Limitations

- Pure-Rust crates are the tested path. A crate that compiles C needs a musl
  C compiler (`musl-gcc` or a container); not covered by the sample.
- No Rust GUI/Qt apps; this kit targets headless daemons and CLI tools.
- No on-unit `cargo`/`rustc` (an optional on-device toolchain was
  prototyped separately but is not shipped).
- Systemd is not running in the research VM boot; the VM test starts the
  daemon directly. The unit is installed/checked, but the on-car systemd
  start remains pending.
- App data is preserved on restore; a firmware update can still replace
  `/etc` and `/app`. Re-run the installer after a firmware update if needed.

## Security

- `build.rs` and proc macros run on the **builder**, with the builder user's
  privileges, not on the car. Build only crates you trust.
- The unit installer writes only the app directory and its own unit; it does
  not patch stock binaries or touch the updater/bootloader/partitions.
- The binary and manifest are hash-gated before install. SMACK exec labels
  are read back and a mismatch aborts.

## VM validation

The `rust-app` scenario (Mode V, vendor kernel 4.1.27) attaches the
generated USB tree as `--payload` and checks:

- install (`status=OK`, SMACK readback), static linking (`ldd`)
- daemon start, HTTP `/status`, no libc mapping
- SMACK process/exec labels
- SIGTERM clean shutdown and status mirror
- idempotent reinstall (`action=up-to-date`)
- restore removes the binary/unit and preserves app data

Run:

```sh
cd tools/vm
python3 -B launch.py run --scenario rust-app \
    --payload ../../third_party/rust/out/scourge-statusd-usb \
    --timeout 300 --memory 1024 --smp 1 --label rust-app
```

The result is recorded in `tools/rust/docs/FINDINGS.md`. **2026-11-20:
PASS** on the vendor kernel at 1 vCPU / 1 GiB (all eight checks), with the
sample daemon started directly because the research boot has no systemd.
This is VM evidence only, not a car claim.
