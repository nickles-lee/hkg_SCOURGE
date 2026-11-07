# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Synthetic writable filesystems, not a recovered production eMMC layout."""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import uuid

from .images import reserve_outputs, validate_output

FEATURES = ('none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,'
            'flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize')
VOLUMES = {
    'update': {'mountpoint': '/update', 'size_mib': 512, 'directories': ['usb/bin', 'fota']},
    'rwdata': {'mountpoint': '/rw_data', 'size_mib': 256,
               'directories': ['app_data', 'usr_data', 'var/run/dbus', 'var/log', 'home',
                               'vr_data', 'mnt/root', 'dlt']},
    'logdata': {'mountpoint': '/log_data', 'size_mib': 128,
                'directories': ['AppUpgrade/fota']},
}


def parse_sizes(values: list[str]) -> dict[str, int]:
    sizes = {}
    for value in values:
        match = re.fullmatch(r'(update|rwdata|logdata)=([0-9]+)', value)
        if not match:
            raise ValueError(f'invalid storage size: {value}; use update|rwdata|logdata=MiB')
        name, amount = match.group(1), int(match.group(2))
        if name in sizes or not 16 <= amount <= 8192:
            raise ValueError('storage sizes must be unique and between 16 and 8192 MiB')
        sizes[name] = amount
    return sizes


def create_storage(run_dir: Path, sizes: dict[str, int] | None = None) -> dict:
    sizes = sizes or {}
    if set(sizes) - VOLUMES.keys() or any(type(n) is not int or not 16 <= n <= 8192 for n in sizes.values()):
        raise ValueError('invalid synthetic volume sizes')
    validate_output(run_dir / 'storage-build.log')
    nonce = uuid.uuid4().hex
    storage = {'mode': 'split', 'nonce': nonce, 'synthetic': True,
               'production_layout_verified': False, 'transport': 'QEMU USB mass storage',
               'seed': 'minimal harness directories, not stock rwdata/settings', 'volumes': {}}
    created = []
    try:
        with (run_dir / 'storage-build.log').open('w') as log:
            for name, spec in VOLUMES.items():
                seed = run_dir / f'storage-seed-{name}'
                seed.mkdir()
                for directory in spec['directories']:
                    (seed / directory).mkdir(parents=True, exist_ok=True)
                (seed / '.harness-volume').write_text(f'IONIQ_VOLUME_V1 {nonce} {name}\n')
                output = run_dir / f'storage-{name}.raw'
                reserve_outputs([output])
                created.append(output)
                size = sizes.get(name, spec['size_mib'])
                with output.open('r+b') as image:
                    image.truncate(size * 1024 ** 2)
                subprocess.run(['fakeroot', 'mkfs.ext4', '-F', '-b', '4096', '-I', '256',
                                '-L', f'HNS_{name}', '-O', FEATURES,
                                '-E', 'lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0',
                                '-d', str(seed), str(output)], check=True, stdout=log,
                               stderr=subprocess.STDOUT)
                storage['volumes'][name] = {'mountpoint': spec['mountpoint'], 'size_mib': size,
                                            'backing': str(output), 'overlay_name': f'storage_{name}',
                                            'filesystem': 'ext4', 'mount_options': 'rw,noatime'}
        (run_dir / 'storage.json').write_text(json.dumps(storage, indent=2) + '\n')
        return storage
    except BaseException:
        for output in created:
            output.unlink(missing_ok=True)
        raise


def cleanup_storage(run_dir: Path, storage: dict) -> None:
    for name, item in storage.get('volumes', {}).items():
        path = Path(item['backing'])
        if name not in VOLUMES or path != run_dir / f'storage-{name}.raw' or path.is_symlink():
            raise ValueError(f'refusing unexpected generated storage cleanup: {path}')
        path.unlink(missing_ok=True)


