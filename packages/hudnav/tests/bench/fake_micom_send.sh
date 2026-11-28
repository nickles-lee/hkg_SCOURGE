#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Bench stand-in for micom-send/CANService: records the array arg it was given.
echo "$1" >> "${HUDNAV_BENCH_LOG:-/tmp/hudnav_tx.log}"
