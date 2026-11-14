# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
Battery app for Ioniq 5 Gen5W (firmware 260128)
================================================

This USB stick installs a "Battery" app tile on the page-2 home screen and a
small background service that can start and stop battery preconditioning and
open or close the charge door.

Install
-------
1. Make sure the stick contains, at its root:
     run_stuff.sh
     payload/
     backup/
     backup_current/     (may be empty)
     manifest.md5
     README.txt
2. Plug the stick into the head unit USB port.
3. Wait about 15 seconds. The initial-root failsafe runs run_stuff.sh as root.
4. Remove the stick and read run_stuff_result.txt from its root.
   Expected: status=OK, restart=dbus-ok, daemon=systemd-ok,
   smack_self=_, daemon_smack=_, smack_check=ok, micom=CANService.
5. Swipe to page 2. Tap the "Battery" tile. The page opens as a full-screen
   overlay; use the Home button or the system back button to close it.

Restore
-------
1. Copy run_stuff_restore.sh over run_stuff.sh on the stick.
2. Plug the stick in and wait for run_stuff_result.txt.
   This restores the QML that was present before installation and removes the
   service.

Notes
-----
- If the "Hello" app is installed, the Battery installer keeps it and adds the
  Battery page after it.
- The installer refuses to run if the home-screen QML is not the 260128
  reference, a Hello-patched file, or a previous Battery install.
- Nothing is flashed. The updater, bootloader and partitions are untouched.
- The daemon listens only on 127.0.0.1:8088 and writes /rw_data/battery/.
- The daemon and its dbus-send/dbus-monitor helpers are stamped with the
  installer's own SMACK label (normally the floor label "_"). The install
  aborts if chsmack/getfattr cannot apply and read that label back.
- CAN frames go through /rw_data/battery/bin/micom-send, which runs a copy
  of the vendor dbus-send installed as CANService. The vendor micom only
  accepts sends from a short process whitelist; see docs/MICOM_WHITELIST.md
  on the stick and in the app package.
- On wide screens the overlay has a live Daemon/Install log console and a
  Dump button. The header "Save logs" button works on every layout.
- A log bundle is also written automatically on a micom/preheat/charge-door
  error, at most once per 10 minutes. Bundles are named battery_logs_<UTC>
  and only the newest 5 are kept.
- The battery temperature is read from 0x2FC byte 2 by default (byte 4 is
  available with --temp-byte 4 for a Gen2 car). A transient 0 is held at the
  last non-zero value.
- The charge-door toggle is greyed out only when a charging cable is observed.
  The gear/Park signal is advisory for now (0x036 is unreliable). It uses the
  factory micom D-Bus path.
