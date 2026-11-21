# Updating Wifi1b over SSH (daemon-only hot swap)

This is the owner procedure for replacing `wifi1b_daemon` on a running head
unit without a USB install. It is deliberately **detached** from the SSH
session: stopping `wifi1b-daemon.service` also stops the dropbear children,
so an update run in the foreground would be killed halfway through.

Only the daemon binary is replaced. Configuration, credentials, the SSH host
keys, `authorized_keys`, QML and the USB-SSH boot unit are untouched.

> Owner-authorized maintenance only. This procedure is not car-validated and
> does not replace the normal bundle install/release checks.

## Prerequisites

- SSH access to the head unit, normally over the Orange Pi USB link:
  `ssh wifi1b@10.55.0.2 -p 2224`
  or through the Pi reverse tunnel from the desktop:
  `ssh -p 2222 <pi-user>@127.0.0.1`, then the same car command.
- A statically linked x86-64 `wifi1b_daemon` built from the intended commit.
  The script checks the ELF magic and x86-64 machine field directly and checks
  the static-link flag when `file` is installed.
- Root on the head unit (the `wifi1b` account is UID 0).
- `ssh=1` in `/rw_data/wifi1b/config.conf` if the script should also verify
  that the USB SSH listener comes back. With `ssh=0` the script still requires
  a verified daemon API response; it reports `ssh_enabled=0` and `listener=no`.
- A local HTTP probe (`socat`, `wget` or `curl`) and, when `ssh=1`, a listener
  probe (`netstat` or `ss`). The script fails closed **before** stopping the
  daemon if it cannot verify success.

## Build

From the repository:

```sh
make -C packages/wifi1b/src wifi1b_daemon
file packages/wifi1b/src/wifi1b_daemon
# ELF 64-bit LSB executable, x86-64, ... statically linked
sha256sum packages/wifi1b/src/wifi1b_daemon | cut -d' ' -f1 \
    > packages/wifi1b/scripts/wifi1b_daemon.new.sha256
```

Keep the sha file as a plain hash, or as normal `sha256sum` output; the
hot-swap script reads the first 64 hex characters and compares them with the
staged binary.

## Stage the files

Upload the binary, its hash and the hot-swap script while the daemon is still
running. A failed or partial upload leaves the live daemon untouched.

```sh
# Direct car access:
scp -P 2224 packages/wifi1b/src/wifi1b_daemon \
    wifi1b@10.55.0.2:/rw_data/wifi1b/wifi1b_daemon.new
scp -P 2224 packages/wifi1b/scripts/wifi1b_daemon.new.sha256 \
    wifi1b@10.55.0.2:/rw_data/wifi1b/wifi1b_daemon.new.sha256
scp -P 2224 packages/wifi1b/scripts/hotswap_daemon.sh \
    wifi1b@10.55.0.2:/rw_data/wifi1b/hotswap_daemon.sh
```

If the car has no `scp`, use an SSH pipe:

```sh
ssh wifi1b@10.55.0.2 -p 2224 \
    'umask 077; cat > /rw_data/wifi1b/wifi1b_daemon.new' \
    < packages/wifi1b/src/wifi1b_daemon
# repeat for the .sha256 and the script
```

Then verify and set the executable bits:

```sh
ssh wifi1b@10.55.0.2 -p 2224 '
    chmod 755 /rw_data/wifi1b/hotswap_daemon.sh /rw_data/wifi1b/wifi1b_daemon.new
    sha256sum /rw_data/wifi1b/wifi1b_daemon.new
    cat /rw_data/wifi1b/wifi1b_daemon.new.sha256
'
```

The script re-checks the hash, the ELF magic/x86-64 machine field and (when
`file` is available) the static-link flag before it stops anything. It also
checks that a probe tool exists, so it never stops the daemon unless success
can be verified.

## Launch detached

`systemd-run` puts the script in its own service, so the car SSH session can
drop without interrupting the swap:

```sh
ssh wifi1b@10.55.0.2 -p 2224 '
    systemd-run --unit=wifi1b-hotswap-$(date +%s) --no-block \
        /bin/sh -c "exec /bin/sh /rw_data/wifi1b/hotswap_daemon.sh \
            >> /rw_data/wifi1b/hotswap.log 2>&1"
'
```

If `systemd-run` is unavailable, use a new session with `setsid`:

```sh
ssh wifi1b@10.55.0.2 -p 2224 '
    setsid /bin/sh -c "exec /bin/sh /rw_data/wifi1b/hotswap_daemon.sh \
        >> /rw_data/wifi1b/hotswap.log 2>&1" </dev/null >/dev/null 2>&1 &
'
```

