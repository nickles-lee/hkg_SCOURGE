# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wno-format-truncation
LDFLAGS ?= -static
BUILD_ID ?= $(shell sha256sum projection_daemon.c | cut -c1-12)
CPPFLAGS += -DPROJECTION_BUILD_ID='"$(BUILD_ID)"'

all: projection_daemon

projection_daemon: projection_daemon.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f projection_daemon

.PHONY: all clean
