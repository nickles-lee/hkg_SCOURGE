# Wireless CarPlay car-session test plan

**Status:** executable plan for a parked, owner-authorized car session.
Nothing in this plan has been end-to-end validated. Read
[`VALIDATION.md`](VALIDATION.md) before claiming any result.
**Firmware:** `NE_E.EUR.S5W_L.001.001.260128`. **Branch:**
`carplay-vbtd51-broker`.

## 0. Rules

- Owner's own car, parked and off-road; owner present or reachable.
- Preserve USB SSH, BlueLink/modem routing, the AP, wired CarPlay and unknown
  owner state. Fail closed.
- Never fake MFi authentication, Bluetooth pairing, AirPlay pair-verify, AP
  credentials, receiver attestation or projection.
- No OEM binary is executed off-car. On-car execution is limited to the
  documented patched `vbtd` and the first-party helpers in Phase 3/4.
- Every deployment gets a stock backup, a hash check, and a rollback path
  before the first write.
- Keep phone MACs, AP PSKs, keys and personal data out of Git and out of chat;
  evidence goes under `/tmp/opencode/carplay-<date>/`.
- HCI snoop traces are raw Bluetooth traffic. Keep them outside the repo and
  out of shared chat; record only lengths, frame types, DLCIs and hashes.
  Never print or paste `pi`, `deviceid`, BSSIDs or Bluetooth addresses.
- A failed gate is a stop, not a workaround.

**Command context:** *host* commands run in the repo checkout; *car* commands
run on the head unit over SSH. `/tmp/opencode/carplay-<date>` is host evidence;
`/rw_data/carplay` is on-car backup/evidence. Build artifacts stay on the host
until a step explicitly copies them to the car.

## 1. Time boxes

| Window | Do | Do not |
|---|---|---|
| 15 min | Phase 0 (host preflight), Phase 1 (non-destructive snapshot + stock backup), Phase 2 (SPP registration + TX ladder steps 1-2) | Deploy or replace `vbtd` |
| 45 min | Above + Phase 2b (D1 receiver identity, stock only) and Phase 3 (P1 RX hook temporary deployment and RX capture) | Start broker auth/projection without Phase 3 passing |
| 90 min | Above + Phase 4 (broker/MFi), Phase 5 (AP/iAP2 bootstrap) | Patch AppProjection |
| 120 min | Above + Phase 6 (regression/rollback) | Leave an unvalidated patch installed |

If only 15 minutes are available, do **not** deploy. The live P2 probe is
read-only except for the listener and adapter discoverability; it is the
highest-value short session. D1 (Phase 2b) is the highest-value stock-only
probe and needs no listener at all; run it first when time allows.

## 2. Phase 0 — host preflight

Run from the repo checkout. Record every command/result in
`/tmp/opencode/carplay-<date>/host.log`.

```sh
DATE=$(date +%Y%m%d)                 # capture once; reuse in all phases
D=/tmp/opencode/carplay-$DATE
git status --short
git log --oneline -3
make -C packages/wifi1b/src carplay-broker
python3 packages/wifi1b/tests/test_carplay_broker_main.py
python3 packages/wifi1b/tests/test_carplay_usb.py
python3 packages/wifi1b/tests/test_carplay_services.py
SCOURGE_ROOTFS=/path/to/rootfs \
  python3 packages/wifi1b/tests/test_carplay_vbtd51_patch.py
SCOURGE_ROOTFS=/path/to/rootfs \
  python3 packages/wifi1b/tests/test_carplay_spp_patch.py
SCOURGE_ROOTFS=/path/to/rootfs \
  python3 packages/wifi1b/tests/test_carplay_broker_target.py
python3 scripts/policy_check.py all
```

Build the private candidate once. Record its payload path for the later
host steps; no separate `$D/build` is needed:

```sh
umask 077
mkdir -p "$D"
sh packages/wifi1b/scripts/build_carplay_candidate.sh /path/to/rootfs \
  > "$D/build.log" || exit 1
PAYLOAD=$(sed -n 's/^Private WIP candidate (not car-validated): //p' "$D/build.log")
[ -n "$PAYLOAD" ] && (cd "$PAYLOAD" && sha256sum -c SHA256SUMS) || exit 1
printf '%s\n' "$PAYLOAD" > "$D/payload-path"
```

The payload contains a firmware-derived artifact; keep it private. It does
not contain owner config, firmware libraries, or a Wifi1b daemon replacement.
It does not install or start services.

Expected current P1 output for the recorded toolchain: size `14117064`, SHA
`8691ad1468122c5f9f8c577dedb0f2185bb126b2f8afe8b94ceb44bcc095a9a9`.
The security gates are the input/hash/preimage/span checks, not a cross-
toolchain output hash. The target broker manifest must say
`EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED`, `car_validated:false`,
`installer_activated:false`, `oem_binary_executed:false`; record its SHA and
compare it with the deployed file before copying.

**Stop** if any hash/gate differs, any test fails, or the build manifest is not
the expected WIP status.

## 3. Phase 1 — non-destructive car snapshot and stock backup

