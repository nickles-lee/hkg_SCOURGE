# Android Auto interception — sanitized protocol notes (WIP)

## 1. Surface

`AppProjection` loads `liblinux-head-unit.so`, which exports the
`NavigationStatusCallback` virtuals and `INavigationStatusServiceCB` setters. The
shim interposes them by overriding vtable entries, calls the original first via
`dlsym(RTLD_NEXT)`, and forwards normalized records to `hudnavd` over a Unix socket.
Fail-open: malformed or unexpected input forwards nothing.

Exported mangled names (from the selected profile, never hand-edited in C):

- `_ZN24NavigationStatusCallback24navigationStatusCallbackEi` (status)
- `_ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits`
- `_ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii`
- `_ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB`
- `_ZN21CAndroidAutoInterface37setInstanceINavigationStatusServiceCBEP26INavigationStatusServiceCB`
- `_ZN20ProtocolEndpointBase15onChannelOpenedEh`
- `_ZN24NavigationStatusEndpoint5startEv`, `_ZTV24NavigationStatusEndpoint`

The legacy receiver (build tag `260312102711`, projectionprotocol 1.5 / 0.6.9-r3)
exposes only the legacy `NavigationStatus*` callbacks: no lanes, no destination
distance. The absence of `NavigationNotification` is expected and recorded as an
`EXPECTED` row in the 260319 profile.

## 2. Loader / activation facts (260319)

- There is **no per-app environment argument** in AppLauncher: it spawns via Qt
  `QProcess` and only inserts a fixed set of variables; `LD_PRELOAD` is not among them
  and no `remove`/`clear` exists. The AppLauncher `.sign` gate validates binary bytes,
  not the environment, so an inherited preload entry is not rejected.
- The boot wrapper already exports `LD_PRELOAD=/usr/lib/libEGL.so.1`. Appending the
  shim there maps it into all launcher children; a process self-gate makes it inert
  unless `/proc/self/exe` is `AppProjection` (`APPID=110`, host override
  `HUDNAV_FORCE=1`). **This global route is banned as a persistent install.**
- The default activation is a per-process `DT_NEEDED` bind mount of a patched *copy*
  of `AppProjection`; a reboot restores stock with no persistent change.
- The real rootfs is glibc 2.25, so `dlsym` is pinned to the pre-2.34 version with
  `__asm__(".symver dlsym,dlsym@GLIBC_2.2.5")`. The shim must not reference a bare
  `dlsym` or add a `NEEDED` entry beyond `libc.so.6` / the loader; `scripts/build.sh`
  gates on both with `readelf`.

## 3. Ownership

AppManager broadcasts the guidance owner on the session bus: `110` = CarPlay,
`111` = Android Auto, `0` = none, `30` = native (260319 values — re-derive elsewhere).
`hudnavd` stays silent while native owns guidance and requires a fresh maneuver after
an owner change. `OWNER` is a log-only fixture input, not a shim-reported value.

## 4. Validation evidence

- VM-proven: `LD_DEBUG=bindings` A/B against the real vendor library showed all three
  callback symbols rebind from the vendor lib to the shim under `LD_PRELOAD`.
- A fuller VM attempt still did not deliver a live event: stock AppProjection reached
  D-Bus/Qt setup but crashed after EGL init failed; stock controls crashed too, so the
  VM lacked a vehicle interface. This does not establish a live end-to-end callback.
- Car-proven: working Android Auto event path on 260319 after the later fix. Next-turn
  argument order remains PROVISIONAL.

See `docs/STATUS.md` for the tier table and `PORTING.md` for the safe ladder.
