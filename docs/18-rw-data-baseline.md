# 18 — `/rw_data` reference and space triage

**Evidence status: tentative offline inventory of one owner-supplied, post-root
archive, not a verified stock baseline or a recovery image.** This sample's
device identity indicates Ioniq 5 EU; its update record indicates 260128.
Firmware, region, usage, updates and installed tools affect the contents. No
claim of car validation follows from the archive inspection.

## Provenance and limitations

The supplied `virgin_unmodified_rw_data.tar.gz` is 103,180,808 bytes; SHA-256
`bbfb6de1f3e1bfbf31aa72d8ec8bb965e129cb019b98f984ed75e03ea7fd495f`.
`gzip -t` reports CRC and length errors, and normal tar listing reports a bad
header. A best-effort scan found many individually checksum-valid tar headers,
but also unexpected shifts between entries. Header checksums do not verify file
contents. Completeness, contents, the cause of corruption, and capture-time
consistency are unknown. Do not use this archive for restoration or infer that
an unlisted file was absent. Obtain a fresh, independently verified copy.

The archive contains owner-install staging, WiFi overlays, backups, receipts and
diagnostic traces. It is **not unmodified**, regardless of its name. Transaction
records describe a failed, rolled-back hotfix and an active overlay. These
are observations of records, not proof of the car's present state. The archive
also contains personal information, including phone and navigation data. Keep
raw captures private and never commit them or OEM artifacts to the repository.

The sizes below are approximate **decimal MB of declared regular-file lengths**
from best-effort header scanning, not measured allocated blocks or trustworthy
totals. GNU long-name records and damaged data complicate parsing. No size is a
limit, deletion recommendation, or expected value for another car.

| Observed area | Approx. MB | Interpretation |
|---|---:|---|
| `var/` | 143 | Package database and transaction logs under `var/lib`; crash artifacts under `var/core`. Database logs are not necessarily disposable text logs. |
| `app_data/` | 71 | App state, databases, profiles and caches; `AppVH` accounts for roughly 54 MB in this sample. |
| `navi_data/` | 58 | Navigation and user data, including a roughly 50 MB charging-station database. Sizes vary substantially. |
| `home/` | 9 | Root user application state, including a media scanner database and WAL. |
| `ioniq-*` | 150 | Owner-tool staging, overlays, backups and diagnostic traces; not stock. |
| `dlt/`, `usr_data/`, `wifi/`, `mnt/` | small | Configuration, user state and directories; a small or apparently empty directory is not necessarily safe to remove. |

`/app/data` resolves to `/rw_data/app_data` on the documented target. Persistent
MOF, provisioning and user data can therefore survive a firmware reflash.
The [BlueLink investigation](bluelink/architecture-and-diagnostics.md) explains
the observed cache failure and its narrow, owner-approved repair. Never copy
another vehicle's MOF cache, encrypted configuration, keys, provisioning,
profiles or databases as a generic repair. File size alone does not prove a
cache is healthy or damaged.

## Investigating space use

For an owner's parked vehicle, with explicit approval before any car access,
copy `tools/rw_data_report.sh` to the head unit over the
[documented SSH channel](bluelink/architecture-and-diagnostics.md#access-keep-the-diagnostic-channel-independent)
and run `sh rw_data_report.sh` from the directory where you placed it **on the
head unit**, not on the workstation. The script has been checked offline but
is not car-validated. It reports filesystem space, inode use, mount information,
top-level allocated usage and large file paths.
It does not read file contents or delete anything. Output can reveal personal
filenames, mount details and device identifiers; review it before sharing.
Commands and options vary on the head unit; unavailable checks are reported and
the script exits nonzero if a check is unavailable or fails. The large-file
scan does not cross filesystem device boundaries. Top-level `du` checks size
each entry separately, so a top-level mount may be reported as its own tree.
`du` may differ
from `df` due to deleted-but-open files, reserved blocks, metadata or mounts.

Investigate growth over time, not only one snapshot. Check both `/rw_data` and
`/log_data`; the observed `dlt/log` symlink points at `/log_data`, so that
target's contents are not established by this archive. The sample DLT config
contains an offline-trace setting of 525,000,000 bytes and a core-dump setting
of 100,000,000 bytes; configuration is not proof of active enforcement, actual
usage, or other cars' settings. `/var/core` and custom application logs or
installer snapshots are worth measuring. Core dumps and logs may contain
private data and may be needed for diagnosis.

Do not delete package databases, SQLite WAL/SHM files, navigation data,
provisioning, or arbitrary caches based on size. Preserve evidence and a
verified **same-car** backup first; investigate the writer and specific recovery
procedure before changing anything. In particular, the sample does not prove
that deleting voice-update files, scanner state, cores or owner-tool backups is
safe. Never automate cache removal for a generic BlueLink failure. For the
documented full-filesystem incident and retention fixes see
[troubleshooting](11-troubleshooting.md) and the
[MOF postmortem](bluelink/postmortem-2026-11-mof-cache.md).

## Better future captures

Record model, region, installed firmware, capture date, root/tool history,
mount layout, free blocks and inodes, tool versions, exclusions and any errors.
Obtain a consistent snapshot or quiesce writers through an owner-approved,
device-specific procedure; a live tar can capture inconsistent databases. Do
not assume `sync` alone makes an active filesystem consistent. Preserve numeric
ownership, permissions, symlinks, hard links, ACLs and security xattrs where
supported, and explicitly record mount boundaries and separately captured
filesystems. Confirm actual tar capabilities first. Write to storage outside the
source filesystem, check every command's exit status, independently verify gzip
integrity and full tar listing, and compare the transferred archive's SHA-256
with the source. A passing archive check is not proof of a consistent capture or
successful restore. Test any restore only with a separately reviewed,
same-device recovery plan; metadata and security labels matter.
