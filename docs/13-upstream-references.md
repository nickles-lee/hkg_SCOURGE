# 13 — Upstream references

SCOURGE vendors no third-party code. The dependencies below are fetched
at setup time into `third_party/` (gitignored) by
`scripts/00_fetch_third_party.sh`, pinned per `config/third_party.lock`.
This index tells agents (and humans) what is worth reading in the fetched
material and where to find it, plus the wider ecosystem.

## The fetched TOCTOU repo (`third_party/ioniq5-usb-toctou/`)

Its documentation is the deep background for docs/01–05. Read it from the
local clone (paths below) or on GitHub.

| Need | Read |
|---|---|
| TOCTOU deep-dive + honest evidence grading | `docs/01-vulnerability.md`, `docs/03-evidence.md` |
| Rig architecture + nbdkit swap logic | `docs/02-attack-chain.md` |
| **Every rig bring-up bug and fix** (swap trigger, watchdog reboot, offline logging) — the single most valuable file when the rig misbehaves | `docs/04-field-journal.md` |
| Update-engine verification behavior + vendor-side fix patterns — needed to (a) understand why our package shapes work and (b) **recognize when a future firmware closes the TOCTOU door** (copy-then-verify = stop; see docs/09 Gate 2) | `docs/05-mitigations.md` |
| Owner Tools image design/build (the earlier payload; its docs/07 §3 is the original static-musl dropbear recipe that `packages/wifi/scripts/build_dropbear.sh` implements) | `docs/06-owner-menu-v2.md`, `docs/07-build-and-flash-runbook.md` |
| From-nothing rig reproduction | `REPRODUCE.md`, `pizero/DEPLOY.md` (re-arm, offline logging), `pizero/STATUS.md` |
| No-car race demo | `poc/run_poc.sh` |
| Rig deployment detail | `pizero/DEPLOY.md` |

**Boundary:** the upstream docs describe *their* payload ("Owner Tools"),
which differs from SCOURGE's stage-1. Our docs stay authoritative for
SCOURGE workflows; the upstream set is background/deep-dive only. Note
that the upstream repo grants no license — which is exactly why we fetch
it pinned and never copy its code into this repo.

## The fetched key source (`third_party/hkmcavn-tools/`)

GPL-2.0 public toolset for HMG/LGe head-unit firmware. `xcrypt/xcrypt.py`
is the canonical public reference for the TE2 container format and the
source `00_fetch_third_party.sh` extracts the content key from.

## The fetched downloader (`third_party/nudl/`)

GPL-3.0 unofficial client for the OEM's own update servers (same publicly
available firmware as the official Navigation Updater, no login).
`nudl list -b hyundai`, `nudl download -m <model> -r <region> -v <ver>`,
`nudl verify -d <dir>` (offline integrity check against the package's own
`.ver` checksums). Releases are SSH-signed; verify per the upstream
instructions.

## Ecosystem

| Resource | What |
|---|---|
| [JaredReabow/ioniq-5-Reverse-Engineering](https://github.com/JaredReabow/ioniq-5-Reverse-Engineering) | Foundational Ioniq 5 RE work (credited by the TOCTOU repo) |
| The Gen5W Discord | The community behind this work; SCOURGE has a channel there (see README) |
| Costantino, De Vincenzi, Matteucci — *A vehicle firmware security vulnerability: an IVI exploitation* (2024), DOI `10.1007/s11416-024-00522-4` | Peer-reviewed prior work on this platform family |
| [xwtk/hmg_nemesis](https://github.com/xwtk/hmg_nemesis) | The (unrelated, unaffiliated) Android-side counterpart for Gen5W S5W_M units; SCOURGE's README credits it as inspiration. Note: per their compatibility table, their approach does not apply to S5W_L or Ioniq 6 |
| galaxy4public/hkmcavn-tools, chenxiaolong/nudl | Fetched dependencies above |
| rgerganov/gen5fw, greenluigi1/HyundaiFirmwareDecrypter | Public HMG firmware tooling for *other* platform generations (Telechips Gen5, D-Audio2) — background only, not applicable to Gen5W_L |

## Takedown resilience

If an upstream disappears: keep local copies (the fetch script accepts
`--from-archive <path>` to seed `third_party/` from a saved clone or
tarball — maintainers should keep one). A failed pin is a hard stop;
restore from your archive or ask in the Discord. The
`config/third_party.lock` file records every pin and the sha256 of each
consumed file so restored copies can be verified.
