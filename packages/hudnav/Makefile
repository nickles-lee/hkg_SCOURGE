# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# HUD Nav backend — WIP, car-proven on ONE firmware only. See docs/STATUS.md.
# Builds are host-only (WSL/Linux gcc + python3); no car, no firmware, no OEM bytes.
.PHONY: all daemon tools test smoke clean

all: daemon tools

daemon:
	sh scripts/build.sh

tools: daemon

test: daemon
	sh scripts/test.sh

smoke: daemon
	python3 scripts/smoke.py

clean:
	rm -rf build out
	rm -f src/hudnavd src/*.so
