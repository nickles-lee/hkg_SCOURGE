# Projection QML prototype — build notes

Status: **WIP** (historical build notes; current status in docs/STATUS.md). Rendered with the
offscreen QML harness only; no head-unit, SSH or git access was used.

## What was built

Deployment Model A inside stock `AppHomeScreen/qml_eco3.0`, as in Tweaks/Wi-Fi:
a 306×249 home tile and a self-owned full-page overlay at `y=90`, `h=630`,
with all foreground-loss hooks, an all-button input shield and release beeps.

Repo files created:

- `artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/ProjectionPage.qml` —
  overlay ownership, daemon HTTP/JSON, status→plain-language mapping, group
  list model, scrollable HUD pane, diagnostics, More menu, error states.
- `artifacts/qml/AppHomeScreen/qml_eco3.0/component/ProjectionTile.qml`
- `.../component/ProjectionRow.qml` — own copy: normal + plate + stacked rows.
- `.../component/ProjectionSwitch.qml` — own copy of the 96×54 pill toggle.
- `.../component/ProjectionButton.qml` — own copy of the themed push/segment button.
- `.../component/ProjectionSegmented.qml` — own three-way picker.
- `.../component/ProjectionStatusBlock.qml` — "Right now" block.
- `.../component/ProjectionSourceSection.qml` — shared per-source section.
- `.../component/ProjectionDiagnosticsPane.qml` — B3 pane.
- `.../component/ProjectionMenu.qml` — More menu card.
- `.../component/ProjectionModal.qml` — stock-style confirmation.
- `.../component/ProjectionLoading.qml` — loading spinner stand-in.
- `artifacts/assets/projection/projection_icon_n.png` — newly authored icon
  (phone + turn arrow); no OEM asset was copied.

Harness files (new folder, original `.work/qmlharness` untouched):

- `.work/qmlharness_projection/{main.cpp,build.sh,stage.sh,feed.sh,shoot.sh,shoot_tile.sh,all.sh,shot_projection.qml,shot_tile.qml,make_icon.py,verify_shots.py}`.
- Shell scripts are LF-only and are run with `wsl.exe sh <path>`.

## Spec change applied (owner, this task)

**Every setting is now per phone source.** APP_SPEC §C2 rows 5–8 are replaced
by two instances of `ProjectionSourceSection` (`source: "aa"` / `"cp"`):

- `Right now` status block (unchanged) → master switch `HUD navigation`
  (helper: one meaning only, "When off, the car behaves as stock.") →
  section `Android Auto` (`Use Android Auto`, `Distance steps`, `Units`) →
  section `CarPlay` (same three controls, independent values).
- When a source switch is off, its distance/units rows are dimmed to 0.4 and
  their buttons cannot be tapped (see `p_source_off_day.png`).
- JSON keys: `hudnav_enabled` stays the master; per-source keys are
  `aa_enabled`, `aa_distance_steps`, `aa_units`, `cp_enabled`,
  `cp_distance_steps`, `cp_units`; `{"restore_stock": true}` still posts via
  `/config`. Values: steps `precise|balanced|calm`, units `car|metric|imperial`.
- A future per-source setting is one row in `ProjectionSourceSection.qml`
  plus one key pair in `applyStatus()`/`postConfig()`; both sources inherit it.
  The commented example row is in that file.
- The pane is now ~1260 px tall, so the 538 px pane scrolls; the
  scrolled-to-bottom shot shows the complete CarPlay section.

There is **no driving lockout**: APP_SPEC §B5/§F are deliberately not built
(PRODUCT.md 2026-11-27 owner decision).

## How to render

From Windows PowerShell:

```
wsl.exe sh <workspace>/.work/qmlharness_projection/build.sh
wsl.exe sh <workspace>/.work/qmlharness_projection/all.sh
```

`stage.sh` copies the stock context components from the existing WSL rootfs,
copies our QML into the staged tree, installs the icon and seeds
`/rw_data/hudnav/status.json`; `feed.sh` rewrites fake status JSON for each
state. `shoot.sh name wide night [k=v...]` renders one frame after 1.5 s into
`packages/projection/design/shots`. Harness hooks: `pane=diagnostics`,
`modal=1`, `menu=1`, `scroll=bottom`, `loading=1`, `down=1`, plus
`H_statusFile` to point at another JSON file.

Shots produced (all verified visually, 1280×720 unless noted):

