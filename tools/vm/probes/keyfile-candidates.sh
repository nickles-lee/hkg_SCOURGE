#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Tests explicit key-file hypotheses against the real vendor decryptor in QEMU.
# No package validation is bypassed, and no update installer is launched.
set -eu
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
mkdir -p /media/tools /tmp/keyfile-probes
found=0
for attempt in {1..20}; do
  for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/tools 2>/dev/null || continue
    if [ -f /media/tools/AppUpgrade.usb ]; then found=1; break; fi
    umount /media/tools
  done
  [ "$found" = 1 ] && break
  sleep 1
done
[ "$found" = 1 ]
if [ "${KEYFILE_ENTROPY:-0}" = 1 ]; then
  echo 'KEYFILE_ENTROPY_SOURCE virtual-cpu-rdrand'
  /media/tools/entropy-seed --rdrand
fi
export LD_LIBRARY_PATH=/app/lib:/usr/lib
dd if=/media/tools/AppUpgrade.usb of=/tmp/keyfile-probes/metadata-tail bs=1 skip=36 count=16384 2>/dev/null
dd if=/dev/zero of=/tmp/keyfile-probes/zeros bs=16384 count=1 2>/dev/null
matches=0
for candidate in ${KEYFILE_CANDIDATES:-metadata-tail zeros}; do
  echo "KEYFILE_CANDIDATE $candidate"
  # Vendor bash treats an empty array under set -u as unbound. Keep the command
  # array nonempty even when tracing is disabled.
  decrypt=(/app/share/AppUpgrade/DecryptToPIPE -d "/tmp/keyfile-probes/$candidate" /media/tools/AppUpgrade.usb 0)
  if [ "${KEYFILE_TRACE:-0}" = 1 ]; then
    # Include getrandom and other syscalls: a selective I/O filter previously
    # hid the actual wait after the key file had been read.
    decrypt=(strace -f -s 60 -o "/tmp/keyfile-probes/$candidate.trace" "${decrypt[@]}")
  fi
  "${decrypt[@]}" \
    >/tmp/keyfile-probes/output 2>"/tmp/keyfile-probes/$candidate.log" &
  pid=$!
  for attempt in {1..15}; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo 'KEYFILE_DECRYPT_DEADLINE'
    # The vendor kernel lacks /proc/.../children. strace -f prefixes every
    # observed task with its PID, including the otherwise invisible child.
    observed_pids=$pid
    if [ "${KEYFILE_TRACE:-0}" = 1 ]; then
      while read -r traced_pid; do
        [[ "$traced_pid" =~ ^[0-9]+$ ]] && observed_pids="$traced_pid $observed_pids"
      done < <(awk '{print $1}' "/tmp/keyfile-probes/$candidate.trace" | sort -un)
    fi
    for observed in $observed_pids; do
      echo "KEYFILE_WAIT_PID $observed"
      cat "/proc/$observed/wchan" "/proc/$observed/syscall" 2>/dev/null || true
      [ "$observed" = "$pid" ] || kill -KILL "$observed" 2>/dev/null || true
    done
    kill -KILL "$pid" 2>/dev/null || true
  fi
  set +e
  wait "$pid"
  status=$?
  set -e
  echo "KEYFILE_DECRYPT_RC $status"
  echo -n 'KEYFILE_OUTPUT_PREFIX '
  od -An -N16 -tx1 /tmp/keyfile-probes/output
  wc -c /tmp/keyfile-probes/output
  sha256sum /tmp/keyfile-probes/output /app/bin/AppUpgrade
  head -50 "/tmp/keyfile-probes/$candidate.log"
  [ "${KEYFILE_TRACE:-0}" = 1 ] && tail -80 "/tmp/keyfile-probes/$candidate.trace"
  # USB AppUpgrade is Velix-packed and is not necessarily byte-identical to the
  # installed executable. Equality is only an extra observation, not our oracle.
  if cmp -s /tmp/keyfile-probes/output /app/bin/AppUpgrade; then echo 'KEYFILE_MATCHES_INSTALLED yes'; fi
  if grep -aq 'mbedtls_rsa_pkcs1_decrypt returned' /tmp/keyfile-probes/output; then
    echo -n 'KEYFILE_RSA_ERROR '
    grep -a 'mbedtls_rsa_pkcs1_decrypt returned' /tmp/keyfile-probes/output
  elif [ "$status" = 0 ] && [ "$(od -An -N4 -tx1 /tmp/keyfile-probes/output | tr -d ' \n')" = 7f454c46 ]; then
    echo 'KEYFILE_ELF_CANDIDATE validation-required'
    matches=$((matches+1))
  fi
done
if [ "$matches" = 0 ]; then
  echo 'HARNESS_BLOCKED no tested credential candidate produced an ELF updater without a reported RSA error'
  exit 77
fi
echo 'HARNESS_BLOCKED ELF candidate requires independently established expected bytes; no stock decryption verdict'
exit 77