def mount_script(storage: dict, kernel_target: str = 'vendor') -> str:
    nonce = storage['nonce']
    if not re.fullmatch('[0-9a-f]{32}', nonce):
        raise ValueError('invalid storage nonce')
    bootstrap = ('' if kernel_target == 'compat'
                 else 'modprobe ehci-pci && modprobe usb-storage && modprobe sd_mod\n')
    return f'''set -eu
{bootstrap}mkdir -p /media/harness-scan /update /rw_data /log_data
update_dev= rwdata_dev= logdata_dev=
for attempt in {{1..20}}; do
  for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mounted=0
    while read -r source target fstype options rest; do
      if [ "$source" = "$dev" ]; then mounted=1; fi
    done </proc/mounts
    [ "$mounted" = 1 ] && continue
    mount -t ext4 -o ro "$dev" /media/harness-scan 2>/dev/null || continue
    magic= id= role= extra=
    if [ -f /media/harness-scan/.harness-volume ] && [ ! -L /media/harness-scan/.harness-volume ]; then
      read -r magic id role extra </media/harness-scan/.harness-volume || true
    fi
    umount /media/harness-scan || exit 1
    [ "$magic" = IONIQ_VOLUME_V1 ] && [ "$id" = {nonce} ] && [ -z "$extra" ] || continue
    case "$role" in
      update) target=/update; previous=$update_dev; update_dev=$dev ;;
      rwdata) target=/rw_data; previous=$rwdata_dev; rwdata_dev=$dev ;;
      logdata) target=/log_data; previous=$logdata_dev; logdata_dev=$dev ;;
      *) echo 'HARNESS_FAIL unexpected synthetic volume role'; exit 1 ;;
    esac
    [ -z "$previous" ] || {{ echo 'HARNESS_FAIL duplicate synthetic volume'; exit 1; }}
    mount -t ext4 -o rw,noatime "$dev" "$target" || exit 1
    echo "HARNESS_STORAGE $role $dev $target ext4 rw,noatime"
    echo "HARNESS_CHECK storage-$role PASS"
  done
  [ -n "$update_dev" ] && [ -n "$rwdata_dev" ] && [ -n "$logdata_dev" ] && break
  sleep 1
done
[ -n "$update_dev" ] && [ -n "$rwdata_dev" ] && [ -n "$logdata_dev" ] || {{
  echo 'HARNESS_FAIL missing synthetic storage volumes'; exit 1;
}}
[ "$update_dev" != "$rwdata_dev" ] && [ "$update_dev" != "$logdata_dev" ] && [ "$rwdata_dev" != "$logdata_dev" ]
echo 'HARNESS_SUBSTITUTION synthetic-storage'
'''


def probe_script(storage: dict) -> str:
    nonce = storage['nonce']
    if not re.fullmatch('[0-9a-f]{32}', nonce):
        raise ValueError('invalid storage nonce')
    # Only this dedicated scenario remounts volumes, before starting services.
    return f'''set -eu
update_dev= rwdata_dev= logdata_dev=
for role in update rwdata logdata; do
  case "$role" in update) point=/update ;; rwdata) point=/rw_data ;; logdata) point=/log_data ;; esac
  read -r magic id actual <"$point/.harness-volume"
  [ "$magic $id $actual" = "IONIQ_VOLUME_V1 {nonce} $role" ]
  found=0
  while read -r source target fstype options rest; do
    [ "$target" = "$point" ] || continue
    [ "$fstype" = ext4 ] && [[ ",$options," = *,rw,* ]] || exit 1
    case "$role" in update) update_dev=$source ;; rwdata) rwdata_dev=$source ;; logdata) logdata_dev=$source ;; esac
    found=1
  done </proc/mounts
  [ "$found" = 1 ]
  printf '%s\\n' "$role" >"$point/.harness-probe-{nonce}-$role"
done
[ "$update_dev" != "$rwdata_dev" ] && [ "$update_dev" != "$logdata_dev" ] && [ "$rwdata_dev" != "$logdata_dev" ]
for point in /update /rw_data /log_data; do
  count=0
  for file in "$point"/.harness-probe-{nonce}-*; do [ -f "$file" ] && count=$((count+1)); done
  [ "$count" = 1 ]
done
echo 'HARNESS_CHECK storage-isolation PASS'
sync
umount /log_data
mount -t ext4 -o rw,noatime "$logdata_dev" /log_data
[ "$(cat /log_data/.harness-probe-{nonce}-logdata)" = logdata ]
echo 'HARNESS_CHECK storage-remount-persistence PASS'
trap 'mount -o remount,rw /update' EXIT
mount -o remount,ro /update
readonly=0
while read -r source target fstype options rest; do
  if [ "$target" = /update ] && [[ ",$options," = *,ro,* ]]; then readonly=1; fi
done </proc/mounts
[ "$readonly" = 1 ]
if ( printf forbidden >/update/.harness-readonly-{nonce} ) 2>/dev/null; then
  echo 'HARNESS_FAIL read-only volume accepted a write'; exit 1
fi
mount -o remount,rw /update
trap - EXIT
echo 'HARNESS_CHECK storage-readonly PASS'
'''
