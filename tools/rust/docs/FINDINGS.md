# Rust apps/daemons — findings

## Toolchain

- Pinned: Rust **1.100.0** (2026-11-12, commit
  `0ef1bc386059596dada8fb65b50425aab2ea637a`), host
  `x86_64-unknown-linux-gnu`, target `x86_64-unknown-linux-musl`.
- The official host toolchain requires glibc 2.17+ / kernel 3.2+; the
  Gen5W rootfs is glibc 2.25 / kernel 4.1.27, so the toolchain only runs on
  the builder. Nothing is installed on the unit.
- `rust-lld` from the same toolchain links the static-musl target; no
  `musl-gcc` is needed for pure-Rust crates. Host `cc` is used only for
  build scripts/proc macros.

## Size (release default, symbols kept)

Measured with `scourge-statusd` (Tokio + serde_json + anyhow):

| Artifact | Size |
|---|---:|
| `scourge-statusd` release default | **1,190,944 bytes (~1.14 MiB)** |
| static C `battery_daemon` stripped (existing package) | ~1.1 MiB |
| stripped Rust build (not used by default) | ~0.85 MiB |
| pinned toolchain (builder only) | ~825 MiB installed |

Static linking is not the size problem; unstripped debug builds are. The
deployed daemon is in the same range as the existing static C daemons.
Shared `libstd` (`libstd-<hash>.so`, 2.8 MiB, no stable ABI) was rejected:
it would require on-unit runtime/version management to save ~300 KiB per
app.

## VM evidence

Scenario `rust-app`, Mode V (vendor kernel `4.1.27-abl`), 1 vCPU / 1 GiB,
2026-11-20:

- Verdict **PASS**, `scenario_seconds` 8.5, all checks PASS:
  `rust-install`, `rust-static`, `rust-daemon`, `rust-status`,
  `rust-shutdown`, `rust-smack`, `rust-idempotent`, `rust-restore`.
- Installer result: `status=OK`, `action=installed`,
  `smack_check=ok`, `start=skipped:no-systemd`.
- `ldd` reports a static binary; `/proc/<pid>/maps` has no libc mapping.
- `/status` returns
  `{"app":"scourge-statusd",...,"smack":"_","state":"running","static":true,...}`.
- SIGTERM exits 0 and updates the status mirror to `stopped`.
- Reinstall is a no-op (`action=up-to-date`); restore removes the binary and
  unit but preserves `user-data.txt`.
- The sample daemon is started directly: the research boot has no systemd.
  The unit file is installed and checked, but the on-car systemd start is
  **not** exercised and remains car-pending.

## Notes

- The installer uses the generic `packages/common/install_io.sh` verified
  copy/remount helpers and the same SMACK exec-label pattern as Battery/WiFi.
- The app package's `manifest.md5` covers `payload/`; the loader md5-gates
  `run_stuff.sh` itself. A corrupt manifest stops before any write.
- The generic installer's root check has an `APP_TEST_UID` override for the
  offline sandbox test only; the real USB flow runs as root.
- The daemon status mirror is pretty-printed JSON, while the HTTP body is
  compact JSON; the VM probe accepts whitespace in the mirror.
