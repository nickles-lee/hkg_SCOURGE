/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Pluggable output backend. The CAN backend (micom SetCANMicomRecv via the
 * car-proven CANService helper) is a stub until the AU frame layouts land
 * (docs/hudnav/HUD_NAV_REPORT.md). The log backend is for host tests / bench mocks.
 */
#ifndef HUDNAV_OUTPUT_H
#define HUDNAV_OUTPUT_H

#include "hudnav.h"
#include <stdio.h>

typedef struct {
    void *ctx;
    bool (*emit_turn)(void *ctx, const hn_native *n);
    bool (*emit_clear)(void *ctx);
    bool (*emit_guidance)(void *ctx, bool active);  /* 0x4EA-style claim/clear */
    void (*destroy)(void *ctx);
} hn_output;

/* Log backend: writes frames to a FILE* (stderr or a capture file). */
hn_output hn_output_log(FILE *f);

/* CAN backend: emits the confirmed micom SetCANMicomRecv wire format with a
 * PROVISIONAL turn-byte layout. send_path NULL = print only; else frames are also
 * transmitted by exec'ing that helper as "CANService" (whitelisted IDs only). */
hn_output hn_output_can(FILE *f, const char *send_path);

#endif
