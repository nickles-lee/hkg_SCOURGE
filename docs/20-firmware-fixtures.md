# 20 — Firmware fixtures (official packages)

Read-only provenance for the official packages the current pins were checked
against. No OEM bytes are stored here — only sizes, hashes, header identity
and the row-level results needed to reproduce the audit.

Retrieved **2026-11-21** with **nudl 1.3.0** (pinned in
`config/third_party.lock`) from the OEM update servers, region `EU`.

## Captured packages

| Target | nudl `-b` / `-m` | Version | Package map | `.ver` size / sha256 |
|---|---|---|---|---|
| Ioniq 5 (MY21–24, Gen5W_L) | `hyundai` / `IONIQ5_EU` | `NE_E.EUR.S5W_L.001.001.260128` | `IONIQ5_EU.ver` | 2469 / `53da7ec0fbb3ec3a6876c98f3720588d8caf2ebf92561b1f96478e35e80ad7b3` |
| Kia EV6 (MY22–24, Gen5W_L) | `kia` / `EV6_MY_22_EU` | `CV_E.EUR.S5W_L.001.001.260128` | `EV6_MY_22_EU.ver` | 2577 / `8779b06c58c706e5100ddb418e58910f16216f6e7e98f4421811d9a12e7e00a6` |

Header lines (the two fields that move between server revisions are the
package-id counter and the identity fields):

```
+|21393|NE_E.EUR.S5W_L.001.001.260128|HM|IONIQ5_EU|101489|1
+|21392|CV_E.EUR.S5W_L.001.001.260128|KM|EV6_MY_22_EU|101556|1
```

Rows verify with `scripts/01b_audit_inputs.py --layout package --root
<download-dir>`; the EV6 package needs its fixture locks because the default
locks describe the Ioniq identity:

```sh
# Ioniq 5 (default config/firmware_locks.env)
scripts/01b_audit_inputs.py --layout package --root <ion5-download-dir>

# EV6 (fixture locks; payload pins are shared)
scripts/01b_audit_inputs.py --layout package --root <ev6-download-dir> \
    --locks config/fixtures/ev6-my22-eu-260128.env
```

The consumed rows are `341174|1243982759|879126179` (rootfs),
`341142|609028602|10803676` (wrapper), and `341175|-444740824|17670` (XML).

## Payload identity

Both packages ship the same payload bytes for the files SCOURGE touches:

| File | Size | sha256 |
|---|---|---|
| `HU/images/mango-rootfs.tar.gz` | 879,126,179 | `362d9e329d970f403df4e69fece976ed6b4aaa27537e27a99e96e7796b9ad491` |
| `AppUpgrade` (wrapper container) | 10,803,676 | `5355059b8793f90440bbddafcb11ac884374d6b98b95309ac693a46aa70c2eca` |
| `.lge.upgrade.xml` | 17,670 | `7b6f9817d0ac67c36237dd0a08c1c9c73404bfc013d7c8f53b009c982e086c0e` |

The rootfs container md5 is `7f510002ed35972bed8edd0479c91679`; the
normalised row lists (product name replaced by a placeholder) are identical
between the two packages. The EV6 package differs only in its header
identity and the product prefix on each row.

## Findings

- The current Ioniq 5 package map is **not** byte-identical to the
  `2026.01.28` USB-tree pin: the header's package-id is `101489` instead of
  `1489`, which changes the file size (2469 vs 2467) and sha256. **Every row
  is unchanged.** This is the official server repack that made a correct
  package look like a hash mismatch; `01b_audit_inputs.py` reports it as a
  warning.
- The EV6 MY22–24 260128 package is byte-identical to the Ioniq 5 260128
  payload (same wrapper, rootfs and signed XML; same patch table applies).
  EV6 target wiring and an on-car build are **WIP** — this fixture does not
  claim EV6 support. The 2025+ EV6 (`CV1…ccNC`) is a different platform.
- The older wrong lock pin `VER_ROOTFS_ROW_STOCK_CRC=1744474344` was the
  CRC32 of the *initial-root* modified container, not the stock rootfs. The
  stock value is `1243982759`.

## Reproduce

```sh
nudl download -b hyundai -m IONIQ5_EU -r EU \
    -v NE_E.EUR.S5W_L.001.001.260128 -o <download-dir>
nudl verify -d <download-dir>
scripts/01b_audit_inputs.py --layout package --root <download-dir>

nudl download -b kia -m EV6_MY_22_EU -r EU \
    -v CV_E.EUR.S5W_L.001.001.260128 -o <download-dir>
nudl verify -d <download-dir>
scripts/01b_audit_inputs.py --layout package --root <download-dir> \
    --locks config/fixtures/ev6-my22-eu-260128.env
```

Keep the downloaded packages outside the repository (they are OEM material).
The fixtures above are the only facts that belong in git.
