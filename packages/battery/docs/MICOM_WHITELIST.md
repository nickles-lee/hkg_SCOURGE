# The micom CAN-send whitelist and the `CANService` helper

Status: implemented in Battery `2026-11-14.1`.

This note documents a vendor-side restriction in the Gen5W head unit that
silently drops every CAN frame sent through `com.lge.car.micom`, and the
workaround Battery ships. It is written from the decompiled 260128 firmware
(`micommanager_userspace`, export `bin_e5f040ffc41cc980`) and the factory
`TmsRemoteApp` (`bin_180c434e881fa2ff`).

## 1. What the micom exposes

`micommanager_userspace` owns `com.lge.car.micom` on the system bus. Its
`com.lge.car.micom.can` interface at `/Can` includes:

```
SetCANMicomRecv(ay)          raw CAN frame: [ID hi, ID lo, DLC, data0..7]
SetCANFixedMicomRecv(ay,i,i,i)
SetBCANMicomRecv(ay)
SetCCANMicomRecv(ay)
SetExtMMCanTxReq(ay)
SetExtICanTxReq(ay)
...
```

The factory `TmsRemoteApp` sends the charge-door frames through
`SetCANMicomRecv` with an 11-byte array:

```
04 A2 08 FF FF FF DF FF FF FF FF   (open)
04 A2 08 FF FF FF EF FF FF FF FF   (close)
```

and sets `dbus_message_set_no_reply(TRUE)` before `dbus_connection_send` +
`flush`. The destination/path/interface/method are exactly:

```
com.lge.car.micom
/Can
com.lge.car.micom.can
SetCANMicomRecv
```

Battery used the same method and byte layout, so the frames were correct.
The problem was the sender.

## 2. The whitelist

Before dispatching a CAN-send request, `micommanager_userspace` calls a
sender check (`FUN_0010b1e0` in the decompiled export). It:

1. asks the bus for the caller's PID (`QDBusConnectionInterface::servicePid`),
2. builds `/proc/<pid>/exe`,
3. requires it to be a symlink and canonicalizes it,
4. takes `QFileInfo::fileName()` — the **basename** of the resolved
   executable,
5. compares that basename against a hardcoded list.

The list is built from these four strings:

```
AppHomeScreen
TmsRemoteApp
micommanager_userspace
CANService
```

If the basename is not in the list, the handler logs:

```
Dose not listed in binary_list : <name>
```

and returns without forwarding the frame. (The typo is the vendor's.)

The check uses the **basename**, not the full path, and it runs in the
service handler, i.e. after the message has been queued. Two consequences:

- A `dbus-send` child has basename `dbus-send` and is rejected.
- `dbus-send` without `--print-reply` exits as soon as the message is
  queued. Even a renamed copy can be gone before the micom resolves
  `/proc/<pid>/exe`, so the check fails with "Target file does not exist".

Battery's old `send_ok` counter only reflected the `dbus-send` exit status.
The bus accepted the message, so it reported success while the micom
dropped every preheat, cluster-display and charge-door frame.

## 3. The workaround

Battery does **not** patch `micommanager_userspace`. The micom binary is
signed vendor code, and modifying it would be a much larger and riskier
change than the app needs.

Instead, the installer ships two small pieces:

```
/rw_data/battery/bin/CANService   copy of the vendor /usr/bin/dbus-send
/rw_data/battery/bin/micom-send   wrapper that runs it and exits 0
```

`CANService` is the **vendor's own `dbus-send` binary**, copied and renamed.
It is dynamically linked against the car's glibc/libdbus, so it runs on the
head unit exactly as the factory tool does. The wrapper runs:

```
CANService --system --type=method_call --print-reply \
    --reply-timeout=500 --dest=com.lge.car.micom /Can \
    com.lge.car.micom.can.SetCANMicomRecv <array>
```

The basename `CANService` is on the vendor list. `--print-reply` keeps the
`dbus-send` process alive for up to 500 ms while the micom resolves
`/proc/<pid>/exe`; the wrapper waits for it and exits 0 even when the micom
does not send a reply (the factory sets `no_reply`). The daemon uses the
wrapper for **all** micom frames:

```
battery_daemon --micom-send /rw_data/battery/bin/micom-send \
               --micom-bin  /rw_data/battery/bin/CANService
```

Both files are installed by `run_stuff.sh` and receive the same
`SMACK64EXEC` label as the daemon (the installer's own label, normally `_`).
The session-bus Home/Star monitor still uses a copy of `dbus-monitor`; it
does not send CAN frames and is not subject to the whitelist.

A host-built libdbus helper was tried first, but the car's glibc is older
than the build host's (the binary asked for `GLIBC_2.34`/`GLIBC_2.38`), so
the vendor `dbus-send` copy is the shippable approach.

## 4. Why not call the real `CANService`?

The navi CAN service (`com.mnsoft.navi.canservice` on the session bus,
`/CanService`) exposes signals such as `SendClusterSpeedToNavi` and
`SendBatteryTempToNavi`. It does not expose a generic "send this frame"
method we could call without reverse-engineering its internal `SQCanFrame`
marshalling. The helper is smaller, deterministic, and uses the same public
micom method the factory uses.

## 5. Security and ownership note

The whitelist is a vendor security control. Naming the helper `CANService`
makes the micom accept frames from it. This is appropriate only on a head
unit the owner controls and has already rooted; it is not a general-purpose
bypass and should not be shipped as one. The helper only sends the frames
Battery already constructs (preheat burst, cluster display, charge door); it
does not expose a remote CAN-send service.

## 6. Verifying it on the car

```
/rw_data/battery/battery_daemon --micom-check
```

prints the system-bus name list, whether `com.lge.car.micom` is owned, the
micom PID and `/proc/<pid>/exe`, and the daemon's own basename. Expected:
`micom_owned=yes` and a micom executable path.

```
/rw_data/battery/battery_daemon --micom-test
```

sends a harmless cluster-display frame through `CANService` and prints the
`dbus-send` reply/exit. A visible cluster change means the whitelist path is
working. A `Dose not listed in binary_list` reply means the sender basename
is wrong; `Target file does not exist` means the helper exited before the
check (increase the reply timeout).

`run_stuff_diag.sh` and the USB log bundle include both checks. The daemon
also reports `micom_name`, `micom_path`, `smack_send` and `last_send` in
`/status`; the QML footer shows the helper name and the last TX.

If the micom still rejects the frame, `daemon.log` contains the helper's
stderr. The two likely messages are:

- `Dose not listed in binary_list : ...` — the helper basename is wrong.
- `Target file does not exist` — the helper exited before the check; keep it
  alive longer.

## 7. References

- `micommanager_userspace` export `bin_e5f040ffc41cc980`:
  `FUN_0010b1e0` (sender check), `FUN_00132450` (`SetCANFixedMicomRecv`
  handler), strings at `0x4f40d`, `0x4f56f`, `0x4f69e`, `0x4f7d5`.
- `TmsRemoteApp` export `bin_180c434e881fa2ff`: `EVRemoteChargeDoorOpen`
  at `0x1ba8e0`, `EVRemoteChargeDoorClose` at `0x1baa40`, method string
  `SetCANMicomRecv` at `0x24d077`, destination/path/interface at
  `0x246d9b` / `0x246d96` / `0x246d80`.
- `src/battery_daemon.c` (`send_micom_frame`, `micom_check`, `micom_test`),
  `scripts/run_stuff.sh` (helper install), and
  `artifacts/systemd/battery-daemon.service`.
