#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Compile every guest helper from guest/*.c. Never downloads dependencies or
# runs firmware. Outputs:
#   build/helpers/*          compiled canonical copies
#   toolsroot/*              helpers shipped on the tools stick
#   guest/micom-dbus-sim     rootfs helpers used by build_rootfs.sh/phase3.sh
#   guest/usb-mount-signal
#   payload/tweaks-vm/tweaks-micom-can-sim
set -eu
cd "$(dirname "$0")"

DEV_ROOT=${DBUS_DEV_ROOT:-../project/vm/build/deb/root}
LIB="$DEV_ROOT/usr/lib/x86_64-linux-gnu/libdbus-1.a"
INC1="$DEV_ROOT/usr/include/dbus-1.0"
INC2="$DEV_ROOT/usr/lib/x86_64-linux-gnu/dbus-1.0/include"
[ -f "$LIB" ] || {
    echo 'Static libdbus development files missing; set DBUS_DEV_ROOT to an existing extracted development package.' >&2
    exit 1
}
[ -d "$INC1" ] && [ -d "$INC2" ] || {
    echo 'libdbus headers missing under DBUS_DEV_ROOT.' >&2
    exit 1
}

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -Wall -Wextra -Werror}
mkdir -p build/helpers

build_static() {
    name=$1
    shift
    "$CC" -static $CFLAGS "$@" \
        -I "$INC1" -I "$INC2" "$LIB" -lpthread -lrt \
        -o "build/helpers/$name"
    echo "built build/helpers/$name"
}

# CPU entropy helper: no D-Bus dependency.
"$CC" -static $CFLAGS -mrdrnd -o build/helpers/entropy-seed guest/entropy_seed.c
echo 'built build/helpers/entropy-seed'

# D-Bus helpers. systemd_stubs.c replaces the vendor socket-activation API.
for spec in \
    applauncher-stub:applauncher_stub.c \
    appmanager-stub:appmanager_stub.c \
    automount-stub:automount_stub.c \
    credman-stub:credman_stub.c \
    micom-dbus-sim:micom_dbus_sim.c \
    usb-mount-signal:usb_mount_signal.c \
    tweaks-micom-can-sim:tweaks_micom_can_sim.c
do
    name=${spec%%:*}
    source=${spec##*:}
    build_static "$name" "guest/$source" guest/systemd_stubs.c
done

# Tools-stick helpers.
for name in applauncher-stub appmanager-stub automount-stub credman-stub entropy-seed; do
    cp "build/helpers/$name" "toolsroot/$name"
done

# Rootfs helpers (build_rootfs.sh injects these into the vendor rootfs).
cp build/helpers/micom-dbus-sim guest/micom-dbus-sim
cp build/helpers/usb-mount-signal guest/usb-mount-signal

# Tweaks app payload.
mkdir -p payload/tweaks-vm
cp build/helpers/tweaks-micom-can-sim payload/tweaks-vm/tweaks-micom-can-sim

file build/helpers/* guest/micom-dbus-sim guest/usb-mount-signal \
     payload/tweaks-vm/tweaks-micom-can-sim
