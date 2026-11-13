# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Persistent VM daemon: boot once, then exec/screenshot/stop over a socket."""
from __future__ import annotations

import fcntl
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from contextlib import ExitStack, contextmanager
from pathlib import Path

from .governor import LockTimeout, run_lock

DEFAULT_DAEMON_DIR = "runs/daemon"


@contextmanager
def _start_lock(daemon_dir: Path, timeout: float = 30.0):
    """Parent-side lock so two `daemon start` calls cannot clobber a dir."""
    path = Path(daemon_dir).with_name(Path(daemon_dir).name + ".start.lock")
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(path, "w")
    deadline = time.monotonic() + timeout
    try:
        while True:
            try:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"another daemon start is in progress: {path}")
                time.sleep(0.1)
        yield
    finally:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        finally:
            handle.close()


def status_path(daemon_dir: Path) -> Path:
    return Path(daemon_dir) / "status.json"


def config_path(daemon_dir: Path) -> Path:
    return Path(daemon_dir) / "config.json"


def read_status(daemon_dir: Path) -> dict:
    try:
        value = json.loads(status_path(daemon_dir).read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def write_status(daemon_dir: Path, status: dict) -> None:
    path = status_path(daemon_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(status, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def pid_alive(pid: int | None) -> bool:
    if not pid:
        return False
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError):
        return False
    return True


def daemon_pid_matches(pid: int | None, daemon_dir: Path | None = None) -> bool:
    """True only if `pid` is a live daemon _serve for this daemon directory."""
    if not pid_alive(pid):
        return False
    try:
        cmdline = Path(f"/proc/{int(pid)}/cmdline").read_bytes()
    except OSError:
        return False
    if b"daemon" not in cmdline or b"_serve" not in cmdline:
        return False
    if daemon_dir is not None:
        expected = str((Path(daemon_dir) / "config.json").resolve()).encode()
        if expected not in cmdline:
            return False
    return True


def request(daemon_dir: Path, payload: dict, timeout: float = 15.0) -> dict:
    path = Path(daemon_dir) / "control.sock"
    deadline = time.monotonic() + timeout
    while not path.exists():
        if time.monotonic() > deadline:
            raise TimeoutError(f"daemon control socket missing: {path}")
        time.sleep(0.05)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(max(0.1, deadline - time.monotonic()))
    try:
        sock.connect(str(path))
        handle = sock.makefile("rwb", buffering=0)
        handle.write((json.dumps(payload) + "\n").encode())
        line = handle.readline()
        if not line:
            raise RuntimeError("daemon closed the control connection")
        response = json.loads(line)
        handle.close()
        if not isinstance(response, dict):
            raise RuntimeError("invalid daemon response")
        return response
    finally:
        sock.close()


def _tail(path: Path, lines: int = 40) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return ""
    return "\n".join(text.splitlines()[-lines:]) + "\n"


def start(config: dict, launch_path: Path, timeout: float = 180.0) -> dict:
    daemon_dir = Path(config["daemon_dir"])
    with _start_lock(daemon_dir, timeout=timeout):
        previous = read_status(daemon_dir)
        if previous.get("state") in ("starting", "ready") and pid_alive(previous.get("daemon_pid")):
            raise RuntimeError(f"daemon already running: {daemon_dir}")
        if daemon_dir.exists():
            shutil.rmtree(daemon_dir)
        daemon_dir.mkdir(parents=True)
        config_path(daemon_dir).write_text(json.dumps(config, indent=2) + "\n")
        log = open(daemon_dir / "daemon.log", "ab")
        try:
            process = subprocess.Popen(
                [sys.executable, "-B", str(launch_path), "daemon", "_serve",
                 "--config", str(config_path(daemon_dir))],
                cwd=str(daemon_dir), stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
        finally:
            log.close()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = read_status(daemon_dir)
            if status.get("state") == "ready":
                status["daemon_pid"] = process.pid
                return status
            if status.get("state") == "error":
                raise RuntimeError(status.get("error") or "daemon failed to start")
            if process.poll() is not None:
                raise RuntimeError(
                    f"daemon exited rc={process.returncode}: "
                    + _tail(daemon_dir / "daemon.log", 30))
            time.sleep(0.2)
        raise TimeoutError("daemon did not become ready; see daemon.log")


def stop(config_dir: Path, timeout: float = 20.0) -> dict:
    daemon_dir = Path(config_dir)
    status = read_status(daemon_dir)
    try:
        response = request(daemon_dir, {"action": "stop"}, timeout=5)
    except (OSError, RuntimeError, TimeoutError):
        response = {}
    pid = status.get("daemon_pid") or response.get("daemon_pid")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = read_status(daemon_dir)
        if current.get("state") == "stopped":
            return current
        if pid and not pid_alive(pid):
            return current
        time.sleep(0.2)
    if pid and daemon_pid_matches(pid, daemon_dir):
        try:
            os.kill(int(pid), signal.SIGTERM)
        except OSError:
            pass
    elif pid and pid_alive(pid):
        # Identity check failed (wrapper/renamed cmdline): never signal an
        # unknown process, but still reap this daemon's QEMU so it cannot
        # keep writing state overlays.
        qemu_pid = status.get("qemu_pid")
        if qemu_pid and pid_alive(qemu_pid):
            try:
                os.kill(int(qemu_pid), signal.SIGKILL)
            except OSError:
                pass
    return read_status(daemon_dir)


def _serve(config: dict) -> int:
    import launch  # late import avoids a module cycle

    daemon_dir = Path(config["daemon_dir"])
    kernel_target = config.get("kernel_target", "vendor")
    status = {"state": "starting", "daemon_pid": os.getpid(), "mode": kernel_target,
              "daemon_dir": str(daemon_dir), "started": time.time()}
    write_status(daemon_dir, status)
    proc = None
    ser = None
    state_lock = None
    lock_stack = None
    server = None
    stop_event = threading.Event()
    guest_lock = threading.Lock()

    def _term_handler(signum, frame):  # noqa: ARG001
        stop_event.set()
        if proc is not None and proc.poll() is None:
            proc.kill()

    old_term = signal.signal(signal.SIGTERM, _term_handler)
    try:
        lock_timeout = min(float(config.get("lock_timeout", 180)),
                           float(config.get("ready_timeout", 180)))
        lock_stack = ExitStack()
        lock_info = lock_stack.enter_context(run_lock(launch.VM, lock_timeout))
        pin_cpu = launch.validate_pin_cpu(config.get("pin_cpu"))
        status.update({"lock": lock_info, "pin_cpu": pin_cpu})
        write_status(daemon_dir, status)
        tools = Path(config["tools"]) if config.get("tools") else None
        usb = Path(config["usb"]) if config.get("usb") else None
        payload = Path(config["payload"]) if config.get("payload") else None
        if tools is not None:
            tools_status = launch.check_tools(tools)
            if not tools_status["ok"]:
                raise RuntimeError(f"tools image stale: {tools_status['detail']}")
        inputs = {"rootfs": launch.ROOTFS}
        if tools is not None:
            inputs["tools"] = tools.resolve(strict=True)
        if usb is not None:
            inputs["usb"] = usb.resolve(strict=True)
        if payload is not None:
            payload = payload.resolve(strict=True)
            if payload.is_dir():
                payload = launch.build_payload_image(payload, daemon_dir / "payload.img")
            inputs["payload"] = payload
        state = Path(config["state"]).resolve() if config.get("state") else None
        if state is not None:
            overlays, state_lock = launch.create_state_overlays(
                state, inputs, reset=bool(config.get("reset")))
        else:
            overlays = {}
            for name, path in inputs.items():
                overlays.update(launch.create_overlays(daemon_dir, {name: path}))
        qmp_socket = daemon_dir / "qmp.sock"
        display_device = config.get("display_device") or "none"
        if display_device == "none":
            display_device = ("virtio-vga-gl" if config.get("display") == "egl-headless"
                              else "virtio-vga") if kernel_target == "compat" else "std"
        argv = launch.qemu_argv(
            daemon_dir, usb, tools, False, payload, overlays,
            qmp_socket=qmp_socket, display_device=display_device,
            smp=int(config.get("smp", 1)), memory_mb=int(config.get("memory_mb", 4096)),
            noapic=not bool(config.get("apic")), kernel_target=kernel_target,
            share=Path(config["share"]) if config.get("share") else None,
            display=config.get("display", "none"),
            vnc_display=config.get("vnc_display", ":1"),
            network=bool(config.get("network")), audio=bool(config.get("audio")),
            usb_host=config.get("usb_host") or None,
            cache=config.get("cache", "writeback"))
        (daemon_dir / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        proc = subprocess.Popen(argv, cwd=daemon_dir, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True,
                                preexec_fn=launch.qemu_preexec(pin_cpu))
        if stop_event.is_set():
            # SIGTERM arrived during setup; do not leave QEMU running.
            proc.kill()
            proc.wait(10)
            status.update({"state": "stopped", "exit_code": proc.returncode})
            return 0
        ser = launch.Serial(proc, daemon_dir / "serial.log")
        if kernel_target == "vendor":
            launch.patch_sdhci(daemon_dir, proc, 30)
        started = time.monotonic()
        ser.read_until(b"IONIQ_VM_READY", float(config.get("ready_timeout", 180)),
                       fatal=True)
        launch.wait_shell(ser, float(config.get("ready_timeout", 180)))
        status.update({"state": "ready", "boot_seconds": round(time.monotonic() - started, 2),
                       "qemu_pid": proc.pid, "qmp": str(qmp_socket),
                       "display": config.get("display", "none"),
                       "display_device": display_device,
                       "overlays": {name: item["path"] for name, item in overlays.items()},
                       "state_dir": str(state) if state else None,
                       "serial": str(daemon_dir / "serial.log")})
        write_status(daemon_dir, status)

        control = daemon_dir / "control.sock"
        control.unlink(missing_ok=True)
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(control))
        os.chmod(control, 0o600)
        server.listen(8)
        server.settimeout(0.5)

        def handle(conn):
            try:
                handle = conn.makefile("rwb", buffering=0)
                line = handle.readline()
                if not line:
                    return
                message = json.loads(line)
                action = message.get("action")
                if action == "status":
                    current = read_status(daemon_dir)
                    current.update({"reachable": True,
                                    "qemu_alive": proc.poll() is None,
                                    "serial_bytes": (daemon_dir / "serial.log").stat().st_size
                                    if (daemon_dir / "serial.log").is_file() else 0})
                    response = {"ok": True, "status": current}
                elif action == "exec":
                    cmd = message.get("cmd")
                    if not isinstance(cmd, str) or not cmd:
                        response = {"ok": False, "error": "missing command"}
                    else:
                        with guest_lock:
                            token, output, rc, seconds = launch.execute_guest(
                                ser, cmd, float(message.get("timeout", 300)))
                        with (daemon_dir / "daemon-commands.log").open("a") as log:
                            log.write(f"=== {time.time()} rc={rc} ===\n{output}\n")
                        response = {"ok": True, "output": output, "rc": rc,
                                    "seconds": seconds}
                elif action == "screenshot":
                    output = Path(message.get("output", daemon_dir / "screenshot.png"))
                    output = output.resolve()
                    root = daemon_dir.resolve()
                    if output != root and root not in output.parents:
                        response = {"ok": False,
                                    "error": f"screenshot outside daemon dir: {output}"}
                        handle.write((json.dumps(response) + "\n").encode())
                        return
                    output.parent.mkdir(parents=True, exist_ok=True)
                    result = launch.qmp_screendump(qmp_socket, output)
                    response = {"ok": True, "screenshot": result}
                elif action == "stop":
                    try:
                        with guest_lock:
                            launch.execute_guest(ser, "sync", 10)
                    except Exception:  # noqa: BLE001
                        pass
                    response = {"ok": True, "stopping": True,
                                "daemon_pid": os.getpid()}
                    handle.write((json.dumps(response) + "\n").encode())
                    stop_event.set()
                    if proc.poll() is None:
                        proc.kill()
                    return
                else:
                    response = {"ok": False, "error": f"unknown action: {action}"}
                handle.write((json.dumps(response) + "\n").encode())
            except Exception as exc:  # noqa: BLE001
                try:
                    conn.sendall((json.dumps({"ok": False, "error": str(exc)}) + "\n").encode())
                except OSError:
                    pass
            finally:
                conn.close()

        while not stop_event.is_set() and proc.poll() is None:
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            threading.Thread(target=handle, args=(conn,), daemon=True).start()
        status.update({"state": "stopped", "exit_code": proc.poll()})
    except LockTimeout as exc:
        status.update({"state": "error", "error": f"lock timeout: {exc}"})
    except Exception as exc:  # noqa: BLE001
        status.update({"state": "error", "error": f"{type(exc).__name__}: {exc}"})
        if proc is not None and proc.poll() is None:
            proc.kill()
    finally:
        stop_event.set()
        if server is not None:
            try:
                server.close()
            except OSError:
                pass
            (daemon_dir / "control.sock").unlink(missing_ok=True)
        if proc is not None:
            try:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(10)
            except Exception:  # noqa: BLE001
                pass
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
        if state_lock is not None:
            state_lock.close()
        if lock_stack is not None:
            lock_stack.close()
        try:
            signal.signal(signal.SIGTERM, old_term)
        except (ValueError, OSError):
            pass
        status["stopped_at"] = time.time()
        write_status(daemon_dir, status)
    return 0 if status["state"] == "stopped" else 1


def console(daemon_dir: Path, lines: int = 200, follow: bool = False) -> int:
    path = Path(daemon_dir) / "serial.log"
    if not path.is_file():
        raise RuntimeError(f"serial log missing: {path}")
    data = path.read_text(errors="replace")
    print("\n".join(data.splitlines()[-lines:]))
    if not follow:
        return 0
    position = len(data)
    try:
        while True:
            time.sleep(0.2)
            if path.stat().st_size < position:
                position = 0
            with open(path, "r", errors="replace") as handle:
                handle.seek(position)
                chunk = handle.read()
                position = handle.tell()
            if chunk:
                sys.stdout.write(chunk)
                sys.stdout.flush()
    except KeyboardInterrupt:
        return 0
