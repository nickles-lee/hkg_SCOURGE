# Postmortem: BlueLink unavailable because MOF could not read its navigation cache

TARGET: Ioniq 5 Gen5W_L EU 260128. Investigation: 2026-11-16–17.

**Outcome:** a targeted, owner-approved cache quarantine and MOF restart restored
request processing and successful BlueLink background server transactions on
the car. Explicit phone-app remote-action, navigation-search, and next-boot
checks were still pending at the close of the captured investigation.

## Impact and initial picture

The owner reported BlueLink unavailable while custom WiFi/SSH was installed.
An earlier capture showed a WiFi default route taking precedence over cellular.
The V22/V23 Wifi1b work corrected that interference and retained the Pi's USB
SSH access. However, BlueLink remained unavailable even with a correct modem
default route and two configured cellular contexts.

These were **two distinct issues**. The evidence does not prove the old WiFi
route caused the later cache failure, or that the route fix alone would have
restored BlueLink. V23 was a bundle-version bump over V22's networking code.

## What actually failed

The current boot's MOF startup log contained:

```text
MOF Service failed to start! ParseException:
data dataDecryption failed result[-74] keyExist[1]tried[4]
```

`MOFManager` remained alive but had only four threads, no observed external TCP
session, and a backed-up `/dev/mqueue/mof_recv_q` (44,352 bytes). TMS producers
could still report successful queue sends, hiding the failed consumer startup.

Static inspection of the matching shipped binaries identified the saved-state
read in `MOFHeader::NaviDataRead`:

1. Open `/app/data/Mofgen2/lastnavidata.dat`.
2. Read its bytes and request decryption through `libmofgen2`/CryptoService.
3. The crypto wrapper retries a failed operation and throws `ParseException`.
4. The startup exception handler logs the failure; normal MOF processing never
   starts, although the wrapper/main process stays alive.

Read-only invocation of the vendor's session-bus `dataDecryption` for the exact
77-byte file, using MOF app ID 204 and key type 1, returned **`-74`**. The regional
MOF configuration had initialized successfully, so deleting or replacing that
configuration was not justified. The cache reader's missing-file branch logs
the absence and continues, giving a narrow recovery path.

`/app/data` is a symlink to `/rw_data/app_data`, so this cache and the regional
configuration live on the **persistent** partition. That is why a firmware
reflash did not clear the failure here: the unreadable cache survives the flash
and the next boot repeats the startup exception.

**Proximate root cause:** unreadable encrypted navigation cache aborts MOF
initialization. **Unresolved upstream cause:** why that cache no longer decrypted.
The evidence does not distinguish ciphertext damage from a key/state mismatch.
No evidence establishes that SCOURGE created or corrupted it.

## What made the cache unreadable: attribution follow-up

**Conclusion: not proven. Storage exhaustion is a credible contributing
mechanism, not an established explanation for these exact bytes.** There are
two separate questions: who wrote the cache, and what conditions made that
write or its later decryption fail.

### Findings we can defend

- The vendor `MOFHeader::NaviDataSave` routine is a writer of this exact path.
  Analysis of the matching binary shows it encrypts the serialized navigation
  data first and opens the destination with `fopen(..., "w")` only after the
  crypto call succeeds. That mode truncates the existing file in place.
- It calls `fwrite`, `fflush`, `fsync`, and `fclose` but does **not check their
  results**. The local routine has no temporary-file/atomic-replace sequence or
  preservation of the preceding good cache. `fsync` being present is not a
  checked durability guarantee. The relevant assembly also confirms the ignored
  return values; this is not merely a decompiler inference.
- Consequently, ENOSPC, a short/buffered write failure, I/O failure, or power
  interruption can leave a damaged replacement without a detected save failure.
  The next boot's reader compounds that risk by allowing a decryption exception
  from a disposable cache to abort the broader service.
- The November 15 runtime filesystem snapshot already showed `/rw_data` at
  **100% with zero available space**. The November 16 log-retention work also
  documented severe pressure (97%), unbounded custom logs, and accumulated
  installer snapshots. It is incorrect to date the first known full-filesystem
  observation only to November 16.
- SCOURGE's pre-fix logs and repeated-install backups demonstrably contributed
  storage pressure. `9a55b01` bounds those writers/retention. This is a real
  project-side defect and is not excused by the vendor's weak failure handling.
- A bounded audit of tracked first-party scripts/packages/tools/payload and
  their history found no reference that directly writes `lastnavidata.dat` or
  resets MOF's crypto keys. That does **not** audit every historical ad-hoc
  remote command, external tool, or firmware operation.

