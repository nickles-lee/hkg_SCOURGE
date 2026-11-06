# 10 — Re-deriving binary patches (optional Ghidra runbook)

**Ghidra is NOT a build dependency.** The normal build (docs/03) needs
only bash + Python. This runbook applies only when a firmware's hashes do
not match `config/firmware_locks.env` (docs/09, Gate 3) and the stub
offsets must be re-derived. An objdump/radare2 fallback is included for
agents without Ghidra.

Tested with Ghidra 12.2 + Temurin JDK 25 (Ghidra 11.3+ with JDK 17–21 also
works). Install Ghidra per upstream; never vendor it.

## The patch sets this applies to

| Target | Stubs | Semantics |
|---|---|---|
| `/app/bin/AppUpgrade` | 4 (two TE2-metadata wrappers, one helper, one final hash/RSA verifier) | `31 C0 C3` = `xor eax,eax; ret` — these gates return **0 on success** |
| `/app/bin/AppScomoDM` | 2 (button-gate verifier + shared entry) | same |
| (future) `/app/bin/AppLauncher` | 2 (`.sign` readSection gate, navi CheckSign) | `B8 01 00 00 00 C3` = `mov eax,1; ret` — these gates return **true on success** (note the different polarity!) |

Always read the decompilation to confirm which polarity a gate uses before
choosing stub bytes.

## Step 1 — extract the plain binary

From the new firmware: decrypt the rootfs container (TE2 key,
`gen5w_decrypt.py decrypt` under the basename `mango-rootfs.tar.gz`) and
extract `./app/bin/AppUpgrade` (and `./app/bin/AppScomoDM`).

## Step 2 — carve the embedded engine

`/app/bin/AppUpgrade` is a host ELF with the AppScomoUpgrade engine (itself
an ELF) embedded — at file offset `0x7C64` on 260128.

```sh
python3 tools/carve_engine.py PLAIN_APPUPGRADE            # verify + dump
python3 tools/carve_engine.py PLAIN_APPUPGRADE --scan     # find the engine
python3 tools/carve_engine.py PLAIN_APPUPGRADE --out engine.elf
```

Engine-relative offsets map to file offsets by adding the embed base:
`0x7C64 + 0x1F5A00 = 0x1FD664 = 2,086,500` (the 260128 worked example).
AppScomoDM's first LOAD segment is identity-mapped (file offset == vaddr).

## Step 3 — headless Ghidra, anchor-based narrowing

```sh
$GHIDRA_INSTALL_DIR/support/analyzeHeadless /tmp/scourge_proj scourge \
    -import engine.elf -processor x86:LE:64:default -cspec gcc \
    -postScript tools/ghidra/scourge_anchors.py
```

The script (first-party, `tools/ghidra/scourge_anchors.py`):

1. Finds the **anchor strings** in the engine's `.rodata` — the shell
   command templates that identify the check and install passes:
   `ionice -c3 md5sum %1 | awk` (CHECK), `/app/share/AppUpgrade/
   DecryptToPIPE %1 | tar` and `dd if=%1` (INSTALL), plus the `PKCS#1`
   signature-verify marker.
2. Resolves xrefs to the functions that reference them.
3. Ranks candidates by the documented **call-convergence shape**: two
   TE2-metadata wrappers (they read the `0x4024` trailer first) that
   converge, through helpers, on one final hash/signature verifier.
4. Dumps the decompilation of every candidate + one-hop callers/callees
   into `anchor_report/`.

## Step 4 — confirm by reading

The script narrows; judgment stays with you. Read the decompilation and
confirm the four roles: the wrappers parse TE2 metadata then call the
helper; the helper calls the final verifier; the final verifier performs
the hash/RSA comparison. On 260128 the shape was:

```
FUN_002f5ba0 → metadata parse → FUN_002f5b60 → FUN_002f5a00 (final)
FUN_002f5c40 → metadata parse → FUN_002f5850 → FUN_002f5a00 (final)
```

Record for each confirmed function: name, engine offset, **file offset**
(+ embed base), and the 8 stock bytes at that offset.

## Step 5 — sanity gates before recording

- Each offset is a plausible function entry (prologue bytes: push
  rbp/rbx/r12 sequences).
- The stock bytes at every offset are unique enough to serve as a gate.
- The binary's size + full sha256 are recorded alongside (the ELF-hash
  rule, docs/09).

## Step 6 — record + rebuild

Write the new offsets/expected-bytes/hashes as a NEW
`config/firmware_locks.env` section, then rebuild via docs/03 — the
existing pipeline (`03_patch_payload.py`, guard, verifiers, preflight)
enforces the new gates mechanically. Bench-test before any car session.

## Data patches (no Ghidra needed)

- **Boot message:** parse the new firmware's
  `AppStandbyClock/localization/{ts,qm}/translation_en-*` for the
  `SAFETY_OBEY` keys; the message must fit the shortest translation
  (206 chars on 260128); the patch is length-preserving UTF-16BE with
  U+0020 padding.
- **`.ver` rows:** find rows by `mango-rootfs.tar.gz` / `AppUpgrade`
  filename; recompute CRCs with `tools/patch_ver*.py`.
- **QML bases:** register the new stock QML hashes in the package
  installers' base-detection tables.

## objdump/radare2 fallback

Once the anchor strings are located (strings/`grep -abo`), disassemble
around their xref sites: `objdump -d --start-address=… engine.elf` and
follow `call` targets. This is usually sufficient to confirm offsets once
the candidate set is small; the decompiler is what makes the initial
narrowing tractable, which is why Ghidra remains the recommended path.