Use the owner’s route. The product USB-ECM default is
`ssh -p 2224 wifi1b@10.55.0.2`; the Pi/Wi-Fi route is topology-specific.
Redact the AP PSK, head-unit Bluetooth MAC/CarPlay identity, and any client
SSID before saving logs.

```sh
DATE=$(date +%Y%m%d)                 # same date as Phase 0
D=/tmp/opencode/carplay-$DATE
mkdir -p "$D"
ssh -p 2224 wifi1b@10.55.0.2 '
  set -u
  redact() {
    sed -r \
      -e "s/\"(carplay_psk|carplay_btaddr|carplay_id|ssid|client_ssid|client_bssid|client_gateway|dhcp_bssid|ap_ipv6_error|imported_from|pair_message)\":\"[^\"]*\"/\"\1\":\"<redacted>\"/g" \
      -e "s/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/<redacted-mac>/g" "$1"
  }
  echo "=== date"; date -u
  echo "=== kernel"; uname -a
  echo "=== vbtd"; pidof vbtd; readlink -f /proc/$(pidof vbtd)/exe
  ls -l /app/bin/vbtd-51
  sha256sum /app/bin/vbtd-51
  echo "=== status"; redact /rw_data/wifi1b/status.json
  echo "=== bt state"; grep -E "^(discoverable|pairable)=" /rw_data/wifi1b/bt.state 2>/dev/null || true
  echo "=== session bus"; cat /tmp/session_appmgr.env
  echo "=== adapter"; A=$(cat /tmp/session_appmgr)
  busctl --address="$A" list --no-legend | grep -iE "bluez|SerialPort|Adapter" | head -60
  echo "=== ap clients"; grep -o "\"ap_clients\":[0-9]*" /rw_data/wifi1b/status.json
' | tee "$D/car-preflight.log"
```

Record:

- `vbtd` whole-file SHA and size; must match the pin.
- `bt_discoverable`/`bt_pairable` from `bt.state`; `pair_message` from the
  redacted `status.json`. Do not cat the whole `bt.state`; it includes the
  head-unit Bluetooth address.
- Phone connection state (media/HFP profiles present does not prove CAFF).
- The stock vendor log sink that carries `VBT`/`[BSA]` lines, or `none` if it
  cannot be identified read-only. TX ladder step 2 depends on it.
- Existing `org.bluez.SerialPort.SerialPort.v2_0_*` count (normally 0 before
  the listener).
- AP SSID/channel/security/link-local, `ap_clients`, `carplay_ready`.
  `carplay_session_observed` is derived from a fresh root-owned broker
  status (`running`, receiver observed, AP on air, peer, `rx_packets > 0`)
  and requires a joined client; that derivation is offline-tested only, so
  it is not pass evidence by itself. Use the broker’s session counters for
  the live session check.
- USB SSH works; BlueLink/modem state is unchanged.
- The owner’s local/serial recovery method, or explicitly `none`; if none and
  USB SSH is the only route, do not replace `vbtd`.
- Wired CarPlay baseline if a cable/phone is available.

Save the stock `vbtd` **and its SMACK labels** before any write. Run this
block **on the car**. `cp -p` does not preserve xattrs, so the label values are
stored separately and re-applied on restore:

```sh
mkdir -p /rw_data/carplay
cp -p /app/bin/vbtd-51 /rw_data/carplay/stock-vbtd-51
STOCK_WANT=55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0
[ "$(sha256sum /app/bin/vbtd-51 | awk '{print $1}')" = "$STOCK_WANT" ] || {
  echo "stock hash mismatch"; exit 1; }
[ "$(sha256sum /rw_data/carplay/stock-vbtd-51 | awk '{print $1}')" = \
  "$STOCK_WANT" ] || { echo "backup hash mismatch"; exit 1; }

# Read labels fail-closed. Distinguish "absent" from "present but unreadable".
ALL=$(getfattr -d -m - /app/bin/vbtd-51 2>/dev/null) || {
  echo "cannot read vbtd labels"; exit 1; }
printf '%s\n' "$ALL" | grep -q '^security.SMACK64=' || {
  echo "SMACK64 absent"; exit 1; }
if getfattr -n security.SMACK64 --only-values /app/bin/vbtd-51 \
     > /rw_data/carplay/stock-vbtd-51.SMACK64 2>/dev/null; then
  L=$(tr -d '\000\n' < /rw_data/carplay/stock-vbtd-51.SMACK64)
else
  echo "SMACK64 unreadable"; exit 1
fi
[ -n "$L" ] || { echo "SMACK64 empty"; exit 1; }

if getfattr -n security.SMACK64EXEC --only-values /app/bin/vbtd-51 \
     > /rw_data/carplay/stock-vbtd-51.SMACK64EXEC 2>/dev/null; then
  E=$(tr -d '\000\n' < /rw_data/carplay/stock-vbtd-51.SMACK64EXEC)
  [ -n "$E" ] || { echo "SMACK64EXEC empty"; exit 1; }
elif printf '%s\n' "$ALL" | grep -q '^security.SMACK64EXEC='; then
  echo "SMACK64EXEC present but unreadable"; exit 1
else
  : > /rw_data/carplay/stock-vbtd-51.SMACK64EXEC
fi
chmod 600 /rw_data/carplay/stock-vbtd-51.SMACK64 \
  /rw_data/carplay/stock-vbtd-51.SMACK64EXEC
sha256sum /app/bin/vbtd-51 /rw_data/carplay/stock-vbtd-51
```

