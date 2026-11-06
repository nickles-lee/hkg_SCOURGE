# Home-screen tiles: dynamic placement and theming

This note applies to every custom tile added to `AppHomeScreen` on the
Ioniq 5 Gen5W (260128): Hello, Battery, WiFi and Tweaks.

## 1. Layout model

There are two home-screen QML files:

| File | Layout |
|---|---|
| `HomeScreen/QHMI_HomeScreenMain_Advanced.qml` | Swipeable multi-page home. Used at 1280 (4 columns) and 1920 (6 columns). |
| `HomeScreen/QHMI_HomeScreenMain_Wide.qml` | Classic single-page home with the All Menus tile and a horizontal grid. |

The installer patches both. The actual file loaded depends on the head unit's
home-screen mode (`isAdvancedMode`), not only on the screen width.

In the Advanced file:

- page 0 is `pluginPage` (widgets);
- pages 1..3 are `listPage1`..`listPage3` (the app grids);
- `totalPagesNormalMode` is the number of app-grid pages (1, 2 or 3);
- the Battery app adds a separate `batteryPage` at the end and increments
  `totalPageCount`.

Do **not** assume the first free slot is on page 1. On a real 1920 car the
stock icons can fill page 1; the free cells are often on page 2 (and sometimes
page 3).

## 2. Placement rule: first free slot across all pages

Never hard-code `page 1`, `page 2`, or `skip=1`. Use one global allocator:

```qml
function emptySlots(pageModel) {
    var slots = []
    if (!pageModel) return slots
    var n = pageModel.getCount ? pageModel.getCount() : pageModel.count
    for (var i = 0; i < n; i++) {
        var item = pageModel.getItem ? pageModel.getItem(i) : null
        var isEmpty = true
        if (item) {
            if (typeof item.isEditingVisible === "function")
                isEmpty = !item.isEditingVisible()
            else if (typeof item.visible !== "undefined")
                isEmpty = item.visible !== true
            else
                isEmpty = false
        }
        if (isEmpty) slots.push(i)
    }
    for (var j = n; j < nbrItemsPerPage; j++) slots.push(j)
    return slots
}

function firstEmptySlot(customIndex) {
    var remaining = customIndex
    for (var p = 0; p < totalPagesNormalMode; p++) {
        var slots = emptySlots(getPageDisplayListModel(p))
        if (remaining < slots.length)
            return { page: p, index: slots[remaining] }
        remaining -= slots.length
    }
    return null
}
```

Add the tile to **every** app page. Only the page returned by the allocator is
visible:

```qml
WifiTile {
    id: wifiTile1
    property var slot: firstEmptySlot(WIFI_INDEX)
    visible: slot !== null && slot.page === 0
    x: menuPage.x + (visible ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
    y: menuPage.y + (visible ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
    width: k_itemWidth
    height: k_itemHeight
    z: 6
}
```

Repeat the block on `listPage2` (`menuPage2`, `slot.page === 1`) and
`listPage3` (`menuPage3`, `slot.page === 2`). This gives exactly one tile, at
the first free cell across all pages, and no duplicate tiles on later pages.

## 3. Install-time ordinal

`customIndex` is the tile's position among custom tiles, fixed at install
time. Count the custom tiles already present in the installed QML:

- Hello = 0
- Battery = 1 (when the Battery tile is present)
- WiFi = 2
- Tweaks = 3

The installer/build script counts the markers (`id: helloTile1`,
`id: batteryTile1`, `id: wifiTile1`, `id: tweaksTile1`) and writes the next
ordinal into the generated QML. If Battery is installed after WiFi, Battery
takes the next ordinal. If WiFi is reinstalled after Battery, WiFi keeps its
ordinal and Battery moves after it. This is the "first available empty spot"
rule without hard-coded skips.

The ordinal is fixed at install time; the QML still scans the live page
models at load time, so rearranging icons later does not create overlaps.

## 4. Account for the top menu bar

The Advanced page components are positioned below the top status/menu bar:

```qml
Item {
    id: itemPage1
    y: (ConstantModel.MENU_BAR_Y) + (ConstantModel.MENU_BAR_Y) / 2
    ...
}
```

The tile coordinates must stay relative to the page's `menuPageN`:

```qml
y: menuPage2.y + Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight
```

Do **not** use absolute screen coordinates (`y: 0`, `y: 90`, etc.) for a tile.
`menuPageN` is already inside the offset page item. The Wide layout does the
same via `itemMainMenu` / `ConstantModel.HOME_1920_MENU_Y`; add the tile inside
`itemMainMenu`, not at the root.

The top bar is roughly 90 px high; the page item adds the remaining margin so
the first tile row starts below it. If you add a new page, copy the existing
`itemPageN` pattern rather than inventing a new offset.

## 4b. Full-screen overlay pages

A full-screen page (WiFi, Battery overlay) is **not** a ListView page. It is
an overlay item at the root of the host screen. Follow
`docs/ui/APP_OVERLAY_CHECKLIST.md`; the short version:

