# SCOURGE

Lets you run your own software on Linux-based Gen5W head units & fixes some annoyances in the stock firmware that have gone unfixed after all these years. Think of this as the Linux counterpart to the (unrelated, unaffiliated) [HMG NEMESIS](https://github.com/xwtk/hmg_nemesis) project.

**Repository:** <https://github.com/nickles-lee/hkg_SCOURGE>

We have a [Discord channel](https://discord.com/channels/1074629698379841567/1272790207971721216)!

## Support

Research & implementation of SCOURGE and its addons on the Gen5W Ioniq 5 costs electricity, time, and tokens. If you want to donate money either via GitHub or directly, this is always appreciated. *(Donation links coming soon.)*

## Warning

SCOURGE is subject to the GNU AGPL 3.0 License. You can read the full agreement [here](LICENSE). TL;DR: use it, study it, change it, share it — including commercially — but carry the license and attribution forward, and if you serve it over a network, share your source too. Improvements flow back to everyone.

We like credit, so the license carries two additional terms (under Section 7 of the AGPL): you must preserve the SCOURGE project attribution (the copyright notices in the source files and the LICENSE notice), and modified versions must be marked as modified. Fork away — just don't pass it off as your own.

Everything in this repo comes with zero warranty. You are fully responsible for all risks associated with installing and running user-supplied code on your head unit. Flashing firmware is never a zero-risk operation, *especially* when the firmware in question is your own.

## Hardware Requirements

This is a semi-tethered jailbreak, meaning that additional hardware is required for the initial rooting. Once you are up and running, no additional hardware is needed.

During the initial root phase, or if you need to reflash firmware:

- A Raspberry or Orange Pi device that can run in "USB Gadget" mode is needed
- A 64GB+ microSD card
	- We are hopeful that we can soon reduce this to 32GB
- A flash drive, preferably 32GB+
- An SD card reader, because network I/O on the Pi is slow
- A Gen5W Ioniq 5 that is running the 260128 firmware (if older, update to 260128)

Availability of a capable model is non-optional at the moment. At some point in time, I hope to provide more deterministic & more reproducible build scripts, but I'm mindful that:

- Regional firmware differences introduce variance
- People want to try this on multiple models of cars, which requires some sloppiness
- Updated firmware versions add more variance
- Installation of additional apps / plugins / tweaks further add variance. Interfaces are not yet stable.

Keeping it agent driven at this early phase lets us avoid shipping whole binaries & it helps people get started sooner.

## Compatibility & Legwork

This research & rooting runbook is validated and tested with:

- 2022 Hyundai Ioniq 5 (EU, Linux, Gen5W_L)
- Raspberry Pi Zero 2W
- Deepseek 4.1 Flash 1017 Uncensored, IQ2_XS quantization
- Arch Linux as the builder-OS

This is likely readily adaptable to other cars & environments, but I am noting this here for reproducibility and so you know what has actually been proven versus what is plausible-but-untested:

| Component | Status |
|---|---|
| TOCTOU delivery + stage-1 root firmware (sideloading, failsafe) | Validated on the target car |
| Updater signature-check bypass baked into the firmware (unsigned normal-USB update) | Validated on the target car (2026-11-15); see `docs/17-usb-update-confirmed.md` |
| Battery / WiFi+SSH / Tweaks (incl. Reboot) app packages | Work-in-progress; the WiFi package is VM-validated on the vendor kernel/Qt (on-car retest pending). Reboot's clean path and daemon endpoint are host/QML-tested; the micom hard-reset fallback is host-tested with a fake D-Bus helper and derived from the vendor script (car pending) |
| Rust apps/daemons (host-built static musl) | WIP — host build/package path offline-validated; sample `scourge-statusd` VM-validated on the vendor kernel; car pending. No Rust toolchain or runtime is installed on the unit; see `docs/19-rust-apps.md` |
| Tweaks: safety-disclaimer auto-accept | Working reliably |
| Kia EV6 MY22–24 (EU, Gen5W_L), `CV_E.EUR.S5W_L.001.001.260128` | Fixture captured; payload byte-identical to the Ioniq 5 260128 package. Target wiring/build is WIP — not car-validated (docs/20) |
| Any other firmware, region or vehicle | Untested — see the porting runbook before trying |

No claims are made about other vehicles or platforms (including S5W_M/Android units, which are a fundamentally different architecture, or models whose update schemes are known to differ).

### Firmware package warnings

`scripts/01_stage_inputs.sh` audits the downloaded package before any build.
HMG's update server sometimes republishes the same firmware with a new
package-id counter in the `.ver` header; every row is identical, but the
file's size/hash changes. That is a **warning** (`ver.sha ... but header
identity and rows verify`), not a stop — a package-map variant, not a bad
download. A **row** that disagrees with its file or a lock pin is an error,
and the audit names the stale pin. Do not re-download or edit the package to
silence it. Inspect by hand with:

```sh
scripts/01b_audit_inputs.py --layout package --root /path/to/IONIQ5_EU
```

## Quick Installation Guidance

- Clone this repo
- Set up a fresh / blank Debian install on your Raspberry Pi & make sure it's accessible via SSH
- Using the AI harness, model, and provider, of your choice (e.g. OpenCode), point it to this repo & tell it to prepare a stage1 "toctou" setup
- At some point, it'll tell you to connect the SD card for the Pi to copy files onto it directly. Do so.
- Tell it to prepare a "run_stuff.sh" payload for the flash drive. It *SHOULD* ask you about which packages/tweaks you want to build and install.
- Go to your car, plug in both the power supply & usb gadget USB cables on the Pi & perform a "same version" update.
	- This will take approx. 1 hour to 1.5 hours to run. I/O from the Pi is slower than from an actual flash drive.
	- It is best if you do NOT use the "install after shutdown" firmware update option, since you might be relying on the 12V power supply to keep the Pi powered & I don't know if this shuts off.
- When the update completes, you should have an obvious startup-screen message letting you know you're jailbroken. Connect the flash drive containing your run_stuff.sh script. The system might lag for a bit, and then it'll reboot.
- You should see additional apps on your homescreen afterwards. Have fun!

## Networking

- The run_stuff.sh installer can enable networking on your car & an SSH server.
- We give you a convenient initial username/pass ("car" / "electric") to sign in.
- This is a dedicated, but root, user. If you connect the Ioniq to a network, someone can sign into your head unit as root.
- You SHOULD ABSOLUTELY change the password after first login.
- You SHOULD set up an authorized_keys file.

## Updating Firmware

As a general rule of thumb:

- If you are not yet rooted, but you think you might want to root, DO NOT install updates from Hyundai without first checking this repo / the discord to see whether it blocks the initial jailbreak.
- If you are rooted, DO NOT install stock OEM updates out of the box. Custom firmware can be installed as an ordinary USB update through the patched updater; the normal-USB flow is validated on the target car (see `docs/08-postroot-updates.md` and `docs/17-usb-update-confirmed.md`).

## Optional / Further in Development Steps

- The patched-updater custom-firmware update path is **confirmed on the car**. Stage-1 build 5 carries the eleven updater/launcher stubs (including the FUMO engine gate) and the SMACK permissive flag, so a plain exFAT flash drive can install a custom 260128 rootfs through `Settings > General > Software Update > Update via USB` — faster and more reliable than the Pi rig. See `docs/17-usb-update-confirmed.md`.
- An alternative semi-tethered jailbreak that does not require building and deploying an entire 27GB firmware image is being validated. Others are also investigating an option that strips this down to a ~8GB image, which is faster and easier to deploy.
- A "dealer mode" firmware image that is rooted & has disabled signature checks, but which has no obvious visible signs of being rooted. We can also try password protecting the update path.
- Packaging & making it easier to sideload apps
- Making sideload app installers less likely to interfere with each other
- Support networked installation scripts/packages
- A QEMU/KVM **VM lab** for bench-testing packages, installers and patches against your own decrypted firmware before they go near the car, with a planned graphics mode for UI work — see `docs/14-vm-lab.md` (WIP: the harness runs today, but is not yet packaged in-repo)
- Separate-process custom apps: the platform's AppLauncher RSA-verifies the `.sign` section of startup-group/preload spawns (on-demand `request.launch` spawns already skip it), which blocks the "official app" route. A sigstub for those paths ships in stage-1 build 5 and is car-validated (`docs/07-packages.md`), and there is a second route (an own Wayland process outside AppLauncher) that needs no patch at all. See `docs/07-packages.md` and `docs/ui/deployment-modes.md`.

## AI Usage Disclaimer

The entire process of rooting, building a payload deployer, and building plugins / tweaks / apps for the infotainment unit is heavily AI assisted (with extensive human review & steering). If you don't like the usage of AI, or you disagree with this approach, please know that I have a day job & a family that make it unrealistic to lovingly handcraft things for side-projects anymore.

If a markdown file in this repo is handwritten, I will highlight that. The README.md is handwritten.

## Repo Map

| Path | What |
|---|---|
| `docs/01–19` | The runbooks: TOCTOU explainer, firmware format, stage-1 build, Pi rig, car install, failsafe/packages, post-root updates (confirmed), porting, Ghidra re-derivation, troubleshooting, safety/legal, upstream references, VM lab (WIP), MICOM research, unsigned normal-USB update confirmation, a tentative writable-data reference, and host-built Rust apps/daemons (WIP) |
| `docs/ui/` | The Gen5W UI/UX research: styleguide, deployment modes (A–L), UX behavior patterns, design tokens, component library, glossary, and 15 evidence reports |
| `scripts/` | The build pipeline (fetch → stage → decrypt → patch → repack → verify → package → image → preflight → Pi deploy) |
| `payload/` | What gets baked into the rootfs: the failsafe, the eleven-stub patch tables, the sample run_stuff.sh |
| `packages/` | Battery, WiFi (+CarPlay+SSH), Tweaks (incl. the Reboot overlay) — sources, patchers, installers — plus `compose.sh` (the combo-stick builder) |
| `tools/` | First-party firmware-format tools, the update-debug (dmclient_gate/button_gate) tooling, the optional Ghidra/carve porting aids, and `tools/micom/` (command/signal-table and MCU-image readers) |
| `verify/` | Offline package verification |
| `config/` | `firmware_locks.env` (the compatibility pins), `third_party.lock` (fetch pins), paths/key examples |

Start with `AGENTS.md` if you're an AI agent (or want to work like one).

## Credits

- The [ioniq5-usb-toctou](https://github.com/CloudPapi/ioniq5-usb-toctou) repo — the TOCTOU path was documented & running there first
- The nice people on the Gen5W Discord
- The faceless people that wrote Ghidra and decided to open-source it.
- The public HMG firmware-tooling community (hkmcavn-tools, nudl)
- [HMG NEMESIS](https://github.com/xwtk/hmg_nemesis) for the inspiration

SCOURGE is not affiliated with, authorized by, or endorsed by Hyundai, Kia, HKMC, or LG Electronics. All trademarks belong to their respective owners.
