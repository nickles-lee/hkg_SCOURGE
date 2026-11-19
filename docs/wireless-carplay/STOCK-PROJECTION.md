# Stock wireless projection: static findings

**Decision for `NE_E.EUR.S5W_L.001.001.260128`: keep the stock receiver and
leave `AppProjection` unmodified.** The firmware has separate receiver
startup, phone discovery/connection, and USB iAP2 start paths. What remains
unproven is whether the first-party bootstrap leads the stock application to
select the phone and complete an authenticated projection session.

## Evidence

Addresses below are ELF virtual addresses; Ghidra in this analysis adds
`0x100000`. The evidence is disassembly, REA/Ghidra references, and
decompilation of the following owner-supplied artifacts. No OEM code was run.

| Artifact | SHA-256 |
|---|---|
| `AppProjection` | `8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3` |
| `libCarPlayAppFramework.so` | `e3603e6b0744cf07724cc5cc79ea01ea3d9207d7eaf37c544c7ce26536ecd7a2` |
| `libAirPlay.so` | `ef9e69e3b3388d4a7a1d185827c710af25b8ee99d406c60c664a9609b00aada0` |
| `iAP2Service-2.0` | `38419131df82ec7e3c3b29a68ecc1a078c7f0b403051f137fe9f46f82b99f26e` |

1. **Receiver startup already exists.** `AppProjection` at `0xd15a6`
   passes `0xeb0b0` to `pthread_create` (`0xd15cd`). This thread
   initializes the framework at `0xeb126`, supplies config at `0xeb952`,
   audio config at `0xebcff` and a delegate at `0xebe3e`, then calls
   `CarPlayAppFrameworkStart` at `0xebfeb` (PLT `0x17b00`). The cached
   pseudocode missed that call; REA's reference and the ELF agree. In
   `libCarPlayAppFramework.so`, `CarPlayAppFrameworkStart` (`0x9809`)
   launches the engine thread (`0x79b0`), which creates the receiver and
   control client, installs receiver/session callbacks, invokes
   `startServer`, and starts the control client. This does not prove which
   runtime conditions enter the outer application startup path.
2. **The receiver advertises itself.** In `libAirPlay.so`,
   `AirPlayReceiverServerControl` (`0xedab`) creates/starts the HTTP
   listener and calls the Bonjour update (`0xd4a4`). Registration
   (`0xcde5`, `DNSServiceRegister` call at `0xd390`) uses the listener's
   actual port and, if specified, the configured mDNS interface index.
   This traced segment has no USB-session check. It does not prove that
   DNS-SD is working on the candidate AP. The older port-7000/455
   observation establishes neither AP-side discovery nor session readiness.
3. **Stock has a separate outbound wireless control path.** The control
   client browses `_carplay-ctrl._tcp`; the framework's
   `handleControlClientEvent` (`0x15afc`) tracks controllers and dispatches
   discovery events. `AppProjection` calls
   `CarPlayAppFrameworkControlClientConnect` at `0xf9b66` with a controller
   object. That helper is reached from two stock call sites (`0xbaecc`,
   `0xeaa4d`); it is not an unconditional part of receiver startup.
   The framework wrapper (`0xee94`) calls
   `CarPlayControlClientConnect` (`libAirPlay.so:0x25d86`), which expects a
   controller belonging to that client. Its dispatched worker (`0x24022`)
   resolves the controller's endpoint and sends the stock connection
   request. By contrast, the application calls `iAp2AppStartCarPlay` at
   `0x110f93` and `0x11584a` in separate paths; the presence of those calls
   is not evidence that wireless receiver startup or selection requires
   USB. It is not yet established whether this phone, after the broker's
   offer, needs the outbound request or already initiates incoming setup,
   nor whether the firmware selects it automatically.
4. **Connection is not projection.** The framework installs
   `handleSessionCreated` and `handleSessionFailed` (`0x1582e` and
   `0x15a1b`) on the receiver. The screen-stream Started and Played callbacks
   are later, distinct milestones. No evidence yet ties the first-party
   offer to successful OEM pair-verify, session creation, or a first frame
   on this vehicle. Do not spoof any of these milestones or credentials.

## What to do next

| Observation on the parked car, with wired phone absent | Next decision |
|---|---|
| AP-side `_airplay._tcp` absent | Check stock process/listener and local vs AP DNS-SD, interface scope, and mDNS delivery. Absence alone does not imply a startup patch. |
| Receiver is visible but the offer names a different endpoint | Correct the candidate's receiver inputs using independently corroborated stock state. The broker's port is config-supplied; do not assume `7000` equals SRV, or treat TXT alone as proof of identity. |
| Phone joins and receiver matches, but no session setup appears | Observe both directions: phone `_carplay-ctrl._tcp` discovery, stock controller selection/outbound request, and any phone-initiated setup. If selection is the missing step, investigate the existing OEM UI/selection path before considering any change. |
| Connection is attempted but no session or frame follows | Distinguish stock authentication/setup refusal from successfully created sessions with no screen stream. Preserve authentication and don't count discovery, a request ACK, or a listener as projection. |

The existing `cp_oem_receiver` parsers and pins are reused.  The candidate
helper now has a bounded, read-only preflight wired into its entry point
(WIP, offline-only, not car-validated):

- `--check-receiver` browses and resolves up to four distinct
  `(interface_index, service_name)` instances on the configured interface.
  Duplicate Add/Remove events for one instance are not ambiguity; each
  distinct candidate is resolved once and evaluated by policy.
- Policy requires an observed SRV port, a `deviceid` consistent with the
  configured Bluetooth MAC (+1; the `..:FF` boundary wraps in our derivation
  and is recorded by test, not car-observed), a `srcvers`, and a present TXT
  `pi`.  An optional `receiver_pi_sha256` pin is
  compared by hash only; the raw `pi` is never copied into status or errors.
- The advertised port must have a local LISTEN socket on a wildcard or the
  receiver-interface address.  Loopback-only is `listener-scope`.  The
  socket inode is mapped to a PID with a bounded `/proc/<pid>/fd` readlink
  scan; the owner must be allowlisted (`AppProjection` by default) or the
  result is `listener-owner`.
- The diagnostic classifies the daemon snapshot (`fresh`, `stale`,
  `missing`) without letting a stale snapshot block the receiver checks.
  The runtime path still requires a fresh snapshot and refuses before it
  creates `/run/wifi1b/carplay/rx.sock`.

This is still an observer plus policy, not receiver attestation.  It has not
been run on the car, it does not prove AP-side DNS-SD delivery or phone
selection, and a passing result is not projection.  The parked-car D1
receiver probe in the validation plan remains the evidence step that can
corroborate a running receiver.

Keeping stock projection is the smaller integration: it already owns the
receiver, rendering, input, audio, and wired coexistence paths. LIVI remains
useful as a bootstrap reference. Replacing the receiver would add those
integration tasks without resolving the current car-specific uncertainty
about discovery and phone selection.
