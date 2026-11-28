# hudnav prototype notes (historical)

**Historical document.** Its early "nothing car-validated" header was written before
the later car sessions. The current, authoritative status is [`STATUS.md`](STATUS.md):
car-proven on one AU 260319 car, untested everywhere else. WIP — not a supported
multi-car release. This is the original working skeleton of the forwarder daemon;
see `hudnav/HUD_NAV_REPORT.md` for the condensed design report.

## What is implemented and host-tested

- `src/hudnav.h` — shared types: input events, normalized maneuvers, native output, config.
- `src/convert.c` — maneuver→native conversion (Android prior-art tables), AA/CarPlay
  normalization (**full CarPlay EManeuverType 0..53 table**), distance bar, far icons
  (never 64). LHT/RHT handling (fixed 2026-11-24): only the roundabout exit angle and
  U-turn angle are traffic-side mirrored (measurement-confirmed); plain turn/slight/sharp/
  on-ramp angles are the physical angle, unmirrored — see the comment above the switch in
  `hn_convert`.
- `src/rgd_tlv.c` — **CarPlay iAP2 RouteGuidance TLV parser** (0x5201/0x5202), per
  `hudnav/CP_PROTOCOL.md`; whole-message validation (fail-open), normalizes to
  the common `hn_turn`. Integer widths PROVISIONAL pending a live capture.
- `src/motion.c` — distance glide (4 Hz, gain 0.8, back-step hold), maneuver-bound
  distances, lease + passed-maneuver expiry with one-time clear, link-stale clear,
  guidance-ownership gating.
- `src/output.c` / `src/output_can.c` — pluggable backend. CAN backend emits the
  **confirmed** micom wire format (`SetCANMicomRecv`, `[id>>8,id&0xff,0x08,data0..7]`,
  turn ID `0x4CC`) with a **PROVISIONAL** data-byte layout. Also emits `0x4E8`
  destination distance (tenths/unit/LE-int/flag, from CarPlay TLV `0x07`) and the
  `0x4EA`/`0x4E9` guidance/route flags.
- `src/daemon.c` — `hudnavd`: unix-socket line-protocol input, 4 Hz tick, kill-switch
  file (`--enabled`), `--can`/log backend, `--lht`/`--rht`.
- `src/aa_shim.c` — **AA interceptor prototype** (`out/aa_shim.so`): LD_PRELOAD shim
  interposing the three exported `NavigationStatusCallback` virtuals (vtable-entry
  override), calls originals first via `dlsym(RTLD_NEXT)`, forwards normalized records to
  the daemon. Fail-open. Compiles + exports the exact mangled symbols; interposition is
  VM-proven against the real vendor library (`hudnav/AA_PROTOCOL.md`). A
  2026-11-24 fuller-environment attempt (`hudnav/AA_PROTOCOL.md`) still did
  not deliver a live event — real AppProjection reaches D-Bus/Qt setup but SIGSEGVs after
  EGL init fails in the VM, and `hudnavd` logs only its banner. Next-turn arg order is
  PROVISIONAL.
- `src/cp_shim.c` / `src/cp_shim.h` — **CarPlay iAP2 interceptor logic model**
  (`out/cp_shim.so`): host-only LD_PRELOAD-style skeleton for `iAP2Service-2.0`.
  Patches outgoing Identify TLV 6 (sent) / 7 (recv) with the five BE `0x52xx` ids and
  advertises route guidance (`0x001E` / param 30), builds `40 40 00 06 52 00` plus the
  `[0x02][session]` link body, parses 0x5201/0x5202 through `rgd_tlv.c`, and emits the
  same `hn_turn`/DIST line protocol as `aa_shim.c`. Byte-level encoding from
  [cp_iap2_impl_spec.md](hudnav/CP_PROTOCOL.md); the param-30 value is
  still PROVISIONAL. Fail-open: malformed input forwards nothing.
