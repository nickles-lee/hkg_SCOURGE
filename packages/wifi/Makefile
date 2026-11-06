# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
.PHONY: all daemon icons artifacts payload test clean

all: payload

daemon:
	$(MAKE) -C src

icons:
	python3 scripts/make_icons.py

artifacts: icons
	python3 scripts/build_artifacts.py

payload: daemon
	sh scripts/build_payload.sh

test: payload
	python3 tests/test_tiles.py
	sh tests/test_daemon.sh
	sh tests/test_ports.sh
	sh tests/test_wifi_stack.sh
	sh tests/test_logdump.sh
	sh tests/test_ssh_persistence.sh
	sh tests/test_installer.sh stock
	sh tests/test_installer.sh hello
	sh tests/test_installer.sh battery
	sh tests/test_installer.sh hello_battery
	sh tests/test_installer.sh battery_tile
	sh tests/test_installer.sh hello_battery_tile

clean:
	$(MAKE) -C src clean
	rm -rf out/usb
