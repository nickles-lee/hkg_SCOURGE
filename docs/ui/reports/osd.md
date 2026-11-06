# OSD strips (AppOSDManager) — deep-dive (round 2)

OSDManager decompile = `bin_b126383d7e804f2a` (`f1.c` = functions_0001.c).
QML: `/tmp/opencode/gen5w_ui/AppOSDManager/qml_eco3.0/` (staged copy).

## Trigger chain — it's an unauthenticated D-Bus server

Owns (session bus): `AppEngineBase::initCommon("AppOSDManager")` →
`com.lge.hkmc.avn.appfunc.AppOSDManager` at
`/com/lge/hkmc/avn/appfunc/appInterface` (f1.c:52996-53017); AppID 0x16.
Adaptor methods (f1.c:87703-87930):

| Method | Engine path |
|---|---|
| `NotifyOSDRequest(ay)` / `(ay,i)` | `ProcessOSD(QByteArray,int)` f1.c:34618 |
| `NotifyOSDRequestHide(ay)` | f1.c:38968 |
| **`NotifySystemOSDRequest(appId, index, plane, show[, priority])`** | `ProcessSystemOSD` f1.c:36298 |
| `NotifyClimateOSDRequest(i,ay[,i])` | `OSDManager::climateOSD` |
| `NotifyNaviMuteState(i)`, `NotifyRadioScanBlink(i)`, `NotifyRequestBacklight`, `AskAvMuteState` | misc |

Subscribes to appfunc signals (no sender filter ⇒ any emitter):
NotifyScreenState, NotifyStatusQuietMode, NotifyRevertBeepPriority,
NotifyBatDischargeOSDOnOff, notifyEVChargingStatus, volume/driver-talk
signals, notifyTmsState, appmanager.info. System bus (micom):
`com.lge.car.micom.audio` NotifyAudioVolumeStep* → volume OSD.
**No caller authentication anywhere** — a third-party daemon can show an
OSD with a plain method call. Show/hide is broadcast as session-bus signal
**`NotifyOsdStatus`** (f1.c:48572-48593) — what sets AppHomeScreen's
`isOnScrShowing`/bar dim.

## systemOSD.xml — runtime-parsed registry

`AppOSDEngine::initResource()` reads
`/app/share/AppOSDManager/systemOSD/systemOSD.xml` (strings.tsv:695) with
QXmlStreamReader, root `systemosd`, element `key`, attributes
keySpec…keyStringD (f1.c:41322-41618). **Called once, in the ctor**
(f1.c:42268) — edits need a process restart. Entries = int[12] + 4 QStrings;
`ProcessSystemOSD` looks up by **keyIndex** (f1.c:36329 → 33389); overrides
show←request.show, id←appId, type+=8 when plane==1; serializes to the byte
struct (12 LE u32 + 4 UTF-8 strings, f1.c:47046-47078). **An added entry is
honored** (plain parse, no signature).

Field semantics: **keySpec** family (drives clear groups: 1=AV, 4=call,
11=driver talk — mainOSD.qml:230-308); **keyIndex** request lookup id;
**keyType** OSD family (2/3 → default `osdGeneral.qml`); **keyShow** flag
(overridden); **keyEvt** event → `getEvent()` routing (1 generic, 50
SOS/driver-talk, 90 AV/EV-off, 100 2-line); **keyInfo** icon id →
`OSDIconFile()` (0=none); **keyMode** mode tag; **keyIcon1-3** spare (all 0);
**keyTimeOut** in **100 ms units** (30=3 s, 50=5 s; must be <20000000 else
the timer never starts, mainOSD.qml:584); **keyStringA-D** text —
`translate("QMLTranslationModel", …)` (f1.c:46199-46251) so STR_* keys
translate and **plain text passes through as-is**.

Active entries (33; 7 commented SD-card ones): idx 10 (EV off), 20-25
(call/projection/media), 30/32/33 (BT/media off), 50-54 (SOS family), 70/71
(mute), 100/101 (camera/user-agreement), 120 (battery discharge warning),
140 (VR), 200 (app initializing), 300 (valet feature), 310 (TA), 320-323
(driver talk), 330 (loading), 340/341 (valet), 345 (profile), 350 (driver
talk support).

