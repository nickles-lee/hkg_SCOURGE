# NotificationManager (SystemPopup) — deep-dive (round 2)

NotificationManager decompile = `bin_51e45f2c5c6de865`. QML:
`/tmp/opencode/gen5w_ui/SystemPopup/qml_eco3.0/` (staged copy).

## XML parse (`noti_popup.xml`)

Parser: `AppSystemPopupEngine::initResource(QString)` = FUN_0015e880
(functions_0001.c:39594); opens `/app/share/SystemPopup/popup/noti_popup.xml`
(string 0xc2268) with QXmlStreamReader; root `<notification>`, one `<key>` per
entry, **attributes only** (element text skipped). **Read once at process
start** (sole call site = engine ctor); a modified XML requires a restart.
Entry struct 0x48 bytes in a QList at engine+0x300.

| Field | Semantics |
|---|---|
| `keyID` | lookup key; `findNotiPopup` linear scan, first match wins, no bounds check (FUN_001484f0) |
| `keyNotiPopupType` | enum string: E_Type_A=0…G=6, NAVI_2BTN=7, B_INFO=4003, NAVI_DAY_NIGHT_2BTN=32145, 3BTN=32146, NOTIFY_IGN_OFF=32155, MEDIA_GATE=32165; unknown → keeps 0 |
| `keyStatusErr` | parsed, **never read** — inert |
| `keyTitleStr`/`keyBodyStr` | `QCoreApplication::translate("QMLTranslationModel", …)` — **literal strings pass through as-is** |
| `keyNormalPriority` | **+9999 added when shown**; comparisons bucket to 10 000-multiples (FUN_00148660) |
| `keyImmediateNoti` | stored, never queried — inert |
| `keyDispTimeout` | −1 = no auto-close; ≥0 = per-item QTimer; re-show uses **remaining** time |
| `keyBtnA/B/C` | style-enum ids, toInt at use, 0 = no button; ~60 ids resolved by `getBtnString()` → STR_HOME_OK/YES/NO/CANCEL… |
| `keyLongBodyType, keyListPopupType, keySymbolID, keyUserLockTimeout, keyMinDispTimeout, keyAudioChime` | **not parsed at all** — dead config in 260128 |

No schema/count validation; duplicate keyIDs tolerated (5001 appears twice);
commented entries skipped.

## Trigger path (session bus, service `com.lge.hkmc.avn.appfunc.NotificationManager`)

- `NotifySystemPopupRequest(i id, ay text)` — id = **requesting app's AppID,
  self-declared, unvalidated** (logging/response routing only). Handler
  FUN_00176eb0 → Process; may be stashed and replayed when the front app is
  changing. The `ay text` is a binary struct (header + UTF-8 title/body via
  QByteArray::remove/left) — fiddly; avoid.
- **`NotifyIndexSystemPopupRequest(i appId, i index, i plane, i show)`** —
  "index" = **XML keyID**; fully XML-driven (FUN_00165a20 → 00164b90);
  translates title/body, uses entry's type/priority/timeout/buttons; missing
  entry → error response 200, no popup. **This is the custom-popup path.**
- `NotifyNaviPopupRequest(ay)`; `NotifySystemPopupResponse(i id, i result)`
  (force-close).
- Validation: no id bounds check; unknown id in the custom path still shows
  with default priority 209999. `isPopUpProhibited` (FUN_00138200) gates by
  camera/standby/SOS/valet/TMS state. Special-cased ids: 1001, 11214,
  11269/70, 10009-12, 4036, 9999.
- Callers in firmware: AppHomeScreen, AppSetup, AppNavi, AppMediaPlayer,
  AppUpgrade, BluetoothHMI, AppEURadio, AppVH, AppEngineerMode, TmsFgApp,
  AppLogExtractor, libKeyboard.so, NotificationManager itself.

## Priority & queueing

`addPopupWithNotificationItem` (FUN_00160f00): same-ID rows replaced
("Update content popup"); model sorted so highest priority is front; eviction
and block rules ("block show popup in camera/RSPA/valetMode/VI screen",
"Don't show popup in display off"). mainNoti.qml renders every model row
(`Repeater { model: notificationModel }`, mainNoti.qml:169-231). XML
priorities present: 20, 10000, 20000, 30000, 40000, 50000, 80000, 200000.
**Outside-press closes only when shown priority > 89999** (typeA.qml:15,57).
`notiPlane` = display-plane bitmask (1=first display; everything here = 1).

## Response path

QML buttons → `systemPopupResponse(plane, fromAppID, notiID, btnResponse)`
(FUN_0013f310) → `CB_Notification` emits session-bus signal
**`NotifySystemPopupResponse(appid, popupid, result)`** — the requesting app
listens and filters on its own appid. **Codes: 1=A/left/OK, 2=B/right, 3=C,
4=D, 0=dismissed** (outside/timeout; OK-type btnA ∈ {1,8,101,108} responds 1),
11=typeD banner, 200=no-such-popup error. Queries: `AskPopupShowingStatus()`,
`AskSystemPopupIDExist`, `AskLastSystemPopupIDOfApp`; signals
`NotifySystemPopupRequestStatus`, `NotifyCurrentPopUpStatus`,
`NotifySystemPopupState(plane, state)` (state 1 = showing → apps dim).

## Free keyID ranges

Occupied: 1001-1019, 1050, 2001-2005, 2101-2111, 2201, 2301, 2401, 2501-2502,
3000-3011, 5001-5005, 9999, 10009-10012, 10020, 11010, 11020, 11040-41,
11200-11229 (spotty), 11245-11260, 12205-12215.
**Free: 4000-4999, 6000-9998, 11230-11244, 11261-12204, 12216-29999, 30000+**
(30000+ safest against future firmware).

## Feasibility verdict

**Route B (recommended)**: one `<key>` entry (type A/B/C; literal strings;
priority 200000; timeout −1; keyBtnA=1) + trigger via
`NotifyIndexSystemPopupRequest(appId, keyID, 1, 1)`. **No PopupGenerator.qml
change needed** — unmapped types fall through to `typeA.qml`
(PopupGenerator.qml:307-311). Restart NotificationManager after the XML edit
(read-once). Risks: firmware update reverts `/app/share/SystemPopup`; ro
mount (rw-remount install pattern); suppressed in prohibited states.
Route A (custom-path `ay` struct, no XML) exists but requires reversing the
binary header.

## List & long-body variants

`keyListPopupType`/`keyLongBodyType` are inert; the features are driven by
dialogType/request data instead: list popups via `QString::split(title,'\n')`
→ `popupListModel` (used by type C/D, e.g. DM drive mode ucType 202); long
body = typeA.qml itself (wraps, flicks, Scrollbar past 7 lines; wide "max"
variant = dialogType 11000).