Create a **self-contained** rollback script on the car before any replacement.
It does not depend on the Phase 3 shell surviving:

```sh
cat > /rw_data/carplay/rollback-vbtd.sh <<'EOS'
#!/bin/sh
set -eu
STOCK=/rw_data/carplay/stock-vbtd-51
STOCK_WANT=55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0
L=$(cat /rw_data/carplay/stock-vbtd-51.SMACK64)
[ -f /rw_data/carplay/stock-vbtd-51.SMACK64EXEC ] || {
  echo "missing saved SMACK64EXEC state"; exit 1; }
E=$(cat /rw_data/carplay/stock-vbtd-51.SMACK64EXEC)
[ -n "$L" ] || { echo "missing SMACK64"; exit 1; }
apply_labels() {
  target=$1
  if command -v chsmack >/dev/null 2>&1; then
    chsmack -a "$L" "$target" || return 1
    if [ -n "$E" ]; then chsmack -e "$E" "$target" || return 1; fi
  else
    setfattr -n security.SMACK64 -v "$L" "$target" || return 1
    if [ -n "$E" ]; then
      setfattr -n security.SMACK64EXEC -v "$E" "$target" || return 1
    fi
  fi
  RL=$(getfattr -n security.SMACK64 --only-values "$target" 2>/dev/null |
       tr -d '\000\n')
  [ "$RL" = "$L" ] || return 1
  if [ -n "$E" ]; then
    RE=$(getfattr -n security.SMACK64EXEC --only-values "$target" 2>/dev/null |
         tr -d '\000\n')
    [ "$RE" = "$E" ] || return 1
  fi
}
[ "$(sha256sum "$STOCK" | awk '{print $1}')" = "$STOCK_WANT" ] || {
  echo "stock hash mismatch"; exit 1; }
R=/app/bin/.vbtd-51.restore.$$
cp -p "$STOCK" "$R" && chmod 755 "$R" && chown 0:0 "$R" || {
  rm -f "$R"; exit 1; }
apply_labels "$R" || { rm -f "$R"; echo "label restore failed"; exit 1; }
sync
mv -f "$R" /app/bin/vbtd-51 || { rm -f "$R"; exit 1; }
systemctl reset-failed vbtd.service 2>/dev/null || true
systemctl restart vbtd.service
sleep 2
pidof vbtd >/dev/null || { echo "vbtd did not restart"; exit 1; }
[ "$(sha256sum /app/bin/vbtd-51 | awk '{print $1}')" = "$STOCK_WANT" ] || {
  echo "restored hash mismatch"; exit 1; }
echo "stock vbtd restored"
EOS
chmod 700 /rw_data/carplay/rollback-vbtd.sh
sh -n /rw_data/carplay/rollback-vbtd.sh || { echo "rollback syntax"; exit 1; }
```

If the stock copy hash differs from the pin, any label is missing/unreadable,
or the rollback script cannot be created, **stop**. Do not replace `vbtd`
without a self-contained, label-preserving restore path.

## 4. Phase 2 — SPP registration and TX ladder (no binary patch)

This repeats the 2026-11-18 registration probe; only TX ladder step 1 was
observed then. Do not run it concurrently with another listener/adapter
change.

### Capture prerequisite (for TX ladder step 3)

Step 3 needs a host-side HCI trace, and the snoop writer is configured at
daemon startup, not per probe:

- `BSA_Boot` ELF `0x84ffe0` calls `DispBtSnoopInit` ELF `0x8a2260`;
  `DispHciAclData` ELF `0x883ac0` runs inside the `bte_hcisu_send` ELF
  `0x84f7b0` path, before the queued send. These are inside `vbtd-51`, re-verified by `objdump` against the
  pinned whole-file SHA-256 `55e21d…cec9e0`; there is no separate per-function
  pin. No capture has been taken and no OEM code executed.
- `DispBtSnoopInit` opens with `O_CREAT|O_TRUNC` and exits the daemon if the
  open fails. The startup path can supply a filename (`-b`/snoop config); the
  in-binary pattern is `log_data/BT/vbt_snoop_<pid>.cfa`.

Check for an already-enabled trace (read-only):

```sh
ls -l /proc/$(pidof vbtd)/fd | grep -i 'cfa'
for d in /log_data /rw_data /tmp /var/log; do
  [ -d "$d" ] && find "$d" -maxdepth 3 -name 'vbt_snoop_*.cfa' -type f
done
```

Record how the daemon was started and where a filename could come from
(`tr '\0' ' ' < /proc/$(pidof vbtd)/cmdline; echo`, plus the `vbtd` unit's
snoop-related lines via `systemctl cat vbtd.service`). Do not edit the unit
without owner approval. Never assume a filename from the pattern alone.

