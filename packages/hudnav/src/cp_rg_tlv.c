/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_rg_tlv.h"
#include <string.h>

const unsigned short CP_RG_SENT_IDS[2] = { 0x5200, 0x5203 };
const unsigned short CP_RG_RECV_IDS[3] = { 0x5201, 0x5202, 0x5204 };

/* iAP2 parameter framing (cp_iap2_impl_spec.md §2.2, rgd_tlv.c RGD_WALK):
 * BE16(value_len + 4), BE16(id), value bytes. */
static size_t put_u16_param(unsigned char *p, unsigned short id, unsigned short value)
{
    p[0] = 0;
    p[1] = 6;                              /* value_len (2) + 4 */
    p[2] = (unsigned char)(id >> 8);
    p[3] = (unsigned char)(id & 0xff);
    p[4] = (unsigned char)(value >> 8);
    p[5] = (unsigned char)(value & 0xff);
    return 6;
}

size_t cp_rg_append_ids(unsigned char *out, size_t cap, const unsigned char *in,
                        size_t inlen, const unsigned short *ids, size_t nids)
{
    if (!out || (nids && !ids) || (inlen && !in) || inlen > cap) return 0;
    size_t room = cap - inlen;
    if (nids > room / 2) return 0;         /* avoids size_t overflow */

    if (inlen) memcpy(out, in, inlen);
    size_t pos = inlen;
    for (size_t i = 0; i < nids; i++) {
        out[pos++] = (unsigned char)(ids[i] >> 8);
        out[pos++] = (unsigned char)(ids[i] & 0xff);
    }
    return pos;
}

size_t cp_rg_build_component(unsigned char *out, size_t cap)
{
    static const unsigned char name[] = "RouteGuidance";   /* 13 chars + NUL */
    /* six U16 params (6 B each) + name param (4 + sizeof name B) = 54 B */
    if (!out || cap < 6u * 6u + 4u + sizeof name) return 0;

    size_t p = 0;
    p += put_u16_param(out + p, 0, 42);
    out[p++] = 0; out[p++] = (unsigned char)(4 + sizeof name);
    out[p++] = 0; out[p++] = 1;            /* param 1: Name */
    memcpy(out + p, name, sizeof name); p += sizeof name;
    p += put_u16_param(out + p, 2, 64);
    p += put_u16_param(out + p, 4, 64);
    p += put_u16_param(out + p, 6, 8);
    p += put_u16_param(out + p, 7, 64);
    p += put_u16_param(out + p, 8, 8);
    return p;
}

size_t cp_rg_build_start(unsigned char *out, size_t cap, int with_component_id)
{
    size_t plen = with_component_id ? 6u : 0u;
    size_t total = 6u + plen;              /* 40 40 + len + msgid + params */
    if (!out || cap < total) return 0;

    out[0] = 0x40; out[1] = 0x40;
    out[2] = (unsigned char)(total >> 8);
    out[3] = (unsigned char)(total & 0xff);
    out[4] = 0x52; out[5] = 0x00;          /* 0x5200 StartRouteGuidanceUpdates */
    if (with_component_id) {
        out[6] = 0; out[7] = 6;            /* one U16 param */
        out[8] = 0; out[9] = 0;            /* id 0 = component Identifier */
        out[10] = 0; out[11] = 42;
    }
    return total;
}
