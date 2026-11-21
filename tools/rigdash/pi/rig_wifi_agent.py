#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""rig_wifi_agent.py — the rig's own control page.

Runs ON the rig. Two jobs, one page:

* **WiFi** — while the fallback access point is up (rig-ap-fallback.service) the
  rig could not find any known network, so it became an AP at 10.42.0.1 and this
  page is what you reach there: pick a network, type its passphrase, submit.
* **USB gadget** — connect or disconnect the peripheral link (binds/unbinds the
  dwc2 UDC), and pick what the next boot should come up as (`dr_mode` in
  config.txt). The rig can therefore be taken in and out of USB mode from a
  phone instead of by unplugging cables.

    python3 rig_wifi_agent.py [--address 0.0.0.0] [--port 80]

Gates, both from /etc/rig/wifi.env:

* `RIG_AP_PASSPHRASE` — required for `/api/connect`; whoever joined the setup AP
  already knows it.
* no API token. The page is reachable over the owner's LAN or the setup AP the
  owner raised in their own car, and a token in front of that bought a field to
  retype on a phone and nothing else. `/connect` still needs the AP passphrase,
  because that is a network credential rather than a gate on this API.

Standard library only: the rig does not get a Python site-packages tree, and
this must work on a freshly flashed card.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import hmac
import html
import json
import os
import re
import time
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

#: The AP's address. Kept as a constant of the device, not of the LAN: DHCP
#: gives clients 10.42.0.x and this is the gateway they land on.
AP_ADDRESS = "10.42.0.1"
AP_PASSPHRASE_ENV = "RIG_AP_PASSPHRASE"

#: The rig dials out; nothing dials in. Target, port and key live on the board in
#: /etc/rig/tunnel.env, never in this repo — it names a host and holds a key path.
TUNNEL_UNIT = "rig-tunnel.service"
TUNNEL_ENV = "/etc/rig/tunnel.env"
MAX_BODY = 4096

SSID_RE = re.compile(r"^[^\x00-\x1f\x7f]{1,32}$")
PSK_RE = re.compile(r"^(?:[ -~]{8,63}|[0-9a-fA-F]{64})$")