If a trace exists, copy it to a root-only path under `/rw_data/carplay/`
**before** any restart (the next start truncates it), then record its size and
SHA-256. Use it only if it spans the phone connection and the `SendData`
call.

If no trace exists, do not enable snoop silently. Either run Phase 2 without
step 3 (`host emission: unrecorded`) or get explicit owner approval for one
controlled restart with the Phase 1 stock backup in place. Capture must start
before the phone connects, or the RFCOMM SABM/PN negotiation is missing. A bad
path exits the daemon, so a restart is a Bluetooth outage risk. A restart also
drops the current SPP connection: wait for `pidof vbtd`, then let the phone
reconnect while the trace is active.

### Registration probe

```sh
DATE=$(date +%Y%m%d)                 # same date as Phase 0
D=/tmp/opencode/carplay-$DATE
A=$(cat /tmp/session_appmgr)
busctl --address="$A" call \
  org.bluez.Adapter.Adapter.v2_0_lge1 /lge1 \
  org.bluez.Adapter.Adapter.v2_0 \
  ListenSPP ss "00000000-DECA-FADE-DECA-DEAFDECACAFF" "SCOURGE-iAP"
```

Expected: `i 0`. Then, while the phone is connected:

```sh
busctl --address="$A" list --no-legend | grep -i SerialPort
S=$(busctl --address="$A" list --no-legend | awk '/SerialPort/{print $1; exit}')
P=$(busctl --address="$A" tree "$S" | grep -o "/dev_.*serial_[0-9a-f_]*" | head -1)
busctl --address="$A" introspect "$S" "$P" org.bluez.SerialPort.SerialPort.v2_0
for m in getStatusAttribute getLocalUUIDAttribute getRemoteUUIDAttribute; do
  busctl --address="$A" call "$S" "$P" \
    org.bluez.SerialPort.SerialPort.v2_0 "$m"
done
```

Expected live evidence:

- Object path `.../serial_00000000_deca_fade_deca_deafdecacaff`.
- `status="connected"`.
- Local and remote UUID both CAFF.
- `SendData` is present with signature `ay -> i`.
- If a trace spans the connection: the header version/datalink, then RFCOMM
  SABM/PN before the probe. A listener object without a trace is still a
  registration-only result.

TX evidence ladder (one byte only, not a CAFF PDU). The call below is step 1:

```sh
busctl --address="$A" call "$S" "$P" \
  org.bluez.SerialPort.SerialPort.v2_0 SendData ay 1 0
```

1. **D-Bus ACK** — expected `i 0`. `StackSerialPortStub::SendData`
   (`0x612e50`) ignores the vendor result at `0x613011` and builds the reply
   at `0x613038`.
2. **Vendor acceptance** — read the stock log sink identified in Phase 1 for
   `VBTSppSendDataCfm` (`0x490b60`) at the time of the call. No sink means no
   step 2: record `vendor acceptance: unobserved`. Do not infer it from step
   1 or step 3.
3. **Host emission** — in the trace, locate the ACL stream for the connected
   phone after the call and require one outbound RFCOMM UIH on the SPP DLCI.
   Read the header before decoding:

   ```sh
   CAP=<path to the trace>
   wc -c < "$CAP"; sha256sum "$CAP"
   head -c 16 "$CAP" | od -An -tx1
   ```

   The first eight bytes are the btsnoop magic
   `62 74 73 6e 6f 6f 70 00`; record the 4-byte version and 4-byte datalink
   that follow. Do not assume H4 vs USB. Each record then has original length,
   included length, 8-byte flags, 8-byte cumulative drops and an 8-byte
   timestamp (big-endian); bit 0 of the flags is direction (0 = host to
   controller). Follow L2CAP signaling, then RFCOMM SABM/PN, then UIH. If a
   btsnoop-capable decoder is available, for example:

   ```sh
   command -v tshark >/dev/null && tshark -r "$CAP" -Y btrfcomm -T fields \
     -e frame.number -e btrfcomm.frame_type -e btrfcomm.dlci -e btrfcomm.length
   ```

   If no decoder is available, stop at the header/framing check and record
   `host emission: not decoded`; do not infer a UIH from hex. Decode on the
   car where possible; copying the trace to `$D` moves raw phone traffic
   off-car and needs owner approval.

   Pass: one TX UIH on the SPP DLCI after the call, recorded as frame type,
   DLCI and included length. Do not copy the payload into any log.
4. **Peer receipt** — out of scope for Phase 2. A bare `0x00` need not
   produce a reply; with an iPhone this needs the real iAP2 exchange
   (Phase 3/4). An owned cooperative SPP peer proves the stack path only.

"RFCOMM delivery to phone" stays `NOT VALIDATED` in
[`VALIDATION.md`](VALIDATION.md) until step 4 exists. Phase 2 is a
registration-only result unless step 3 is recorded; step 1 never passes the
TX gate on its own.

If no object appears:

- Confirm the phone is connected and known/paired.
- Check `bt_discoverable` / `bt_pairable`; the daemon’s `/carplay/pair` action
  may report “Bluetooth control unavailable.”
- Check SPP service `0xa` is enabled.
- Do **not** patch `SetSppListen`/`SetSppListenStatus`; they are state
  notification stubs, not registration gates.
