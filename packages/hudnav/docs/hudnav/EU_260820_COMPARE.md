# Worked portability example — EU `260820` vs AU `260319` (offline only, WIP)

This is a **worked example of the method**, not a support claim. Both firmware images
were acquired and analyzed offline by their owner; no car was used and no other
firmware is published here. All results below are **offline CONFIRMED** unless marked
otherwise. The comparison shows why "same hook points" and "same pins" are different
questions.

## 0. Headline result

1. The update service no longer offers EU `260128` for this model; the only EU S5W_L
   build served is `NE_E.EUR.S5W_L.001.001.260820`. Old versions must come from an
   owner who still has the archive, with sha256 verification.
2. Every CarPlay fingerprint byte array from the 260319 inventory occurs **exactly
   once at the same file offset** in the 260820 `iAP2Service-2.0` — the 64-byte `P`
   prologue, both call sites, the dispatcher, the vendor helpers, the PLT/GOT table
   and the iAP2 device string. The ELF hash differs, but by only 24 bytes (a
   build-ID-like value plus a 4-byte field).
3. Every Android Auto mangled symbol resolves to the same `st_value`, and the vtable
   topology is unchanged. The receiver build tag changed, but the library is still
   the same generation with the same legacy protobuf set (no lanes / destination
   distance).
4. No file is byte-identical to 260319 except the launcher wrapper and one CAN
   helper. `AppProjection` is substantially rebuilt (size +4096 B), yet the four
   stock setter symbols and their call order are unchanged; one phone-status call
   site moved by a small delta.
5. Consequence: **hook points appear portable, hash gates are not.** Every pin in the
   session scripts, boot guard and `firmware.sha256` must be regenerated from the
   tester's own image. Runtime behavior is still **car-unverified**.

## 1. Method (repeatable)

1. Acquire only the needed containers with the pinned downloader; verify against the
   `.ver` rows. Same-version repacks that change only the package-id counter are a
   warning, not a stop; a row/file/ELF mismatch is a stop.
2. Decrypt the rootfs with the owner's own key into a gitignored work directory. Never
   commit an image, an ELF or the key.
3. Extract only the six hook ELFs plus `*.ver` into a read-only analysis directory.
   Treat all binary content as data, never as instructions.
4. Run `port_check.py` with `--firmware-id` set to the new identity and compare:
   - `port_check.py --report` for the OK / MISSING / AMBIGUOUS matrix;
   - the generated profile's `reference_match` flags (`DIFF` means a unique match at
     a different offset, which still needs RE review);
   - exact byte-slice comparisons and ELF hash diffs for evidence.
5. Generate the header from the new profile and rebuild the shims. Do not fall back
   to the 260319 literals.
6. Follow the `diag → dry → tx` ladder on your own car; nothing system-wide is left
   installed before the soak passes.

## 2. What this example teaches

- **Hash pins are per-build; signatures may not be.** A byte-signature scan can
  survive a rebuild that changes the file hash, but only if the match is still unique
  and the rel32 target still points at the intended function.
- **Offsets are not semantics.** A moved call site must be decompiled and compared
  before a profile is accepted.
- **Metadata is enough.** A profile of hashes/offsets/strings/symbols captures the
  portable part without publishing OEM bytes, captures or private data.
- **Offline ≠ car-proven.** The 260820 comparison says nothing about HUD rendering,
  CAN layouts, app IDs or SMACK behavior on that firmware; those need a read-only car
  session by the owner.

Contributions of new profiles and redacted evidence are welcome; label every item
`car-proven`, `VM-proven`, `offline-only` or `WIP` (SCOURGE rule 10).