- `src/cp_hook.c` (`out/cp_hook.so`) — **production CarPlay LD_PRELOAD hooks**: thin
  `open`/`write`/`read` interposers around the tested logic. Hooked `open` finds
  `/dev/ttyIAP20`; `write` patches Identify on a heap copy and submits 0x5200 once;
  `read` forwards 0x5201/0x5202 lines without altering the vendor buffer. Self-gated
  on `iAP2Service-2.0`, glibc ≤2.14 symbols only. `CP_HOOK_DUMP`/`CP_HOOK_LOG` capture
  the first four Identify frames and hook events for the first car session.
- `src/aa_shim.c` — **self-gated**: the launcher-wide preload from
  [aa_activation_path.md](hudnav/AA_PROTOCOL.md) maps the shim into all
  children, but it is inert unless `/proc/self/exe` is `AppProjection` (or
  `APPID=110`). `HUDNAV_FORCE=1` is a host-test override.
- `tests/test_core.c` + `tests/test_rgd.c` + `tests/test_cp.c` — **169 C host checks**
  (conversion + motion + destination distance + CarPlay TLV parser + Identify patch
  and registration), plus 10 AA self-gate checks (`tests/test_aa_gate.py`), 26 CarPlay
  hook checks (`tests/cp_hook_probe.c`), and the log-only handoff/frame replay
  (`tests/test_handoff.py`).
- `scripts/smoke.py` — synthetic AA maneuver stream → printed CAN frames.

## Build / test (WSL or any Linux host with gcc + python3)

```sh
sh scripts/test.sh      # builds all binaries and runs the unit tests and replay
python3 scripts/smoke.py  # end-to-end synthetic drive (no car, no D-Bus)
sh scripts/build-aa-diagnostic.sh  # builds the 260319 log-only Android Auto kit
sh scripts/make-car-kit.sh        # static hudnavd + both gated shims + HU scripts
```

### Firmware profiles (porting without editing C)

The firmware-specific literals used by `src/cp_rg.c` (offsets + fingerprints) and
`src/aa_shim.c` (mangled symbol names) now come from a checked-in JSON profile at
**build time**. The default is
`profiles/NE_E.AUS.S5W_L.001.001.260319.json`; all three build scripts accept
`PROFILE=profiles/<other>.json` as an override. They run
`tools/profile_to_header.py` once, generating `build/fw_profile.h`, which the two
sources include. The injected libraries do **not** parse JSON or files at runtime:
every existing fingerprint/hash/rel32/fail-closed check is unchanged.

```sh
# Derive a profile from another firmware's own stock rootfs (host-side, read-only):
python3 -I tools/port_check.py --input /path/to/stock-rootfs \
    --firmware-id <id> --output profiles/<id>.json --report
# Build with it:
PROFILE=profiles/<id>.json sh scripts/make-car-kit.sh out/car-kit-<id>
```

The build **fails with an explicit message** if a required profile row is missing,
ambiguous or malformed; there is no fallback to the 260319 literals. See
[tools/PROFILE_FORMAT.md](tools/PROFILE_FORMAT.md) for the schema and the required
row list. `tests/test_port_check.py` proves the 260319 profile reproduces every
previous hard-coded value, and `tests/test_profile_to_header.py` covers the failure
modes. With the default profile the built `.so` files are byte-identical to the
pre-profile build (hashes recorded in the build evidence log).

The [car-day runbook](../CAR_DAY_RUNBOOK.md) orders the first on-car session so
that "do directions appear at all?" is answered before data fidelity. The kit
installer touches only `/rw_data/hudnav`; the two `*-preload.sh` scripts each
change exactly one line of a data-only file (with byte-identical backups) and
can restore it. No script in the kit transmits CAN.

The [diagnostic kit notes](scripts/aa-diagnostic.README.md) cover the package,
firmware checks and log output. The kit is a background app with no home-screen UI.

### 260319 diagnostic loading and head-unit status

The kit was copied into the head unit's temporary filesystem over SSH. The head unit
verified both firmware hashes, started the log-only receiver, and removed its socket
when stopped. The receiver smoke test did not load the shim into AppProjection.