- `p_main_day_1280.png`, `p_main_night_1280.png`
- `p_main_day_1920.png`, `p_main_night_1920.png` (1920×720)
- `p_main_scrolled_bottom.png`, `p_main_cp_night.png`
- `p_diagnostics.png`, `p_restore_modal.png`, `p_more_menu.png`
- `p_daemon_not_running.png`, `p_loading.png`, `p_waiting_phone.png`
- `p_source_off_day.png`, `p_tile.png`

Check pass: first render found a clipped "Right now" source line, elided nav
copy, an elided diagnostics button/footnote, a loading overlay that let rows
bleed through, a semi-transparent menu/modal card, a missing tile import and a
down-state shot stuck in loading. All were fixed and re-rendered. Final pass
also removed all QML binding loops from our components; the only remaining
console warnings come from the stock `QHMI_ToggleButton` and tile theme images
that the harness stub does not define (they resolve on the head unit).

## Deviations from the spec (and why)

1. **Strings are inline**, as in `TweaksPage.qml`/`WifiPage.qml`. Moving them to
   `TranslationsModel.STR_*` in 26 `.ts` files is open question Q10; the task
   said to mirror the reference if it does not use translation keys.
2. **Scrolling**: a clipped `Flickable` + 6 px themed thumb is used, not the
   15 px `Q_StraightScrollBar`. The stock scroll component needs
   `co_bg_scroll_*` assets absent from the harness; the 6 px custom thumb is
   the documented WF/TW pattern and renders correctly offscreen.
3. **Option menu** is a local card (104 px row, 0.7 dim) because
   `QHMI_OptionMenu` is a C++-provided stock type not registered in the QML
   harness. Entries are a model, so more can be appended.
4. **Loading spinner / popup surface**: `QLoadingComponent`'s `img_loading`
   frames and `co_bg_popup_mid_*` 3-slice images are not in the harness, so a
   local ring and a themed `Qt.tint` surface stand in. The stock
   `QPopupAnimationItem` fade, 972 px card, 78 px buttons and Yes-left order
   are preserved.
5. **Pane height** is 538 px starting at absolute y=180 (title bar is a full
   90 px as in TW/WF) rather than APP_SPEC's y=170; 2 px bottom margin remain.
6. **Group list caption** is shortened ("Turns · Android Auto") to avoid
   elision in the 340 px nav column; the full sentence stays in "Right now".
7. **Row geometry** for the per-source pickers is 168 px (title + helper +
   picker stacked) instead of the original single-line 92 px row: three 150 px
   buttons plus a 39 px title cannot fit the 816 px pane without clipping.
8. **Local write echo**: a 1.5 s suppression window prevents the 1 s poll from
   overwriting a just-toggled control before a real daemon echoes it. This is
   a prototype stand-in for a proper pending-write model.
9. Tile keeps a local rounded backing rectangle so the harness (no themed tile
   images) still shows the cell; the real `home_bg_icon_n/p` swap remains.

## Open points

1. Public name: "Projection" is still a working name (APP_SPEC Q1).
2. Port 8092 must be confirmed free on the target firmware (Q2).
3. `hudnavd` must actually emit the per-source keys; this prototype assumes the
   new per-source schema and the `aa_*`/`cp_*` switch-file mapping.
4. Default for CarPlay: currently on, matching the old spec (Q3).
5. EV routing group: model entry and pane stub are commented out, not shown.
6. Restore semantics (immediate vs next reboot) still unknown (Q7).
7. Home/Star plumbing (`home_seq`/`home_epoch`) assumed in `/status` (Q8).
8. Z-order uses the reference pages' `z: 100`; `QRootScreen` z 2000 not applied
   (Q9), pending host wiring review.
9. Translation keys, boot-guard notice outside Diagnostics, status file path
   and all firmware-dependent values remain to be validated on the car.

## Review pass 1

Design-lead review applied in full, then all 15 PNGs were re-rendered and
inspected again. No new QML errors or binding loops.

1. **Diagnostics**: heading is now a `ProjectionRow` section header (30 px
   accent text at the same 24 px left margin as every row). The left nav stays
   visible: "HUD navigation" is unselected and "Diagnostics" is selected;
   tapping "HUD navigation" returns to the main pane. The Back button is gone.
   "Restore stock behaviour" is now the only bottom row, styled with the
   catalog's custom warn colour (`#a64b00` day / `#ffb300` night) and its
   helper "Turns everything off. The car works as it did before installing."
   The jargon footer was removed and replaced by "Android Auto connection",
   "CarPlay connection" and "Boot safety" rows (no "iAP2"); firmware in the
   fake status is now `260319`.
