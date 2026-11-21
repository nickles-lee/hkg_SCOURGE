#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Compile and run the Orange Pi usb0 SSH lifecycle fixture."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="wifi1b-usb0-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    app = tmp / "app"
    (app / "bin").mkdir(parents=True)
    fake_c = tmp / "fake.c"
    fake_c.write_text(
        "#include <unistd.h>\n"
        "int main(void) { for (;;) sleep(60); }\n")
    subprocess.run(["gcc", "-O2", "-static", "-o",
                    str(app / "bin/dropbearmulti"), str(fake_c)],
                   check=True, timeout=60)

    test = tmp / "test_usb0_ssh"
    subprocess.run([
        "gcc", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-format-truncation",
        "-Wno-unused-function", "-Wno-unused-variable",
        "-Wno-unused-but-set-variable", "-DWIFI_TEST",
        f'-DAPP_DIR="{app}"',
        f'-DRADIO_OWNER_PATH="{tmp / "radio"}"',
        f'-DWIFI1B_USB0_SYSFS="{tmp / "usb0"}"',
        f'-DUSB0_RULE_PATH="{tmp / "99-opi-usb0.rules"}"',
        '-DWIFI_BUILD_ID="test"', "-static", "-o", str(test),
        str(ROOT / "tests/test_usb0_ssh.c")], check=True, timeout=90)
    subprocess.run([str(test)], check=True, timeout=30)

    helper = app / "bin/opi-usb0-up.sh"
    rule = tmp / "99-opi-usb0.rules"
    assert helper.is_file(), helper
    text = helper.read_text()
    assert "--usb0-apply" in text
    assert "ifconfig" not in text and "route del" not in text
    assert "iptables" not in text
    assert rule.is_file()
    rule_text = rule.read_text()
    assert 'KERNEL=="usb0"' in rule_text
    assert "opi-usb0-up.sh usb0 2224" in rule_text

print("PASS: USB SSH is ensured in WiFi client mode, under a foreign radio owner, and when WiFi is off")
