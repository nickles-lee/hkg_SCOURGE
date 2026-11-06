# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS ?= -static

all: tweaks_daemon wrandr_wrapper

tweaks_daemon: tweaks_daemon.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

wrandr_wrapper: wrandr_wrapper.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

persist:
	sh ../scripts/build_target_helper.sh

clean:
	rm -f tweaks_daemon wrandr_wrapper tweaks_persist

.PHONY: all persist clean
