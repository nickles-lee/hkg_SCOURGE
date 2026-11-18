# Wireless CarPlay implementation checkpoints

## Resume here

Current branch: `wireless-carplay-confirmed-fixes`.
Current V24 integration and release gates: [V24.md](V24.md).
Historical V23 offline checkpoint: [V23.md](V23.md).
Reviewed defect checkpoints: [CONFIRMED-FIXES.md](CONFIRMED-FIXES.md).
Exact stock-gap/patch/address map for reproduction:
[REPRODUCTION.md](REPRODUCTION.md).
The original `wireless-carplay-integration` branch is preserved as history.

Goal: complete a wireless bootstrap compatible with the pinned LIVI reference,
while retaining the OEM projection/media engine and unchanged wired CarPlay.
This is work in progress; **no end-to-end iPhone or vehicle validation yet**.

The original worktree is frozen in the staged baseline tree
`a1243f0d913f9948320a2612373d6e8e6abb5f4e`, based on `01965fa`.
Baseline committed as `4279a70` after the independent DeepSeek review passed.
The two initial blocking findings were corrected; see BASELINE-ISSUES.md for
pre-existing WIP that still gates release. No implementation files were included
in that preservation commit.

Owner policy update: one independent DeepSeek V4.1 Flash-Max review per commit
(`deepseek-v4.1-flash#max` / `deepseek v4.1-flash-1017#max`, provider not
pinned); remove the previous two-model requirement. GLM was unavailable
(provider budget).

## Milestones

| Checkpoint | Work | Status |
|---|---|---|
| C0 | Preserve existing WIP; update review policy; dedicated branch | Complete: `4279a70` |
| C1 | Bounded wire codec, identification and bootstrap state-machine tests | Offline independent PASS: original `6e4d7f1`; auth/session/link follow-ups `cc18b9d`, `8287120`, `3815f5f`. No phone validation |
| C2 | AP IPv6/readiness/firewall and current-network snapshot | Offline/namespace independent PASS at `8014e6c`; V23 modem-policy integration and target behavior require separate gates |
| C3 | Recover and verify OEM Bluetooth RX/TX/listener and MFi/identity ABI | Static research complete for SPP; live bus/transport proof still pending |
| C4 | Integrate wireless transport and authenticated bootstrap | Deferred auth, SPP transport and serialized runtime broker independently reviewed offline. All remain unactivated; trusted target receiver integration and live phone/OEM validation still gated |
| C5 | Connect existing OEM discovery/receiver and lifecycle observations | Offline passive-probe/pin follow-up PASS at `dd970f3`; trusted live receiver mapping and lifecycle integration pending |
| C6 | Installer/restore, regression gates, experimental release pair | V23 independent offline review and fresh private build/artifact gates PASS at `42f5e4b`, including 32 side-by-side groups; phone/car proof remains unverified and optional firmware SPP readback remains blocked |
| C7 | Parked vehicle/iPhone validation, explicitly owner-authorized | Not authorized |

At each checkpoint record: exact commit/tree, commands and results, incomplete
coverage, and the next action. Never equate a mocked exchange with vehicle
validation. Update this document before ending a work session.

## Non-negotiable boundaries

- Country GB; preserve owner credentials/keys and station-network priority.
- Preserve stock USB, modem routing, shared micom helper, Battery, charge door,
  cluster, Tweaks and approved Reboot behavior.
- New standalone V24 must not reinstall WiFi or require migration. Retain existing
  released bundles; do not publish an experimental build as a verified fix.
- First-action bounded cleanup remains first. Stop owned radios in installers.
- Firmware-specific binary modifications require exact hash and byte gates.
- No OEM binaries, copied decompilation, credentials or fetched third-party
  source in Git. Fetched dependencies live pinned under ignored `third_party/`.
- No global radio/Bluetooth replacement, guessed ABI, host execution of OEM
  binaries, unconditional launcher-wide preload or authentication bypass.

## Audit baseline and evidence

Reference LIVI tree: `64a05b68358db52ed887169143afd5944fd6f1f7`.
Principal reference modules: `iap2-csm`, `iap2-link`, `livi-helper-stack` and
`livi-wifi`. References supply protocol facts and test expectations, not a
claim of formal certification. Preserve required attribution if code is reused.

