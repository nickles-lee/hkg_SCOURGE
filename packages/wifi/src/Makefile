# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS ?= -static
BUILD_ID = $(shell python3 ../scripts/build_id.py)

all: wifi_daemon

wifi_daemon: wifi_daemon.c FORCE
	$(CC) $(CFLAGS) -DWIFI_BUILD_ID='"$(BUILD_ID)"' $(LDFLAGS) -o $@ $<

clean:
	rm -f wifi_daemon

FORCE:
.PHONY: all clean FORCE