- Record the negative result.

Stop the listener after the probe unless the next phase needs it:

```sh
busctl --address="$A" call org.bluez.Adapter.Adapter.v2_0_lge1 /lge1 \
  org.bluez.Adapter.Adapter.v2_0 StopListenSPP ss \
  "00000000-DECA-FADE-DECA-DEAFDECACAFF" "SCOURGE-iAP"
```

A connected SerialPort may persist until the phone disconnects; that is
expected and not a failed stop.

## 5. Phase 2b — D1 receiver identity on the AP (stock only)

Run before any projection work, before the broker, and before any persistent
change. It deploys nothing: no receiver restart, no `AppProjection` change,
no config write, no `vbtd` replacement. A missing field is `unobserved`,
never a pass. Stop conditions are in
[`DEFERRED-CAR-PROBES.md`](DEFERRED-CAR-PROBES.md) §4. This probe is
independent of Phase 2 and may be run before it; nothing in Phase 2 is a
prerequisite.

### Listener owner and scope on 7000

Port 7000 is `0x1B58`; LISTEN is state `0A`:

```sh
awk 'NR>1 && $4=="0A" { split($2,a,":"); if (toupper(a[2])=="1B58")
  print FILENAME, $2, "uid=" $8, "inode=" $10 }' /proc/net/tcp /proc/net/tcp6
```

The bind address must be the wildcard or the receiver interface address;
loopback-only is `listener-scope` and a stop. Resolve the inode to an owner
once, not per poll:

```sh
INODE=<inode from the line above>
for d in /proc/[0-9]*; do
  ls -l "$d/fd" 2>/dev/null | grep -q "socket:\[$INODE\]" || continue
  printf 'pid=%s comm=%s exe=%s\n' "${d#/proc/}" \
    "$(cat "$d/comm" 2>/dev/null)" "$(readlink "$d/exe" 2>/dev/null)"
done
```

Require an allowlisted stock owner (`AppProjection` by default). Unknown owner
is `listener-owner` and a stop.

### AP-side `_airplay._tcp` SRV/TXT

Browse from the AP side with a read-only client already present on the unit:
the target-built passive probe if an approved earlier step left one there, or
an existing browse tool. D1 installs nothing. If neither client exists, record
`DNS-SD: unobserved` and stop before projection work; do not install a browser
just for the probe.

Record the instance count, SRV port, `srcvers`, `deviceid_match`, and
`pi_sha256` + length. Hash `pi` in place and never print it (the snippet is
bash-only; `read -s` is not POSIX `sh`):

```sh
read -rs pi || { echo "pi input failed"; exit 1; }   # paste; no echo
printf '%s' "$pi" | sha256sum
unset pi
```

If `read -s` is unsupported (POSIX `sh` or busybox without it), hash on the
owner's device instead; do not echo the value or put it on a command line.

### Identity rule and pass/stop

Exactly one corroborated candidate:

- `deviceid` equals the head-unit Bluetooth address with the last byte
  incremented, or the pinned `device_id`; anything else is
  `deviceid_match=no` and a stop;
- `pi` matches `receiver_pi_sha256` when pinned;
- the 7000 listener is owned by the stock receiver and is not loopback-only.

Record how any beacon/Apple IE was read (AP config readback or an on-air scan
from an owner device). No readback is `IE: unverified`; `app_info=1` is
Interworking, not the Apple Device IE. A port answer is not identity.

**Stop** (report, do not patch) on: no advertisement, two different
identities, unknown listener owner, loopback-only listener, `deviceid`/`pi`
mismatch, or an IE claim without readback. Do not restart `AppProjection` or
change DNS-SD scope on the basis of D1.

## 6. Phase 3 — P1 RX hook deployment and capture

**Do this only after Phase 2 and only with the owner present.** Confirm the
owner’s local/serial recovery method before replacing `vbtd`; if USB SSH is
the only route and it can be lost, do not deploy. Follow
[`VBT-SPP-HOOK-PORTING.md`](VBT-SPP-HOOK-PORTING.md) and docs/09 for the
temporary replacement. Do not rely on `cp -p` for labels: it does not preserve
SMACK xattrs.

Copy the patched binary and its host-recorded hash to the car:

```sh
# host
DATE=$(date +%Y%m%d)                 # replace with the session date
D=/tmp/opencode/carplay-$DATE
PAYLOAD=$(cat "$D/payload-path") || exit 1
[ -n "$PAYLOAD" ] && (cd "$PAYLOAD" && sha256sum -c SHA256SUMS) || exit 1
sha256sum "$PAYLOAD/vbtd-51.spp-rx.patched" | awk '{print $1}' \
  > "$D/patch.sha256"
scp -P 2224 "$PAYLOAD/vbtd-51.spp-rx.patched" \
  wifi1b@10.55.0.2:/rw_data/carplay/vbtd-51.spp-rx.patched
scp -P 2224 "$D/patch.sha256" \
  wifi1b@10.55.0.2:/rw_data/carplay/patch.sha256
```

Then run this block **on the car**. It is fail-closed: every hash and label
readback must match before `mv`/restart.

