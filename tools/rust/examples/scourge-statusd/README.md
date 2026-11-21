# scourge-statusd — sample Rust daemon

This is the proof app for the host-built Rust workflow in `docs/19-rust-apps.md`.
It is **not** a production SCOURGE app and is not installed by default.

It is a static-musl Tokio daemon that:

- serves `GET /status` on `127.0.0.1:18088` with JSON
- writes an atomic status mirror (default
  `/rw_data/apps/scourge-statusd/status.json`)
- reports its SMACK label and whether it is statically linked
- exits cleanly on SIGTERM/SIGINT

Build and package on the builder host:

```sh
bash scripts/00_fetch_rust.sh
bash tools/rust/scripts/package_app.sh \
    tools/rust/examples/scourge-statusd --fetch \
    --out third_party/rust/out/scourge-statusd-usb
```

Copy the resulting USB tree to a FAT/exFAT stick and plug it in; the
initial-root failsafe runs `run_stuff.sh` as root. Restore by copying
`run_stuff_restore.sh` over `run_stuff.sh`.

The head unit needs no Rust toolchain and no Rust runtime: the binary is
self-contained and statically linked against musl.
