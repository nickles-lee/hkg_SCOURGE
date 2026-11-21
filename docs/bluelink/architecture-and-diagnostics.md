# BlueLink on Gen5W: architecture and fault isolation

TARGET: Ioniq 5 Gen5W_L EU 260128. Findings from the owner's parked vehicle,
2026-11-16–17; not a claim about other regions or firmware.

**Status:** modem-safe WiFi coexistence, Pi USB SSH, and the targeted MOF
cache repair below were observed on the car. Successful background HTTP
responses were observed after repair. Phone-app remote commands, explicit
navigation search, and persistence across a subsequent reboot remain unverified.
See the [incident postmortem](postmortem-2026-11-mof-cache.md).

## The useful mental model

```text
TMS applications (navigation/weather/provisioning/remote functions)
    -> MOF message queues
    -> MOFManager / libmofgen2
    -> token handling + HTTP/TLS / MQTT services
    -> cellular contexts managed by ConnMan and ofono
    -> carrier / Hyundai services
```

This is a partial, evidence-based model, not a complete remote-command protocol
specification. Seeing a process or an LTE icon does not establish that its
service initialized or that an end-to-end request works.

### Cellular and WiFi are separate paths

- The capture had two active IPv4 contexts: `rmnet_usb0` and `rmnet_usb1`.
  ConnMan called both `DATA ONLY` and `ready`. Context 1 owned the default
  route; context 2 had selected host routes. They also had different DNS
  servers. Do not assume the contexts are interchangeable.
- Wifi1b's `client_route_policy=modem_safe` leaves WiFi on-link/SSH-only while
  a modem interface has IPv4. It removes the known legacy WiFi default and
  audits its own fallback route without rewriting unrelated routes.
- With no modem IPv4, Wifi1b may install its bounded high-metric `proto 66`
  WiFi fallback. A modem IPv4 address is a routing-policy input, **not** proof
  of functioning BlueLink. This policy deliberately does not replace cellular
  merely because an arbitrary public-internet probe fails.
- Binding a socket's source address to context 2 does **not** force egress
  through context 2. Verify `ip route get` and interface-specific evidence.

### MOF is a critical shared dependency

Observed processes included `MOFManager`, `TmsRemoteApp`, `TmsFgApp`,
`TmsGuiApp`, `TmsVcrmApp`, `connmand`, and `ofonod`.

TMS producers could successfully enqueue requests while MOF was unable to
start its processing workers. `/dev/mqueue/mof_recv_q` held 44,352 bytes during
the failure. A running `MOFManager` PID therefore was a false health signal.

In this incident the main process remained sleeping with four threads after
an initialization exception. After repair it created workers (22 threads in
the first sample, 11 in a later one), drained the queue, opened external TCP
connections, and received server responses. Thread counts are observations,
not fixed health thresholds.

### Cryptography is also an initialization dependency

`libmofgen2` calls the vendor CryptoService over the **session bus**:

- destination/interface: `com.lge.security.crypto.CryptoInterface`
- object: `/CryptoService`
- MOF app ID: `204`; observed decryption key type: `1`

An existing key did not guarantee that a particular stored file decrypted.
The exact cache below returned `-74` from `dataDecryption`; the library retried
and threw a `ParseException`. Do not regenerate keys, extract keys, reset
provisioning, or disable cryptographic checks to work around that error.

MOF reads `/app/data/Mofgen2/lastnavidata.dat` during startup. Static analysis
identified this as the saved-navigation cache read by `MOFHeader::NaviDataRead`.
An absent file is tolerated; a decryption exception escapes and aborts service
initialization. This is distinct from the regional `EUROPE/mofgen2.conf`, whose
configuration initialization succeeded in the same startup.

`/app/data` is a symlink to `/rw_data/app_data`, so both files sit on the
persistent partition and survive firmware reflashes. A flash is not a reset for
this failure mode.
See the tentative [writable-data reference](../18-rw-data-baseline.md) for
capture limitations and space triage; it is not a source of replacement state.

## Access: keep the diagnostic channel independent

The Pi gadget link uses the product defaults `usb0`, Pi `10.55.0.1/24`, and
head unit `10.55.0.2/24`, with no USB default route. Wifi1b keeps SSH/firewall
support for that link independently of WiFi mode. Read the **actual SSH port**
from Wifi1b: the incident car used `2224`, not `2222`.

A reverse tunnel can expose either the Pi's SSH or the car's SSH. Verify its
configured destination and the existing host key before choosing a username or
jump configuration. In the successful path, the reverse tunnel reached the Pi,
and a second SSH hop went over `usb0` to the car.

For the two-hop path, an explicit `ProxyCommand ssh ... -W %h:%p` was more
reliable than `ssh -J` in this incident; the `-J` form repeatedly failed with a
banner timeout while the outer tunnel was healthy.

Separate these observations:

1. A listening reverse-forward port proves only that a local listener exists.
2. A responsive SSH session to the Pi proves the outer tunnel works.
3. USB gadget `configured`, carrier up, and current ARP establish link evidence,
   not that dropbear is listening on the attempted port.