## mainOSD.qml routing

`onOsdData(display,index)` (mainOSD.qml:84-215): family switch — 0/8/18
general, 1/9/19 alert (osdAlert.qml), 100/102-106/110-117 climate files;
inside general, `switch(AppOSDEngine.getEvent())` → osdGeneralDAB/SXM_*/
Media_*/VoiceMemo/AVOff/USMDriveMode/DriverTalk/2_line/AirClean/Center;
**600 → osdNavi.qml into osdBaseLoader** (persistent base); **default →
osdGeneral.qml**. Mirrored in onOsdUpdate (:324-436) and onOsdBaseData
(:444-578). Show: visible + `delay(getTimeOut())` (×100 ms) +
`broadcastOsdStatusSignal`; touch dismisses (:43-60). Custom layout = drop
`osdMyToast.qml` + one `case` in the inner switch (optionally onOsdUpdate/
onOsdBaseData), trigger with keyEvt=NNN.

## Strip anatomy

- `osdGeneral.qml`: 1280/1920×90 at y0; bg tiled `co_bg_osd_title`, **red
  `co_bg_osd_title_red` when getPriority()==1** (:34-40); icon via
  `OSDIconFile(getInfo())` (forced warning icon at priority 1/2); text
  `getStringB()+language` FONT_BOLD 42 (2-line fallback 36), centered, RTL
  via getMidEast() (:8-10,58-116).
- `osdAlert.qml`: same bg/red; text 42→36 auto-shrink on wrap (:67-77).
- Volume: separate QQuickView (f1.c:29116), root h=75, bottom-anchored;
  `osdVolume.qml` bg `co_bg_osd_volume` (`_8` on 1280); title 32/36, mute
  icon co_ic_vol_off/on, level text, bar `max==20 ? (level<=15 ? level*33 :
  15*33+(level−15)*36) : max===45 ? level*15 : level*9` (:136,186); timeout
  getVTimeOut()×100 ms; plane 2, id 22.

## AppHomeScreen in-window loaders

`HomeScreen/main.qml:428-454`: `onsDataList` 4 slots, `load_OSD(Onsdata)`
(AppHomeScreen strings.tsv:2816), z:4, h=630 (x+640 RHD) — driven by
`AppManager::reqShowOnscreen(uint)`; paths loaded are **full-screen pages**
(`RearSeatFeatures/Screen_DriverTalk.qml`, `Screen_RearQuietMode.qml`,
`Climate/APPClimate_CLIMATE_SCREEN.qml`) — "onscreens", **not strips**.
The in-window "call OSD" = the call widget at z:100 (Widget screen
:234-236, gated by visibleOSDCall). All 90/75-px strips run in the
AppOSDManager process.

## Feasibility verdict

| Path | Daemon trigger | Visibility | Effort/risk |
|---|---|---|---|
| (a) XML entry + `NotifySystemOSDRequest(appId, index, 1, 1)` | **Yes** | **Global** — above any app, 90 px over the bar; auto-timeout; touch-dismiss | Low: XML edit + process restart; no QML patch for text+icon. Suppressed when display-off/camera/SOS-active/valet; preempted by higher-priority OSDs |
| (b) SystemPopup type-D | Yes | Global, centered card, sets sysPopupShowing | Low-med; heavier UX |
| (c) Own overlay in AppHomeScreen | own code only | Home window only | Lowest effort, full control |
| (d) Home-window onsDataList slot | needs C++ patch | Home window, full-content pages | High effort, wrong shape |

Recommendation: home-only → (c); global toast → **(a)**; card with buttons →
(b). Raw `NotifyOSDRequest(ay)` byte struct is the no-XML variant (12 LE u32:
type,evt,spec,show,timeout,id,info,mode,icon1-3 + 4 UTF-8 strings).
