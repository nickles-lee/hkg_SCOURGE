# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Framed serial command execution; completion cannot be satisfied by TTY echo."""
from __future__ import annotations

import re
import shlex
import uuid


def command_frame(command: str, token: str | None = None) -> tuple[str, str]:
    token = token or uuid.uuid4().hex
    if not re.fullmatch(r'[0-9a-f]{32}', token):
        raise ValueError('invalid command token')
    # Split marker words: even a wrapped echoed command cannot contain a marker.
    prefix = f"printf '\\n%s%s %s\\n' IONIQ_ COMMAND_BEGIN {token}; "
    suffix = ("; __vm_rc=$?; printf '\\n%s%s %s %s\\n' "
              f"IONIQ_ COMMAND_END {token} \"$__vm_rc\"\n")
    return token, prefix + '/bin/bash -c ' + shlex.quote(command) + suffix


def completion_pattern(token: str) -> bytes:
    # Require the newline, not merely the end of the current read buffer. A
    # serial chunk ending in " 7" must not complete a still-arriving " 77".
    return rb'(?m)^IONIQ_COMMAND_END ' + token.encode() + rb' ([0-9]{1,3})\r?\n'


def command_output(text: str, token: str) -> tuple[str, int]:
    begin = re.search(r'(?m)^IONIQ_COMMAND_BEGIN ' + token + r'\r?\n', text)
    end = re.search(completion_pattern(token).decode(), text)
    if begin is None or end is None or end.start() < begin.end():
        raise ValueError('missing or out-of-order command frame')
    rc = int(end.group(1))
    if rc > 255:
        raise ValueError('invalid guest exit status')
    return text[begin.end():end.start()].rstrip('\r\n'), rc
