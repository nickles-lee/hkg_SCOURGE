/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * See cp_rg_send.h. Pure logic + injected vendor calls; no libc beyond string.h.
 */
#include "cp_rg_send.h"
#include <string.h>

/* libstdc++ std::vector<T> as used by the vendor code: three pointers begin/end/capacity (research T20 R3). */
typedef struct { void *b, *e, *c; } vec3;

int cp_rg_msg_id(const unsigned char *d, int n)
{
    if (!d || n < 6) return -1;
    if (d[0] != 0x40 || d[1] != 0x40) return -1;
    int len = (d[2] << 8) | d[3];
    if (len < 6 || len > n) return -1;
    return (d[4] << 8) | d[5];
}

void cp_rg_send_init(cp_rg_send_ctx *c, const cp_rg_vendor *v)
{
    memset(c, 0, sizeof *c);
    c->v = v;
}

long cp_rg_send_start(cp_rg_send_ctx *c, void *self, int session)
{
    const cp_rg_vendor *v = c->v;
    if (!v || !v->encode || !v->pkt_ctor || !v->send || !v->pkt_dtor) return -1;

    vec3 ids = {0, 0, 0}, ptrs = {0, 0, 0}, lens = {0, 0, 0};
    int idv[1];
    const unsigned char *pv[1];
    int lv[1];
    static const unsigned char comp_id[2] = { 0x00, 0x2a };   /* RouteGuidanceDisplayComponent Identifier 42 */
    if (c->with_component_id) {
        idv[0] = 0; pv[0] = comp_id; lv[0] = 2;
        ids.b = idv;  ids.e = idv + 1;  ids.c = idv + 1;
        ptrs.b = pv;  ptrs.e = pv + 1;  ptrs.c = pv + 1;
        lens.b = lv;  lens.e = lv + 1;  lens.c = lv + 1;
    }

    unsigned char *out = 0;
    int n = v->encode(0 /* control */, CP_MSG_START_RGU, &ids, &ptrs, &lens, &out);
    if (n <= 0 || !out) { if (out && v->free_arr) v->free_arr(out); return -1; }

    /* packet object: 16 bytes in the stock caller (local at -0x80, next local at -0x70); give it more room */
    unsigned char pkt[64] __attribute__((aligned(16)));
    memset(pkt, 0, sizeof pkt);
    v->pkt_ctor(pkt, out, n);
    long rc = v->send(self, pkt, session, 0);
    if (v->free_arr) v->free_arr(out);      /* stock frees the encoder buffer after the send (0x2072f) */
    v->pkt_dtor(pkt);
    c->sends++;
    c->last_rc = (int)rc;
    return rc;
}

int cp_rg_after_dispatch(cp_rg_send_ctx *c, void *self, const unsigned char *d, int n, int session)
{
    int id = cp_rg_msg_id(d, n);
    if (id < 0) return 0;
    unsigned s = (unsigned)session & 0xff;
    if (id == CP_MSG_START_IDENT) { c->sent[s] = 0; return 0; }
    if (id != CP_MSG_AUTH_OK) return 0;
    if (!c->ident_patched || c->sent[s]) return 0;
    c->sent[s] = 1;                       /* mark first: a failure must not retry in a loop */
    cp_rg_send_start(c, self, session);
    return 1;
}