1. **Lay out below the 90px status bar.** `main.qml` draws the status bar at
   `z: 2` over every screen. Declare the page at the host root:

   ```qml
   WifiPage {
       id: wifiPage
       x: 0
       y: ConstantModel.MENU_BAR_Y
       width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
       height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
       z: 2000
       // do NOT set visible: the page owns it
   }
   ```

   The page's internal layout is designed for 630 px
   (`DEVICE_HEIGHT - MENU_BAR_Y`), not 720 px. Leave ~16-24 px of padding
   below the bar before the title/Exit row; do not put controls at the
   page's y=0 edge. Tiles/menu entries call `wifiPage.open()`, never
   `wifiPage.visible = true`.

2. **The page owns visibility and foreground state.**

   ```qml
   Item {
       id: root
       property bool requested: false
       property bool atOtherApp: typeof AppHomeModel !== "undefined"
                                  ? AppHomeModel.isAtOtherApp : false
       visible: requested && !atOtherApp
       function open()  { requested = true; refresh() }
       function close() { if (!requested) return; requested = false }
   }
   ```

   Never paint while another app owns the Wayland surface: the clicks would
   land on that app. Close on `onButtonBackClicked`, `isAtOtherApp`,
   `currentScreen`, `currentHomeAdvancedPageIdx`, `requestHomeByPP`, and
   `onSwitchToOtherApp`.

3. **Consume all input.** A bare `MouseArea { onClicked: {} }` is not
   enough. Use a full-size shield at `z: 1` with
   `preventStealing: true`, `acceptedButtons: Qt.AllButtons`, accepted
   press/release/click/wheel, and put the real content at `z: 2`.

4. **The steering-wheel Home/Star key is not a QML key event.** It arrives
   on the session bus (`NotifyRequestHomeScreen` / `custom_key_value`).
   A helper daemon tails `dbus-monitor` and exposes a counter; the WiFi
   daemon writes `home.json` (`{"seq":N,"epoch":E,...}`) and the QML polls
   it at 500 ms while open. `AppHomeModel.onButtonBackClicked` is only the
   on-screen back affordance. Do not claim it handles Home/Star.

`z: 2000` matches the Battery overlay and keeps the page above the home grid,
page indicators and animation helpers.

## 5. Theme colours

The home screen has light and dark themes. The stock `QText` component already
uses `HomeThemeHelper.normalTextColor`. Do not hard-code `color: "#FFFFFF"` in
a custom tile, or the label will be invisible on the light theme (this is why
the first Hello/WiFi tiles looked faint in the car photo).

Use:

- text: let `QText` use `HomeThemeHelper.normalTextColor`;
- normal icon: a medium blue such as `#2F6FED`, which reads on both light and
  dark backgrounds;
- disabled icon: grey `#8A8A8A`;
- tile background: `defineModel.home_bg_icon_n` / `home_bg_icon_p` via
  `Q_ThemeImage`.

Generate icons from the app's `scripts/make_icons.py` so reinstallations stay
consistent. Do not ship pure-white line art for a light theme.

## 6. Battery page vs Battery tile

They are different things:

- **Battery page**: a full-screen `BatteryPage` appended to the Advanced page
  list; it needs `totalPageCount = totalPagesNormalMode + 2` (one extra page).
- **Battery tile**: a `BatteryTile` on the app grid; it uses the same global
  allocator as Hello/WiFi/Tweaks and does not change `totalPageCount`.

The WiFi app does **not** add a page. Its tile is enough, so there is no
page-count conflict with Battery.

If a new app genuinely needs a whole page, append it after `batteryPage` and
increment the count by one more; update the history clamp in
`goHistorypage()` as well.

## 7. Installer behaviour for credentials

The WiFi installer copies credentials but does not auto-connect:

- copy `wifi.txt` from the USB root to `/rw_data/wifi/wifi.txt`;
- copy `authorized_keys` from the USB root to `/rw_data/wifi/ssh/authorized_keys`
  and `/rw_data/wifi/home/.ssh/authorized_keys`;
- import `wifi.txt` into the daemon config so the UI shows the SSID and SSH
  port;
- leave `mode=off` and `wlan_on=0`.

The user enables WLAN, Client/CarPlay and SSH from the WiFi page. The UI has
`Import USB` and `Import SSH keys` buttons for later re-imports.

## 8. Checklist for a new tile

1. Add `MyTile.qml` with theme text and a blue normal icon.
2. Add the global allocator to the Advanced QML.
3. Insert the tile on every app page with the correct install-time ordinal.
4. Add the Wide-layout `itemMyTile` inside `itemMainMenu`, after the existing
   custom tiles; shrink the cell width if needed.
5. Add the tile to All Menus / Setup if desired (they use a single-page
   `firstEmptyIndex` helper).
6. Copy the tile component and icons in the installer.
7. Do not add a page unless the app needs one; do not change
   `totalPageCount` for a tile.
8. Run the offline QML/installer tests.
9. On the car, run `run_stuff_diag.sh` and inspect:
   - which QML variant is installed;
   - `grep -n 'firstEmptySlot|WifiTile|HelloTile|BatteryTile|TweaksTile'`;
   - the tile's `slot.page` and the page indicator;
   - `systemctl status wifi-daemon`;
   - `/rw_data/wifi/install.log`.
