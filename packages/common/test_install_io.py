#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fault-inject mount races, corrupt copies and icon failures without root."""
from pathlib import Path
import os
import subprocess
import tempfile

LIB = Path(__file__).with_name("install_io.sh")
FAKE = '''#!/usr/bin/python3
import os,sys,pathlib,json
p=pathlib.Path(os.environ['CASE'])
s=json.loads((p/'state').read_text())
name=pathlib.Path(sys.argv[0]).name
def save(): (p/'state').write_text(json.dumps(s))
def opts(value): (p/'mounts').write_text('device '+str(p/'root')+' ext4 '+value+' 0 0\\n')
if name=='mount':
 s['mounts']+=1
 if s.get('never_rw') and 'remount,rw' in sys.argv:
  save();sys.exit(1)
 opts('ro' if 'remount,ro' in sys.argv else 'rw')
 save();sys.exit(0)
if name=='sleep':
 if s.get('flaps',0)>0:
  s['flaps']-=1;opts('ro')
 save();sys.exit(0)
if name=='cp':
 s['copies']+=1;save()
 if s.get('always_fail'): print('Read-only file system',file=sys.stderr);sys.exit(1)
 if s['copies']==1 and s.get('copy_race'):
  opts('ro');print('Read-only file system',file=sys.stderr);sys.exit(1)
 if s['copies']==1 and s.get('corrupt'):
  pathlib.Path(sys.argv[-1]).write_text('truncated');sys.exit(0)
 os.execv('/usr/bin/cp',['cp',*sys.argv[1:]])
sys.exit(1)
'''


def case(flags, body, initial="ro", restricted_bin=None, md5_coreutils=False, real_cp=False):
    import json
    with tempfile.TemporaryDirectory(prefix="install-io-", dir="/tmp/opencode") as d:
        p = Path(d)
        (p / "root").mkdir()
        (p / "bin").mkdir()
        tool = p / "bin/tool"
        tool.write_text(FAKE)
        tool.chmod(0o755)
        if restricted_bin is None:
            for name in ("mount", "sleep", "cp"):
                if real_cp and name == "cp":
                    continue
                (p / "bin" / name).symlink_to(tool)
            search = str(p / "bin") + ":/usr/bin:/bin"
        else:
            # Firmware-like toolbox: md5sum exists only as md5sum.coreutils.
            # PATH deliberately excludes the host dirs so a bare `md5sum`
            # cannot resolve; every other helper the library needs is present.
            for name in ("awk", "cut", "ls", "rm", "tail", "stat", "basename", "dirname", "mktemp", "chmod", "chown", "cat"):
                (p / "bin" / name).symlink_to("/usr/bin/" + name)
            for name in ("mount", "sleep", "cp"):
                (p / "bin" / name).symlink_to(tool)
            if md5_coreutils:
                (p / "bin" / "md5sum.coreutils").symlink_to("/usr/bin/md5sum")
            search = str(p / "bin")
        (p / "source").write_text("expected bytes\n")
        (p / "target").write_text("old bytes\n")
        (p / "state").write_text(json.dumps(dict(mounts=0, copies=0, **flags)))
        (p / "mounts").write_text(f"device {p}/root ext4 {initial} 0 0\n")
        env = dict(os.environ, CASE=d, INSTALL_IO_ROOT=str(p / "root"),
                   INSTALL_IO_MOUNTS=str(p / "mounts"), PATH=search)
        script = f'''set -eu
LOG="$CASE/log"
log() {{ echo "$*" >> "$LOG"; }}
. "{LIB}"
io_init
trap io_restore_root EXIT
io_settle_root
{body}
'''
        subprocess.run(["/bin/sh", "-c", script], env=env, check=True, timeout=15)
        assert not list((p / "root").glob(".scourge-write.*"))
        assert f" {initial} " in (p / "mounts").read_text()
        # The temp dir is removed on return; capture everything needed first.
        return {"state": json.loads((p / "state").read_text()),
                "target": (p / "target").read_text()}


import json


def copies(result):
    return result["state"]["copies"]


for flags in ({"flaps": 3, "copy_race": True}, {"corrupt": True}):
    result = case(flags, 'io_copy "$CASE/source" "$CASE/target"\ncmp "$CASE/source" "$CASE/target"')
    assert copies(result) == 2
result = case({}, 'io_copy "$CASE/source" "$CASE/target"\nio_copy "$CASE/source" "$CASE/target"', "rw")
assert copies(result) == 1
case({"always_fail": True}, '''ASSET_WARNINGS=0
inode=$(stat -c%i "$CASE/target")
io_asset "$CASE/source" "$CASE/target"
[ "$ASSET_WARNINGS" = 1 ]
[ "$(stat -c%i "$CASE/target")" = "$inode" ]
if io_copy "$CASE/source" "$CASE/target"; then exit 1; fi
''')
print("PASS: mount flaps, mid-copy EROFS, corruption, idempotency, nonfatal icons, inode preservation, root cleanup")
try:
    case({"never_rw": True}, 'exit 0')
except subprocess.CalledProcessError:
    print("PASS: permanently read-only root fails closed within bounded attempts")
else:
    raise AssertionError("permanently read-only root accepted")

# Firmware toolbox without a bare md5sum: the helper must honor MD5SUM_BIN.
result = case({}, 'MD5SUM_BIN=md5sum.coreutils io_copy "$CASE/source" "$CASE/target"',
              restricted_bin=True, md5_coreutils=True)
assert result["target"] == "expected bytes\n"
try:
    case({}, 'io_copy "$CASE/source" "$CASE/target"', restricted_bin=True)
except subprocess.CalledProcessError:
    print("PASS: MD5SUM_BIN resolves the coreutils name; bare md5sum absence fails closed")
else:
    raise AssertionError("copy succeeded without any resolvable md5 tool")

# Real cp -f (no stub): the destination inode must survive the copy.
result = case({}, '''inode=$(stat -c%i "$CASE/target")
io_copy "$CASE/source" "$CASE/target"
[ "$(stat -c%i "$CASE/target")" = "$inode" ]
[ "$(cat "$CASE/target")" = "expected bytes" ]
''', "rw", real_cp=True)
assert result["target"] == "expected bytes\n"
print("PASS: real cp -f keeps the destination inode (SMACK attributes survive)")
