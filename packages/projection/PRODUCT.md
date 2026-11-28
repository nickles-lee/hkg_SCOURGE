# Product

<!-- impeccable:product-schema 1 -->

## Platform

embedded (Qt 5 / QML page inside the stock `AppHomeScreen` of the Hyundai Gen5W head unit; not web, iOS or Android)

## Stack

Existing codebase answers it: QML page + home-screen tile inside `/app/share/AppHomeScreen/qml_eco3.0/`
(same mechanism as `packages/tweaks` and `packages/wifi`), backed by a small static daemon on
`127.0.0.1` (HTTP/JSON) like `tweaks_daemon`. No new AppLauncher app ID, no separate GUI process.

## Users

- The owner, parked, adjusting how phone projection behaves.
- Their partner and passengers: non-technical, must understand every control at a glance.
- Other Ioniq 5 (Gen5W) owners who install it from the public SCOURGE repo: the app must explain
  itself and be safe without the author present.
Confirmed (owner, 2026-11-27): settings may be changed at any time, including while driving (a
passenger adjusting them is expected). No driving lockout; controls stay large and simple.

## Product Purpose

A home-screen app for **phone projection improvements** (Android Auto and Apple CarPlay) on the
Gen5W head unit. First feature: **HUD Nav**, which forwards turn-by-turn guidance from Android Auto
and CarPlay to the car's head-up display and cluster. It exists because the stock head unit only
sends its own built-in navigation to the HUD. Success: owners can turn features on or off, tune
them, and see whether they are working, without SSH.

## Positioning

The stock car offers no settings for projection beyond on/off; this app adds car-integrated
features the phone apps cannot do themselves (HUD guidance today; later e.g. reporting the car as
an EV to the phone for EV routing, if the protocols allow it).

## Operating Context

- 1280x720 or 1920x720 touchscreen, top 90 px owned by the status bar, used from the driver's seat
  or by a passenger, day and night (the car has a night/amber mode; see docs/ui).
- Opened from a tile on the home screen like the Tweaks, Wi-Fi and Reboot tiles.
- Feature state lives on the car in `/rw_data/hudnav/` (switch files: `enabled`, `cp-enabled`,
  `aa-nav-enable`, ...); the HUD daemon is `hudnavd`.

## Capabilities and Constraints

Confirmed HUD Nav controls/data the app must be able to grow into:
- Master on/off; Android Auto on/off; CarPlay on/off.
- Distance rounding (currently 1 m under 100 m, 10 m to 999 m, 0.1 km above), units.
- Status/diagnostics: is guidance flowing, which phone source, last error, boot guard state.
Future (undecided, needs research): EV reporting to the phone for EV routing; more projection tweaks.
Constraints: follow `docs/ui/styleguide.md`, `tokens.md`, `components.md`, `UI_CONTEXT.md`,
`ux-patterns.md` (driving lockout, foreground loss, daemon pattern); never draw under the top 90 px;
installs through the SCOURGE USB/SSH package house rules; must fail safe (car boots stock if the
feature is broken).
Public name: **Projection+** (owner, 2026-11-27). Package/file names keep `projection`.

## Brand Commitments

Must look native to the stock Hyundai Gen5W `qml_eco3.0` home-screen UI (owner request: "native and
beautiful, with room to expand"). Sits beside the existing Tweaks / Wi-Fi / Reboot apps.

## Evidence on Hand

- Stock UI research: `01_SCOURGE_REPO/docs/ui/*` (styleguide, tokens, components, 15 reports).
- Reference implementations: `packages/tweaks/artifacts/qml/...`, `packages/wifi/artifacts/qml/...`.
- Working HUD Nav backend: `packages/hudnav` (WIP; car-proven AA + CarPlay on one AU 260319 car, 2026-11-27).
No screenshots of the real screen are in the repo; do not invent any.

## Product Principles

1. Native first: indistinguishable from a stock Hyundai settings page.
2. Plain language for non-technical people; technical detail only behind a diagnostics view.
3. Safe by default: every feature can be switched off from the app, and off means stock.
4. Built to grow: features are sections/cards, so new projection features slot in without redesign.

## Owner decisions log
- 2026-11-27: every setting is adjustable per phone source (Android Auto and CarPlay each have their
  own on/off, distance steps and units), not one shared value. Backend (hudnavd) must apply rounding
  and units per source.
- 2026-11-27 roadmap wishes (owner): lane guidance (high); street name on cluster/HUD if possible
  (high interest); speed cameras / speed limits from the phone (interesting); default to the projection
  screen when CarPlay connects (AA already takes focus, CarPlay does not); wireless CarPlay/AA could move
  here from the Wi-Fi app. Fixing the AA "not available on user agreement screen" warning is a BUG FIX,
  never an option.
- 2026-11-27: before Projection+ is submitted to the public SCOURGE repo, the installer must work on any
  owner's car regardless of which other packages (Tweaks, Wi-Fi, Battery, Hello, ...) are installed:
  build the tile patch from that car's own current home-screen files (anchor-based, refuse on doubt),
  integrate with `packages/compose.sh`, and keep the other packages' installers/restorers working when
  Projection+ is present (order-independent). The first install on the owner's car may be car-specific.