def nmcli(*args: str, timeout: float = 40.0) -> tuple[int, str]:
    """Run nmcli with an argv list — never a shell."""
    try:
        proc = subprocess.run(["nmcli", *args], capture_output=True, text=True,
                              timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, f"{type(exc).__name__}: {exc}"
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def tunnel_state() -> dict:
    """Whether the rig can be reached from outside, and through which door.

    There are two: `usb0` (the gadget's network function - ssh over the same
    cable, no WiFi and no access point in the way) and the reverse tunnel the
    `rig-tunnel` unit keeps dialling. Only the target and the remote port are
    read back; the key path stays out of the page.
    """
    state = {"unit": TUNNEL_UNIT, "configured": os.path.exists(TUNNEL_ENV),
             "enabled": "", "active": "", "target": "", "remote": "", "usb0": ""}
    for name, argv in (("enabled", ["systemctl", "is-enabled", TUNNEL_UNIT]),
                       ("active", ["systemctl", "is-active", TUNNEL_UNIT])):
        _, out = _run(argv, timeout=10)
        state[name] = out.splitlines()[0] if out else ""
    if state["configured"]:
        try:
            with open(TUNNEL_ENV, encoding="utf-8") as handle:
                for line in handle:
                    name, _, value = line.strip().partition("=")
                    value = value.strip().strip('"')
                    if name.strip() == "RIG_TUNNEL_TARGET":
                        state["target"] = value
                    elif name.strip() == "RIG_TUNNEL_REMOTE":
                        state["remote"] = value
        except OSError as exc:
            state["active"] = f"env unreadable: {type(exc).__name__}"
    rc, out = _run(["ip", "-4", "-br", "addr", "show", "usb0"], timeout=5)
    words = out.split()
    if rc == 0 and len(words) >= 3 and "/" in words[2]:
        state["usb0"] = words[2]
    return state


def status() -> dict:
    code, out = nmcli("-t", "-f", "GENERAL.STATE,GENERAL.CONNECTION", "device", "show", "wlan0")
    state, _, conn = out.partition("\n")[0].partition(":")[0], "", ""
    for line in out.splitlines():
        key, _, value = line.partition(":")
        if key == "GENERAL.STATE":
            state = value.strip()
        elif key == "GENERAL.CONNECTION":
            conn = value.strip()
    code4, addr = nmcli("-t", "-f", "IP4.ADDRESS", "device", "show", "wlan0")
    ip = ""
    for line in addr.splitlines():
        if line.startswith("IP4.ADDRESS"):
            ip = line.partition(":")[2].split("/")[0]
            break
    code5, wifi = nmcli("-t", "-f", "ACTIVE,SSID", "device", "wifi")
    ssid = ""
    for line in wifi.splitlines():
        active, _, name = line.partition(":")
        if active.strip() == "yes":
            ssid = name.strip()
            break
    return {
        "state": state or "unknown",
        "connected": "connected" in state and "disconnected" not in state,
        "connection": "" if conn in ("--", "") else conn,
        "ssid": ssid,
        "ip": ip,
        "ap_active": "AP" in state.upper(),
    }


def scan(timeout: float) -> list[dict]:
    code, out = nmcli("-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                      "--rescan", "yes", timeout=timeout)
    networks: list[dict] = []
    seen: set[str] = set()
    for line in out.splitlines():
        ssid, _, rest = line.partition(":")
        signal, _, security = rest.partition(":")
        ssid = ssid.replace("\\:", ":")
        if not ssid or ssid in seen:
            continue
        seen.add(ssid)
        networks.append({"ssid": ssid, "signal": signal.strip() or "0",
                         "security": security.strip() or "open"})
    networks.sort(key=lambda net: int(net["signal"]) if net["signal"].isdigit() else 0,
                  reverse=True)
    return networks


# --- USB gadget ----------------------------------------------------------------

CONFIG_TXT = "/boot/firmware/config.txt"
USB_MODES = ("peripheral", "host")


def _config_txt() -> str:
    for path in (CONFIG_TXT, "/boot/config.txt"):
        if os.path.exists(path):
            return path
    return CONFIG_TXT


def usb_mode() -> str:
    """The dr_mode currently configured for the next boot ('' = not set)."""
    mode = ""
    try:
        with open(_config_txt(), encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("dtoverlay=dwc2") and "dr_mode" in line:
                    mode = line.split("dr_mode=", 1)[1].split(",")[0].split()[0]
    except OSError:
        return ""
    return mode


def udc_name(known: list[str] | None = None) -> str:
    """The controller name, even while it is unbound.

    /sys/class/udc empties as soon as the gadget driver is unbound, so the
    platform device (fe980000.usb on a Pi 4, 3f980000.usb on a Zero 2 W) is what
    we fall back to — otherwise connect-after-disconnect could never reconnect.
    """
    if known:
        return sorted(known)[0]
    try:
        for name in sorted(os.listdir("/sys/bus/platform/devices")):
            if re.fullmatch(r"[0-9a-f]+\.usb", name):
                return name
    except OSError:
        pass
    return ""


def usb_state() -> dict:
    udcs: list[str] = []
    try:
        udcs = sorted(os.listdir("/sys/class/udc"))
    except OSError:
        pass
    bound = []
    for udc in udcs:
        link = f"/sys/bus/platform/devices/{udc}/driver"
        if os.path.exists(link):
            bound.append(os.path.basename(os.path.realpath(link)))
    gadget = ""
    try:
        root = "/sys/kernel/config/usb_gadget"
        names = [n for n in os.listdir(root) if not n.startswith(".")]
        gadget = names[0] if names else ""
    except OSError:
        pass
    connected = bool(bound)
    return {
        "udc": udcs,
        "driver": bound,
        "connected": connected,
        "gadget": gadget,
        "mode": usb_mode(),
        "available_modes": list(USB_MODES),
    }


#: What the page calls the two USB roles. `toctou` is the one the rig exists for:
#: peripheral role, link up, the image exposed to the head unit. `normal` is the
#: board acting like any other Pi (host role, link down).
USB_ROLES = {
    "toctou": {"dr_mode": "peripheral", "link": True,
               "label": "TOCTOU mode", "detail": "peripheral · link up"},
    "normal": {"dr_mode": "host", "link": False,
               "label": "Normal mode", "detail": "host · link down"},
}


def usb_role(usb: dict | None = None) -> str:
    """Which of the two roles the board is in right now ('' when unclear)."""
    usb = usb or usb_state()
    if usb["mode"] == "host" and not usb["connected"]:
        return "normal"
    if usb["mode"] == "peripheral" and usb["connected"]:
        return "toctou"
    return ""


def usb_apply_role(role: str) -> tuple[bool, str]:
    """Switch to a role: boot mode in config.txt, plus the live link."""
    spec = USB_ROLES.get(role)
    if not spec:
        return False, "role must be: " + ", ".join(USB_ROLES)
    # Link first: that is the part with an immediate effect. The boot mode is a
    # file edit that only matters at the next boot, so it must not veto it.
    ok_link, link_message = usb_link(spec["link"])
    ok_mode, mode_message = usb_set_mode(spec["dr_mode"])
    message = f"{spec['label']}: {link_message}"
    message += (f" · boot: dr_mode={spec['dr_mode']}" if ok_mode
                else f" · boot mode not saved ({mode_message})")
    return ok_link, message


def usb_set_mode(mode: str) -> tuple[bool, str]:
    """Write dr_mode into config.txt. Takes effect on the next boot."""
    if mode not in USB_MODES:
        return False, "mode must be one of: " + ", ".join(USB_MODES)
    path = _config_txt()
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    line = f"dtoverlay=dwc2,dr_mode={mode}"
    replaced = False
    out = []
    for current in lines:
        stripped = current.strip()
        if stripped.startswith("dtoverlay=dwc2") and "dr_mode" in stripped:
            out.append(line)
            replaced = True
        else:
            out.append(current)
    if not replaced:
        out.extend(["", "[all]", line])
    try:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(out) + "\n")
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    return True, f"dr_mode={mode} written — reboot to apply"


def usb_link(connect: bool) -> tuple[bool, str]:
    """Bind/unbind the UDC: the peripheral link appears or disappears now."""
    state = usb_state()
    udc = udc_name(state["udc"])
    if not udc:
        return False, "no UDC — dwc2 is not in peripheral mode (set the mode, reboot)"
    if state["connected"] == connect:
        return True, "already " + ("connected" if connect else "disconnected")
    node = "/sys/bus/platform/drivers/dwc2/" + ("bind" if connect else "unbind")
    try:
        with open(node, "w", encoding="ascii") as handle:
            handle.write(udc)
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    return True, ("connected " if connect else "disconnected ") + udc


def rig_state() -> dict:
    """Everything the page shows at a glance."""
    info = status()
    usb = usb_state()
    services = {}
    for unit in ("rig-ap-fallback", "rig-wifi-agent", "nbdkit", "ssh"):
        code, out = subprocess.run(  # noqa: PLW1510 - we want the status code
            ["systemctl", "is-active", unit], capture_output=True, text=True
        ), None
        services[unit] = (code.stdout or "").strip() or "unknown"
    build = os.path.expanduser("~/scourge/build")
    artifacts = {}
    for name in ("usb.img", "stage1.te2", "regions.json"):
        path = os.path.join(build, name)
        artifacts[name] = os.path.getsize(path) if os.path.exists(path) else 0
    try:
        st = os.statvfs("/")
        free_gb = round(st.f_bavail * st.f_frsize / 1e9, 1)
    except OSError:
        free_gb = 0.0
    with open("/proc/uptime", encoding="ascii") as handle:
        uptime = float(handle.read().split()[0])
    return {
        "wifi": info,
        "usb": usb,
        "files": deploy_state(),
        "images": images(),
        "toctou": toctou_state(),
        "tweaks": tweaks_state(),
        "tunnel": tunnel_state(),
        "devices": removable_devices(),
        "log": recent_actions(),
        "services": services,
        "artifacts": artifacts,
        "free_gb": free_gb,
        "uptime_h": round(uptime / 3600, 1),
        "kernel": os.uname().release,
        "hostname": os.uname().nodename,
    }


def _run(argv: list[str], timeout: float = 15.0) -> tuple[int, str]:
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, f"{type(exc).__name__}: {exc}"
    return proc.returncode, (proc.stdout + proc.stderr).strip()


#: What the control page consists of, and where each piece has to end up. The
#: same table drives scripts/18_deploy_rig_agent.sh and the Files section here,
#: so "which folder does the file come from" has one answer.
REPO_DIR = os.environ.get("RIG_REPO_DIR", "/home/pi/scourge")
DEPLOY_MAP = (
    ("tools/rigdash/pi/rig_wifi_agent.py", "/usr/local/lib/rig/rig_wifi_agent.py"),
    ("tools/rigdash/pi/rig-ap-fallback.sh", "/usr/local/sbin/rig-ap-fallback"),
    ("tools/rigdash/pi/rig-ap-fallback.service",
     "/etc/systemd/system/rig-ap-fallback.service"),
    ("tools/rigdash/pi/rig-wifi-agent.service",
     "/etc/systemd/system/rig-wifi-agent.service"),
    ("tools/rigdash/pi/rig-tunnel.service",
     "/etc/systemd/system/rig-tunnel.service"),
)


def deploy_state() -> list[dict]:
    """Source path, target path, and whether each is actually there."""
    rows = []
    for source_rel, target in DEPLOY_MAP:
        source = os.path.join(REPO_DIR, source_rel)
        row = {"source": source_rel, "target": target,
               "source_present": os.path.exists(source),
               "present": os.path.exists(target), "size": 0,
               "current": False, "stale": False}
        if row["present"]:
            try:
                st = os.stat(target)
                row["size"] = st.st_size
                row["mode"] = oct(st.st_mode & 0o777)[2:]
                row["current"] = (row["source_present"]
                                  and st.st_size == os.path.getsize(source))
                row["stale"] = row["source_present"] and not row["current"]
            except OSError:
                pass
        rows.append(row)
    return rows


# --- images ---------------------------------------------------------------------
#: The depot the rig serves from. `serve.img` is a symlink to the chosen image, so
#: swapping is an atomic pointer move, not a 27 GB copy.
BUILD_DIR = os.environ.get("RIG_BUILD_DIR", "/home/pi/scourge/build")
SERVE_LINK = os.path.join(BUILD_DIR, "serve.img")
HASH_CACHE = os.path.join(BUILD_DIR, ".hashes.json")


def _load_hashes() -> dict:
    try:
        with open(HASH_CACHE, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def _save_hashes(data: dict) -> None:
    tmp = HASH_CACHE + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(data, handle)
        os.replace(tmp, HASH_CACHE)
    except OSError:
        pass


def _image_path(name: str) -> str | None:
    """Resolve a candidate image, refusing anything outside BUILD_DIR."""
    if (not name.endswith(".img") or "/" in name or name.startswith(".")
            or name == os.path.basename(SERVE_LINK)):
        return None
    path = os.path.join(BUILD_DIR, name)
    if not os.path.isfile(path):
        return None
    return path


#: Where a stick shows up on a desktop image; the agent scans these for images so
#: a 23 GB file never has to cross the WiFi.
MOUNT_ROOTS = ("/media", "/run/media", "/mnt")
SCAN_DEPTH = 2


def _usb_mounts() -> list[dict]:
    """Mount points under MOUNT_ROOTS — the stick, not the root filesystem."""
    code, out = _run(["findmnt", "-rno", "TARGET,FSTYPE,SOURCE"])
    if code != 0:
        return []
    mounts = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        target, fstype, source = parts[0], parts[1], parts[2]
        if not target.startswith(MOUNT_ROOTS) or target in MOUNT_ROOTS:
            continue
        mounts.append({"target": target, "fstype": fstype, "source": source})
    return mounts


def _scan_images(root: str, depth: int = SCAN_DEPTH) -> list[str]:
    """`*.img` files under one mount root, two levels deep, no symlink games."""
    found: list[str] = []
    base = root.rstrip("/")
    for current, dirs, files in os.walk(base):
        level = current[len(base):].count("/")
        if level >= depth:
            dirs[:] = []
        for name in files:
            if name.endswith(".img") and not name.startswith(".") \
                    and name != os.path.basename(SERVE_LINK):
                found.append(os.path.join(current, name))
    return sorted(found)


def _entry(path: str, source: str, served_real: str, cached: dict) -> dict | None:
    try:
        st = os.stat(path)
    except OSError:
        return None
    name = os.path.basename(path)
    entry = cached.get(path) or cached.get(name) or {}
    digest = (entry.get("sha256")
              if entry.get("size") == st.st_size and entry.get("mtime") == int(st.st_mtime)
              else "")
    return {
        "name": name,
        "path": path,
        "source": source,
        "size": st.st_size,
        "mtime": int(st.st_mtime),
        "sha256": digest,
        "served": os.path.realpath(path) == served_real,
        "recovery": "stock" in name.lower(),
        "deletable": source == "build",
    }


def images() -> list[dict]:
    """Candidates to serve: the build depot plus anything on a mounted stick."""
    served_real = ""
    if os.path.islink(SERVE_LINK):
        served_real = os.path.realpath(SERVE_LINK)
    cached = _load_hashes()
    rows: list[dict] = []
    for name in sorted(os.listdir(BUILD_DIR)) if os.path.isdir(BUILD_DIR) else []:
        path = _image_path(name)
        if not path:
            continue  # the pointer itself, dotfiles and non-images are skipped
        entry = _entry(path, "build", served_real, cached)
        if entry:
            rows.append(entry)
    for mount in _usb_mounts():
        for path in _scan_images(mount["target"]):
            entry = _entry(path, f"usb {mount['target']}", served_real, cached)
            if entry:
                rows.append(entry)
    return rows


def removable_devices() -> list[dict]:
    """Removable block devices and whether anything of theirs is mounted."""
    code, out = _run(["lsblk", "-J", "-o", "NAME,SIZE,RM,TYPE,FSTYPE,MOUNTPOINT,LABEL"])
    if code != 0:
        return []
    try:
        data = json.loads(out)
    except ValueError:
        return []
    devices = []
    for disk in data.get("blockdevices", []):
        if str(disk.get("rm")) not in ("1", "True", "true"):
            continue
        for part in disk.get("children") or [disk]:
            devices.append({
                "device": f"/dev/{part.get('name')}",
                "size": part.get("size") or "",
                "fstype": part.get("fstype") or "",
                "label": part.get("label") or "",
                "mountpoint": part.get("mountpoint") or "",
            })
    return devices


def mount_media(device: str) -> tuple[bool, str]:
    """Mount one removable partition read-only under /media/<label-or-name>."""
    known = {d["device"]: d for d in removable_devices()}
    if device not in known:
        return False, "not a removable device I can see"
    entry = known[device]
    if entry["mountpoint"]:
        return True, f"already mounted at {entry['mountpoint']}"
    name = (entry["label"] or os.path.basename(device)).replace("/", "_") or "usb"
    target = os.path.join("/media", name)
    try:
        os.makedirs(target, exist_ok=True)
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    # Read-only: serving only ever reads, and a stick you pulled from a camera
    # should not be written by a web page.
    code, out = _run(["mount", "-o", "ro", device, target], timeout=30)
    if code != 0:
        return False, out or "mount failed"
    return True, f"mounted {device} at {target}"


def _candidate(value: str) -> str | None:
    """Resolve a requested name/path against what the scan actually found.

    Nothing is served by path alone: an image has to be in the list, which keeps
    a crafted path from pointing the gadget at something else on the board.
    """
    if not value:
        return None
    rows = images()
    for row in rows:
        if value == row["path"]:
            return row["path"]
    for row in rows:
        if value == row["name"] or value == os.path.basename(row["path"]):
            return row["path"]
    return None


def image_serve(name: str) -> tuple[bool, str]:
    """Point serve.img at one image. Only with the USB link down."""
    path = _candidate(name)
    if not path:
        return False, f"no such image: {name}"
    if usb_state()["connected"]:
        return False, "the USB link is up — press Normal mode first, then swap"
    tmp = SERVE_LINK + ".tmp"
    try:
        if os.path.lexists(tmp):
            os.unlink(tmp)
        os.symlink(os.path.abspath(path), tmp)
        os.replace(tmp, SERVE_LINK)
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    # The plugin opens the baseline once, at config time, and keeps that fd, so
    # moving the pointer is not enough: the gadget has to be re-armed for the new
    # file to reach the wire. Re-arming also re-binds the UDC, so the head unit
    # sees the drive disappear and come back as the freshly chosen image.
    # --no-ask-password: without it a non-root caller (or one on a host without
    # the unit) blocks on polkit for 90 s waiting for a desktop auth dialog.
    rc, message = _run(["systemctl", "--no-ask-password", "restart", "toctou-gadget"],
                       timeout=90)
    detail = " · gadget re-armed" if rc == 0 else \
        f" · gadget restart failed ({message[:120]})"
    return True, f"now serving {os.path.basename(path)}{detail}"


UPLOAD_CHUNK = 1 << 20


def _free_gib() -> float:
    return round(_free_bytes() / 1e9, 1)


def _free_bytes() -> int:
    try:
        st = os.statvfs(BUILD_DIR)
        return st.f_bavail * st.f_frsize
    except OSError:
        return 0


def save_upload(name: str, stream, length: int) -> tuple[bool, str]:
    """Write an uploaded image straight to disk, then rename it into place.

    The body is the file itself (no multipart): the browser sends the File as the
    request body, so a 27 GB image never lands in memory. The `.part` suffix means
    an interrupted upload is never mistaken for a usable image.
    """
    if not name.endswith(".img") or "/" in name or name.startswith("."):
        return False, "name must be a plain *.img filename"
    if length <= 0:
        return False, "Content-Length is required (the body must be the file)"
    if length > _free_bytes() * 0.98:
        return False, (f"not enough space: {length / 1e9:.1f} GB needed, "
                       f"{_free_bytes() / 1e9:.1f} GB free")
    part = os.path.join(BUILD_DIR, "." + name + ".part")
    written = 0
    try:
        with open(part, "wb") as handle:
            # Read exactly Content-Length bytes: a socket file has no EOF to hit,
            # so an over-sized read would block until the client gave up.
            remaining = length
            while remaining > 0:
                chunk = stream.read(min(UPLOAD_CHUNK, remaining))
                if not chunk:
                    break
                handle.write(chunk)
                written += len(chunk)
                remaining -= len(chunk)
            handle.flush()
            os.fsync(handle.fileno())
        if written != length:
            os.unlink(part)
            return False, f"short upload: {written} of {length} bytes"
        os.replace(part, os.path.join(BUILD_DIR, name))
    except OSError as exc:
        try:
            os.unlink(part)
        except OSError:
            pass
        return False, f"{type(exc).__name__}: {exc}"
    return True, f"uploaded {name} ({written / 1e9:.2f} GB)"


def image_delete(name: str) -> tuple[bool, str]:
    """Remove one image from the depot. Nothing on a stick is deleted here."""
    path = _candidate(name)
    if not path:
        return False, f"no such image: {name}"
    if not path.startswith(BUILD_DIR.rstrip("/") + "/"):
        return False, "that image is on external media — manage it on the stick"
    try:
        if os.path.islink(SERVE_LINK) and \
                os.path.realpath(SERVE_LINK) == os.path.realpath(path):
            return False, "that image is being served — press Serve on another first"
        os.unlink(path)
    except OSError as exc:
        return False, f"{type(exc).__name__}: {exc}"
    cached = _load_hashes()
    if cached.pop(name, None) is not None:
        _save_hashes(cached)
    return True, f"deleted {name}"


def image_hash(name: str) -> tuple[bool, str]:
    """Compute and cache one image's sha256 (27 GB takes a few minutes)."""
    path = _candidate(name)
    if not path:
        return False, f"no such image: {name}"
    code, out = _run(["sha256sum", path], timeout=900)
    if code != 0 or not out:
        return False, out or "sha256sum failed"
    digest = out.split()[0]
    st = os.stat(path)
    cached = _load_hashes()
    cached[path] = {"sha256": digest, "size": st.st_size, "mtime": int(st.st_mtime)}
    cached.setdefault(os.path.basename(path), cached[path])
    _save_hashes(cached)
    return True, f"{os.path.basename(path)}: {digest[:16]}…"


#: What the page did lately, newest first. Short on purpose: a rig is a box on a
#: desk, and "did the last thing work" is the question people actually ask.
ACTION_LOG: "collections.deque" = collections.deque(maxlen=20)


def record(action: str, ok: bool, message: str) -> None:
    ACTION_LOG.appendleft({"at": time.strftime("%H:%M:%S"), "action": action,
                           "ok": ok, "message": message})


def recent_actions() -> list[dict]:
    return list(ACTION_LOG)


def rig_steps() -> list[dict]:
    """The rig-side checklist, computed here — no ssh, no build host.

    This is the same set 12_verify_pi.sh checks from the outside, plus what only
    the rig itself can see (services, disk, USB link).
    """
    st = rig_state()
    wifi, usb, arts = st["wifi"], st["usb"], st["artifacts"]
    steps: list[dict] = []

    def add(sid: str, title: str, ok: bool, detail: str) -> None:
        steps.append({"id": sid, "title": title,
                      "status": "ok" if ok else "todo", "detail": detail})

    add("wifi", "WiFi", wifi["connected"],
        f"{wifi['ssid']} · {wifi['ip']}" if wifi["connected"] else "not associated")
    add("fallback", "WiFi fallback (setup AP)",
        os.path.exists("/etc/rig/wifi.env")
        and _run(["systemctl", "is-enabled", "rig-ap-fallback"])[0] == 0,
        "installed" if os.path.exists("/etc/rig/wifi.env") else
        "run scripts/17_provision_wifi_fallback.sh")
    add("udc", "USB peripheral (UDC)", bool(usb["udc"]),
        usb["udc"][0] if usb["udc"] else "none — set dr_mode=peripheral and reboot")
    add("driver", "dwc2 driver bound", bool(usb["driver"]),
        ", ".join(usb["driver"]) if usb["driver"] else "unbound")
    code, out = _run(["systemctl", "show", "-p", "RuntimeWatchdogUSec", "--value"])
    add("watchdog", "Hardware watchdog off", code == 0 and out.strip() in ("0", "0s"),
        f"RuntimeWatchdogUSec={out.strip() or '?'}")
    missing = [b for b in ("nbdkit", "nbd-client", "mkfs.exfat")
               if not any(os.path.exists(f"{d}/{b}") for d in ("/usr/bin", "/usr/sbin", "/sbin"))]
    add("packages", "Gadget packages", not missing,
        "installed" if not missing else "missing: " + ", ".join(missing))
    deployed = [n for n, size in arts.items() if size]
    add("artifacts", "Deployed image", len(deployed) == len(arts),
        (f"{', '.join(deployed)} present" if deployed
         else "nothing deployed yet — build on the PC, then 11_deploy_pi.sh"))
    add("disk", "Free space", st["free_gb"] > 5, f"{st['free_gb']} GB free")
    files = deploy_state()
    missing = [f["target"] for f in files if not f["present"]]
    stale = [f["target"] for f in files if f["present"] and not f["current"]]
    add("files", "Control page files", not missing,
        "all installed" if not missing
        else "missing: " + ", ".join(os.path.basename(m) for m in missing)
        + (" (stale: " + ", ".join(os.path.basename(s) for s in stale) + ")"
           if stale else ""))
    return steps


def _esc(value: object) -> str:
    return html.escape(str(value))


def _tally(steps: list[dict]) -> str:
    ok = sum(1 for s in steps if s["status"] == "ok")
    return f"{ok}/{len(steps)} checks ok"


UPLOAD_JS = """<script>
(function (){
  var f = document.getElementById('up');
  var b = document.getElementById('upbtn');
  var p = document.getElementById('upprog');
  if (!f || !b) return;
  b.addEventListener('click', function () {
    if (!f.files.length) { p.textContent = 'choose a .img first'; return; }
    var file = f.files[0];
    var xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/images/upload?name=' + encodeURIComponent(file.name));
    b.disabled = true;
    xhr.upload.onprogress = function (e) {
      if (!e.lengthComputable) return;
      var pct = (e.loaded / e.total * 100).toFixed(1);
      p.textContent = pct + '% · ' + (e.loaded / 1e9).toFixed(2) + ' / ' +
        (e.total / 1e9).toFixed(2) + ' GB';
    };
    xhr.onload = function () {
      b.disabled = false;
      var out = {};
      try { out = JSON.parse(xhr.responseText); } catch (err) {}
      p.textContent = (out.ok ? 'done: ' : 'failed: ') + (out.message || xhr.status);
      if (out.ok) setTimeout(function () { location.reload(); }, 1200);
    };
    xhr.onerror = function () { b.disabled = false; p.textContent = 'upload failed'; };
    xhr.send(file);
  });
})();
</script>"""


#: The plugin's own log: one line per sweep of the swapped region, carrying the
#: byte counts and the duration it measured. Small enough to read whole on every
#: page load (the per-read log next to it is not touched here).
TOCTOU_LOG = os.environ.get("RIG_TOCTOU_LOG", "/var/log/toctou.log")
_REGION_RE = re.compile(r"region \[(\d+),(\d+)\) mode=(\S+) switch_after_pass=(\d+) evil=(\S+)")
_PASS_END_RE = re.compile(
    r"region@(\d+) PASS #(\d+) END reads=\d+ bytes_genuine=(\d+) bytes_evil=(\d+) "
    r"served=(\w+) span=\[[^\]]*\] dur=([\d.]+)s")
_SWITCH_RE = re.compile(r"\*\*\* region@(\d+) SWITCH at pass #(\d+)")
_BASELINE_RE = re.compile(r"open baseline=(\S+) size=(\d+) regions=(\d+)")


#: Poll the pass log only while the page is open: every tick is a read of a small
#: file on the rig, not an ssh round trip, but it still costs nothing to be lazy.
TOCTOU_JS = """<script>
(function (){
  var box = document.getElementById('toctou-body');
  if (!box) return;
  setInterval(function (){
    fetch('/api/toctou', {cache: 'no-store'})
      .then(function (r){ return r.json(); })
      .then(function (d){ if (d && d.html) box.innerHTML = d.html; })
      .catch(function (){});
  }, 5000);
})();
</script>"""


def toctou_state() -> dict:
    """What the swap has done: the region, and every completed pass with its rate.

    The rate is derived from the plugin's own numbers — bytes served over the
    duration it measured — so the page reports what the link delivered rather
    than a figure this agent invented.
    """
    state: dict = {"log": TOCTOU_LOG, "exists": False, "armed": False,
                   "serving": "", "size": 0, "passes": [], "switched": False,
                   "regions": [], "error": ""}
    try:
        with open(TOCTOU_LOG, encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()[-800:]
    except OSError as exc:
        state["error"] = type(exc).__name__
        return state
    state["exists"] = True
    for raw in lines:
        match = _BASELINE_RE.search(raw)
        if match:
            state["serving"] = os.path.basename(match.group(1))
            state["size"] = int(match.group(2))
            continue
        match = _REGION_RE.search(raw)
        if match:
            off, end, mode, switch, evil = match.groups()
            state["regions"] = [{
                "off": int(off), "len": int(end) - int(off), "mode": mode,
                "switch_after_pass": int(switch), "evil": os.path.basename(evil),
            }]
    # Only the newest session counts: the plugin numbers passes from 1 each time it
    # starts, and yesterday's "SWITCH" would otherwise read as done.
    starts = [i for i, raw in enumerate(lines) if "SESSION START" in raw]
    session = (lines[starts[-1]:] if starts else lines)
    last_pass = None
    for index, raw in enumerate(session):
        match = _PASS_END_RE.search(raw)
        if match:
            number, genuine, evil_bytes = int(match.group(2)), int(match.group(3)), int(match.group(4))
            served, dur = match.group(5), float(match.group(6))
            total = genuine + evil_bytes
            state["passes"].append({
                "n": number, "served": served, "bytes": total, "dur": dur,
                "mbps": round(total / dur / 1e6, 1) if dur > 0 else 0.0,
            })
            last_pass = index
            continue
        if _SWITCH_RE.search(raw):
            state["switched"] = True
    # The last pass is closed either by the next sweep or by the plugin exiting. In
    # the second case its duration runs to shutdown, so most of it is idle time and
    # the rate is only a lower bound — a real run showed 0.3 MB/s that way against
    # 7.8 MB/s of actual install.
    if last_pass is not None and state["passes"]:
        closed_by_session = any("SESSION END" in line for line in session[last_pass:])
        state["passes"][-1]["closed_at_session_end"] = closed_by_session
    state["passes"] = state["passes"][-6:]
    try:
        proc = subprocess.run(["systemctl", "is-active", "toctou-gadget"],
                              capture_output=True, text=True)
        state["armed"] = (proc.stdout or "").strip() == "active"
    except OSError:
        pass
    return state


def render_toctou(t: dict) -> str:
    """The swap as two lines and a pass table: what flips, and how fast it went."""
    if not t.get("exists"):
        return ('<p class="dim">No plugin log yet at '
                f'{_esc(t.get("log", ""))} ({_esc(t.get("error") or "missing")}) — '
                'the gadget writes it when it starts.</p>')
    bits = ["armed" if t.get("armed") else "not armed"]
    if t.get("serving"):
        bits.append(f'serving {t["serving"]} ({t.get("size", 0) / 1e9:.1f} GB)')
    for region in t.get("regions", []):
        bits.append(f'region {region["off"] / 1e6:.1f}–'
                    f'{(region["off"] + region["len"]) / 1e6:.1f} MB · {region["mode"]} · '
                    f'switch after pass {region["switch_after_pass"]} · '
                    f'evil {region["evil"]}')
    bits.append("switched" if t.get("switched") else "not switched yet")
    def pass_row(p: dict) -> str:
        # A pass closed by the plugin exiting carries shutdown time in its
        # duration, so its rate is a lower bound — say so instead of showing 0.3.
        tail = (f'{p["dur"]:.0f} s until session end (rate a lower bound)'
                if p.get("closed_at_session_end")
                else f'{p["dur"]:.1f} s · {p["mbps"]:.1f} MB/s')
        return (f'<li class="{"ok" if p["served"] == "GENUINE" else "todo"}">'
                f'<span class="mark">#{p["n"]}</span><b>{_esc(p["served"])}</b>'
                f'<span class="dim">{p["bytes"] / 1e6:.0f} MB · {tail}</span></li>')

    rows = "".join(
        pass_row(p)
        for p in t.get("passes", [])
    ) or ('<li class="todo"><span class="mark">!</span><b>no completed pass</b>'
          '<span class="dim">the head unit has not swept the region yet</span></li>')
    return f'<p class="dim">{" · ".join(bits)}</p><ul class="lines">{rows}</ul>'


def _render_tunnel(t: dict) -> str:
    """The way out: the USB network function, and the tunnel that dials home."""
    bits = [f'usb0 {t["usb0"]}' if t.get("usb0") else "usb0 absent"]
    link_note = "" if t.get("usb0") else (
        '<p class="dim">No network function on the cable. On the rig: '
        '<code>sudo scripts/23_usb_debug_link.sh --on</code>, then from a laptop '
        'on the same WiFi <code>ssh -J pi@&lt;rig&gt; root@10.55.0.2 -p 2224</code>. '
        'The head unit answers only once the Wifi1b package gives it 10.55.0.2 and '
        'dropbear.</p>')
    if t.get("configured"):
        bits.append(f'tunnel {t.get("active") or "?"} / {t.get("enabled") or "?"} '
                    f'-> {t.get("target") or "(no target)"} '
                    f'(-R {t.get("remote") or "0"}:127.0.0.1:22)')
        note = ""
    else:
        bits.append("tunnel not configured")
        note = (
            '<p class="dim">Write <code>/etc/rig/tunnel.env</code> on the rig (0600, '
            'root), then start it here: <code>RIG_TUNNEL_TARGET=user@host</code>, '
            '<code>RIG_TUNNEL_REMOTE=2222</code>, '
            '<code>RIG_TUNNEL_KEY=/root/.ssh/id_ed25519</code>. It stays out of the '
            'repo because it names a host.</p>')
    buttons = "" if not t.get("configured") else (
        '<div class="row">'
        '<form method="post" action="/api/tunnel">'
        '<input type="hidden" name="action" value="start">'
        '<button type="submit">Start tunnel</button></form>'
        '<form method="post" action="/api/tunnel">'
        '<input type="hidden" name="action" value="stop">'
        '<button type="submit">Stop tunnel</button></form></div>')
    return ('<section><strong>Remote access</strong>'
            f'<p class="dim">{" - ".join(bits)}</p>{note}{link_note}{buttons}</section>')


# --- tweaks (the disclaimer stick) ------------------------------------------
#: A stick built by packages/tweaks/scripts/build_disclaimer_stick.sh carries its
#: own id in the header of run_stuff.sh, a manifest over the payload, and the last
#: run's result on the drive root. The page reads all three. The acceptance itself
#: is the owner's tap on the head unit's standby screen at the next boot — the rig
#: has no way to make that decision for the car, so it only reports.
TWEAKS_ID_RE = re.compile(r"^# TWEAKS_DISCLAIMER_ID=(\S+)", re.MULTILINE)
TWEAKS_STANDBY = os.path.join("payload", "qml", "AppStandbyClock")


def _md5_of(path: str) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_manifest(root: str) -> tuple[int, int, list[str]]:
    """Check every file the stick's manifest lists — no shell, no cwd, read-only."""
    checked = failed = 0
    problems: list[str] = []
    try:
        with open(os.path.join(root, "manifest.md5"), encoding="utf-8",
                  errors="replace") as handle:
            lines = handle.readlines()
    except OSError:
        return 0, 0, ["no manifest.md5 on the stick"]
    for line in lines:
        parts = line.split()
        if len(parts) < 2:
            continue
        want, relative = parts[0].lower(), parts[-1]
        try:
            got = _md5_of(os.path.join(root, relative))
        except OSError:
            failed += 1
            problems.append(f"missing {relative}")
            continue
        checked += 1
        if got != want:
            failed += 1
            problems.append(f"mismatch {relative}")
    return checked, failed, problems


def _stick_result(root: str) -> dict:
    """The last run's key=value result, if the stick has one."""
    try:
        with open(os.path.join(root, "run_stuff_result.txt"), encoding="utf-8",
                  errors="replace") as handle:
            text = handle.read(4096)
    except OSError:
        return {}
    result = {}
    for line in text.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            result[key.strip()] = value.strip()
    return result


def tweaks_state() -> dict:
    """The disclaimer stick when one is plugged in, and what it last did."""
    out: dict = {"found": False, "sticks": []}
    for mount in _usb_mounts():
        root = mount["target"]
        try:
            with open(os.path.join(root, "run_stuff.sh"), encoding="utf-8",
                      errors="replace") as handle:
                head = handle.read(4096)
        except OSError:
            continue
        match = TWEAKS_ID_RE.search(head)
        if not match:
            continue
        standby = os.path.join(root, TWEAKS_STANDBY)
        bases: list[str] = []
        files = 0
        if os.path.isdir(standby):
            for base in sorted(os.listdir(standby)):
                try:
                    count = len([n for n in os.listdir(os.path.join(standby, base))
                                 if n.endswith(".qml")])
                except OSError:
                    continue
                if count:
                    bases.append(base)
                    files += count
        checked, failed, problems = _verify_manifest(root)
        out["found"] = True
        out["sticks"].append({
            "target": root, "id": match.group(1), "bases": bases, "files": files,
            "manifest_checked": checked, "manifest_failed": failed,
            "problems": problems[:5], "result": _stick_result(root),
        })
    return out


def render_tweaks(t: dict) -> str:
    """One block per disclaimer stick: what it carries, and what it last did."""
    if not t.get("found"):
        return ('<p class="dim">No disclaimer stick plugged into the rig. Build one with '
                '<code>packages/tweaks/scripts/build_disclaimer_stick.sh /path/to/stick</code> '
                'and plug it in here to check it before it goes to the car.</p>')
    blocks = []
    for stick in t.get("sticks", []):
        manifest = ("manifest ok" if stick["manifest_checked"] and not stick["manifest_failed"]
                    else f'manifest {stick["manifest_failed"]} problem(s)')
        heads = [f'id {stick["id"]}', f'{stick["files"]} qml',
                 "bases " + (" ".join(stick["bases"]) or "none"), manifest]
        result = stick.get("result") or {}
        rows = "".join(
            f'<li class="{"ok" if key != "status" or value == "OK" else "todo"}">'
            f'<span class="mark">·</span><b>{_esc(key)}</b>'
            f'<span class="dim">{_esc(value)}</span></li>'
            for key, value in sorted(result.items()) if key != "SCOURGE_DISCLAIMER"
        ) or ('<li class="todo"><span class="mark">!</span><b>not run yet</b>'
             '<span class="dim">plug it into the head unit; its failsafe runs run_stuff.sh</span></li>')
        problems = "".join(f'<li class="todo"><span class="mark">!</span>'
                           f'<b>{_esc(p)}</b></li>' for p in stick.get("problems", []))
        blocks.append(f'<p class="dim">{_esc(stick["target"])} · {" · ".join(heads)}</p>'
                      f'<ul class="lines">{problems}{rows}</ul>')
    blocks.append('<p class="dim">The patch applies on the head unit\'s next boot; the '
                  'disclaimer is accepted on its standby screen, not here.</p>')
    return "".join(blocks)


def render_body(st: dict, steps: list[dict]) -> str:
    """The page body: steps, files, USB control, WiFi setup.

    Dense on purpose — one line per check, everything green folded away, so the
    phone shows what needs attention without scrolling.
    """
    usb, wifi = st["usb"], st["wifi"]

    def line(mark_ok: bool, title: str, detail: str) -> str:
        return (f'<li class="{"ok" if mark_ok else "todo"}" title="{_esc(detail)}">'
                f'<span class="mark">{"&#10003;" if mark_ok else "!"}</span>'
                f'<b>{_esc(title)}</b><span class="dim">{_esc(detail)}</span></li>')

    rows = "".join(line(s["status"] == "ok", s["title"], s["detail"]) for s in steps)
    bad = [s for s in steps if s["status"] != "ok"]
    files = st.get("files", [])
    files_rows = "".join(
        line(f["present"] and not f.get("stale"),
             os.path.basename(f["target"]),
             f'{f["source"]} → {f["target"]}'
             + (f' · {f["size"]} B' if f["present"] else ' · missing')
             + (' · stale' if f.get("stale") else ''))
        for f in files
    )
    files_ok = sum(1 for f in files if f["present"] and not f.get("stale"))
    # Name the two roles instead of the plumbing: "TOCTOU mode" is what the rig
    # is for, "Normal mode" is the board behaving like any other Pi.
    role = usb_role(usb)
    role_line = (USB_ROLES[role]["label"] if role
                 else f"unclear (dr_mode {usb['mode'] or 'unset'}, "
                      f"link {'up' if usb['connected'] else 'down'})")
    actions = "".join(
        f'<button type="submit" form="usb-{key}"'
        f'{" class=\"on\"" if role == key else ""}>{_esc(spec["label"])}</button>'
        f'<form id="usb-{key}" method="post" action="/usb/role">'
        f'<input type="hidden" name="role" value="{key}"></form>'
        for key, spec in USB_ROLES.items()
    )
    if wifi["ap_active"]:
        networks = scan(timeout=25)
        options = "".join(
            f'<option value="{_esc(n["ssid"])}">{_esc(n["ssid"])} · '
            f'{_esc(n["security"])} · {n["signal"]}%</option>'
            for n in networks
        ) or '<option value="">(no networks found)</option>'
        wifi_body = (
            '<form method="post" action="/connect">'
            '<label for="ssid">Network</label>'
            f'<select id="ssid" name="ssid">{options}</select>'
            '<label for="psk">Passphrase (empty for an open network)</label>'
            '<input id="psk" name="psk" type="password" autocomplete="off">'
            '<label for="token">This access point\'s passphrase</label>'
            '<input id="token" name="token" type="password" autocomplete="off">'
            '<button type="submit">Connect</button></form>'
            f'<p class="sub">{len(networks)} network(s) visible.</p>'
        )
    else:
        wifi_body = (f'<p class="sub">Connected to {_esc(wifi["ssid"])} · '
                     f'{_esc(wifi["ip"] or "no address")}. The join form appears '
                     'while the rig runs its own setup access point.</p>')

    def gib(size: float) -> str:
        return f"{size / 1e9:.1f} GB" if size >= 1e9 else f"{size / 1e6:.0f} MB"

    imgs = st.get("images", [])

    def image_row(img: dict) -> str:
        name = _esc(img["name"])
        key = _esc(img.get("path") or img["name"])
        slug = re.sub(r"[^A-Za-z0-9.]", "_", img.get("path") or img["name"])
        serve = "" if img["served"] else (
            f'<button type="submit" form="serve-{slug}">Serve</button>'
            f'<form id="serve-{slug}" method="post" action="/images/serve">'
            f'<input type="hidden" name="name" value="{key}"></form>'
        )
        delete = "" if img.get("deletable", True) else ""
        if img.get("deletable", True):
            delete = (
                f'<button type="submit" class="danger" form="del-{slug}"'
                f' onclick="return confirm(\'Delete {name}?\')">Delete</button>'
                f'<form id="del-{slug}" method="post" action="/images/delete">'
                f'<input type="hidden" name="name" value="{key}"></form>'
            )
        where = "" if img.get("source", "build") == "build" else \
            f' · {_esc(img["source"])}'
        return (
            f'<li class="{"ok" if img["served"] else "todo"}">'
            f'<span class="mark">{"&#9679;" if img["served"] else "&#9675;"}</span>'
            f'<b>{name}</b>'
            f'<span class="dim">{gib(img["size"])}'
            f'{" · recovery" if img.get("recovery") else ""}'
            f'{" · " + _esc(img["sha256"][:12]) if img.get("sha256") else " · no hash"}'
            f'{where}</span>' + serve + delete +
            f'</li>'
        )

    if imgs:
        rows_img = "".join(image_row(i) for i in imgs)
        link_note = ("link is up — press <b>Normal mode</b> before swapping"
                     if usb["connected"] else "link down — safe to swap")
        images_block = (
            f'<details class="fold"{" open" if not any(i["served"] for i in imgs) else ""}>'
            f'<summary>Images <span class="dim">{len(imgs)} · '
            f'{next((i["name"] for i in imgs if i["served"]), "none served")}</span></summary>'
            f'<ul class="lines images">{rows_img}</ul>'
            f'<p class="dim">{link_note}. Serve re-arms the gadget, so the head '
            f'unit sees the drive disappear and come back as the new image.</p></details>'
        )
    else:
        images_block = (
            '<details class="fold"><summary>Images <span class="dim">none</span></summary>'
            f'<p class="dim">Nothing in {_esc(BUILD_DIR)} — deploy with '
            'scripts/11_deploy_pi.sh.</p></details>'
        )
    devices = st.get("devices", [])

    def device_row(dev: dict) -> str:
        slug = _esc(dev["device"].replace("/", ""))
        detail = _esc(dev["size"])
        for key in ("fstype", "label"):
            if dev.get(key):
                detail += " · " + _esc(dev[key])
        detail += " · " + (_esc(dev["mountpoint"]) if dev["mountpoint"] else "not mounted")
        mount = "" if dev["mountpoint"] else (
            f'<button type="submit" form="mnt-{slug}">Mount ro</button>'
            f'<form id="mnt-{slug}" method="post" action="/media/mount">'
            f'<input type="hidden" name="device" value="{_esc(dev["device"])}"></form>'
        )
        return (
            f'<li class="{"ok" if dev["mountpoint"] else "todo"}">'
            f'<span class="mark">{"&#10003;" if dev["mountpoint"] else "!"}</span>'
            f'<b>{_esc(dev["device"])}</b><span class="dim">{detail}</span>'
            + mount + '</li>'
        )

    if devices:
        rows_dev = "".join(device_row(d) for d in devices)
        devices_block = (
            '<details class="fold"><summary>Removable devices '
            f'<span class="dim">{len(devices)}</span></summary>'
            f'<ul class="lines images">{rows_dev}</ul>'
            '<p class="dim">Mounted read-only under /media — serving only reads. '
            'Images on a stick show up in the list above and can be served without '
            'copying them over the WiFi.</p></details>'
        )
    else:
        devices_block = ""
    upload_block = (
        '<section><strong>Upload an image</strong>'
        '<div class="row"><input id="up" type="file" accept=".img"'
        ' style="flex:1 1 220px">'
        '<button id="upbtn" type="button">Upload</button></div>'
        '<p id="upprog" class="dim">body is the file itself — progress shows here; '
        f'free {_free_gib():.1f} GB</p></section>' + UPLOAD_JS
    )

    # Green lists stay folded; the page only opens what needs a decision.
    checks = (
        f'<details class="fold"{" open" if bad else ""}>'
        f'<summary>Checks <span class="dim">{_tally(steps)}</span></summary>'
        f'<ul class="lines">{rows}</ul></details>'
    )
    files_block = (
        f'<details class="fold"{" open" if files_ok != len(files) else ""}>'
        f'<summary>Files <span class="dim">{files_ok}/{len(files)}</span></summary>'
        f'<ul class="lines">{files_rows}</ul></details>'
    )
    tweaks = st.get("tweaks") or {}
    tweaks_label = "disclaimer stick" if tweaks.get("found") else "none"
    tweaks_block = (
        f'<details class="fold"{" open" if tweaks.get("found") else ""}>'
        f'<summary>Tweaks <span class="dim">{tweaks_label}</span></summary>'
        f'<div id="tweaks-body">{render_tweaks(tweaks)}</div></details>'
    )
    toctou = st.get("toctou") or {}
    toctou_block = (
        '<details class="fold" open><summary>TOCTOU passes '
        f'<span class="dim">{len(toctou.get("passes", []))} done</span></summary>'
        f'<div id="toctou-body">{render_toctou(toctou)}</div></details>' + TOCTOU_JS
    )
    return (
        f'<p class="summary">{_tally(steps)} · {files_ok}/{len(files)} files · '
        f'{st.get("free_gb", 0)} GB free · up {st.get("uptime_h", 0)} h</p>'
        + checks + files_block + images_block + toctou_block + tweaks_block
        + devices_block + upload_block
        + GUIDE.format(open="" if role == "toctou" else " open")
        + '<section><strong>USB</strong>'
        f'<p class="dim">now: {_esc(role_line)} · boot dr_mode {_esc(usb["mode"] or "unset")}'
        f' · link {_esc("up" if usb["connected"] else "down")}</p>'
        f'<div class="row">{actions}</div></section>'
        + _render_tunnel(st.get("tunnel") or {})
        + f'<section><strong>WiFi</strong>{wifi_body}</section>'
        + _render_log(st.get("log", []))
    )


#: The run book, in the order it has to happen. Kept next to the buttons it talks
#: about, because "power, mode, plug, update" is the whole procedure and the
#: order is the part people get wrong.
GUIDE = """<details class="fold"{open}>
<summary>How to use <span class="dim">in this order</span></summary>
<ol class="lines numbered">
<li>Power the rig on its own 5 V supply — never from the head unit's USB port.</li>
<li>Press <b>TOCTOU mode</b>: the board becomes a USB mass-storage device.</li>
<li>Connect the rig's USB-C to the head unit.</li>
<li>On the head unit: <code>Settings &gt; General &gt; Software Update &gt;
Update via USB</code>, then accept the same-version confirmation.</li>
<li>Let it run to the reboot. Do not unplug while the update is writing.</li>
<li>Afterwards press <b>Normal mode</b>, then unplug.</li>
<li>Not updating? The same cable can carry ssh instead of the drive
(<code>scripts/23_usb_debug_link.sh --on</code>).</li>
</ol></details>"""


def _render_log(entries: list[dict]) -> str:
    """Pass/fail lines: what was asked, and whether it actually worked."""
    if not entries:
        return ''
    rows = "".join(
        f'<li class="{"ok" if e["ok"] else "todo"}">'
        f'<span class="mark">{"&#10003;" if e["ok"] else "&#10007;"}</span>'
        f'<span class="dim">{_esc(e["at"])} · {_esc(e["action"])} · '
        f'{_esc(e["message"])}</span></li>'
        for e in entries
    )
    return ('<details class="fold" open><summary>Recent actions'
            f'<span class="dim">{len(entries)}</span></summary>'
            f'<ul class="lines">{rows}</ul></details>')


def join(ssid: str, psk: str) -> tuple[bool, str]:
    """Hand one network to NetworkManager. Returns (ok, human message)."""
    if not SSID_RE.match(ssid):
        return False, "invalid SSID"
    if psk and not PSK_RE.match(psk):
        return False, "invalid passphrase (8-63 characters, or 64 hex digits)"
    # The AP profile must not win the race back into AP mode.
    if psk:
        code, out = nmcli("device", "wifi", "connect", ssid, "password", psk,
                          "ifname", "wlan0")
    else:
        code, out = nmcli("device", "wifi", "connect", ssid, "ifname", "wlan0")
    if code == 0:
        return True, f"connected to {ssid}"
    return False, out.splitlines()[-1] if out else "nmcli failed"


PAGE = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{host}</title>
<style>
 body{{font:15px/1.45 system-ui,sans-serif;margin:0 auto;max-width:720px;
   padding:12px 12px 28px;background:#10131a;color:#e8ebf1}}
 h1{{font-size:20px;margin:0}} p.sub{{color:#9aa4b4;font-size:13px;margin:2px 0 10px}}
 p.summary{{font-size:13px;color:#9aa4b4;margin:8px 0}}
 section{{background:#171c25;border:1px solid #2a3140;border-radius:12px;
   padding:10px 12px;margin-bottom:8px}}
 section>strong{{font-size:14px}}
 details.fold{{background:#171c25;border:1px solid #2a3140;border-radius:12px;margin-bottom:8px}}
 details.fold>summary{{padding:11px 12px;cursor:pointer;font-size:14px;min-height:44px;
   display:flex;align-items:center;gap:8px;list-style:none}}
 details.fold>summary::-webkit-details-marker{{display:none}}
 details.fold>summary::before{{content:"\\203a";color:#9aa4b4;transition:transform .15s}}
 details.fold[open]>summary::before{{transform:rotate(90deg)}}
 ul.lines{{list-style:none;margin:0;padding:0 12px 8px}}
 ul.lines li{{display:flex;gap:8px;align-items:baseline;padding:3px 0;font-size:13px;
   white-space:nowrap;overflow:hidden}}
 ul.lines li b{{flex:none;max-width:48%;overflow:hidden;text-overflow:ellipsis;font-weight:600}}
 ul.lines .mark{{flex:none;width:12px;text-align:center;font-weight:700}}
 ul.lines li.ok .mark{{color:#45c98a}} ul.lines li.todo .mark{{color:#e0aa45}}
 .dim{{color:#9aa4b4;font-size:13px;overflow:hidden;text-overflow:ellipsis}}
 ol.numbered{{counter-reset:step;padding-left:12px;padding-right:12px;margin:0 0 8px}}
 ol.numbered li{{white-space:normal;align-items:flex-start;padding:4px 0;line-height:1.35}}
 ol.numbered li::before{{counter-increment:step;content:counter(step) ".";flex:none;
   width:16px;color:#45c98a;font-weight:700}}
 code{{font:12.5px ui-monospace,Menlo,monospace;color:#c9d3e0}}
 .row{{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px}}
 .row form.inline{{display:flex;gap:8px;align-items:center;margin:0}}
 label{{display:block;font-size:13px;color:#9aa4b4;margin:8px 0 4px}}
 select,input{{width:100%;font:inherit;font-size:16px;padding:10px 12px;border-radius:10px;
   border:1px solid #2a3140;background:#10131a;color:inherit;min-height:44px}}
 button{{min-height:44px;padding:10px 16px;font:inherit;font-size:14px;font-weight:600;
   border-radius:10px;border:1px solid #45c98a;background:transparent;color:#45c98a;cursor:pointer}}
 .err{{color:#e26773}} .okc{{color:#45c98a}}
 button.danger{{border-color:#e26773;color:#e26773}}
 ul.lines.images li{{flex-wrap:wrap;white-space:normal}}
 ul.lines.images button{{min-height:32px;padding:4px 10px;font-size:13px;margin-left:auto}}
</style></head><body>
<h1>{host}</h1>
<p class="sub">{state}</p>
{body}
</body></html>"""

class Handler(BaseHTTPRequestHandler):
    server_version = "rig-wifi-agent"
    # A stalled upload must not hold a worker thread forever.
    timeout = 120

    def log_message(self, fmt: str, *args) -> None:      # journald already has it
        return

    # --- helpers ------------------------------------------------------------
    def _send(self, code: int, body: bytes, kind: str = "text/html") -> None:
        self.send_response(code)
        self.send_header("Content-Type", f"{kind}; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload: dict) -> None:
        self._send(code, json.dumps(payload).encode(), "application/json")

    def _token_ok(self, token: str) -> bool:
        expected = os.environ.get(AP_PASSPHRASE_ENV, "")
        if not expected:
            return False
        return hmac.compare_digest(token.encode(), expected.encode())

    def _page(self, message: str = "", error: bool = False) -> bytes:
        st = rig_state()
        subtitle = (f"setup access point · {st['wifi']['ip'] or 'no address'}"
                    if st["wifi"]["ap_active"] else st["wifi"]["state"])
        page = PAGE.format(host=_esc(st["hostname"]),
                           state=_esc(subtitle),
                           body=render_body(st, rig_steps()))
        if message:
            css = "err" if error else "okc"
            page = page.replace("</h1>", f'</h1><p class="{css}">'
                                        f'{_esc(message)}</p>', 1)
        return page.encode()

    # --- routes -------------------------------------------------------------
    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/":
            self._send(200, self._page())
        elif path == "/api/status":
            self._json(200, rig_state())
        elif path == "/api/wifi":
            self._json(200, status())
        elif path == "/api/usb":
            self._json(200, usb_state())
        elif path == "/api/steps":
            self._json(200, {"steps": rig_steps()})
        elif path == "/api/scan":
            self._json(200, {"networks": scan(timeout=25)})
        elif path == "/api/log":
            self._json(200, {"actions": recent_actions()})
        elif path == "/api/tweaks":
            t = tweaks_state()
            self._json(200, {"state": t, "html": render_tweaks(t)})
        elif path == "/api/toctou":
            t = toctou_state()
            self._json(200, {"state": t, "html": render_toctou(t)})
        elif path == "/api/images":
            self._json(200, {"dir": BUILD_DIR, "images": images()})
        elif path == "/api/devices":
            self._json(200, {"devices": removable_devices(),
                             "mounts": _usb_mounts()})
        else:
            self._send(404, b"not found", "text/plain")

    def _cookie(self, name: str) -> str:
        for part in (self.headers.get("Cookie") or "").split(";"):
            key, _, value = part.strip().partition("=")
            if key == name:
                return value
        return ""

    def _agent_ok(self, form_token: str = "") -> bool:  # noqa: ARG002 - call sites kept
        """Always true: the page is not gated.

        It runs on the rig, on whatever network the rig is on, and the only ways
        anyone reaches it are the owner's LAN and the setup AP that the owner
        created by powering the board in their own car. A token in front of that
        bought nothing but a field to retype on a phone, so it is gone; the
        access-point passphrase still guards `/connect`, because that one is a
        network credential, not an API gate.
        """
        return True

    def _form(self) -> dict[str, str]:
        length = min(int(self.headers.get("Content-Length") or 0), MAX_BODY)
        raw = self.rfile.read(length).decode("utf-8", "replace")
        return {key: values[0] for key, values in
                parse_qs(raw, keep_blank_values=True).items()}

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path in ("/connect", "/api/connect"):
            self._post_connect(path)
        elif path in ("/usb/role", "/api/usb/role",
                      "/usb/connect", "/usb/disconnect", "/api/usb/connect",
                      "/api/usb/disconnect", "/usb/mode", "/api/usb/mode"):
            self._post_usb(path)
        elif path in ("/images/serve", "/api/images/serve",
                      "/images/hash", "/api/images/hash",
                      "/images/delete", "/api/images/delete"):
            self._post_images(path)
        elif path in ("/images/upload", "/api/images/upload"):
            self._post_upload()
        elif path in ("/tunnel", "/api/tunnel"):
            self._post_tunnel(path)
        elif path in ("/media/mount", "/api/media/mount"):
            self._post_mount(path)
        else:
            self._send(404, b"not found", "text/plain")

    def _post_connect(self, path: str) -> None:
        if not status()["ap_active"]:
            # Reachable on the LAN too; the join form only means something while
            # the rig is serving its own AP, and this keeps it inert otherwise.
            self._json(409, {"ok": False, "error": "the rig is not in setup-AP mode"})
            return
        form = self._form()
        ssid = form.get("ssid", "")
        psk = form.get("psk", "")
        if not self._token_ok(form.get("token", "")):
            if path == "/api/connect":
                self._json(403, {"ok": False, "error": "bad access-point passphrase"})
            else:
                self._send(403, self._page("wrong access-point passphrase", error=True))
            return
        ok, message = join(ssid, psk)
        record(f"wifi join {ssid}", ok, message)
        if path == "/api/connect":
            self._json(200 if ok else 400, {"ok": ok, "message": message})
            return
        self._send(200, self._page(message, error=not ok))

    def _post_images(self, path: str) -> None:
        form = self._form()
        name = form.get("name", "")
        if path.endswith("hash"):
            ok, message = image_hash(name)
            record(f"image hash {name}", ok, message)
        elif path.endswith("delete"):
            ok, message = image_delete(name)
            record(f"image delete {name}", ok, message)
        else:
            ok, message = image_serve(name)
            record(f"image serve {name}", ok, message)
        if path.startswith("/api/"):
            self._json(200 if ok else 400, {"ok": ok, "message": message})
            return
        self._send(200, self._page(message, error=not ok))

    def _post_mount(self, path: str) -> None:
        form = self._form()
        ok, message = mount_media(form.get("device", ""))
        record(f"mount {form.get('device', '?')}", ok, message)
        if path.startswith("/api/"):
            self._json(200 if ok else 400, {"ok": ok, "message": message})
            return
        self._send(200, self._page(message, error=not ok))

    def _post_upload(self) -> None:
        """Raw-body upload: the request body IS the image."""
        query = parse_qs(urlparse(self.path).query)
        name = (query.get("name") or [""])[0]
        length = int(self.headers.get("Content-Length") or 0)
        ok, message = save_upload(name, self.rfile, length)
        record(f"image upload {name}", ok, message)
        self._json(200 if ok else 400, {"ok": ok, "message": message})

    def do_PUT(self) -> None:
        if urlparse(self.path).path in ("/images/upload", "/api/images/upload"):
            self._post_upload()
        else:
            self._send(404, b"not found", "text/plain")

    def _post_usb(self, path: str) -> None:
        form = self._form()
        if path.endswith("role"):
            role = form.get("role", "")
            ok, message = usb_apply_role(role)
            record(f"usb {role or '?'}", ok, message)
        elif path.endswith("mode"):
            ok, message = usb_set_mode(form.get("mode", ""))
            record(f"usb mode {form.get('mode', '?')}", ok, message)
        else:
            # "disconnect" ends with "connect" — match the negative explicitly.
            ok, message = usb_link("disconnect" not in path)
            record("usb connect" if ok and "disconnect" not in path else
                   "usb disconnect" if ok else "usb link", ok, message)
        if path.startswith("/api/"):
            self._json(200 if ok else 400, {"ok": ok, "message": message})
            return
        self._send(200, self._page(message, error=not ok))

    def _post_tunnel(self, path: str) -> None:
        """Start or stop the reverse tunnel."""
        form = self._form()
        action = form.get("action", "")
        ok = False
        if action not in ("start", "stop"):
            message = "action must be start or stop"
        else:
            rc, out = _run(["systemctl", "--no-ask-password", action, TUNNEL_UNIT],
                           timeout=45)
            ok = rc == 0
            message = (f"tunnel {action}: running" if ok
                       else f"tunnel {action} failed: {out[:160]}")
        record(f"tunnel {action or '?'}", ok, message)
        if path.startswith("/api/"):
            self._json(200 if ok else 400, {"ok": ok, "message": message})
            return
        self._send(200, self._page(message, error=not ok))


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address) -> None:
        """A phone that navigates away mid-request is not an error worth a
        traceback in journald."""
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def main() -> int:
    parser = argparse.ArgumentParser(description="rig WiFi setup agent (AP mode)")
    parser.add_argument("--address", default=AP_ADDRESS)
    parser.add_argument("--port", type=int, default=80)
    args = parser.parse_args()
    if not os.environ.get(AP_PASSPHRASE_ENV):
        print(f"warning: {AP_PASSPHRASE_ENV} unset — every connect will be refused",
              flush=True)
    httpd = Server((args.address, args.port), Handler)
    print(f"rig wifi agent on http://{args.address}:{args.port}/", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
