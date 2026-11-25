# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
.PHONY: all payload test test-carplay
all: payload
payload:
	sh scripts/build_payload.sh
test-carplay:
	@set -e; for test in tests/test_carplay_*.py; do \
		PYTHONDONTWRITEBYTECODE=1 python3 "$$test"; \
	done
test: payload test-carplay
	sh -n scripts/hotswap_daemon.sh
	sh tests/test_hotswap.sh
	python3 tests/test_release1b.py
	python3 tests/test_qml1b.py
	python3 tests/test_qmlpatch.py
	python3 tests/test_daemon1b.py
	python3 tests/test_usb0_ssh.py
	python3 tests/test_bt_readback.py
	sh tests/test_apconfig.sh
	python3 tests/test_dhcp.py
	sh tests/test_ports1b.sh
	python3 tests/test_qt_overlay.py
	python3 tests/test_side_by_side.py
	python3 ../common/test_radio_owner.py
