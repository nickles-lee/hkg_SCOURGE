#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Expose guest gdbserver on a second UART backed by a local Unix socket, no NIC."""
from pathlib import Path
import argparse
import sys

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
import launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--wait', type=int, default=20)
    args = parser.parse_args()
    if not 5 <= args.wait <= 60:
        parser.error('--wait must be 5..60 seconds')
    original = launch.qemu_argv

    def debugger_uart(run_dir, *positional, **keywords):
        argv = original(run_dir, *positional, **keywords)
        print(f'QT_GDB_SOCKET {run_dir / "userspace.sock"}', flush=True)
        # ttyS0 remains the command channel; ttyS1 is isolated debugger traffic.
        return argv + ['-serial', 'unix:userspace.sock,server=on,wait=off']

    launch.qemu_argv = debugger_uart
    command = (f'export GRAPHICS_ENTROPY=1 GRAPHICS_GDB=1 GRAPHICS_WAIT_SECONDS={args.wait}\n' +
               (VM / 'probes/graphics-diagnose.sh').read_text())
    try:
        return launch.run(command, False, 300, tools=args.tools, payload=args.payload,
                          storage_mode='split', collect_audit=True)
    finally:
        launch.qemu_argv = original


if __name__ == '__main__':
    sys.exit(main())