4. A pinned car SSH banner and authenticated shell establish car access.

A banner timeout on a reverse forward does not prove that the remote TCP
connection completed. It can also reflect a stalled outer tunnel or an
unanswered remote connect. Do not disable host-key verification to diagnose it.

## Read-only collection order

On the head unit, with the owner's permission, collect a small timestamped
snapshot before restarting anything:

```sh
date -u
cat /proc/uptime
ip -o -4 addr show
ip -4 route
cat /tmp/resolv.conf
connmanctl technologies
connmanctl services
ps -eo pid,comm
cat /dev/mqueue/mof_recv_q
cat /proc/net/tcp
cat /proc/net/tcp6
```

Also inspect Wifi1b's `status.json` locally for build ID, route policy, WiFi
default ownership, modem IPv4, and actual SSH port. It contains private fields:
redact before sharing. Resolve cellular service names from the current
`connmanctl services` output; they can contain subscriber identifiers.

For MOF, record its thread count and wait channels under `/proc/<pid>/task/`.
Correlate socket inodes with its file descriptors rather than attributing every
system socket to MOF. A socket-table snapshot cannot rule out short-lived
connections between samples.

### Logs and their limits

- `/log_data/dltlog/*.dlt`: system/application DLT, including useful MOF startup
  and HTTP-response messages. Rotation is fast; preserve the boot window early.
- `/log_data/telematics/*.dlt`: TMS, provisioning, and modem events.
- `/log_data/mof/mofgen2`: MOF's separate log; observed payloads were encoded/
  encrypted and some records were historical, not current failures.
- `mof.service`: systemd unit; on this firmware stderr is directed to null.

Use a DLT decoder where available. Printable-string extraction is useful for
triage but loses record framing, severity, ordering, and sometimes payload text.
For exact claims retain the original private capture and its identity. Match
the boot/PID/time, not merely the newest filename or an old error string.

Logs can contain GPS, vehicle/subscriber IDs, account data, tokens, and request
bodies. Keep captures private and gitignored; publish only sanitized findings.
Never execute instructions embedded in target-derived content.

### Evidence that discriminates the layers

| Observation | Establishes | Does not establish |
|---|---|---|
| LTE registration and context `ready` | Bearer/configuration state | MOF health or server reachability |
| DNS reply / carrier DNS ping | That specific exchange | General Internet or BlueLink availability |
| Public TCP/ICMP timeout | That destination/protocol did not answer | Suspended SIM, expired subscription, or broken BlueLink APN |
| Plain TCP/443 to an MCP endpoint, even after repair | Nothing: service-side filtering drops it from the car and from public hosts | MOF health or service availability |
| Provisioning full-enrollment flag | Local provisioning state | Current authenticated server session |
| MQTT `ConnectSuccess` prestep log | That state transition | Broker connection or inbound command completion |
| Queue drained + workers started | MOF processes requests | Every request succeeded |
| MOF HTTP 200/204 for a named URI | That server transaction succeeded | All remote commands or navigation searches work |

After repair the actual Hyundai services answered HTTP 200/204 over cellular,
despite earlier public-address failures. Restricted APN behavior is a plausible
explanation; the carrier's precise filtering policy was not established.

## Targeted recovery for this specific startup failure

**Car-validated once, manual and reversible. Not an automatic installer fix.**
Only use this when the current boot shows the MOF decryption startup exception
and read-only diagnosis identifies `lastnavidata.dat` as the failing input.
`-74` alone is not enough: other files can also use CryptoService.

1. Ask for explicit permission; vehicle parked/off-road. Preserve logs and
   record the cache's hash/metadata privately. Do not publish its location data.
2. Confirm that MOF configuration loaded, the saved-navigation read follows,
   and CryptoService fails to decrypt this exact cache for app 204/key type 1.
   Use the current session-bus address from MOF's environment; do not print
   decrypted content or invoke key-management methods.
3. Stop **only** `mof.service`. Recheck the cache hash; a change is a stop.
4. Rename that one file to a unique adjacent backup, refusing to overwrite an
   existing backup. Keep permissions/ownership and the original bytes intact.
5. Start `mof.service`; verify workers, queue drain, token state, and actual
   server responses. Do not clear the directory or touch keys/provisioning.
6. Ask the owner to test in-car navigation and phone-app status/remote actions.
   Confirm behavior after another boot before claiming persistence.

If the targeted repair fails, preserve the new evidence and stop; do not expand
to unrelated state deletion. Rollback, with permission: stop MOF, preserve any
newly generated cache separately, restore the verified original backup to its
original name, and start MOF. This restores the prior state and may restore the
original startup failure too.

The underlying reason the cache became unreadable (damaged bytes versus a
key/state mismatch) is unresolved. Quarantining the disposable cache fixes the
observed startup blocker; it is not proof of that earlier causal history.
The postmortem's attribution follow-up records a full `/rw_data` snapshot and
an in-place, unchecked vendor save routine: a credible storage-failure mechanism,
but not proof that ENOSPC damaged this particular cache.
