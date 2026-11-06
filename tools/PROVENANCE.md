# tools/ provenance

Everything in this directory is either first-party SCOURGE code (AGPL-3.0,
see ../LICENSE) or a factual record. No third-party code is redistributed
here — third-party dependencies are fetched at setup time by
`scripts/00_fetch_third_party.sh` per `config/third_party.lock`.

| File | Origin | Notes |
|---|---|---|
| `surgery.py` | First-party (state_freeze project) | Streaming tar-entry surgery + faithful gzip repack; replaces member payloads inside the decrypted rootfs tar.gz byte-preservingly |
| `patch_ver.py` | First-party (state_freeze project) | Patches the rootfs row (CRC32 + size) of `IONIQ5_EU.ver` |
| `patch_ver_row_id.py` | First-party (state_freeze project) | Patches any `.ver` row by row ID (CRC32 + size) |
| `carve_engine.py` | First-party (SCOURGE) | Carves + verifies the embedded AppScomoUpgrade engine ELF from a plain `AppUpgrade`; dumps prologue bytes at candidate patch offsets. Porting aid — see `docs/09`/`docs/10` |
| `ghidra/scourge_anchors.py` | First-party (SCOURGE) | Ghidra headless post-script: finds the verifier anchor strings, resolves xrefs, ranks candidate functions, dumps decompilation. Optional porting aid — Ghidra is NOT a build dependency |
| `update-debug/` | First-party (SCOURGE) | The `both_copies` watcher/wrapper-swap/diagnostics tooling for the WIP signatureless-update workflow — see `docs/08-postroot-updates.md` |

## Relationship to upstream community tooling

The TE2 container tools used by the build (`te2_encrypt.py`,
`gen5w_decrypt.py`, `chunkgz.py`, the nbdkit swap plugin and the Pi gadget
scripts) live in the fetched `third_party/ioniq5-usb-toctou` clone and are
invoked from there — they are not copied into this repo. The upstream
`xcrypt.py` (galaxy4public/hkmcavn-tools, GPL-2.0) is the public reference
for the TE2 format and the source of the content key; where SCOURGE needed
a patched variant of it, the delta ships as a patch file, never the whole
file. See `docs/13-upstream-references.md`.
