/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host test of cp_rg_send.c with fake vendor functions that record every call.
 */
#include "../src/cp_rg_send.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { void *b, *e, *c; } vec3;
static char calls[256];
static int enc_svc, enc_msg, enc_nids, enc_param0, enc_len0, enc_ptr0_b0, enc_ptr0_b1;
static int ctor_len, send_session, send_flag, free_called, dtor_called, send_ret = 7, enc_ret_len = 6, enc_null;
static void *send_self, *ctor_pkt, *send_pkt, *dtor_pkt;
static unsigned char outbuf[16];

static int f_encode(int svc, int msgid, void *ids, void *ptrs, void *lens, unsigned char **out)
{
    strcat(calls, "E");
    enc_svc = svc; enc_msg = msgid;
    vec3 *i = ids, *p = ptrs, *l = lens;
    enc_nids = (int)(((char *)i->e - (char *)i->b) / (int)sizeof(int));
    if (enc_nids == 1) {
        enc_param0 = ((int *)i->b)[0];
        enc_len0 = ((int *)l->b)[0];
        const unsigned char *d = ((const unsigned char **)p->b)[0];
        enc_ptr0_b0 = d[0]; enc_ptr0_b1 = d[1];
    }
    if (enc_null) { *out = 0; return 0; }
    outbuf[0] = 0x40; outbuf[1] = 0x40; outbuf[2] = 0; outbuf[3] = 6; outbuf[4] = 0x52; outbuf[5] = 0x00;
    *out = outbuf;
    return enc_ret_len;
}
static void f_ctor(void *pkt, const unsigned char *d, int n) { strcat(calls, "C"); ctor_pkt = pkt; ctor_len = n; (void)d; }
static long f_send(void *self, void *pkt, int session, int flag) { strcat(calls, "S"); send_self = self; send_pkt = pkt; send_session = session; send_flag = flag; return send_ret; }
static void f_dtor(void *pkt) { strcat(calls, "D"); dtor_pkt = pkt; dtor_called++; }
static void f_free(void *p) { strcat(calls, "F"); free_called++; (void)p; }

static const cp_rg_vendor V = { f_encode, f_ctor, f_send, f_dtor, f_free };

static void reset(void) { calls[0] = 0; free_called = dtor_called = 0; enc_nids = -1; enc_null = 0; enc_ret_len = 6; send_ret = 7; }

static int mk(unsigned char *b, int id, int extra)
{
    int len = 6 + extra;
    b[0] = 0x40; b[1] = 0x40; b[2] = (unsigned char)(len >> 8); b[3] = (unsigned char)len; b[4] = (unsigned char)(id >> 8); b[5] = (unsigned char)id;
    for (int i = 0; i < extra; i++) b[6 + i] = 0xee;
    return len;
}

int main(void)
{
    unsigned char m[64];
    int n;

    /* message id parsing */
    n = mk(m, 0xaa05, 0);
    CHECK(cp_rg_msg_id(m, n) == 0xaa05);
    n = mk(m, 0x1d00, 4);
    CHECK(cp_rg_msg_id(m, n) == 0x1d00);
    CHECK(cp_rg_msg_id(m, 5) == -1);                 /* too short */
    CHECK(cp_rg_msg_id(0, 10) == -1);
    m[0] = 0x41; CHECK(cp_rg_msg_id(m, n) == -1);    /* bad header */
    m[0] = 0x40; m[3] = 99; CHECK(cp_rg_msg_id(m, n) == -1);   /* length beyond the buffer */
    m[3] = 3; CHECK(cp_rg_msg_id(m, n) == -1);       /* length below the header size */

    /* direct send: the stock call sequence and arguments */
    cp_rg_send_ctx c; cp_rg_send_init(&c, &V); reset();
    int self_obj; long rc = cp_rg_send_start(&c, &self_obj, 5);
    CHECK(rc == 7);
    CHECK(strcmp(calls, "ECSFD") == 0);              /* encode, pkt ctor, send, free out, pkt dtor */
    CHECK(enc_svc == 0 && enc_msg == 0x5200 && enc_nids == 0);
    CHECK(ctor_len == 6 && ctor_pkt == send_pkt && dtor_pkt == send_pkt);
    CHECK(send_self == &self_obj && send_session == 5 && send_flag == 0);
    CHECK(free_called == 1 && dtor_called == 1 && c.sends == 1);

    /* component-id experiment: one parameter id 0 = 00 2a, length 2 */
    cp_rg_send_init(&c, &V); c.with_component_id = 1; reset();
    cp_rg_send_start(&c, &self_obj, 1);
    CHECK(enc_nids == 1 && enc_param0 == 0 && enc_len0 == 2 && enc_ptr0_b0 == 0x00 && enc_ptr0_b1 == 0x2a);

    /* encoder produced nothing: no packet, no send */
    cp_rg_send_init(&c, &V); reset(); enc_null = 1;
    rc = cp_rg_send_start(&c, &self_obj, 1);
    CHECK(rc == -1 && strcmp(calls, "E") == 0 && c.sends == 0);

    /* dispatch hook: only AuthenticationSucceeded, only when the Identification was patched, once per session */
    cp_rg_send_init(&c, &V); reset();
    n = mk(m, 0xaa05, 0);
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 0 && calls[0] == 0);   /* not patched: nothing */
    c.ident_patched = 1;
    n = mk(m, 0xaa00, 0);
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 0 && calls[0] == 0);   /* other message: nothing */
    n = mk(m, 0xaa05, 0);
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 1);
    CHECK(strcmp(calls, "ECSFD") == 0 && send_session == 1);
    calls[0] = 0;
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 0 && calls[0] == 0);   /* once per session */
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 2) == 1);                    /* another session sends */
    n = mk(m, 0x1d00, 0);
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 0);                    /* new identification round */
    calls[0] = 0;
    n = mk(m, 0xaa05, 0);
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 1) == 1 && strcmp(calls, "ECSFD") == 0);   /* sends again after reset */
    CHECK(cp_rg_after_dispatch(&c, &self_obj, 0, 0, 1) == 0);                    /* garbage never sends */

    /* send failure must not retry in a loop */
    cp_rg_send_init(&c, &V); c.ident_patched = 1; reset(); enc_null = 1;
    n = mk(m, 0xaa05, 0);
    cp_rg_after_dispatch(&c, &self_obj, m, n, 3);
    calls[0] = 0;
    CHECK(cp_rg_after_dispatch(&c, &self_obj, m, n, 3) == 0 && calls[0] == 0);

    printf("CP send: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