2. **Right now**: the second line is no longer a repeat of the source. It shows
   `Next turn: 350 m` when guiding, `Start a route on your phone.` when a phone
   is connected but idle, and is empty when off. While a source is guiding,
   its section header also shows the same `Next turn` detail on the right,
   so a scrolled-to-CarPlay screenshot still carries it.
3. **Icon**: redrawn as a flat phone with a small curved turn arrow on the
   screen (no rising chart). Canvas, blue, light screen and padding are
   unchanged; `make_icon.py` regenerates it.
4. **Picker rows**: at 1920 px the pane is wide enough, so `Distance steps`
   and `Units` keep their segmented pickers on the same line, right-aligned
   like the switches (128 px buttons, 21 px helper, 23 px button labels so the
   helper does not elide). At 1280 px the two-line layout is kept. Verified
   in both `p_main_day_1920.png` and `p_main_night_1920.png`.
5. **New render**: `p_main_cp_guiding_1280.png` shows CarPlay as the active
   source, scrolled to the CarPlay section with `Next turn: 350 m` visible.
   Harness hook `scroll=cp` (`page.scrollToSection("cp")`) was added for it.

Only the icon asset plus these existing shots changed; the new shot was added:
`p_main_day_1280.png`, `p_main_night_1280.png`, `p_main_day_1920.png`,
`p_main_night_1920.png`, `p_main_scrolled_bottom.png`, `p_main_cp_night.png`,
`p_diagnostics.png`, `p_restore_modal.png`, `p_daemon_not_running.png`,
`p_waiting_phone.png`, `p_tile.png`, `p_loading.png`, `p_more_menu.png`,
`p_source_off_day.png`, `p_main_cp_guiding_1280.png`.

## Review pass 2 (2026-11-27, owner feedback)

- Icon redrawn to match the sibling glyphs (99x99, #2F6FED line style, transparent, plus a grey
  `_d` variant): phone outline with a clean right-turn arrow. Source is now
  `packages/projection/scripts/make_icons.py`; the harness `make_icon.py` is superseded.
- "More" button and `ProjectionMenu.qml` removed: Diagnostics is reached only from the left list.
- The "Next turn" text beside the Android Auto / CarPlay section headings was removed; it now
  appears only in the "Right now" box.
- `p_more_menu.png` dropped; all other shots re-rendered. Still offline-only (host render), not on the car.

## Home-screen tile patch (2026-11-27)

Status: **offline-rendered only, not car-validated.** No SSH, car or git use.

Base: the car's current home QML, pulled 2026-11-27 and byte-verified
(`base/car_2026-11-27/qml_eco3.0`, hashes in `car_sha256.txt`; OEM and
git-ignored). It already carries WiFi, Tweaks and Reboot (`*_PAYLOAD_260128`).
Hello and Battery have only their own tile components in this base, with no
layout entries in these files.

`scripts/build_artifacts.py` (env `PROJECTION_BASE` overrides the base dir)
checks each base file's sha256 against `car_sha256.txt`, applies an additive
patch, and writes only changed layout files to `build/qml_eco3.0/`, plus
`build/manifest.txt`. Both are git-ignored. Marker: `// PROJECTION_PAYLOAD_260319`,
added after the existing marker lines. `--self-check` asserts
`patch(patch(x)) == patch(x)`. Any missing or duplicated anchor, or a hash
mismatch, exits 2 with `REFUSED: …`.

What changes, per base file (all additions; no existing line changes):

| File | Change | Anchor |
|---|---|---|
| `HomeScreen/QHMI_HomeScreenMain_Advanced.qml` | marker; `ProjectionTile` `projectionTile1..3` after each `rebootTileN` (one per page), `slot: root.firstEmptySlot(3)`, same visible/x/y/z 7 as Reboot, `onClicked: projectionPage.open()`; `ProjectionPage { id: projectionPage }` with the WifiPage/RebootPage host geometry (`y: MENU_BAR_Y`, full width, `z: 2000`) | `RebootTile {\n id: rebootTileN` (exactly once each); `RebootPage {\n id: rebootPage` |
| `HomeScreen/Screen_HomeScreen_AllMenus.qml` | marker; `onProjectionClicked: projectionPage.open()` after `onRebootClicked`; `ProjectionPage` instance | `onRebootClicked: rebootPage.open()`; `RebootPage` block |
| `HomeScreen/Screen_HomeScreen_Submenu_Setup.qml` | same as All Menus | same |
| `component/QCustomizedMenuListAllowEmpty.qml` | marker; `signal projectionClicked()` after `signal rebootClicked()`; `ProjectionTile { id: projectionTile }` after `rebootTile`, `firstEmptyIndex(menuPage.pageModel, 3)`, `onClicked: root.projectionClicked()` | `signal rebootClicked()`; `RebootTile {\n id: rebootTile` |
| `HomeScreen/QHMI_HomeScreenMain_Wide.qml` | **not patched** (see Q1) | `itemReboot` checked to exist |

