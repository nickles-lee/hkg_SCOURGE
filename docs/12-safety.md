# 12 — Safety, legal posture, and responsible use

## Safety first

- **Your own hardware only.** SCOURGE is right-to-repair / own-device
  tooling. Never run any of it against a vehicle you do not own or are not
  explicitly authorized to modify.
- **Parked and off-road.** The TOCTOU run, the update flow, and any
  package that sends commands (battery preconditioning, charge door) are
  for a stationary vehicle with an adequately charged 12 V battery. Never
  interact with these tools while driving.
- **Never interrupt an update.** A firmware update writes the inactive A/B
  slot; interrupting it (unplugging, power loss) is the main brick risk.
  The A/B fallback and the rig (docs/04) are the recovery paths.
- **Weston is watchdog-protected.** A Wayland crash triggers a MICOM
  watchdog reset of the whole head unit (docs/ui, Mode G). Any UI
  experiment follows the phased, parked-car procedures in the UI docs.
- **The failsafe is a root channel.** Any stick with a changed
  `run_stuff.sh` executes as root on the unit. Treat your USB sticks like
  root keys; do not plug in sticks you do not control. Change the default
  SSH password and prefer keys (docs/07).
- **OTA discipline.** Do not install OEM updates while rooted without
  checking this repo / the Discord first — an update may close the
  TOCTOU and will revert `/app` customizations. The Tweaks OTA blocker
  exists for this reason and is off by default so USB updates keep
  working.

## Legal posture

*This section describes the posture SCOURGE is designed around. It is not
legal advice; you are responsible for compliance with the laws that apply
to you.*

SCOURGE's design choices, in order to sit squarely within the classic
reverse-engineering exceptions for **error correction** and
**interoperability** (in the EU, the Software Directive 2009/24/EC
Article 5(1) use-including-error-correction and 5(3)
interoperability-decompilation bases; implemented in the Netherlands in
Auteurswet art. 45h ff.; comparable doctrines exist elsewhere):

1. **Purpose.** The project's documented purposes are fixing defects and
   long-standing annoyances in firmware the owner lawfully acquired with
   their vehicle (the DST handling, the night-mode resets, the disclaimer
   flow), and achieving interoperability with the owner's own software,
   network, and devices (running owner code, SSH/WiFi, CarPlay with the
   owner's phone). The docs and README frame the work accordingly —
   because that is what it genuinely is.
2. **Lawful acquirer.** Every tool operates on firmware the vehicle owner
   already lawfully possesses. The repo says so prominently, and the tools
   are useless without the owner's own package + key.
3. **No redistribution of OEM material.** The repo ships **no** firmware
   images, no OEM binaries, no OEM-derived artifacts. Everything
   OEM-derived is generated at build time from the user's own package.
   Patches are expressed as programmatic patchers operating on the user's
   own files, not as redistributed diffs of OEM content.
4. **No third-party code redistributed.** Dependencies (the TOCTOU repo,
   the public TE2 tooling, nudl, dropbear, tzdata) are fetched at
   setup/build time from their upstreams, pinned and hash-verified
   (`config/third_party.lock`). Where a modified upstream tool was
   needed, the delta ships as a patch, never a whole file.
5. **No secrets.** The TE2 content key is fetched from public community
   tooling at setup time, never committed. It is a symmetric content key —
   it cannot forge signatures, which is precisely why the project needed
   the TOCTOU approach rather than any signing capability.
6. **No OEM private key is used or forged anywhere.** The signed manifest
   stays byte-identical; the boot images stay genuine.
7. **Information minimization.** Unused vulnerabilities (e.g. the USB
   automount command injection) are deliberately not published. Platform
   behavior that blocks owners (the AppLauncher `.sign` verification) is
   documented as documentation, not weaponized.

## Warranty and disclosure

- Modifying your head unit can void warranties and, done wrong, brick
  hardware. Everything here is **zero warranty** (AGPL-3.0, see LICENSE).
- If you discover a security-relevant issue in this repo's own tooling,
  report it in the repo's issues or on the Discord rather than publishing
  exploit material.
- SCOURGE is not affiliated with, authorized, or endorsed by Hyundai,
  Kia, HKMC, or LG Electronics. All trademarks belong to their owners.

## Attribution (AGPL-3.0 TL;DR)

SCOURGE is licensed under the GNU AGPL 3.0 (LICENSE) **with two
additional terms under Section 7 of that license**, stated at the top of
the LICENSE file:

1. **Attribution (7b).** Conveyors of SCOURGE or works based on it must
   preserve the SCOURGE project attribution — the copyright notices in
   the source files (`SPDX-FileCopyrightText: 2026 The SCOURGE Project
   contributors`) and the LICENSE notice — and include the project line
   in Appropriate Legal Notices where such notices are displayed.
2. **Marking modifications (7c).** The origin of the material must not be
   misrepresented; modified versions must be marked as different from
   the original.

In short: you may use, study, modify, and redistribute SCOURGE,
including commercially, as long as you carry the license forward,
**keep the credit**, mark your changes, and — because it is the *Affero*
GPL — if you offer it as a network service, you must offer its source to
those users. Improvements flow back to everyone. The full terms are in
LICENSE; this summary is not a substitute for them.

(One deliberate exception: the three files under `payload/rootfs/` carry
no SPDX header — their bytes are baked into the firmware image and pinned
by the known-good hash table in docs/03, so they stay byte-identical to
the validated build. The project-level LICENSE notice covers them.)
