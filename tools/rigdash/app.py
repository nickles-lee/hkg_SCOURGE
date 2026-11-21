# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""rigdash — follow (and trigger) the SCOURGE build/provisioning steps in a browser.

Local tool:  python -m uvicorn tools.rigdash.app:app --host 127.0.0.1 --port 8765
or simpler:  scripts/rig_ui.sh

The dashboard is read-mostly: state comes from the filesystem and one ssh probe.
Destructive steps (anything that writes a device) require an explicit confirm.
"""
from __future__ import annotations

import asyncio
import hmac
import json
import re
import shlex
import subprocess
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
from starlette.requests import Request

from . import pipeline
from .config import Settings
from .runs import Registry

HERE = Path(__file__).resolve().parent
app = FastAPI(title="SCOURGE rig dashboard", docs_url=None, redoc_url=None)
app.mount("/static", StaticFiles(directory=HERE / "static"), name="static")
templates = Jinja2Templates(directory=str(HERE / "templates"))
registry = Registry()
SETTINGS = Settings.load()


class RunRequest(BaseModel):
    confirm: str = ""
    device: str = ""


class WifiRequest(BaseModel):
    ssid: str = ""
    psk: str = ""


SSID_RE = re.compile(r"^[^\x00-\x1f\x7f]{1,32}$")
PSK_RE = re.compile(r"^(?:[ -~]{8,63}|[0-9a-fA-F]{64})$")


def _ssh(remote: str, timeout: float = 45.0) -> tuple[int, str]:
    """One ssh round trip to the rig; returns (exit, combined output)."""
    argv = ["ssh", "-i", str(Path(SETTINGS.rig_key).expanduser()),
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
            f"{SETTINGS.rig_user}@{SETTINGS.rig_host}", remote]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, f"{type(exc).__name__}: {exc}"
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def _step(step_id: str) -> pipeline.Step:
    step = pipeline.BY_ID.get(step_id)
    if step is None:
        raise HTTPException(status_code=404, detail=f"unknown step: {step_id}")
    return step


def _plan(step: pipeline.Step, body: RunRequest) -> tuple[list[tuple[list[str], str | None]], str | None]:
    """(plan, error) — commands to run, or why this step cannot run from here."""
    settings = SETTINGS
    if step.device and body.confirm != step.id:
        return [], f"confirm required: re-send with confirm=\"{step.id}\" (this writes a device)"

    if step.script is None:
        return [], step.note or "manual step: nothing to run"
    argv = [a.replace("$DEVICE", body.device or "") for a in pipeline.command(step)]  # noqa: E501
    if any("$DEVICE" in a for a in pipeline.command(step)) and not body.device:
        return [], "this step needs a device path (e.g. /dev/sdc)"

    if step.on_rig:
        user_host = f"{settings.rig_user}@{settings.rig_host}"
        ssh = ["ssh", "-i", str(Path(settings.rig_key).expanduser()),
               "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", user_host]
        if not settings.sudo_password():
            return [], (
                "SUDO_PASSWORD is empty in config/rig.local.env, so this step cannot run "
                f"from here. Run it on the rig:\n  ssh {user_host}\n"
                f"  cd ~/{settings.pi_dir} && sudo bash scripts/{Path(step.script).name}"
            )
        commands: list[tuple[list[str], str | None]] = [
            (ssh + [f"mkdir -p ~/{settings.pi_dir}/scripts ~/{settings.pi_dir}/config"], None),
            (["rsync", "-a", "--relative", "-e",
              " ".join(["ssh", "-i", str(Path(settings.rig_key).expanduser()), "-o", "BatchMode=yes"]),
              "scripts/lib.sh", "config/firmware_locks.env",
              "tools/rigdash/pi",
              "third_party/ioniq5-usb-toctou/pizero/system",
              step.script,
              f"{user_host}:~/{settings.pi_dir}/"], None),
            (ssh + [f"cd ~/{settings.pi_dir} && sudo -S -p '' bash {step.script}"],
             settings.sudo_password() + "\n"),
        ]
        return commands, None

    return [(argv, None)], None


TOKEN = SETTINGS.dashboard_token
LOGIN = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>rigdash</title>
<style>
 body{{font:16px/1.5 system-ui,sans-serif;margin:0;padding:32px 20px;background:#10131a;color:#e8ebf1}}
 form{{max-width:420px;margin:0 auto;background:#171c25;border:1px solid #2a3140;
   border-radius:14px;padding:20px}}
 h1{{font-size:19px;margin:0 0 6px}} p{{color:#9aa4b4;font-size:14px;margin:0 0 14px}}
 input{{width:100%;box-sizing:border-box;font:inherit;padding:12px 14px;border-radius:12px;
   border:1px solid #2a3140;background:#10131a;color:inherit;min-height:48px}}
 button{{margin-top:12px;width:100%;min-height:48px;font:inherit;font-weight:600;
   padding:12px 18px;border-radius:12px;border:1px solid #45c98a;background:transparent;
   color:#45c98a}}
</style></head><body>
<form method="get" action="/">
 <h1>rigdash</h1>
 <p>This dashboard can run build scripts and sudo on the rig. Enter its token —
 the value of RIGDASH_TOKEN in config/rig.local.env.</p>
 <input name="token" type="password" autocomplete="off" autofocus>
 <button type="submit">Open</button>
</form>
</body></html>"""