Exact owner-firmware SHA256 identities:

| Artifact | SHA256 |
|---|---|
| AppProjection | `8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3` |
| iAP2Service-2.0 | `38419131df82ec7e3c3b29a68ecc1a078c7f0b403051f137fe9f46f82b99f26e` |
| vbtd-63 | `c56b68fdd35b16b95688ab54a48ffb9f18034d925c057e86f8b9fe304f8b26b9` |
| vbtd-51 | `55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0` |
| libiAP2AppFramework-2.0.so | `d4d0558c9c53f02ee14024c081ccd83a02ca29404ef400131e2951f68a388120` |
| libCoreUtils.so | `9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae` |
| libAirPlay.so | `ef9e69e3b3388d4a7a1d185827c710af25b8ee99d406c60c664a9609b00aada0` |

Cached Ghidra addresses are ELF virtual addresses plus `0x100000`. Do not mix
those coordinates with ELF offsets or ASLR runtime addresses.

### Confirmed static gaps

- Active Wifi1b backend is `wifi1b_backend.inc`, not disabled ConnMan code.
- vbtd-63 `SetSppListen` (ELF `0x3b5630`) is empty; valid-event
  `VBTSppReceivedDataCfm` (`0x491c90`) returns without forwarding (assembly).
  Other possible SPP paths still require investigation.
- Normal identification (`iAP2Service` ELF `0x207fa`) never selects Bluetooth
  builder selector 4 or wireless builder selector 9. Dormant Bluetooth builder
  serializes its MAC through a string helper; verify six-byte wire encoding.
- Main iAP2 receive dispatcher (`0x13afa`) has no `0x4300/0x4e0d/0x4e0e`
  branches. Existing StartCarPlay API emits internal IPC `0x2aff`, not proven
  wireless wire message `0x4301`.
- AppProjection delegate registered at call `0x11a130`: table at object+`0x30`,
  callback+`0xf8` remains null. Library handler (`0x15773`) silently skips it.
- Credential response export (`0x15157`) has a connection-context argument
  passed by value. AppProjection at `0x110f77..0x110f93` pushes six qwords from
  the context at object+`0x18` for an existing framework call. Recover complete
  context semantics before calling any serializer.
- AP readiness lacks IPv6 checks; address flush is unqualified. Latest station
  capture does have a link-local address, so absence is not a confirmed cause.
- Owned IPv6 firewall omits stock UDP 5020 and TCP 6030; IPv4 AP chain currently
  accepts all traffic on wlan0. Do not accidentally broaden remote control APIs.

### Reuse rather than duplicate

OEM CarPlayAppFramework starts its CarPlayControlClient; libAirPlay browses
`_carplay-ctrl._tcp` and implements `/ctrl-int/1/connect`, including scoped
addresses and receiver identity. Verify activation/binding on wlan0 first.

libCoreUtils exports existing MFi certificate/signature providers and pairing
identity APIs. Its provider uses the existing chip and synchronization. MFi4
is not automatically mandatory. Never synthesize successful authentication.

### Protocol invariants

- iAP accessory-server UUID ends `caff`; phone-client UUID ends `cafe`.
- CarPlay service UUID is `ec884348-cd41-40a2-9727-575d50bf1fd3`.
- `0x5703` security 2 is not the `0x4301` enum: omit security for WPA2 in the
  latter, and use 3/4 only for negotiated WPA3 transition/only.
- Identification, discovery and session-start identity fields must agree;
  Bluetooth MAC, AP MAC and pairing identifier have distinct roles.
- Session-start uses the network actually on air and a usable scoped endpoint.
- AP, pairing, authentication, session negotiation and active projection are
  separate states, with generation-bound teardown/reconnect handling.

## Acceptance gates

1. Wire roundtrips and fixed external expectations; malformed lengths/options,
   fragmentation, coalescing, checksum, sequence wrap and bounded retries.
2. Authentication failures, identification rejection, early/out-of-order
   availability, AP failure/channel fallback, transport loss and cancellation.
