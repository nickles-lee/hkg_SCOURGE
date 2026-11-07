#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""SCOURGE repo policy gate: leaks, spdx, oem, refs, ignore, tests.

One stdlib-only checker for the AGENTS.md rules that are mechanically
checkable. `scripts/install_hooks.sh` wires it into git via
`core.hooksPath scripts/githooks`:

  pre-commit  every check except `tests`, over the staged set
  pre-push    every check except `tests` over the pushed range, then `tests`

Selection modes (apply to every check):

  (default)          full tracked tree
  --staged           the index (pre-commit)
  --range A..B       a commit range (pre-push)

Checks:

  leaks   No personal data in tracked content (AGENTS.md rule 2): absolute
          host paths, usernames, LAN details, key material. Deliberate
          product defaults are NOT secrets: the `car`/`electric` SSH
          credential and the fixed UIDs are intended — never remove them.
          The allowlists below encode exactly that.
  spdx    Every new first-party file carries the SPDX header, per the
          codified exception list (AGENTS.md, Repo conventions).
  oem     No OEM-derived file is added: path rules plus a sha256 match
          against the local `input/` tree when it exists. Scope is added
          files only; QML/image additions warn instead of blocking
          (grandfathered battery class, pending the un-publish decision).
  refs    Path-like string values inside tools/vm scenario/profile JSON
          must resolve in-tree (dangling NOTES.md-style pointers block).
  ignore  The gitignore rules AGENTS.md promises actually ignore.
  tests   Run the tools/vm unittest suite and compare failures against
          scripts/known_test_failures.txt: listed failures do not block,
          NEW failures block, stale entries warn (so the list cannot rot).
          Skipped tests are ignored. `all` does NOT include `tests`.

Exit codes: 0 = clean, 1 = findings, 2 = usage/environment error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# --- leaks --------------------------------------------------------------------
# Absolute /home/<seg>/ or /Users/<seg>/ paths are forbidden except these
# segments: target-device accounts (`root`, `car`), the vendor's own
# no-username device dir (`/rw_data/wifi/home/.ssh`), and image-URI
# internals (`image://AppHomeScreen/home/72x72/...`). Fixtures, not usernames.
HOME_ALLOW = {"root", "car", ".ssh", "72x72", "99x99", "bubble01", "menu"}
# Addresses that are protocol/virtualization/product constants, not LAN
# details: the QEMU virtio gateway/guest range, loopback, and the car-AP
# tether gateway the WiFi app documents (the laptop joined to the car's own
# access point normally gets 10.42.0.1; on-car ssh instructions use it).
LAN_ALLOW = ("10.0.2.", "127.0.0.1", "0.0.0.0", "10.42.0.")
# Whole-file leak-scan exemption: the gate names its own patterns (the
# username token inside NICK_RE, PEM headers) and holds no personal data —
# it cannot flag itself. Nothing else belongs here.
LEAKS_FILE_ALLOW = ("scripts/policy_check.py",)
# LAN-scan exemption: committed, documented placeholder addresses
# (config/paths.env.example carries the PI_HOST example). Other checks
# still scan these files.
LAN_FILE_ALLOW = ("config/paths.env.example",)
# Attribution (LICENSE, README) is intended; a bare username token is not.
# The attribution forms cannot match the username pattern (a letter
# follows the prefix), so only they need allowing.
NAME_ALLOW = ("Nick Lee",)
HOME_RE = re.compile(r"/(?:home|Users)/([A-Za-z0-9._-]+)[/\\\"'\s]")
LAN_RE = re.compile(
    r"\b(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b")
NICK_RE = re.compile(r"(?<![A-Za-z])nick(?![A-Za-z])", re.IGNORECASE)
PEM_RE = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")
SSH_RE = re.compile(
    r"\b(?:ssh-(?:rsa|ed25519|dss)|ecdsa-sha2-[a-z0-9-]+) [A-Za-z0-9+/]{50,}")
MAX_SCAN_BYTES = 4 * 1024 * 1024


def read_text(repo_path: str) -> str | None:
    """Tracked-file contents as text; None for binary/oversized/unreadable."""
    try:
        data = (REPO / repo_path).read_bytes()
    except OSError:
        return None
    if len(data) > MAX_SCAN_BYTES or b"\x00" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace")