```sh
DATE=$(date +%Y%m%d)                 # same date as the host
P=/rw_data/carplay/vbtd-51.spp-rx.patched
WANT=$(cat /rw_data/carplay/patch.sha256)
STOCK_WANT=55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0
abort() { echo "ABORT: $*" >&2; exit 1; }

[ "$(sha256sum "$P" | awk '{print $1}')" = "$WANT" ] || \
  abort "patched hash mismatch"
[ "$(sha256sum /rw_data/carplay/stock-vbtd-51 | awk '{print $1}')" = \
  "$STOCK_WANT" ] || abort "stock hash mismatch"
L=$(tr -d '\000\n' < /rw_data/carplay/stock-vbtd-51.SMACK64)
E=$(tr -d '\000\n' < /rw_data/carplay/stock-vbtd-51.SMACK64EXEC)
[ -n "$L" ] || abort "missing SMACK64"

apply_saved_labels() {
  target=$1
  if command -v chsmack >/dev/null 2>&1; then
    chsmack -a "$L" "$target" || return 1
    [ -z "$E" ] || chsmack -e "$E" "$target" || return 1
  else
    setfattr -n security.SMACK64 -v "$L" "$target" || return 1
    [ -z "$E" ] || setfattr -n security.SMACK64EXEC -v "$E" "$target" || return 1
  fi
  RL=$(getfattr -n security.SMACK64 --only-values "$target" 2>/dev/null |
       tr -d '\000\n')
  [ "$RL" = "$L" ] || return 1
  if [ -n "$E" ]; then
    RE=$(getfattr -n security.SMACK64EXEC --only-values "$target" 2>/dev/null |
         tr -d '\000\n')
    [ "$RE" = "$E" ] || return 1
  fi
  return 0
}

NEW=/app/bin/.vbtd-51.new.$$
cp "$P" "$NEW" && chmod 755 "$NEW" && chown 0:0 "$NEW" || {
  rm -f "$NEW"; abort "stage failed"; }
apply_saved_labels "$NEW" || { rm -f "$NEW"; abort "label apply/readback failed"; }
sync
mv -f "$NEW" /app/bin/vbtd-51 || { rm -f "$NEW"; abort "atomic replace failed"; }
systemctl reset-failed vbtd.service 2>/dev/null || true
systemctl restart vbtd.service
sleep 2
pidof vbtd >/dev/null || {
  sh /rw_data/carplay/rollback-vbtd.sh; abort "vbtd did not restart"; }
[ "$(sha256sum /app/bin/vbtd-51 | awk '{print $1}')" = "$WANT" ] || {
  sh /rw_data/carplay/rollback-vbtd.sh; abort "running hash mismatch"; }
sha256sum /app/bin/vbtd-51
```

Immediate rollback if `vbtd` fails, SSH/BlueLink/AP/wired regresses, or the
hash is wrong. From a **second** SSH connection, verify USB SSH first; if it
fails, restore locally:

```sh
# second connection
ssh -p 2224 wifi1b@10.55.0.2 true || echo "SSH FAILED: restore stock now"
```

```sh
# car-side, self-contained: works from any shell after SSH recovery
sh /rw_data/carplay/rollback-vbtd.sh || { echo "ROLLBACK FAILED"; exit 1; }
pidof vbtd
sha256sum /app/bin/vbtd-51
# Then re-check USB SSH from a second connection and BlueLink/AP/wired state.
```

Read the RX channel **before** connecting the phone; the hook silently drops
when no receiver exists. The broker is not deployed in this phase, so create
the first-party directory and run a bounded standalone reader:

```sh
DATE=$(date +%Y%m%d)                 # same date as the host
D=/rw_data/carplay/evidence-$DATE
mkdir -p "$D"
install -d -m 0700 /run/wifi1b /run/wifi1b/carplay
command -v setsid >/dev/null 2>&1 || { echo "setsid missing"; exit 1; }
umask 077
# Verify no broker/other reader owns the path first; do not delete an unknown
# socket. On a clean Phase 3 there should be no rx.sock.
test ! -e /run/wifi1b/carplay/rx.sock || { echo "rx.sock busy"; exit 1; }
setsid sh -c "exec socat -u UNIX-RECV:/run/wifi1b/carplay/rx.sock - |
  od -An -tx1 | tee '$D/p1-rx.log'" &
READER=$!
sleep 1
[ -S /run/wifi1b/carplay/rx.sock ] || {
  echo "reader did not bind rx.sock"; kill -TERM -"$READER" 2>/dev/null || true
  exit 1; }
# Now connect/trigger the phone's CAFF attempt.
# After capture, kill the whole reader group and wait:
kill -TERM -"$READER" 2>/dev/null || kill "$READER" 2>/dev/null || true
wait "$READER" 2>/dev/null || true
if kill -0 "$READER" 2>/dev/null; then
  echo "reader group still alive; stop it before Phase 4"; exit 1
fi
if ps | grep "[s]ocat.*carplay/rx.sock" >/dev/null 2>&1; then
  echo "reader still running; stop it before Phase 4"; exit 1
fi
# Remove only the standalone reader's socket; the broker must not be running.
[ -S /run/wifi1b/carplay/rx.sock ] && rm -f /run/wifi1b/carplay/rx.sock
chmod 600 "$D/p1-rx.log" 2>/dev/null || true
```