3. Namespace network tests: IPv4/IPv6, link-local retention, firewall packets,
   DHCP renewal/rebind and owned-state cleanup; no modem/global changes.
4. Fake OEM ABI tests and hash-gate refusals before any instrumented guest run.
5. Existing package/Qt/installer/combo tests and preservation/restore checks.
6. Real parked-iPhone trace: transport → identification → authentication →
   credentials/start → IP/discovery → pair-verify → SETUP/RECORD → media/input.
7. Reconnect, long session, ignition restart, USB insertion/removal, calls/Siri,
   credential redaction and bounded disk/log growth.

## Historical C1 worktree verification

The tree hashes and review chronology below describe the original checkpoint,
not the complete current branch. Use the milestone table and linked V23 ledger
for subsequent reviewed changes and remaining gates.

New source modules: `packages/wifi1b/src/carplay/cp_wire.*`, `cp_bootstrap.*`,
and `cp_link.*`. They are not activated by the installer or existing daemon.

Commands repeated by the main agent:

```
python3 packages/wifi1b/tests/test_carplay_bootstrap.py
python3 packages/wifi1b/tests/test_carplay_link.py
```

Both pass. Bootstrap uses an independent Python wire oracle. Link tests include
strict normal and ASan/UBSan builds, fixed SYN/ACK vectors, fragmentation,
backpressure, sequence wrap, retransmission, reset and negotiation limits.
Agent-reported additional compiler/fuzz checks are useful but not a substitute
for the required independent review or real transport validation.

Next: independently review these new modules; integrate a serialized caller
with CSM reassembly, generation invalidation and backpressure. Existing code
deliberately has no fake production authentication provider.

Initial independent C1 review **FAIL**, tree
`e7515d46d08892f859167b0f88c2723b519ac40a` (do not commit this tree):
identification failure could be resurrected; a late malformed rejection left
authentication inconsistent; a false EAP flag was encoded as present/true;
leading IPv6 compression could evade link-local validation; unknown optional
TLVs were rejected; coalesced ACK bursts could exhaust the link output queue;
and session-id zero caused a sequence gap. Corrections must add regression
tests, re-run the exact index snapshot and obtain independent PASS. Passing
the original unit tests was insufficient to establish these semantics.

Additional review follow-ups: explicit live Bluetooth link profile, bounded
post-offer policy/receiver timeout, accurate session-ID direction claims and
SYN retransmission ACKs, documented local certificate/frame bounds, explicit
policy for absent availability, and automatic bootstrap sanitizer coverage.

Corrections are now implemented: terminal identification gates and safe late
rejections; absent false EAP flag; leading-compression rejection; skipping
structurally valid unknown optional TLVs while keeping known duplicates and
malformed fields invalid; coalesced/deferred cumulative ACKs; sequencing of
session-id zero; explicit live-Bluetooth preset; current-ACK SYN retries; and
bounded post-start offer policy checks. Omitted availability remains an
explicit conservative refusal. The reference's hard-coded session IDs do not
prove arbitrary peer-ID negotiation. Certificate/frame bounds remain local
policy, not measurements of the vehicle.

The bootstrap runner now includes ASan/UBSan and a malformed-TLV corpus.
`scripts/check_carplay_index.py` extracts the explicit protocol/test allowlist
from one frozen index tree and checks that the index did not change during
testing. Independent Flash-Max re-review **PASS** for corrected tree
`bd674f4ee5c54aecd2351d85201dcc5d4ad06415`: 3216 bootstrap checks and 26
link groups pass normal and ASan/UBSan builds from the frozen index; staged
policy and whitespace gates also pass. No phone/vehicle validation is implied.

The second C1 review (`00404e80a0f13ba24f1eb2b1dca288296267c28c`)
confirmed B1–B7 fixed but remained **FAIL** for malformed late authentication
duplicates. AA00/AA02/AA05 state checks now precede parameter decoding;
regressions require an authenticated session, unchanged generation/provider
call counts, and a subsequent valid start. That final delta is covered by the
PASS above; the remaining change before commit records this verdict only.
Empty-message decoders remain strict; unknown-TLV tolerance applies to messages
with parameter tables. An outer wrapper must retire the receiver-wait deadline
on a trusted active observation while retaining consent/USB checks. Timeout
alone does not establish that the OEM receiver failed or stopped.