### Why this is not yet a proven ENOSPC incident

The preserved cache is 77 bytes, with mtime November 10 at 11:54:14. Its ctime
changed during the approved quarantine on November 17; that ctime is not the
corruption time. Size alone does not prove truncation: the expected ciphertext
length for the historical save was not established. An encrypted file may also
fail decryption if the applicable key/state changed, with no changed ciphertext.

The same boot's MOF log begins at 11:50:34 and its first error burst is
11:51–11:54 (42 errors), contemporaneous with that write. The failure is
therefore consistent with the first provisioning boot on November 10, not with
the Wifi1b work five days later. The log cannot prove causal direction: its
payloads are encrypted, so only severity and timestamps are readable.

There is no complete, verified earlier good copy of this exact file in the
examined evidence, no captured failing `fwrite`/`fflush` result for its save,
and no contemporaneous November 10 filesystem-capacity snapshot. The examined
local critical archive reported an unexpected EOF; it was not used to assert
absence, an earlier cache hash, or a complete backup. Other extracted binary
identities were checked separately. RTC resets and restored timestamps also
limit timestamp-only attribution.

Thus the defensible chain is:

```text
Confirmed: custom writers contributed disk pressure; /rw_data was full.
Confirmed: vendor cache saves truncate in place and ignore persistence errors.
Hypothesis: those conditions damaged this cache (or related crypto state).
Confirmed: this cache could not decrypt and blocked MOF startup.
Confirmed: quarantining it restored successful server transactions.
```

We should neither claim "we directly corrupted it" nor "Hyundai alone caused
it." A project-induced full filesystem triggering fragile vendor persistence
is plausible, but a key/state mismatch or another interrupted write remains
unexcluded. Proving which requires a verified before/after cache and key-state
history or a contemporaneous write-error trace, not repeated deletion or a
destructive disk-fill experiment on the car.

### Prevention versus repair

Preserve the already-landed bounded-log/backup-retention fix when merging this
work. Do not ship automatic cache removal on a generic BlueLink/network failure.
Any future firmware-side durability repair would need hash-gated development,
offline short-write/ENOSPC/power-interruption tests, and independent car
validation: preserve the preceding good cache, check persistence errors, and
allow an invalid optional cache to degrade to no saved navigation state rather
than disable all MOF processing. No such vendor-binary patch is shipped here.

## Repair and observed result

After explicit approval, on the parked owner's vehicle:

- Verified the file hash against the diagnostic snapshot and refused an
  existing backup destination.
- Stopped only `mof.service`, rechecked the hash, and renamed the cache to an
  adjacent `.bluelink-repair-20261117.bak` file. Original bytes were retained.
- Restarted only `mof.service`; no binary, key, provisioning, or routing changes.

| Before | After |
|---|---|
| Four MOF threads; startup exception | Workers started: 22 threads initially, 11 in a later sample |
| Request queue 44,352 bytes | Queue drained; later samples held only small live messages (80 B) |
| No external MOF TCP session observed | External established server connections appeared |
| No demonstrated successful server transactions | Valid token; speed-camera/weather HTTP 200; traffic/diagnostics HTTP 204 |

Some old queued requests referenced files already gone and produced local
`FileNotFoundException`/400 responses. Those do not negate the fresh successful
200/204 transactions, but are another reason to test a new user request rather
than treating replayed queue contents as a complete acceptance test.

## Investigation errors and corrections

### We overinterpreted public-network tests

Failed TCP/ICMP to public addresses was incorrectly presented as proof of a
suspended SIM, expired subscription, or modem forwarding failure. Carrier DNS
worked, but neither that nor public-address timeouts established BlueLink
endpoint reachability. MOF's own connections later answered over the same
cellular path.

Endpoint-level TCP reachability is not a valid health check. After the repair,
every plain TCP/443 connect to `103.187.35.x` still timed out — from the car,
from a Dutch public host, and from the house Internet. On the probed server,
`443` and most other ports returned no answer at all, while only `8010`,
`8011`, `8306`, and `8308` produced an immediate RST. MOF's own flows still
completed and returned HTTP 200/204. Service-side filtering is the remaining
locus, not the car: a bare `socat`/`nc` probe to the endpoint proves nothing
either way.

The earlier source-bound context-2 test also did not prove context-2 egress:
binding a source address does not replace route selection.

### We did not initially verify probe tools and return codes

The head unit lacked `timeout`; tests wrapped in it never ran and initially
produced false failure claims. `socat -T` alone also did not bound connect time
as assumed. A watcher later treated any exited TCP child as success without
checking its status. Its `RECOVERED` criterion would not have been reliable.
No such recovery was observed, but the scripts must not be reused as validated
health checks. A future probe must verify prerequisites, bound the operation,
check the actual exit code, test a relevant endpoint, and distinguish timeout
from refusal and command failure.