The custom index (3) is not hard-coded. It is one past the indices WiFi, Tweaks
and Reboot already use (0/1/2), and the build refuses if those are not
contiguous. Close wiring needs no extra host code: `ProjectionPage` already
closes itself on Back (`AppHomeModel.onButtonBackClicked`), Home
(`AppHomeEngine.onNotifyHomeMainChanged`), other-app and screen change. The
same is true of WifiPage and RebootPage, which also carry no host-side
Connections.

Manifest: 4 `patch` rows, each with dest, new sha256 and the car base sha256
it replaces, so the installer must refuse on mismatch. There are 13 `file` rows:
`ProjectionPage.qml`, 10 `component/Projection*.qml` and both icons, going to
`/app/share/AppHomeScreen/projection/`.

Verification (all offline, WSL):

- `tests/test_artifacts.sh`: 35/35 pass. It checks two byte-identical builds,
  the self-check, markers (old ones kept), each tile/page/signal insertion,
  that existing tiles keep slots 0/1/2, additive-only diffs, manifest hashes
  against `car_sha256.txt`, and refusal on a missing anchor and on hash drift.
- `qmllint` (Qt 5) passes on the 4 patched files and all 11 Projection QML
  files. A deliberately broken file is rejected as a control
  (`.work/qmlharness_projection/lint_home.sh`).
- Full patched Advanced home loaded in a new harness
  (`.work/qmlharness_projection/{main_home.cpp,build_home.sh,stage_home.sh,shoot_home.sh,all_home.sh}`).
  The harness stubs the C++ models: `AppHomeModel`/`AppHomeEngine`
  invokables and a 3-page `AllMenuPageNDisplay` list model with N visible
  apps. Stock apps are grey placeholders, not OEM icons. Every QML type
  resolves. Remaining warnings are unstubbed C++ theme values
  (`undefined` → QColor/QString) and `AdvancedWidgetQMLInstance`.
  Shots in `design/shots/`:
  - `home_advanced_1280.png`, `home_advanced_1280_night.png`: 4 apps, then
    WiFi · Tweaks · Reboot · **Projection+** on row 2.
  - `home_advanced_1920.png`, `home_advanced_1920_night.png`: 8 apps, then
    WiFi · Tweaks · Reboot · **Projection+** (6 columns).
  - `home_advanced_1280_spill_p1.png` / `_p2.png`: page 1 has only 3 free
    cells. WiFi/Tweaks/Reboot are unchanged on page 1, and Projection+ takes the
    first free cell of page 2.
  - `home_advanced_1280_tap.png`: tapping the tile opens `ProjectionPage`
    (loading frame); the beep event is logged.
- All Menus / Setup custom list: lint only. Not rendered, because it needs
  further C++ list-model stubs.

Open questions:

1. **Wide/Basic home has no free cell.** The strip is `HOME_1920_MENU_W` =
   1266 px (docs/ui). The car base has All Menu 200 + 4 cells × 200 + WiFi
   200 = 1204, so Tweaks already spans 1204–1354 (88 px past the strip) and
   Reboot spans 1354–1504. With `HOME_1920_MENU_X` = 642, Reboot is past the
   1920 px screen edge. A Projection+ tile at `itemReboot.right` would be
   fully off-screen. I left Wide unpatched rather than move existing tiles.
   Projection+ is still reachable from Basic home via All Menus. Owner
   decision: shrink/re-space the custom Wide items, replace one, or leave
   Basic home without the tile. Separately, check on the car whether Reboot
   is visible on Basic home today. These numbers come from the documented
   constant comments, not from car measurement.
2. If all normal pages have fewer than 4 free cells, the Projection+ tile is
   hidden on Advanced (the same rule as the existing tiles). It is still
   reachable via All Menus only if that page has a 4th free cell.
3. Page z: the host uses `z: 2000` as WifiPage/RebootPage do (the component's
   own default stays `z: 100`). Check on the car that it covers the status
   area and popups correctly.
4. Opening Projection+ does not close an already open Tweaks/WiFi/Reboot
   overlay. That matches the existing pages, which don't close each other either.
5. Not car-validated: install, slot allocation with the real models, tap
   beep, Back/Home close and day/night icon swap (`projection_icon_d.png` is
   in the manifest, but the tile references only `_n`).