def check_leaks(files: list[str]) -> list[str]:
    findings: list[str] = []
    for path in files:
        if path in LEAKS_FILE_ALLOW:
            continue
        text = read_text(path)
        if text is None:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if PEM_RE.search(line) or SSH_RE.search(line):
                findings.append(f"{path}:{i} — private key material")
            for seg in HOME_RE.findall(line):
                if seg not in HOME_ALLOW:
                    findings.append(
                        f"{path}:{i} — absolute path under /{seg}/ (personal)")
            if path not in LAN_FILE_ALLOW:
                for addr in LAN_RE.findall(line):
                    if not addr.startswith(LAN_ALLOW):
                        findings.append(f"{path}:{i} — LAN address {addr}")
            if NICK_RE.search(line) and not any(a in line for a in NAME_ALLOW):
                findings.append(f"{path}:{i} — 'nick' as a username/personal ref")
    return findings


# --- spdx ---------------------------------------------------------------------
# Extensions/names that must carry the header. Everything else is out of
# scope by construction (binary, or covered by the exception list below).
SPDX_SUFFIXES = {".sh", ".py", ".c", ".h", ".service", ".env", ".conf",
                 ".cfg", ".txt", ".lock"}
# Codified exceptions (AGENTS.md, Repo conventions): markdown is covered by
# the project-level notice and JSON has no comment syntax; QML/image package
# artifacts are generated by build_artifacts.py or byte-pinned OEM content;
# the marker files' bytes ARE the payload. NOTE: package scripts (.sh/.py)
# are NOT exempt — only their md/json/qml/image trees are.
def spdx_exempt(path: str) -> bool:
    lowered = path.lower()
    if lowered.startswith(("docs/", "payload/rootfs/")):
        return True
    if lowered.endswith((".md", ".json", ".png", ".gif", ".jpg", ".jpeg",
                         ".svg", ".ico", ".img", ".bin", ".pyc", ".qml")):
        return True
    return path in {"LICENSE", ".gitignore", "config/message.txt",
                    "packages/wifi/wifi.txt.sample",
                    "tools/vm/payload/PAYLOAD"}


def spdx_in_scope(path: str) -> bool:
    if spdx_exempt(path):
        return False
    name = path.rpartition("/")[2]
    if name == "Makefile" or name.startswith("Makefile."):
        return True
    suffix = ("." + name.rpartition(".")[2]) if "." in name else ""
    return suffix in SPDX_SUFFIXES or suffix == ""  # extensionless #: init


SPDX_LINES = ("SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors",
              "SPDX-License-Identifier: AGPL-3.0-only")


def check_spdx(files: list[str]) -> list[str]:
    findings: list[str] = []
    for path in files:
        if not spdx_in_scope(path):
            continue
        text = read_text(path)
        if text is None:
            findings.append(f"{path} — in-scope file is not text (no SPDX check)")
            continue
        head = "\n".join(text.splitlines()[:10])
        missing = [line for line in SPDX_LINES if line not in head]
        if missing:
            findings.append(f"{path} — missing SPDX header "
                            f"({missing[0][:44]}…)")
    return findings


def spdx_fix(files: list[str]) -> list[str]:
    """Interactively prepend the header to staged files (sh/python style)."""
    fixed: list[str] = []
    for path in files:
        if not spdx_in_scope(path):
            continue
        p = REPO / path
        try:
            lines = p.read_text().splitlines(keepends=True)
        except OSError:
            continue
        head = "\n".join("".join(lines).splitlines()[:10])
        if all(line in head for line in SPDX_LINES):
            continue
        if (path.rpartition(".")[2] or "").lower() in ("c", "h"):
            print(f"policy_check: {path} — C files need the /* */ form; fix by hand")
            continue
        sys.stdout.write(f"policy_check spdx --fix: add header to {path}? [y/N] ")
        if sys.stdin.readline().strip().lower() not in ("y", "yes"):
            continue
        insert_at = 1 if lines and lines[0].startswith("#!") else 0
        header = f"# {SPDX_LINES[0]}\n# {SPDX_LINES[1]}\n"
        p.write_text("".join(lines[:insert_at]) + header
                     + "".join(lines[insert_at:]))
        sh_out = subprocess.run(["git", "-C", str(REPO), "add", "--", path],
                                 capture_output=True)
        if sh_out.returncode == 0:
            fixed.append(path)
    return fixed


# --- oem ----------------------------------------------------------------------
OEM_BLOCK_PREFIX = ("input/", "work/", "out/", "binroot/", "third_party/")
OEM_BLOCK_SUFFIX = (".te2", ".tar.gz")
OEM_BLOCK_NAME = {"usb.img"}


