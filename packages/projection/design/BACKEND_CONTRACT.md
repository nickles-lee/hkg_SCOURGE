# Projection+ backend contract (v1, 2026-11-27)

The single source of truth between the three parts. Status: WIP; car-run once on one AU 260319 car (install, tile, status); restore VM-only; settings writes and AA status not yet car-tested.

- **hudnavd** (HUD Nav daemon, `packages/hudnav` — WIP, car-proven on one AU 260319 car only): applies settings, publishes status.
- **projection_daemon** (`01_SCOURGE_REPO/packages/projection/src`): HTTP/JSON on `127.0.0.1:8092` for the QML page.
- **QML page** (`packages/projection/artifacts/qml/...`): talks only to projection_daemon.

## 1. Switch files (existing, unchanged meaning) in `/rw_data/hudnav/`

| File | Meaning | Who reads | Effect of change while running |
|---|---|---|---|
| `enabled` | master switch | boot script, hudnavd | hudnavd: absent → clear HUD and send nothing (runtime). Boot script: absent → stock boot next time. |
| `aa-nav-enable` | Android Auto source | hudnavd (+ AA overlay) | hudnavd: absent → ignore AA input, clear if AA was showing. |
| `cp-enabled` | CarPlay source | boot script, hudnavd | hudnavd: absent → ignore CP input, clear if CP was showing. CP session itself stops at next boot. |

projection_daemon creates/removes these files (create = `open(O_CREAT)` empty file, remove = `unlink`).
It NEVER starts/stops overlays, sessions, AppProjection or iAP2Service. Runtime effect is hudnavd ignoring input;
full stock comes back at the next boot. hudnavd checks the three files at most once per second (stat).

## 2. Settings file `/rw_data/hudnav/settings.conf`

Plain `key=value` lines, `#` comments, unknown keys ignored, invalid values → default. Written atomically by
projection_daemon (write `settings.conf.tmp`, fsync, rename). hudnavd re-reads when mtime/size changes (≤1 s).

| Key | Values | Default |
|---|---|---|
| `aa_steps` | `precise` \| `balanced` \| `calm` | `balanced` |
| `cp_steps` | same | `balanced` |
| `aa_units` | `car` \| `metric` \| `imperial` | `car` |
| `cp_units` | same | `car` |

`car` = metric for now (the AU car's unit setting is not yet read); documented as such.

### Distance presets (applied per source to the maneuver distance sent to the HUD/cluster)

| Preset | < 100 m | 100–999 m | ≥ 1 km |
|---|---|---|---|
| precise | 1 m | 1 m | shown in km with 0.1 km resolution* |
| balanced (current car-proven behaviour) | 1 m | 10 m | 0.1 km |
| calm | 10 m | 10 m | 0.1 km |

Round to nearest step, never below 0; 995–999 m under balanced/calm clamps to 990 m (as today).
*The HUD/cluster km encoding only carries tenths (0x63E B0 high nibble), so "precise" cannot show 0.01 km;
precise therefore equals balanced above 1 km. Imperial: the only known HUD unit code is 3 = ft (0x63E B0 low
nibble); mile encoding is unknown. Implement imperial as ft below 0.1 mi (528 ft, rounding 10 ft / 50 ft per
preset) and, until a mile code is found on the car, metric above that, and mark imperial **car-unverified**.

## 3. Status file `/tmp/hudnav-status.json` (tmpfs; never written to flash)

Written by hudnavd atomically (tmp + rename), on change and at least every 2 s while running:

```json
{"v":1,"updated_ms":1764200000000,"daemon":"hudnavd","version":"<string>",
 "health":"guiding","source":"android_auto","next_turn_m":350,"dest_m":1600,
 "aa_connected":true,"cp_connected":false,"last_error":""}
```

- `updated_ms`: CLOCK_REALTIME ms. Consumers treat > 8 s old as stale.
- `health`: `disabled` (master off) | `waiting_phone` | `not_guiding` (phone owns nav but no turns) | `guiding` | `error`.
- `source`: `android_auto` | `carplay` | `none`.
- `next_turn_m`, `dest_m`: integers, -1 when unknown. Raw metres (before rounding).
- No street names, coordinates or other personal data, ever.

## 4. projection_daemon HTTP API (`127.0.0.1:8092`, loopback only)

`GET /status` → merged JSON: everything from the status file (or `health:"error"`, `stale:true` if missing/old)
plus the switches and settings, using the keys the QML already uses:
`hudnav_enabled`, `aa_enabled`, `cp_enabled` (bools from the switch files), `aa_distance_steps`, `cp_distance_steps`
(`precise|balanced|calm`), `aa_units`, `cp_units` (`car|metric|imperial`), `stale` (bool), `boot_guard`
(state from the kit's boot guard files), `versions`. Exact key names must match what `ProjectionPage.qml` reads;
if the QML and this table disagree, change the QML to this table and record it.

`POST /config` with a JSON object: merges only keys present (same keys as above), validates every value (reject the
whole request with 400 on any invalid key/value), applies switch-file and settings.conf changes, returns the new
`/status`. `restore_stock: true` removes `enabled`, `aa-nav-enable` and `cp-enabled` (= stock at next boot,
hudnavd goes quiet within 1 s).

Anything not in this file is undecided; propose changes in your BUILD notes rather than inventing silently.
