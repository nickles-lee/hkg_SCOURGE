# tools/rust — host-built Rust apps/daemons

This is the **builder-side** Rust kit. The head unit never gets `rustc` or
`cargo`; it receives a self-contained static-musl binary plus a systemd
unit through the normal SCOURGE USB installer (docs/06).

Runbook: `docs/19-rust-apps.md`. Pins: `config/third_party.lock`
`[rust-toolchain]`.

## Quick start

```sh
# once per builder checkout (network; ~160 MiB download, ~825 MiB installed)
bash scripts/00_fetch_rust.sh

# build + package an app
bash tools/rust/scripts/package_app.sh tools/rust/examples/scourge-statusd \
    --fetch --out third_party/rust/out/scourge-statusd-usb
```

Copy `third_party/rust/out/scourge-statusd-usb/` to a FAT/exFAT stick.
The initial-root failsafe runs `run_stuff.sh`; `run_stuff_restore.sh`
removes the binary/unit while preserving app data.

## Layout

| Path | Purpose |
|---|---|
| `scripts/package_app.sh` | build a Cargo project, verify static, emit USB tree |
| `assets/install_app.sh` | generic installer (manifest, SMACK, idempotency, systemd) |
| `assets/restore_app.sh` | generic restore (data preserved unless `APP_PURGE=1`) |
| `template/` | minimal std-only daemon project |
| `examples/scourge-statusd/` | Tokio proof daemon with HTTP status/SIGTERM |
| `tests/` | offline installer/layout tests |
| `docs/FINDINGS.md` | size/linking evidence and VM results |

## Linking policy

- **Static musl** (`x86_64-unknown-linux-musl`) is the default and the only
  supported deployment form. One self-contained file; no runtime package.
- **Cargo release default** is used: symbols are kept for debugging. The
  sample daemon is ~1.2 MiB unstripped; stripping gets it to ~0.85 MiB.
- **Shared `libstd` is not supported**: Rust has no stable ABI, and the
  hashed `libstd-<hash>.so` would force on-unit runtime/version management.
- A future per-app dynamic-glibc mode for vendor libraries is possible, but
  it needs a glibc 2.25 sysroot and is out of scope here.

## Offline crates

`package_app.sh` builds with `--offline` against
`third_party/rust/vendor/<app>/`. `--fetch` is the explicit network step;
it runs `cargo vendor` and writes `Cargo.lock` if missing. Commit the
lockfile. Pure-Rust crates only; C-dependency crates need a musl C compiler
and are not covered by the sample.

## Tests

```sh
sh tools/rust/tests/test_installer.sh
sh tools/rust/tests/test_package_layout.sh
cd tools/vm && python3 -B -m unittest tests.test_rust_app -q
```