`p1-rx.log` contains peer addresses and opaque iAP2 payload bytes. Keep it
root-only (`0600`). Before Phase 4, record only the header/length evidence
needed for the report, then `rm -f "$D/p1-rx.log"` (or move it to explicitly
approved private storage). Do not copy the raw log into Git or a shared
directory.

If the broker is already running from a previous session, read
`/run/wifi1b/broker-status.json` instead; `rx_packets`/`rx_dropped` must move
and the broker state must remain running. Do not run `socat` and the broker on
the same socket.

P1 pass criteria:

- Datagram starts with ASCII `W1BSPP01` (`57 31 42 53 50 50 30 31`).
- Header length matches the payload; peer is a 17-character address; UUID
  kind is CAFF/CAFE/CarPlay; payload length is 1..16384.
- The OEM binary outside the replaced span is unchanged (builder readback).
- SSH, BlueLink/modem routing, AP and wired CarPlay still work.
- No payload bytes are logged to a world-readable file.

P1 fail/rollback trigger:

- SSH/BlueLink/AP/wired regression, hash mismatch, `vbtd` crash loop,
  unexpected bytes outside the span, or a malformed datagram that reaches the
  session. If USB SSH is lost and no local/serial recovery is available, stop
  and escalate to the owner; do not improvise.
- Restore the stock `vbtd` with the label-preserving block above, restart, and
  verify SSH before further work.

## 7. Phase 4 — broker and MFi helper deployment

Preconditions: Phase 3 passed, target broker and MFi helper hashes verified,
owner config prepared, AP PSK matches the daemon, `consent=1` deliberately
chosen.

1. Stage only the intended files from the same verified Phase 0 payload.
   Install the helpers, units and private owner config as described in the
   payload's `INSTALL.md`, but **do not start either unit yet**.
2. Compare the on-car hashes with the host-recorded ones; abort on mismatch:

```sh
# host: record and copy the exact hashes being staged
DATE=$(date +%Y%m%d)         # same date as Phase 0
D=/tmp/opencode/carplay-$DATE
PAYLOAD=$(cat "$D/payload-path") || exit 1
[ -n "$PAYLOAD" ] && (cd "$PAYLOAD" && sha256sum -c SHA256SUMS) || exit 1
sha256sum "$PAYLOAD/carplay-broker" | awk '{print $1}' \
  > "$D/broker.sha256"
sha256sum "$PAYLOAD/carplay-native-auth" | awk '{print $1}' \
  > "$D/auth.sha256"
scp -P 2224 "$D/broker.sha256" "$D/auth.sha256" \
  wifi1b@10.55.0.2:/rw_data/wifi1b/
# car: compare
BROKER_WANT=$(cat /rw_data/wifi1b/broker.sha256)
AUTH_WANT=$(cat /rw_data/wifi1b/auth.sha256)
[ "$(sha256sum /rw_data/wifi1b/carplay-broker | awk '{print $1}')" = \
  "$BROKER_WANT" ] || { echo "broker hash mismatch"; exit 1; }
[ "$(sha256sum /rw_data/wifi1b/carplay-native-auth | awk '{print $1}')" = \
  "$AUTH_WANT" ] || { echo "auth hash mismatch"; exit 1; }
```

3. `usb_active=1` is a manual block. The broker also scans `usb_sysfs` for an
   Apple USB device; a charging iPhone blocks the offer. For the first live
   wireless test, set `usb_active=0` only with no Apple USB device attached and
   confirm `usb_blocked:false` in `/run/wifi1b/broker-status.json`; the scan is
   point-in-time and repeated on the session policy recheck, not a continuous
   hotplug monitor. Otherwise keep `1` and stop.
4. Run the installed, hash-verified broker's read-only preflight on the car:

   ```sh
   /rw_data/wifi1b/carplay-broker --check-ap \
     --config /rw_data/wifi1b/carplay-broker.conf || exit 1
   ```

   Expect `AP snapshot matches`. A refusal is a stop; check settled AP mode,
   status-schema compatibility and exact owner config values. Do not copy
   the snapshot into evidence; it contains credentials.
5. Start `scourge-carplay-auth.service` first, then
   `carplay-broker.service`.
6. Check:

```sh
systemctl status --no-pager scourge-carplay-auth.service carplay-broker.service
cat /run/wifi1b/broker-status.json
```

Broker pass criteria:

- `state:"running"`, `ap_on_air:true`, fresh `status.json` readback.
- `rx_packets` increases on a CAFF connection; `rx_dropped` does not grow
  without a reason.
- MFi socket exists at `/run/scourge-carplay-auth/auth.sock`, root-only; real
  certificate/signature bytes return; no fake AA00-AA05.
- No SSH/BlueLink/AP/wired regression.

Broker fail triggers: missing bus, missing RX socket, missing daemon status,
unsupported AP security, MFi failure, any policy refusal, any fatal status.
Do not bypass with a static consent/USB override.

