# scourge_anchors.py — Ghidra headless post-script for updater re-derivation.
#
# OPTIONAL PORTING AID (docs/10-ghidra-patch-derivation.md). The SCOURGE
# build never needs Ghidra; run this only when a firmware's AppUpgrade hash
# does NOT match config/firmware_locks.env and the validator offsets must be
# re-derived.
#
# Usage (after carving the engine with tools/carve_engine.py):
#   $GHIDRA_INSTALL_DIR/support/analyzeHeadless <proj_dir> scourge \
#       -import engine.elf -processor x86:LE:64:default -cspec gcc \
#       -postScript scourge_anchors.py
#
# Output: an anchor_report/ directory next to the Ghidra project containing
# the decompilation of every candidate function and its one-hop callers and
# callees.
#
# What it does:
#   1. finds the anchor strings in the engine's memory blocks (the shell
#      command templates the engine runs: the md5 CHECK pass and the
#      DecryptToPIPE/dd INSTALL pass, plus the PKCS#1 signature-verify
#      marker),
#   2. resolves xrefs to the functions that reference them,
#   3. ranks candidates by how many distinct anchors they touch (the
#      documented shape is: two TE2-metadata wrappers converging, through
#      helpers, on one final hash/signature verifier),
#   4. dumps the decompilation of every candidate plus one-hop callers and
#      callees for the agent to read and confirm.
#
# The script NARROWS the search; identification judgment stays with the
# agent + human. Record the confirmed offsets + stock prologue bytes as a
# NEW firmware_locks.env section — never overwrite the 260128 pins.
#
# SPDX-License-Identifier: AGPL-3.0-only
# @category: SCOURGE
import os

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = "anchor_report"
ANCHORS = [
    b"ionice -c3 md5sum",   # CHECK pass shell template
    b"DecryptToPIPE",       # INSTALL pass shell template
    b"PKCS#1",              # RSA signature-verify marker
    b"dd if=",              # alternate INSTALL template
    b"tar -xvm",            # tar extraction template
]


def dump_decomp(fn, decomp, outdir):
    res = decomp.decompileFunction(fn, 120, ConsoleTaskMonitor())
    if res.decompileCompleted():
        path = os.path.join(outdir, "%s.c" % fn.getName())
        with open(path, "w") as f:
            f.write(res.getDecompiledFunction().getC())
        return path
    return None


def run():
    outdir = OUT
    try:
        os.makedirs(outdir)
    except OSError:
        pass

    prog = currentProgram
    fm = prog.getFunctionManager()
    mem = prog.getMemory()
    ref_mgr = prog.getReferenceManager()

    # 1. anchor strings -> addresses
    found = {}
    for a in ANCHORS:
        hits = []
        for block in mem.getBlocks():
            try:
                raw = bytes(mem.getBytes(block.getStart(),
                                         block.getSize()))
            except Exception:
                continue
            i = 0
            while True:
                j = raw.find(a, i)
                if j < 0:
                    break
                hits.append(block.getStart().add(j))
                i = j + 1
        found[a] = hits
        print("anchor %-24r -> %d hit(s)" % (a, len(hits)))
        for h in hits:
            print("    %s" % h)

    # 2. xrefs -> candidate functions
    candidates = {}
    for a, hits in found.items():
        for h in hits:
            refs = list(ref_mgr.getReferencesTo(h))
            for ref in refs:
                fn = fm.getFunctionContaining(ref.getFromAddress())
                if fn is not None:
                    candidates.setdefault(fn, set()).add(a)

    # 3. rank and dump
    ranked = sorted(candidates.items(), key=lambda kv: -len(kv[1]))
    print("\n=== candidate functions ===")
    decomp = DecompInterface()
    decomp.openProgram(prog)
    seen = set()
    for fn, anchors in ranked:
        print("%-40s %s" % (fn.getName(),
                            sorted(x.decode() for x in anchors)))
        seen.add(fn.getEntryPoint().toString())
        dump_decomp(fn, decomp, outdir)
        for called in fn.getCalledFunctions(None):
            if called.getEntryPoint().toString() not in seen:
                seen.add(called.getEntryPoint().toString())
                dump_decomp(called, decomp, outdir)
        for caller in fn.getCallingFunctions(None):
            if caller.getEntryPoint().toString() not in seen:
                seen.add(caller.getEntryPoint().toString())
                dump_decomp(caller, decomp, outdir)

    print("\nreport written to %s/ — read the decompilation and confirm the "
          "four roles (two TE2-metadata wrappers -> helper(s) -> final "
          "verifier) before recording any offsets." % outdir)


run()