@app.middleware("http")
async def require_token(request: Request, call_next):
    """Gate everything but the static assets when a token is configured.

    A cookie is set on the first token-bearing request so the SSE stream (which
    cannot carry headers) keeps working.
    """
    if not TOKEN or request.url.path.startswith("/static/"):
        return await call_next(request)
    supplied = (request.query_params.get("token")
                or request.cookies.get("rigdash")
                or request.headers.get("x-rig-token", ""))
    if not supplied or not hmac.compare_digest(supplied, TOKEN):
        return HTMLResponse(LOGIN, status_code=401)
    response = await call_next(request)
    if request.query_params.get("token"):
        response.set_cookie("rigdash", TOKEN, httponly=True, samesite="strict",
                            max_age=30 * 86400)
    return response


@app.get("/")
async def index(request: Request):
    return templates.TemplateResponse(request, "index.html", {"title": "SCOURGE rig"})


@app.get("/api/state")
async def state():
    rows, _remote = await asyncio.to_thread(pipeline.snapshot, SETTINGS)
    return {
        "steps": rows,
        "runs": registry.recent(),
        "secrets": {
            "ready": SETTINGS.secrets_ready,
            "file": SETTINGS.secrets_path.name,
            "missing": [k for k, ok in SETTINGS.secret_status().items() if not ok],
        },
        "rig": {"host": SETTINGS.rig_host, "user": SETTINGS.rig_user},
    }


@app.post("/api/steps/{step_id}/run")
async def run_step(step_id: str, body: RunRequest):
    step = _step(step_id)
    plan, error = _plan(step, body)
    run = registry.start(step_id, plan, cwd=str(SETTINGS.repo), error=error)
    return {"run_id": run.id, "state": run.state, "error": error}


@app.get("/api/runs/{run_id}")
async def run_state(run_id: str, limit: int = 400):
    run = registry.get(run_id)
    if run is None:
        raise HTTPException(status_code=404, detail="no such run")
    return run.as_dict(limit=limit)


@app.get("/api/wifi")
async def wifi_state(rescan: bool = False):
    """Association state plus the network list, read over ssh.

    `rescan` is opt-in: a forced radio scan on every page load is a rude thing
    to do to the rig's WiFi, and NetworkManager already caches the last scan.
    """
    remote = ("nmcli -t -f GENERAL.STATE,GENERAL.CONNECTION device show wlan0; "
              "echo ---; nmcli -t -f ACTIVE,SSID,SIGNAL,SECURITY device wifi list"
              + (" --rescan yes" if rescan else "") +
              "; echo ---; ip -4 -o addr show wlan0")
    code, out = await asyncio.to_thread(_ssh, remote)
    parts = out.split("---")
    if len(parts) < 2:
        return {"reachable": False, "detail": out.splitlines()[-1] if out else "unreachable"}
    state_raw, networks_raw = parts[0], parts[1]
    addr_raw = parts[2] if len(parts) > 2 else ""

    fields = {}
    for line in state_raw.splitlines():
        key, _, value = line.partition(":")
        fields[key.strip()] = value.strip()

    networks, seen, active_ssid, active_signal = [], set(), "", ""
    for line in networks_raw.splitlines():
        active, _, rest = line.partition(":")
        ssid, _, rest = rest.partition(":")
        signal, _, security = rest.partition(":")
        ssid = ssid.replace("\\:", ":")
        if not ssid or ssid in seen:
            continue
        seen.add(ssid)
        if active.strip() == "yes":
            active_ssid, active_signal = ssid, signal.strip()
        networks.append({"ssid": ssid, "signal": signal.strip() or "0",
                         "security": security.strip() or "open"})
    networks.sort(key=lambda n: int(n["signal"]) if n["signal"].isdigit() else 0,
                  reverse=True)

    ip = ""
    for line in addr_raw.splitlines():
        if " inet " in line:
            ip = line.split(" inet ")[1].split()[0].split("/")[0]
            break

    return {
        "reachable": True,
        "connected": fields.get("GENERAL.STATE", "").startswith("100"),
        "state": fields.get("GENERAL.STATE", "unknown"),
        "connection": fields.get("GENERAL.CONNECTION", "").strip("--"),
        "ssid": active_ssid or fields.get("GENERAL.CONNECTION", "").strip("--"),
        "signal": active_signal,
        "ip": ip,
        "networks": networks,
    }


@app.post("/api/wifi/connect")
async def wifi_connect(body: WifiRequest):
    if not SSID_RE.match(body.ssid):
        raise HTTPException(status_code=400, detail="invalid SSID")
    if body.psk and not PSK_RE.match(body.psk):
        raise HTTPException(status_code=400, detail="invalid passphrase")
    argv = ["nmcli", "device", "wifi", "connect", body.ssid]
    if body.psk:
        argv += ["password", body.psk]
    argv += ["ifname", "wlan0"]
    remote = " ".join(shlex.quote(part) for part in argv)
    code, out = await asyncio.to_thread(_ssh, remote, 60.0)
    # The passphrase never reaches the log: the command is not echoed anywhere.
    return {"ok": code == 0, "message": out.splitlines()[-1] if out else ""}


@app.get("/api/runs/{run_id}/stream")
async def run_stream(run_id: str, offset: int = 0):
    run = registry.get(run_id)
    if run is None:
        raise HTTPException(status_code=404, detail="no such run")

    async def events():
        sent = offset
        while True:
            lines = run.lines[sent:]
            if lines:
                sent += len(lines)
                for line in lines:
                    yield f"data: {json.dumps(line)}\n\n"
            if run.state != "running":
                yield f"event: end\ndata: {json.dumps({'state': run.state, 'exit_code': run.exit_code})}\n\n"
                return
            await asyncio.sleep(0.25)

    return StreamingResponse(events(), media_type="text/event-stream")
