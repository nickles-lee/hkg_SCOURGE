# Credits (prior art — knowledge, not code)

No third-party source, firmware bytes or private logs are vendored in this package.
The following public work informed the *method* and the behavior tables; attribution
is textual only.

- **Open-HMG / HMG-API** and the **`hmgapi-wireless-cp-aa`** wireless mod (maintainer
  VysseCodes, AGPL-3.0): per-car/per-version hook maps, hash-pinned in-memory recipes
  and the Car Report workflow are the methodological model. Their `cluster-turns.md`
  and `ar-hud.md` document the near-turn `DisplayType` table, 45° angle steps, the AR
  window, side-street bits, LHT roundabout codes, "never icon 64", reroute behavior
  and the trap/speed-limit frame conflict. Caveat: HMG-API targets ARM `S5W_M`; its
  code cannot run on our x86 `S5W_L` unit.
- **AwsomeFox** (HMG-API PR #75), **hwjang1** (a CarPlay AR-HUD owner check), and
  **KSubedi / Evergreen** (GPL-3.0-only, independent CarPlay-AR build): car evidence
  and notes, credited without importing their code.
- Prior art used by the wider project: **Numen / JacobPantuso** (E-GMP cluster/HUD
  frame set), **dragz/egmpdbc**, **tylerharvey/Ioniq5_CAN**,
  **KSubedi/ultimate-wireless-projection** (maneuver semantics and runtime state
  machine), **SCOURGE's battery package** (car-proven `SetCANMicomRecv` pattern),
  **CloudPapi/ioniq5-usb-toctou**, **hkmcavn-tools**, **nudl** and **HMG NEMESIS**.

Public protocol knowledge (not copied code): Apple's iAP2 RouteGuidance message
family as decoded by the public CarPlay simulators/dissectors, and the VW MIB
CarPlay→cluster integration. See `hudnav/CP_PROTOCOL.md`.