def sha256_of(path: Path) -> str | None:
    try:
        if not path.is_file() or path.is_symlink():
            return None
        if path.stat().st_size > 512 * 1024 * 1024:
            return None
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def input_tree_hashes() -> set[str] | None:
    """sha256 set of the local input/ tree, or None when not staged."""
    input_dir = REPO / "input"
    if not input_dir.is_dir():
        return None
    return {h for p in input_dir.rglob("*")
            if (h := sha256_of(p)) is not None}


def check_oem(added: list[str]) -> list[str]:
    findings: list[str] = []
    hashes = input_tree_hashes()
    for path in added:
        lowered = path.lower()
        blocked = (any(lowered.startswith(p) for p in OEM_BLOCK_PREFIX)
                   or lowered.endswith(OEM_BLOCK_SUFFIX)
                   or lowered in OEM_BLOCK_NAME
                   or (lowered.startswith("packages/") and "/base/" in lowered))
        if blocked:
            findings.append(f"{path} — OEM-derived / never-commit file added")
            continue
        if lowered.startswith("packages/") and lowered.endswith((".qml", ".png")):
            findings.append(f"WARN {path} — QML/image artifact added "
                            "(confirm first-party delta, not OEM bytes)")
            continue
        if hashes is not None:
            digest = sha256_of(REPO / path)
            if digest and digest in hashes:
                findings.append(f"{path} — sha256 matches a file under input/ (OEM bytes)")
    return findings


# --- refs ---------------------------------------------------------------------
REFS_DIRS = ("tools/vm/scenarios", "tools/vm/profiles")
REFS_BASES = ("", "tools/vm/", "tools/vm/probes/")


def looks_like_path(value: str) -> bool:
    """Space-free, relative, non-URL string shaped like a file reference.
    Prose descriptions (they contain spaces) and absolute target-device
    paths (/lib/systemd/...) are skipped on purpose."""
    if not value or " " in value or value.startswith(("/", "http:", "https:")):
        return False
    return value.endswith(".md") or ("/" in value and "." in value)


def check_refs() -> list[str]:
    findings: list[str] = []
    targets: list[Path] = []
    for d in REFS_DIRS:
        targets.extend(sorted((REPO / d).glob("*.json")))
    for json_path in targets:
        rel = json_path.relative_to(REPO).as_posix()
        try:
            data = json.loads(json_path.read_text())
        except (OSError, ValueError) as exc:
            findings.append(f"{rel} — unparseable: {exc}")
            continue
        stack = [data]
        while stack:
            node = stack.pop()
            if isinstance(node, dict):
                stack.extend(node.values())
            elif isinstance(node, list):
                stack.extend(node)
            elif isinstance(node, str) and looks_like_path(node):
                target = node.partition("#")[0]
                if not any((REPO / base / target).exists() for base in REFS_BASES):
                    findings.append(f"{rel} — dangling path-like value '{node}'")
    return findings


# --- ignore -------------------------------------------------------------------
IGNORE_PROBES = (
    "packages/wifi/base/stock/probe.qml",
    "packages/tweaks/base/stock/probe.qml",
    "packages/battery/base/stock/probe.qml",
    "input/mango-rootfs.tar.gz",
    "work/rootfs/probe.img",
    "out/probe.img",
    "binroot/probe.img",
    "third_party/upstream/probe.py",
    "tools/vm/runs/probe/result.json",
    "config/key.local",
    "config/paths.local.env",
    "usb.img",
)


def check_ignore() -> list[str]:
    findings: list[str] = []
    for probe in IGNORE_PROBES:
        out = subprocess.run(["git", "-C", str(REPO), "check-ignore", "-q", probe],
                             capture_output=True)
        if out.returncode != 0:
            findings.append(f"gitignore does not cover {probe}")
    out = subprocess.run(
        ["git", "-C", str(REPO), "ls-files", "-i", "-c", "--stdin"],
        input="\n".join(tracked_files()), capture_output=True, text=True)
    if out.returncode == 0:
        for path in out.stdout.splitlines():
            if path:
                findings.append(f"{path} — tracked but gitignored")
    return findings


# --- selection ----------------------------------------------------------------

def git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(REPO), *args],
                          capture_output=True, text=True)


def tracked_files() -> list[str]:
    out = git("ls-files", "-z")
    if out.returncode != 0:
        raise SystemExit(2)
    return [p for p in out.stdout.split("\0") if p]


