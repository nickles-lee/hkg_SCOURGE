# Confirmed review fixes — isolated follow-up

## Resume point

Branch: `wireless-carplay-confirmed-fixes`, originally based on `c850b4e`.
Upstream `d46cc23` was merged (not rebased) as `e06b4e9` for the separate
[V23 integration](V23.md). Source branches and earlier releases are preserved.

This worktree isolates follow-up fixes from concurrent changes in the original
checkout. Existing commits, including `2b9226f` and `0d6b84b`, are preserved.
Their review trailers do not establish closure of the findings listed below.
The owner has authorized reviewed commits and a sanitized push of this separate
branch. That does not authorize deployment, replacing existing releases, OEM
execution, or vehicle validation.

The earlier C1 protocol checkpoint is `6e4d7f1`. Reviewed follow-up commits:

- `6ffd3e7`: SPP build-output cleanup and fault tests.
- `dd970f3`: passive receiver pin verification and bounded test execution.
- `cc18b9d`: authentication initialization, RPC errors and native build cleanup.
- `8287120`: bounded session receive bursts and generation-bound offer holds.
- `3815f5f`: bounded/deferred EAKs and refreshed retransmission ACKs.
- `8014e6c`: scoped DHCP address/route ownership, lifecycle and kernel readback.
- `4114142`: typed installer/restore destinations, early reports, bounded
  ownership cleanup, key rollback and exclusive validated diagnostic readback.

Each has one independent Flash-Max PASS covering its committed source/tests.
Runtime integration remains WIP; source and mock tests do not establish
working wireless CarPlay or physical USB coexistence.

## Required fixes and verification

| Area | Confirmed findings | Current follow-up |
|---|---|---|
| SPP build output | Temporary-output cleanup could omit remaining-artifact errors or remove replacement files | Independent Flash-Max PASS, reviewed tree `34fe7701`; creation-error, persistent-denial, replacement-preservation and descriptor-cleanup regressions pass; build-only, no activation |
| Passive receiver pins | Leaf symlinks/nonregular inputs, growing-file read bound and test hang risk | Independent PASS; committed `dd970f3`; offline only |
| Installer cleanup | Current-state archive authorization and unreadable process evidence | Independent PASS; committed `4114142`; V23 combined artifact gate separate |
| Installer ownership | Incomplete destination-ancestor checks; foreign service runtime preservation | Independent PASS; committed `4114142`; wrong types and unknown state preserved/refused |
| Installer lifecycle | Restore mode preservation, current masks, cleanup failure reporting and bare-parent archive path rejection | Historical independent PASS at `4114142`; 28 side-by-side groups and main early-report matrix PASS; V23 has additional gates |
| Session stream | Coalesced and reorder-completion bursts could overflow deferred storage | Independent PASS; committed `8287120`; combined link integration tests pass |
| Session lifecycle | Duplicate starts, hidden/new USB identities, zero-clock policy, inactive readback and changed-AP hold invalidation | Independent PASS; committed `8287120`; trusted receiver evidence still required |
| Link output | EAK backpressure/capacity/truncated follow-up and stale retransmission ACKs | Independent PASS; committed `3815f5f`; unit/sanitizer validation only |
| OEM authentication | Initialization failure bypass and hidden earlier RPC errors | Independent PASS; committed `cc18b9d`; authentication activation blocked |
| Authentication hardening | Loading/output gates, caller deadline, rollback reporting and production-build checks | Independent PASS; committed `cc18b9d`; OEM/chip calls are not bounded by the caller timeout |
| DHCP/network | Pending ownership, mode-client worker supersession/shutdown, explicit router requirement and route rollback evidence | Independent PASS; committed `8014e6c`; strict/sanitizer/private-namespace gates PASS. V23 route-policy changes require new combined review |

Each milestone requires one independent DeepSeek 4.1 Flash-Max PASS covering
the exact commit contents. A green old suite or an earlier commit trailer is
not a substitute. Keep unrelated milestone changes out of each commit.

## Tests repeated in this worktree

- `python3 packages/wifi1b/tests/test_carplay_spp_patch.py`: PASS, including
  persistent cleanup-denial and replacement-file regressions, normal and
  ASan/UBSan hook tests, and read-only pinned firmware patch verification.
- `python3 packages/wifi1b/tests/test_carplay_spp_socket.py`: PASS, normal and
  ASan/UBSan first-party fixtures. No live Bluetooth connection was tested.
- OEM auth and async auth Python fixture suites: PASS after verifying the
  exact six-file auth review manifest.
- Link, bootstrap and session suites: PASS together against `3815f5f` link
  and `8287120` bootstrap/session, normal and ASan/UBSan; bootstrap oracle
  passes. This closes the frozen-link integration condition for those bytes.
- `python3 scripts/policy_check.py tests`: PASS after the auth checkpoint;
  this is the VM unit-test gate, not a booted-vehicle test.

Network and installer fixtures and independent review now pass their committed
checkpoints above. Earlier failed candidates are superseded, not release
evidence. The historical `4114142` installer repeat passed 28 side-by-side groups in
268.8 seconds; the earlier 240-second aggregate timeout was too short and was
not recorded as a PASS. The earlier source-only release test reported SKIP.
After the coherent private V23 rebuild, the actual release-artifact gate now
passes, including source/build identity, manifests, metadata and ZIP bytes.
The final pair audit also passes; previous releases have not been replaced or
published. See V23.md for the offline evidence and remaining target gates.

The V23 precursor installer suite passed 31 groups in 373.0 seconds. Subsequent
review found missing prerequisite-failure guards before cleanup. After those
were fixed, 32 groups passed in 406.4 seconds, including stop/active-removal
failure preservation, known V21 override upgrades, USB cleanup failure
preservation and older-installed-CLI avoidance. The corrected V23 installer
scope now has its own independent PASS. Combined artifacts pass their checks;
the final exact-tree review remains a separate delivery control. This work
does not inherit the historical 28-group approval.

Actual iPhone interoperability, real MFi operations, trusted receiver
attestation/observation, target loader/dependencies and USB coexistence remain
unverified. Preserve GB, owner credentials and all three SSH keys, modem
routing, wired USB, Battery/Tweaks/Reboot and shared micom behavior. The V20
and V23 deployable layout remains the two-file wrapper/archive pair.

V23 adds independently reviewed, offline-tested asynchronous completion wiring
and `cp_auth_broker`, which services session/link deadlines around nonblocking
helper calls. The SPP transport also has an independent offline PASS. The new
runtime broker joins those components and now has an independent offline PASS,
including stale-epoch isolation and immediate failed-feed/TX retirement. None
is activated by the daemon. Trusted target receiver mapping remains gated.
Do not use the synchronous session auth path as a real-time link solution.
Branch delivery and release evidence are recorded in V23.md; no deployment or
hardware validation is implied.
