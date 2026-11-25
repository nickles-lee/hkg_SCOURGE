# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LDFLAGS ?= -static
BUILD_ID = $(shell python3 ../scripts/build_id.py)

all: wifi1b_daemon wifi1b_qmlpatch

# Host-only experimental broker helper. The target-rootfs build is a separate
# deferred gate; this target lets the offline fixtures exercise the helper.
BROKER_SOURCES = carplay/cp_broker_main.c carplay/cp_wire.c carplay/cp_link.c \
	carplay/cp_bootstrap.c carplay/cp_session.c carplay/cp_auth_broker.c \
	carplay/cp_oem_auth_async.c carplay/cp_spp_dbus.c carplay/cp_spp_socket.c \
	carplay/cp_runtime_broker.c carplay/cp_usb.c carplay/cp_receiver_lookup.c \
	carplay/cp_receiver_policy.c carplay/cp_oem_receiver.c
carplay-broker:
	mkdir -p ../out/carplay-broker
	$(CC) $(CFLAGS) -DCP_SPP_DBUS_HAVE_LIBXML2 -o ../out/carplay-broker/carplay-broker \
		$(BROKER_SOURCES) $(shell pkg-config --cflags --libs dbus-1 libxml-2.0) -ldl -pthread

wifi1b_daemon: wifi1b_daemon.c wifi1b_backend.inc ../../common/radio_owner.h FORCE
	$(CC) $(CFLAGS) -DWIFI_BUILD_ID='"$(BUILD_ID)"' $(LDFLAGS) -o $@ wifi1b_daemon.c

wifi1b_qmlpatch: wifi1b_qmlpatch.c FORCE
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ wifi1b_qmlpatch.c

clean:
	rm -f wifi1b_daemon wifi1b_qmlpatch
	rm -f ../out/carplay-broker/carplay-broker

FORCE:
.PHONY: all clean carplay-broker FORCE
