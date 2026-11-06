#!/bin/sh
# Re-apply the updater/launcher validator stubs. Idempotent.
#
# Four binaries:
#
#   /app/bin/AppUpgrade   (10,787,256 B) — installed updater engine
#     FUN_002f5a00  2086500  final hash/signature verifier
#     FUN_002f5b60  2086852  helper calling FUN_002f5a00
#     FUN_002f5ba0  2086916  TE2 verifier wrapper
#     FUN_002f5c40  2087076  second verifier wrapper
#
#   /app/bin/AppScomoDM   (1,934,752 B) — UsbUpdateManager
#     FUN_001e3430  930864   final verify
#     FUN_001e3470  930928   shared verifier entry
#
#   /app/bin/AppDMClient  (2,226,072 B) — checkUSBUpdate / button gate
#     FUN_00221000  1183744  final verify
#     FUN_00221040  1183808  shared verifier entry
#
#   /app/bin/AppLauncher  (407,720 B) — .sign gate for preloaded binaries
#     FUN_00115b50  88912    readSection gate
#     FUN_00112100  73984    navi CheckSign
#
# Updater/verifier gates: 31 C0 C3 = xor eax,eax; ret (return 0 = success).
# AppLauncher gates:      B8 01 00 00 00 C3 = mov eax,1; ret (true).
#
# AppLauncher and AppDMClient are running at boot, so they are patched with
# an atomic rename: a copy is patched and moved over the target. The running
# process keeps the old inode; the next boot sees the new file.
F=/app/bin/AppUpgrade
SCDM=/app/bin/AppScomoDM
DM=/app/bin/AppDMClient
L=/app/bin/AppLauncher
F_SIZE=10787256
SCDM_SIZE=1934752
DM_SIZE=2226072
L_SIZE=407720

patch_one() {
    file=$1
    off=$2
    name=$3
    expect=$4
    stub_hex=$5
    stub_octal=$6
    atomic=$7
    count=$(( ${#expect} / 2 ))
    cur=$(dd if="$file" bs=1 skip="$off" count="$count" 2>/dev/null \
          | od -An -tx1 | tr -d ' \n')
    case "$cur" in
        "$stub_hex"*) return 0 ;;
        "$expect") ;;
        *)
            echo "updater_guard: unexpected $name bytes at $off: $cur" >&2
            return 1
            ;;
    esac
    if [ "$atomic" = 1 ]; then
        work="$file.guard.$$"
        cp -a "$file" "$work" 2>/dev/null || return 1
        printf "$stub_octal" \
            | dd of="$work" bs=1 seek="$off" conv=notrunc 2>/dev/null
        mv -f "$work" "$file" 2>/dev/null || { rm -f "$work"; return 1; }
    else
        printf "$stub_octal" \
            | dd of="$file" bs=1 seek="$off" conv=notrunc 2>/dev/null
    fi
    sync
    return 0
}

# Rootfs may be mounted read-only on some boots.
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
[ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

if [ -f "$F" ] && [ "$(stat -c%s "$F" 2>/dev/null)" = "$F_SIZE" ]; then
    patch_one "$F" 2086500 FUN_002f5a00 415455534883ec50 31c0c3 '\061\300\303' 0
    patch_one "$F" 2086852 FUN_002f5b60 4883ec1864488b04 31c0c3 '\061\300\303' 0
    patch_one "$F" 2086916 FUN_002f5ba0 554889f531f65348 31c0c3 '\061\300\303' 0
    patch_one "$F" 2087076 FUN_002f5c40 554889f5534883ec 31c0c3 '\061\300\303' 0
fi

if [ -f "$SCDM" ] && [ "$(stat -c%s "$SCDM" 2>/dev/null)" = "$SCDM_SIZE" ]; then
    patch_one "$SCDM" 930864 FUN_001e3430 4883ec1864488b0425 31c0c3 '\061\300\303' 0
    patch_one "$SCDM" 930928 FUN_001e3470 554889f531f65348 31c0c3 '\061\300\303' 0
fi

if [ -f "$DM" ] && [ "$(stat -c%s "$DM" 2>/dev/null)" = "$DM_SIZE" ]; then
    patch_one "$DM" 1183744 FUN_00221000 4883ec1864488b0425 31c0c3 '\061\300\303' 1
    patch_one "$DM" 1183808 FUN_00221040 554889f531f65348 31c0c3 '\061\300\303' 1
fi

if [ -f "$L" ] && [ "$(stat -c%s "$L" 2>/dev/null)" = "$L_SIZE" ]; then
    patch_one "$L" 88912 FUN_00115b50 4157be1000000041 b801000000c3 '\270\001\000\000\000\303' 1
    patch_one "$L" 73984 FUN_00112100 4157415641554154 b801000000c3 '\270\001\000\000\000\303' 1
fi

sync
[ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
exit 0
