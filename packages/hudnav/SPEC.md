# HUD Nav wire / lifecycle spec (WIP)

## Line protocol (interceptor → `hudnavd`)

```
HB                              heartbeat (phone silence at lights is OK)
TURN src=cp|aa man=<enum> [exit=<deg>] [raw=<n>] [side=<n>] [fourth=<n>]
DIST m=<meters> [disp=<n>] [unit=<0..4>]
DEST m=<meters>                 distance to destination (CarPlay TLV 0x07)
STATE s=0|1|2                   inactive|active|reroute
OWNER src=cp|aa|oem|none        log-only replay fixture
```

`man` uses `hn_maneuver` from `src/hudnav.h`; interposers normalize source enums via
`hn_normalize_aa` / `hn_normalize_cp`. Unknown keys are ignored; invalid values fall
back to defaults (same rule in `settings.c` and the Projection+ daemon).

## Switch files (`/rw_data/hudnav/`)

| File | Meaning | Reader | Absent means |
|---|---|---|---|
| `enabled` | master switch | boot script, `hudnavd` | clear HUD, send nothing |
| `aa-nav-enable` | Android Auto source | `hudnavd` | ignore AA input, clear if AA was showing |
| `cp-enabled` | CarPlay source | boot script, `hudnavd` | ignore CP input; CP session ends at next boot |
| `settings.conf` | per-source rounding / units | `hudnavd` (mtime/size re-read) | defaults |

`projection_daemon` owns these files; `hudnavd` only reads them. `hudnavd` writes
`/tmp/hudnav-status.json` (tmpfs) atomically on change and at least every 2 s while
running. Full contract: [`../projection/design/BACKEND_CONTRACT.md`](../projection/design/BACKEND_CONTRACT.md).

## Output gates

- `src/output_can.c` has a hard transmit whitelist: only the turn/route frames
  observed on the proven car can be emitted. Trap / speed-limit frames are never
  sent. `--micom-send` execs a whitelisted helper and requires `--appmanager-stdin`.
- Guidance ownership: AppManager broadcasts `110` = CarPlay, `111` = Android Auto,
  `0` = none (260319 values; re-derive on your firmware). `hudnavd` is silent while
  native navigation owns guidance and discards the old maneuver on owner change.
- Rounding and units are applied per source, not globally.

## Lifecycle / activation

1. Session-only default: a patched **copy** of the host binary gets an absolute
   `DT_NEEDED` for the shim and is bind-mounted over the stock path for one session.
   Reboot returns to stock with no persistent change.
2. The global `/etc/ld.so.preload` route works on the proven car but is banned as a
   persistent install (no overnight leave without the full guard + soak).
3. The optional persistent boot path (`persist/`) is **DISABLED by default**. It
   requires `install-persist.sh --yes`, an armed boot guard (`bootguard/`) and a
   settings-store guard (`storeguard/`), and must never precede the Phase-4 soak.
4. Every activation is hash-gated against the stock ELF pin in the selected profile.
   A mismatch, an `AMBIGUOUS` signature or an unexpected byte is a hard stop.
5. Revert: `recovery/ssh_revert.sh --yes` (one command, hash-verified), then a full
   cold boot to the home screen. The recovery stick is CRLF-proof.

The 64-byte `P` fingerprint and the three 16-byte call-site fingerprints in a
profile are fail-closed: a foreign build must re-derive them before use.
