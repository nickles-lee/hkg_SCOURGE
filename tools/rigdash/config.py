# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Dashboard settings.

Resolution order for every value: os.environ > config/rig.local.env >
config/paths.local.env > config/paths.env.example. The rig secrets (WiFi PSK,
login password, SSH key line) live only in config/rig.local.env, which is
gitignored; nothing in this module returns them to a template or a response.
"""
from __future__ import annotations

import os
import shlex
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LOCAL_ENV = REPO / "config" / "paths.local.env"
EXAMPLE_ENV = REPO / "config" / "paths.env.example"
SECRETS = REPO / "config" / "rig.local.env"

#: Values the first-boot template needs; a step is only runnable when all of
#: them are set and none is still holding the example placeholder.
SECRET_KEYS = ("RIG_HOSTNAME", "RIG_USER", "RIG_PASSWORD", "SSH_PUBKEY",
               "WIFI_COUNTRY", "WIFI_SSIDS", "WIFI_PSK",
               "RIG_AP_SSID", "RIG_AP_PASSPHRASE")


def parse_env_file(path: Path) -> dict[str, str]:
    """KEY=value lines, shell-quoted values understood. Missing file → {}."""
    values: dict[str, str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return values
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        try:
            parts = shlex.split(value)
        except ValueError:
            parts = [value]
        values[key.strip()] = parts[0] if parts else ""
    return values


def _placeholder(value: str) -> bool:
    return not value or ("<" in value and ">" in value)


@dataclass(frozen=True)
class Settings:
    """Everything the dashboard needs, with secrets kept behind accessors.

    `root` exists so tests can evaluate the pipeline against a temporary tree;
    production always uses the repo this package lives in.
    """

    values: dict[str, str]
    root: Path = REPO

    @classmethod
    def load(cls, secrets: Path | None = None, root: Path | None = None) -> "Settings":
        merged: dict[str, str] = {}
        merged.update(parse_env_file(EXAMPLE_ENV))
        merged.update(parse_env_file(LOCAL_ENV))
        override = os.environ.get("RIGDASH_SECRETS")
        merged.update(parse_env_file(Path(override) if override else (secrets or SECRETS)))
        return cls(merged, root or REPO)

    def get(self, key: str, default: str = "") -> str:
        return os.environ.get(key) or self.values.get(key, default)

    # --- repo layout ----------------------------------------------------------
    @property
    def repo(self) -> Path:
        return self.root

    @property
    def out_dir(self) -> Path:
        return Path(self.get("OUT_DIR", str(REPO / "out")))

    @property
    def work_dir(self) -> Path:
        return Path(self.get("WORK_DIR", str(REPO / "work")))

    @property
    def input_dir(self) -> Path:
        return Path(self.get("INPUT_DIR", str(REPO / "input")))

    # --- rig ----------------------------------------------------------------
    @property
    def rig_host(self) -> str:
        return self.get("PI_HOST") or self.get("RIG_HOSTNAME") or "rig"

    @property
    def rig_user(self) -> str:
        return self.get("PI_USER", "pi")

    @property
    def rig_key(self) -> str:
        return self.get("RIG_KEY", "~/.ssh/scourge_pi")

    @property
    def pi_dir(self) -> str:
        return self.get("PI_DIR", "scourge")

    @property
    def bootfs_label(self) -> str:
        return self.get("PI_BOOTFS_LABEL", "bootfs")

    # --- secrets: presence only ---------------------------------------------
    def secret_status(self) -> dict[str, bool]:
        """Key → 'usable', never the value itself."""
        return {key: not _placeholder(self.get(key)) for key in SECRET_KEYS}

    @property
    def secrets_ready(self) -> bool:
        return all(self.secret_status().values())

    @property
    def secrets_path(self) -> Path:
        override = os.environ.get("RIGDASH_SECRETS")
        return Path(override) if override else SECRETS

    def sudo_password(self) -> str:
        """Only used to answer a `sudo -S` prompt on the rig; never logged."""
        return self.get("SUDO_PASSWORD")

    @property
    def dashboard_token(self) -> str:
        """Shared secret required when the dashboard listens beyond loopback.

        The page can run build scripts and `sudo` on the rig, so exposing it on
        the LAN without a gate would hand that to every device on the network.
        Empty means "loopback only" (the app then trusts the bind address).
        """
        return self.get("RIGDASH_TOKEN")