### Environment ruled out

The incident SIM (`204-04`, Vodafone NL) was roaming: registered on `206-10`
(Orange Belgium), automatic selection, with only that PLMN visible. These were
checked and are not the cause:

- APN contexts `hmg.eu` and `hmg.ota.eu` were byte-identical to the factory
  copy dated 2025-04-29, and the regional MOF configuration had loaded.
- Provisioning reported full enrollment (`BroadcastCurrentProv=1`,
  `GetNetworkReject=0`, modem `HMC5WR18`).
- Both contexts were dual-stack, but the PDP carried no IPv6 settings and the
  interface had no IPv6 address, so IPv6 was not an alternative path.
- A full modem power cycle re-registered LTE, returned the same context-1
  address, and did not change the failure.
- The head-unit firewall had zero `HARDEN_DROP` counters and both contexts had
  correct routes.

None of these changed the startup exception; the repair did. Future
investigations should not re-derive them from scratch.

### Access failures were mixed with application failures

The reverse tunnel changed destination: a host-side listener eventually
forwarded to the **Pi's** SSH, not directly to the car. Its key matched the Pi's
already-known key. The car used SSH port **2224**. A second hop over USB then
worked with the car's known key.

A listening reverse port or an ARP reply did not establish a working car SSH
session. A silent forwarded socket did not establish a completed remote TCP
handshake. Earlier claims that the car was necessarily rebooting, mid-install,
or powered off were not justified by those observations.

The detached V23 reapply log was terminated and had no completion result. Its
pre-install status already showed the V23 build ID. Therefore that launch was
not evidence of a completed install or the cause of a subsequent outage.

### Process existence and intermediate state logs were false health signals

`MOFManager` was alive, provisioning reported full enrollment, and MQTT logged
a `ConnectSuccess` **prestep**. None proved service initialization or broker
connection. The decisive evidence was the startup exception, exact-file crypto
failure, queue backlog, and their reversal after the targeted repair.

## Repository follow-up

- V22/V23 modem-safe route and USB-SSH changes are landed on `main`
  (`4d0bfd1`), preserving the newer log-retention fixes.
- Document the diagnostic layers and the narrow manual repair in the
  [BlueLink guide](architecture-and-diagnostics.md).
- Do **not** add blanket cache deletion, key regeneration, provisioning reset,
  or automatic modem cycling to package installation.
- Combined builds after merging newer `main` changes still need their normal
  offline gates and car acceptance; the observations above describe the V23
  car build, not an untested new bundle.

## Evidence and scope

Raw logs and captures remain private, outside tracked source; they contain
vehicle/account/location information. This report is a sanitized analytical
summary, not a vendored OEM artifact or decompiled source listing.

Matching `MOFManager` SHA256:
`84e3fd380feda27a6bd9971b2e24589900d1bef9d7729ce591181373b9323872`.
Static evidence came from the matching captured binary and `libmofgen2`;
runtime checks used the owner's Pi reverse tunnel and car USB SSH.

`/app/data/Mofgen2/EUROPE/mofgen2.conf` is encrypted. It measured 3416 bytes
before the repair; the repair restart rewrote it to 3418 bytes, so a byte-level
comparison of the causal state needs the pre-repair copy, not the current file.
Other persistent encrypted inputs on this path include
`/app/data/provisionInformation.dat` (1268 bytes) and
`/app/data/Mofgen2/mof_proving.bin` (8 bytes).

Relevant retained REA records:

- `ev_5d7cb2d5e51a8defd8d8eeb83f6efebf2f17e7f2ebc9beb2afd6902f986d917f`:
  saved-navigation cache reader, missing-file branch, decryption call.
- `ev_a9986b64f3d75e81e452636bd3c4dcd478b5ac42dc4b8c4781c203db0418e805`:
  crypto D-Bus request, failure retries, exception propagation.
- `ev_06c95bc57712436e88d0188d3cacd5bb6e7d7f9bdfff92ab49505a7344f9963b`:
  regional configuration and CryptoService destination.
- `ev_fd350e2220eab15c45de0d6cbbea0a9d96394a89eab6d3527b838e6c5e9821e7`:
  saved-navigation writer; in-place truncation and unchecked persistence calls.

Static analysis explains control flow; it does not by itself establish runtime
success. The live exact-file failure and post-repair HTTP responses are the
corroborating observations. No phone-app remote-command success or reboot
persistence is inferred from them.