def changed_files(mode: str, range_str: str | None) -> dict[str, list[str]]:
    """{'A': added, 'M': modified} for staged/range modes; empty for full."""
    if mode == "staged":
        out = git("diff", "--cached", "--name-status")
    elif mode == "range":
        if not range_str or ".." not in range_str:
            raise SystemExit("policy_check: --range needs A..B")
        base, _, head = range_str.partition("..")
        out = git("diff", "--name-status", base.strip(), head.strip())
    else:
        return {"A": [], "M": []}
    if out.returncode != 0:
        raise SystemExit(f"policy_check: git diff failed: {out.stderr.strip()}")
    files: dict[str, list[str]] = {"A": [], "M": []}
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        status, _, rest = line.partition("\t")
        paths = rest.split("\t")
        if status.startswith("A"):
            files["A"].append(paths[-1])
        elif status.startswith(("M", "R", "C")):
            files["M"].append(paths[-1])
    return files


# --- tests gate ---------------------------------------------------------------

def load_known_failures() -> set[str]:
    known: set[str] = set()
    allow = REPO / "scripts/known_test_failures.txt"
    for line in allow.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            known.add(line.partition("::")[0].strip())
    return known


def check_tests() -> tuple[list[str], list[str]]:
    """(findings, warnings). Runs the suite the way its authors do:
    CWD=tools/vm (half the modules import harness.* with no sys.path fix)."""
    findings: list[str] = []
    warnings: list[str] = []
    vm = REPO / "tools/vm"
    try:
        proc = subprocess.run(
            [sys.executable, "-B", "-m", "unittest", "discover",
             "-s", "tests", "-q"],
            cwd=str(vm), capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return (["tools/vm suite did not finish within 300 s"], warnings)
    output = proc.stdout + proc.stderr
    if "Ran " not in output:
        return ([f"tools/vm suite did not run:\n{output.strip()[-1500:]}"],
                warnings)
    # unittest prints "FAIL: method (full.dotted.id)" — the paren content IS
    # the id on modern Pythons; older formats print module.Class there.
    failed: set[str] = set()
    for method, module in re.findall(r"^(?:FAIL|ERROR): (\S+) \((\S+)\)",
                                     output, re.MULTILINE):
        failed.add(module if module.endswith("." + method) else f"{module}.{method}")
    known = load_known_failures()
    findings = sorted(f"NEW TEST FAILURE: {t}" for t in failed - known)
    warnings = sorted(f"stale allowlist entry (now passing): {t}"
                      for t in known - failed)
    return findings, warnings


# --- main ---------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="SCOURGE repo policy gate (see AGENTS.md, Enforcement)")
    parser.add_argument("check",
                        choices=["leaks", "spdx", "oem", "refs", "ignore",
                                  "tests", "all"])
    parser.add_argument("--staged", action="store_true",
                        help="check the index (pre-commit)")
    parser.add_argument("--range", dest="range_str", metavar="A..B",
                        help="check a commit range (pre-push)")
    parser.add_argument("--fix", action="store_true",
                        help="spdx only: interactively prepend missing headers "
                             "to staged files")
    args = parser.parse_args()

    mode = "staged" if args.staged else ("range" if args.range_str else "full")
    changed = changed_files(mode, args.range_str)
    tracked = tracked_files()

    selected: list[tuple[str, list[str]]] = []
    if args.check in ("leaks", "all"):
        files = changed["A"] + changed["M"] if mode != "full" else tracked
        selected.append(("leaks", check_leaks(files)))
    if args.check in ("spdx", "all"):
        files = changed["A"] + changed["M"] if mode != "full" else tracked
        spdx_findings = check_spdx(files)
        if args.fix and mode == "staged" and spdx_findings:
            fixed = spdx_fix(files)
            print(f"policy_check: spdx --fix applied to {len(fixed)} file(s)")
            spdx_findings = check_spdx(
                [f for f in files if f not in fixed])
        selected.append(("spdx", spdx_findings))
    if args.check in ("oem", "all"):
        selected.append(("oem", check_oem(changed["A"])))
    if args.check in ("refs", "all"):
        selected.append(("refs", check_refs()))
    if args.check in ("ignore", "all"):
        selected.append(("ignore", check_ignore()))
    if args.check == "tests":
        findings, warnings = check_tests()
        for w in warnings:
            print(f"policy_check WARNING tests: {w}")
        selected.append(("tests", findings))

    hard: list[str] = []
    for name, findings in selected:
        for line in findings:
            if line.startswith("WARN "):
                print(f"policy_check WARNING {name}: {line[len('WARN '):]}")
            else:
                hard.append(f"{name}: {line}")
    for line in hard:
        print(f"policy_check {line}")
    if hard:
        print(f"policy_check: {len(hard)} finding(s) "
              f"[{args.check}, {mode}] — blocked by policy")
        return 1
    print(f"policy_check: clean [{args.check}, {mode}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())