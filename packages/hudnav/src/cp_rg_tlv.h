/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef HUDNAV_CP_RG_TLV_H
#define HUDNAV_CP_RG_TLV_H

#include <stddef.h>

/* iAP2 message IDs registered in Identification: sent by accessory / received from device. */
extern const unsigned short CP_RG_SENT_IDS[2];   /* 0x5200, 0x5203 */
extern const unsigned short CP_RG_RECV_IDS[3];   /* 0x5201, 0x5202, 0x5204 */

/* Copy in[0..inlen) to out, then append each id as a big-endian 16-bit value.
 * Returns the new length, or 0 if cap is too small / args invalid. */
size_t cp_rg_append_ids(unsigned char *out, size_t cap, const unsigned char *in,
                        size_t inlen, const unsigned short *ids, size_t nids);

/* Build the VALUE of Identification parameter 30 (0x001E),
 * RouteGuidanceDisplayComponent, as nested iAP2 parameters:
 *   0 Identifier U16 42, 1 Name "RouteGuidance" + NUL, 2 U16 64,
 *   4 U16 64, 6 U16 8, 7 U16 64, 8 U16 8.
 * Returns the length, or 0 if cap is too small / args invalid. */
size_t cp_rg_build_component(unsigned char *out, size_t cap);

/* Build a complete iAP2 control message 0x5200 StartRouteGuidanceUpdates:
 * 40 40 len(BE16) 52 00 [params]. len counts from the first 0x40 to the end
 * of the params. with_component_id != 0 adds one U16 param id 0 = 42 as an
 * experiment flag. Returns the length, or 0 if cap is too small / args invalid. */
size_t cp_rg_build_start(unsigned char *out, size_t cap, int with_component_id);

#endif
