# Rust daemon template

Copy this directory to a new project, then edit at least:

- `Cargo.toml` (`name`, `version`, `description`, dependencies)
- `scourge-app.env` (`APP_NAME`, `APP_VERSION`, `APP_BIN`, `APP_SERVICE`,
  `APP_DATA_DIR`, description)
- `src/main.rs`
- `service/my-daemon.service` (rename it to `$APP_SERVICE` and fix
  `ExecStart`)

Build and package on the builder host:

```sh
bash scripts/00_fetch_rust.sh
bash tools/rust/scripts/package_app.sh path/to/project --fetch \
    --out third_party/rust/out/my-daemon-usb
```

The first `--fetch` resolves and vendors crates (network) and writes
`Cargo.lock`; commit the lockfile. Later builds are offline
(`cargo build --release --locked --offline`).

The deployed binary is static musl. The head unit needs no Rust toolchain
and no Rust runtime. See `docs/19-rust-apps.md`.