## 8. Phase 5 — AP/iAP2 bootstrap and projection

1. Start wireless CarPlay on the phone.
2. Watch `status.json` for `ap_clients` and the broker status for session
   counters. Do not use the daemon’s `carplay_session_observed` /
   `carplay_ready` as proof yet: their derivation is offline-tested only and
   the on-car values remain unvalidated.
3. Check the broker counters and logs; confirm the phone received the AP
   credentials and joined `IONIQ5-CarPlay`.
4. Observe `_airplay._tcp` TXT/SRV from the AP side and corroborate it
   against the stock listener and the Phase 2b (D1) baseline. Compare
   before/after phone association; verify that the broker's configured
   receiver port matches. If D1 recorded `unobserved`, record this observation
   as unobserved too rather than assuming it appeared.
5. If the phone joined but projection did not start, distinguish discovery
   and selection (`_carplay-ctrl._tcp`, OEM controller events/request) from
   incoming setup, session creation, and first frame. Follow the
   [stock projection findings](STOCK-PROJECTION.md). Do not patch or
   restart `AppProjection` on the basis of missing mDNS or a 455 alone.

Pass criteria are layered; record each separately:

- AP client joins.
- iAP2 identification/session completes.
- Broker reports a session (not merely “running”).
- `_airplay._tcp` appears with the expected identity.
- Projection/UI appears (only then can “wireless CarPlay works” be claimed).

## 9. Phase 6 — regression and rollback

Rollback is part of the test, not an afterthought.

```sh
systemctl stop carplay-broker.service scourge-carplay-auth.service
systemctl disable carplay-broker.service scourge-carplay-auth.service 2>/dev/null || true
systemctl reset-failed carplay-broker.service scourge-carplay-auth.service 2>/dev/null || true
rm -f /etc/systemd/system/carplay-broker.service
rm -f /etc/systemd/system/scourge-carplay-auth.service
systemctl daemon-reload

# Self-contained, label-preserving stock restore (created in Phase 1).
sh /rw_data/carplay/rollback-vbtd.sh || {
  echo "ROLLBACK FAILED"; exit 1; }

# Remove the PSK-bearing owner config and helper binaries if no longer needed.
# rm -f /rw_data/wifi1b/carplay-broker /rw_data/wifi1b/carplay-native-auth
# rm -f /rw_data/wifi1b/carplay-broker.conf
```

Verify after rollback:

- Running `vbtd` hash is stock.
- USB SSH still works.
- BlueLink/modem route is unchanged.
- AP/client behavior is restored.
- Wired CarPlay still works.
- No listener/SerialPort object is left behind after phone disconnect.

## 10. Evidence to capture

| File | Content |
|---|---|
| `host.log` | Commands, test results, build hashes |
| `car-preflight.log` | Kernel, `vbtd` hash, redacted status, D-Bus names, AP state |
| `p2-listen.log` | `ListenSPP` return, object path, attrs |
| `p2-senddata.log` | `SendData` return, ladder steps 1–2, capture header/datalink |
| `p2-snoop.log` | Trace path/size/SHA-256, btsnoop version/datalink, RFCOMM frame types/DLCI/lengths only |
| `d1-receiver.log` | Listener scope/owner, SRV port, `srcvers`, `deviceid_match`, `pi_sha256`, IE source |
| `p1-rx.log` | Header bytes, counters, any malformed frames; **delete after header extraction** |
| `broker-status.json` | State, counters, AP readback, errors |
| `auth.log` | Real helper status only; no credential bytes |
| `projection.log` | `_airplay._tcp`, SRV/TXT, UI observations |
| `rollback.log` | Restored hashes and regressions |

Redact phone and head-unit Bluetooth MACs, `carplay_btaddr`/`carplay_id`,
AP PSKs, client SSID/BSSID, `imported_from`, keys and personal data before
sharing. TXT `pi` and `deviceid` are never pasted; keep only `pi_sha256` and
`deviceid_match`. Prefer the filtered `redact()` output from Phase 1 over a
raw `status.json` or `bt.state` copy. Raw snoop traces stay outside the repo
and outside shared chat.

## 11. Abort conditions

Stop and roll back on any of:

- USB SSH loss or inability to restore it.
- BlueLink/modem route regression.
- AP/network regression not explained by the test.
- Wired CarPlay regression.
- `vbtd` hash mismatch or crash loop.
- Unexpected bytes outside the patched span.
- Broker fatal state that cannot be explained without bypassing a gate.
- MFi authentication failure or any request to fake a response.
- Any sign that the phone or owner data is at risk.

## 12. If the car is unavailable

Do only Phase 0. Keep the target/broker/MFi builds, their manifests and the
hash evidence. Do not mark any live row in
[`VALIDATION.md`](VALIDATION.md) as validated. The next session starts at
Phase 1.

## 13. Owner inputs needed before deployment

- SSH route and confirmation that USB SSH is the recovery path.
- CarPlay identity fields and AP PSK source (root-only; never commit).
- Consent for the experimental session.
- Decision on `usb_active` until live USB detection exists.
- Stock backup location and rollback approval.
- Phone available for a real CAFF/AirPlay attempt.
