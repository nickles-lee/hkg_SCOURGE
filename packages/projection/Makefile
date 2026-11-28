# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Projection+ (WIP, car-run once on one AU 260319 car (install, tile, status); restore VM-only; docs/STATUS.md).
# The daemon is built static with the host toolchain, as Tweaks does (make -C src).
.PHONY: all daemon icons payload test test-daemon test-installer clean

all: daemon

daemon:
	$(MAKE) -C src

icons:
	python3 scripts/make_icons.py

# Needs build/manifest.txt + build/qml_eco3.0/... from scripts/build_artifacts.py.
payload: daemon
	sh scripts/stage_payload.sh build build/usb

test: test-daemon test-installer

test-daemon: daemon
	python3 tests/test_daemon.py

test-installer:
	sh tests/test_installer.sh

clean:
	$(MAKE) -C src clean
	rm -rf build/usb