On the bench, the extracted 260319 dynamic loader checked the unchanged AppProjection
and its vendor libraries in three separate invocations. The status, turn and distance
callbacks resolved to the stock receiver library in the first and third invocations.
All three resolved to the exact packaged shim in the middle invocation. All three
loader checks finished without unresolved symbols. This was a linker check and did
not run AppProjection's application code or a live Android Auto route.

Read-only inspection of the live launcher confirmed that its launch calls accept an
app ID, name, path and optional test flags. The interface has no per-app environment
argument. The boot wrapper sets the shared preload for the launcher. A reversible,
AppProjection-only activation path remains unverified.

The vendor-kernel VM startup comparison used the packaged receiver and shim, a
private AppManager bus, and two display configurations. For each configuration,
AppProjection was started stock, with the shim, then stock again. The first
attempt omitted the receiver's private bus address. After correcting that setup,
the receiver opened its socket, and both shim runs resolved all three callback
symbols to the packaged shim. All six AppProjection launches crashed before the
nine-second readiness check, including the stock controls, and no navigation
callbacks reached the receiver. VM run `1795522822913836410` records the results.
The VM could not validate a working projection session or a return to stock.

The next bench pass restored the Mesa 11.2 and LLVM 3.8 fixture from the Ubuntu
Xenial archive. All 50 extracted files matched the lab's existing manifest.
The stock Qt graphics probe then loaded `swrast`, created an OpenGL ES 3.0 context,
and fired its QML timer. It hung during exit, so the graphics probe still failed.
VM run `1795523923090689562` records the result.
[Ubuntu Xenial package index](https://archive.ubuntu.com/ubuntu/dists/xenial/main/binary-amd64/).

With the rebuilt fixture, a stock AppProjection control claimed its D-Bus name
after eight seconds and exited with SIGSEGV at nine seconds. The readiness
check required ten consecutive name-and-process checks. Only one succeeded.
This run used the software EGL fixture and no Android Auto shim. Restoring
software graphics alone was insufficient to keep stock projection alive.
VM run `1795524032209079697` records the result.

A second stock run reproduced the crash under syscall tracing. Two attempts
to look up `can0` failed with `ENODEV` before a thread faulted at address
`0x18`. This confirms that the VM still lacks a vehicle interface that
AppProjection expects. The trace does not establish that the missing interface
caused the fault. VM run `1795524385342041629` records the trace.

When checking future VM runs, use the diagnostic receiver's `RX` records as
the callback delivery check. They are logged before ownership is applied.
AppManager ownership is required for the derived `FRAME` records.

## Offline comparison with the 2026-11-24 captures

Rebuilt and ran all 139 host checks, then fed normalized maneuvers through the daemon's Unix socket with the CAN backend in log-only mode. The reference frames came from the native drive log (not shipped). The roundabout replay used a synthetic source angle of 270°, which the existing LHT conversion maps to 90°. This checks the output encoder independently of the phone's angle convention.

- At the 90 m roundabout and the 140 m return-route left turn, `0x4CC` bytes 0–5 matched stock, including type, direction and little-endian metre distance. Stock bytes 6–7 were both zero in all 79 captured plain-turn and roundabout frames. The prototype emitted `03 00` at both comparison points and at the sampled 5 m roundabout point.
- The matching `0x63E` frames confirmed little-endian metres in bytes 2–3. Stock bytes 0–1 were `00 00` for the two maneuvers and trailing bytes 4–7 contained additional data. The prototype emitted `03 00` followed by the distance and four zero bytes. Other stock `0x63E` records had different prefixes, so the two matched prefixes alone do not establish a universal layout.
- Built-in navigation also logged `0x4E8` route-distance frames. The prototype now emits
  `0x4E8` from a `DEST` input (CarPlay TLV `0x07`); the frame layout and the two stock
  reference values (1.6 km and 800 m) are asserted in the replay. Android Auto's legacy
  callback surface has no destination-distance field, so AA runs leave `0x4E8` in its
  stock hidden form. Full decode: [`hudnav/HUD_NAV_REPORT.md`](hudnav/HUD_NAV_REPORT.md).

The parked captures confirm that AppManager broadcasts `110` for CarPlay, `111` for Android Auto, and `0` at cancellation. Native guidance uses `30`. During the CarPlay start, native navigation also called the setter with `30, false` without changing the reported owner. The daemon uses the ownership broadcast and accepts turn records from the corresponding projection source.

Before this fix, the daemon consumed only the socket's `OWNER` messages and both projection shims could assign ownership themselves. A log-only handoff replay reproduced three failures:

- Ownership gain or loss with no maneuver data produced clear frames. When the shim also reported active guidance, the daemon emitted guidance-on frames before it had a turn.
- A synthetic transition from projection to native ownership emitted guidance-off and clear frames after native took ownership. The next tick was silent.
- A later projection status and distance could reclaim the owner and restore the previous turn without receiving a fresh maneuver.

### Current prototype

- AppManager now controls ownership through a dedicated input. The projection shims report navigation data and state. The daemon stays silent when built-in navigation takes over or when the AppManager feed disappears. It discards the old maneuver when ownership changes and requires a fresh one before transmitting. A release to owner `0` clears once if the daemon had already displayed guidance.
- The 90 m and 140 m `0x4CC` payloads now match stock byte for byte. Both `0x63E` prefixes and distances match. The remaining `0x63E` fields and the approach-bar mapping are provisional.
- `0x4E8` now matches the stock layout; the 800 m reference frame is byte-exact and the
  1.6 km frame differs only in `B2` (stock alternates `0x1f`/`0x20`). The stock AU bar
  window fits ~341–350 m rather than the Android 457 m, so `bar_scale_m` stays a tunable.
- The host replay covers these two maneuvers, native takeover, release, stale projection input, and loss of the owner feed. The tests run without a live car. They cannot establish when the HUD renders guidance or whether iAP2 carried visual navigation data.

### AppManager input

The daemon's `--appmanager-stdin` mode reads the text stream from a monitor filtered to
AppManager and its D-Bus name changes. It starts with output inhibited until the first
ownership broadcast. A service restart inhibits output until a new ownership broadcast.
If the monitor disconnects, restart it and the daemon together. The invocation below
logs frames only.

```sh
session_bus=$(sed 's/,guid=.*//' /tmp/session_appmgr)
dbus-monitor --address "$session_bus" \
  "type='signal',sender='com.lge.hyundai.avn.appmanager',path='/Information',interface='com.lge.avn.appmanager.info',member='navi_route_guide_application'" \
  "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='com.lge.hyundai.avn.appmanager'" |
  ./out/hudnavd --appmanager-stdin --can
```

`--micom-send` requires `--appmanager-stdin`. The socket's legacy `OWNER` input
is available for log-only fixtures when no AppManager stream is configured.

## What is PROVISIONAL / pending (do not trust on a car)

1. **CAN data-byte layout** (`output_can.c`): the offline comparison above confirms the
   direction and distance encoding for the captured turns, and the `0x4E8`
   destination-distance layout. The remaining `0x63E` lookahead fields, `0x4E8` B2/B3
   semantics, and the approach-bar window still need verification
   (`hudnav/HUD_NAV_REPORT.md` §5–§7).
2. **HUD output**: stock AppNavi sends HUD guidance as **JSON** via
   `notifyBasicGuidanceUpdated_HUD(string)` (confirmed). The D-Bus method name + consumer
   are not yet identified; no HUD backend implemented yet.
3. **Interceptors**: AA shim source built (`aa_shim.so`); vtable-interposition mechanism
   proven against the **real** `/app/lib/liblinux-head-unit.so`, loaded by the real
   `/app/bin/AppProjection`, **in the real vendor-kernel VM**
   (`hudnav/AA_PROTOCOL.md`: `LD_DEBUG=bindings` A/B shows all three
   `NavigationStatusCallback` symbols rebind from the vendor lib to `aa_shim.so` with
   `LD_PRELOAD` set). Getting there required pinning `dlsym` to its pre-2.34 symbol
   version (`__asm__(".symver dlsym,dlsym@GLIBC_2.2.5")` in `src/aa_shim.c`) since the
   real rootfs is glibc 2.25 — a real, permanent fix, not VM-test-only. The AppLauncher
   `.sign` gate it depends on is likewise proven against the real 260319 binary in the
   real VM (`applauncher-sigstub` scenario, all 10 checks PASS). Still open: a live event
   has not yet been passed through the real vtable end-to-end (AppProjection was only run
   long enough to capture load-time bindings; a 2026-11-24 fuller-environment attempt
   (`hudnav/AA_PROTOCOL.md`, scripts `18_aa_`–`22_aa_`) re-confirmed the
   rebind with a real `hudnavd` listening but still got no live event — EGL init failure →
   SIGSEGV at `0x18`; the gdbstub call path was investigated, not tractable in this VM).
   Next-turn arg order provisional. CarPlay shim source
   is now built (`cp_shim.so`) as a **host-only logic model** — the Identify patch,
   message-ID registration, 0x5200 request, 0x5201/0x5202 parse and line-protocol
   forwarding are host-unit-tested. The interception symbol was identified offline
   (`hudnav/CP_PROTOCOL.md`): the dynamically linked
   `read@GLIBC_2.2.5` in `iAP2Service-2.0` — call site `0xddd9` in
   `Transport::recieverThread()`, `mReaderFd` @ object `+0x84`, count `0x101d0`,
   gated by `select@plt` @ `0xdce1`, GOT JUMP_SLOT `0x299dd0`, fd opened at `0xd55d`
   from `/dev/ttyIAP20`; `write` (PLT `0x4840`) is the outbound hook. No ELF patch is
   needed: an `LD_PRELOAD` line in `/lib/systemd/system/carplay_iAP2.service` is a
   data-only change. **Runtime interposition is now VM-proven against the real binary**
   (`hudnav/CP_PROTOCOL.md`, scripts `cp01`–`cp03` in
   `../tests/vm/`): the real, hash-verified `/app/bin/iAP2Service-2.0` binds
   `read@GLIBC_2.2.5` to `/lib/libc.so.6` without `LD_PRELOAD` and to the proof shim
   `src/cp_read_shim.c` with it (`LD_DEBUG=bindings` A/B), and the wrapper fired 128
   real `read` calls. That proof shim is VM-only: it logs calls and forwards stock
   bytes unchanged; it does **not** parse iAP2, patch Identify, register IDs or send
   `0x5200`. No real iAP2 phone bytes were seen (no phone/link in the VM; no
   `count=0x101d0` / `lo12=0xdde` hit), `write()` was not runtime-tested, and the
   production `cp_shim.c` is still not hash-gated/deployed. The Identify builder
   buffer, any registration exchange and the byte-level protocol details
   (Identify param value encoding, 0x5200 payload, route-state enum, integer widths,
   P2–P5 in `src/cp_shim.c`; P1 now records the VM proof) remain open.
4. **micom send helper**: WIRED (`--micom-send PATH` execs the whitelisted `CANService` helper, ID whitelist + 1 Hz refresh); bench-proven with a fake helper only — VM/car proof pending.
5. **Guidance flag 0x4EA**, TBT-list frames, text/lane channels (0x6E7/0x6DF): unverified
   on AU; not implemented.
6. 260319 micom opcodes re-derivation; SMACK/whitelist confirmation on the live unit.

## Line protocol (interceptor → daemon)

```
HB                              heartbeat (link health; phone silence at lights is OK)
TURN src=cp|aa man=<enum> [exit=<deg>] [raw=<n>] [side=<n>]
DIST m=<meters> [disp=<n>] [unit=<0..4>]
DEST m=<meters>                 distance to destination (CarPlay TLV 0x07)
STATE s=0|1|2                   inactive|active|reroute
OWNER src=cp|aa|oem|none        log-only replay fixture
```

`man` uses `hn_maneuver` (see hudnav.h); the interceptor normalizes source enums via
`hn_normalize_aa` / `hn_normalize_cp`.