The script waits two seconds before stopping the daemon, giving the launching
SSH command time to return. **Do not run it in the foreground**; the stop
will kill that SSH session.

## What the script does

1. Takes an exclusive `flock` and removes an old result file.
2. Verifies the staged hash, executable bit, ELF magic and x86-64 machine;
   checks the static-link flag when `file` is installed; requires a probe tool.
3. Exits without restarting only when the live hash matches **and** the API
   answers; otherwise it reports `ok=0` without stopping the daemon.
4. Backs up the live binary to
   `/rw_data/wifi1b/backup/wifi1b_daemon.pre-hotswap-<timestamp>` **before**
   stopping anything, then records the live `SMACK64`/`SMACK64EXEC` labels
   when readable.
5. Stops `wifi1b-daemon.service`, waits for the daemon process to exit, and
   refuses to replace a binary that is still being executed.
6. Copies the new binary to a same-directory temporary, fixes mode/owner,
   preserves the recorded labels, `sync`s, and renames it over the live path
   atomically.
7. Starts the service and waits up to 120 s for a verified local API response
   and, when `ssh=1`, the `:2224` listener.
8. On any failure, interruption or signal after the stop, restores the backup
   (including its recorded labels), restarts the unit, and writes `ok=0`.
9. Writes `/rw_data/wifi1b/hotswap.result` and appends to
   `/rw_data/wifi1b/hotswap.log`.

## Reconnect and read the result

The car SSH connection drops when the daemon stops. The Pi tunnel to the
desktop is a separate connection and stays up. From the Pi, wait for the car
to return and then read the staged result:

```sh
# On the Pi (or from the desktop through the Pi):
while ! ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
        -p 2224 wifi1b@10.55.0.2 \
        'test -f /rw_data/wifi1b/hotswap.result'; do
    sleep 2
done

ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    -p 2224 wifi1b@10.55.0.2 \
    'cat /rw_data/wifi1b/hotswap.result; tail -100 /rw_data/wifi1b/hotswap.log'
```

A successful result contains `ok=1`, `api=yes`, the new `build_id`, the
retained backup path and, when `ssh=1`, `listener=yes`. With `ssh=0` it
contains `listener=no` and `ssh_enabled=0`; success still requires `api=yes`.

## Verify

```sh
ssh wifi1b@10.55.0.2 -p 2224 '
    sha256sum /rw_data/wifi1b/wifi1b_daemon
    systemctl status wifi1b-daemon.service --no-pager | head
    netstat -ltn | grep 2224
    head -c 400 /rw_data/wifi1b/status.json
'
```

The daemon-only swap does **not** prove the boot path. The next head-unit
startup is the end-to-end check:

- `/rw_data/wifi1b/usb-ssh-boot.log` should show the listener coming up.
- `/rw_data/wifi1b/daemon.log` should no longer contain
  `Radio busy/reserved` for the SSH worker.
- SSH from the Pi over `usb0` should work before WiFi/Client mode is started.

## Rollback

The script restores the backup automatically on any failed start, missing API,
missing listener, interruption or signal after the stop. It also restarts the
unchanged old daemon if it fails before replacing the binary.
For a manual rollback:

```sh
ssh wifi1b@10.55.0.2 -p 2224 '
    BACKUP=$(ls -1t /rw_data/wifi1b/backup/wifi1b_daemon.pre-hotswap-* | head -1)
    systemctl stop wifi1b-daemon.service
    cp -f "$BACKUP" /rw_data/wifi1b/wifi1b_daemon
    chmod 755 /rw_data/wifi1b/wifi1b_daemon
    chown 0:0 /rw_data/wifi1b/wifi1b_daemon
    # The automatic path restores the recorded SMACK64/SMACK64EXEC labels.
    # This manual fallback uses the product's floor label.
    chsmack -e _ /rw_data/wifi1b/wifi1b_daemon 2>/dev/null || true
    sync
    systemctl start wifi1b-daemon.service
'
```

Keep the backup until the next successful startup. The installer’s backup
pruner ignores the `wifi1b_daemon.pre-hotswap-*` files, so they are not
silently removed.

## Notes

- The process deliberately does not touch `wifi1b-usb-ssh.service`; that
  boot-time helper remains a separate safety net.
- Updating the daemon while the radio is owned by another app is safe for the
  USB listener: dropbear, the key files and the usb0 firewall rule do not
  mutate `wlan0`.
- A full payload/QML/dropbear update still uses the normal USB bundle flow;
  this procedure is only for a daemon binary already validated by the build
  and tests.