## Historical later components (outside the original C1 reviewed index)

The pending-review statements in this historical section record that earlier
index only. SPP cleanup, auth, receiver-pin, session, link, network and installer
follow-ups have since landed; their exact commits and limitations are recorded
in CONFIRMED-FIXES.md. They do not establish working wireless CarPlay.

`cp_spp_socket.*` adds an unactivated, root-only Unix datagram receiver for the
build-only Bluetooth RX hook. It checks kernel sender credentials, complete
bounded datagram framing and socket-directory ownership; it does not establish
Bluetooth connection lifecycle, authentication or projection on its own.

```
python3 packages/wifi1b/tests/test_carplay_spp_socket.py
```

Main-agent result: strict production compilation and normal/ASan/UBSan fixture
tests PASS, including a synthetic event through the real first-party hook to
the credential-checked receive decoder. Fixtures use the current uid and
private temporary paths through a
compile-time-only testing interface. Root-owned production binding and rejection
of an actual different-uid sender still need privileged guest validation.

Parallel work remains pending review: AP readiness/firewall, hash-gated RX hook,
OEM MFi adapter/helper, protocol-session wrapper, standalone cleanup/restore and
OEM receiver integration research. Do not activate or publish these components
merely because their individual mocked tests pass.

The SPP author completed the build-only hook/builder and exact vbtd-63 pins.
Author-reported positive build/readback changes only the pinned function span;
no OEM execution or installer activation. A separately reported initial C3
review failed because the patcher CLI accepted replacement firmware pins.
The CLI override has been removed; output/symlink/toolchain corrections and
tests are under re-review. No C3 PASS or activation is claimed by this index.

## OEM receiver research checkpoint (static, not live validation)

Additional exact artifact identities:

| Artifact | SHA256 |
|---|---|
| libCarPlayAppFramework.so | `e3603e6b0744cf07724cc5cc79ea01ea3d9207d7eaf37c544c7ce26536ecd7a2` |
| libCarPlayMediaFramework.so | `69690210d1aa4dec33c77b0e80f1d3368f8a70bcb507c1b158b6245dc29eb2f3` |

- libAirPlay `AirPlayReceiverServerControl` (ELF `0xedab`) handles
  `startServer`; there is no separate receiver-start export. OEM already calls
  it. Listener creation tries port 7000 and can fall back to an ephemeral port;
  obtain the current port from local `_airplay._tcp` SRV, not a constant.
- `interfaceName` scopes mDNS registration only; it does not bind listener
  sockets to wlan0. Do not change global receiver state or restart it for a
  wireless disconnect. USB preservation still needs a live test.
- Actual OEM deviceID normally comes from its configured Bluetooth address
  with the last byte incremented, with a primary-interface fallback. Do not
  derive a substitute; obtain the current receiver value.
- `AirPlayCopyHomeKitPairingIdentity` (ELF `0xcc9f`) calls
  `PairingSessionCopyIdentity` with create flag 1 and can persist a new keypair.
  A helper must never use that convenience wrapper or request private output.
- `PairingSessionCopyIdentity` (CoreUtils ELF `0x8e8c9`) with create flag 0
  performs lookup without the create path. Identity/public-key ABI and
  namespace constants require exact binding; public-key role must still be
  compared with pair-verify before use in session-start.
- `_airplay._tcp` TXT carries `deviceid`, `pi` and `srcvers` (`320.17.6`),
  but no `pk`. `pi` is the pairing account identifier, not the public key.
  Network advertisements alone are not trusted local readiness evidence.
- Screen-stream Started/Stopped/Played callbacks are imported through PLT by
  the media library, unlike vbtd's internal PIE RX callback. A future optional
  observation-only shim could forward those exact callbacks; no such preload
  is activated. Started and first-frame Played are distinct observations.

Next receiver step: bounded passive probe and fixture tests, with explicit
unverified-vs-trusted observation states. Do not create a second receiver,
identity or discovery client. Live listener, keychain-read permission, mDNS
scope, callback forwarding and USB behavior remain unverified.
