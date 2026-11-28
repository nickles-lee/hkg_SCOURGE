/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CP-A2 host unit test: link the REAL wrapper from src/cp_rg.c (compiled with
 * -DCP_RG_UNIT_TEST, so no constructor / no runtime code patching) against a
 * fake "real P" that records every argument. Compile-time hook:
 * cp_rg.c declares `extern cprg_p_fn cp_rg_test_real_p` in this mode.
 */
#include "../src/cp_rg_tlv.h"
#include <stdio.h>
#include <string.h>

typedef void (*cprg_p_fn)(void *, int, int, void *, void *, void *);
extern void cp_rg_p_wrapper(void *, int, int, void *, void *, void *);

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

struct recorded {
    unsigned char buf[2048];
    int len;
    int id;
    void *ids, *ptrs, *lens;
};
static struct recorded calls[8];
static int ncalls;

static char dummy_ids, dummy_ptrs, dummy_lens;
static void *VID = &dummy_ids, *VPT = &dummy_ptrs, *VLN = &dummy_lens;

/* The compile-time hook target (defined here, used by cp_rg.c). */
cprg_p_fn cp_rg_test_real_p;

static void fake_p(void *buf, int len, int id, void *ids, void *ptrs, void *lens)
{
    if (ncalls < 8) {
        struct recorded *r = &calls[ncalls];
        memset(r, 0, sizeof *r);
        if (len > 0 && len <= (int)sizeof r->buf) memcpy(r->buf, buf, (size_t)len);
        r->len = len; r->id = id; r->ids = ids; r->ptrs = ptrs; r->lens = lens;
    }
    ncalls++;
}

static void reset(void)
{
    memset(calls, 0, sizeof calls);
    ncalls = 0;
    cp_rg_test_real_p = fake_p;
}

static int vecs_ok(int i)
{
    return calls[i].ids == VID && calls[i].ptrs == VPT && calls[i].lens == VLN;
}

int main(void)
{
    unsigned char in[1200];
    unsigned char expected[128];
    unsigned char comp[128];
    size_t cn;

    printf("CP RG wrapper test: ids %04x %04x / %04x %04x %04x\n",
           CP_RG_SENT_IDS[0], CP_RG_SENT_IDS[1],
           CP_RG_RECV_IDS[0], CP_RG_RECV_IDS[1], CP_RG_RECV_IDS[2]);
    CHECK(CP_RG_SENT_IDS[0] == 0x5200 && CP_RG_SENT_IDS[1] == 0x5203);
    CHECK(CP_RG_RECV_IDS[0] == 0x5201 && CP_RG_RECV_IDS[1] == 0x5202 &&
          CP_RG_RECV_IDS[2] == 0x5204);
    cn = cp_rg_build_component(comp, sizeof comp);
    CHECK(cn > 0);

    /* 1. paramId 6: original bytes + two BE16 sent ids, one call. */
    reset();
    memset(in, 0, sizeof in); in[0] = 0xaa; in[1] = 0xbb;
    cp_rg_p_wrapper(in, 2, 6, VID, VPT, VLN);
    CHECK(ncalls == 1);
    CHECK(calls[0].id == 6 && calls[0].len == 6 && vecs_ok(0));
    memcpy(expected, in, 2);
    expected[2] = 0x52; expected[3] = 0x00; expected[4] = 0x52; expected[5] = 0x03;
    CHECK(memcmp(calls[0].buf, expected, 6) == 0);

    /* 2. paramId 7: original + three BE16 received ids, then param 30 component. */
    reset();
    in[0] = 0xcc; in[1] = 0xdd; in[2] = 0xee;
    cp_rg_p_wrapper(in, 3, 7, VID, VPT, VLN);
    CHECK(ncalls == 2);
    CHECK(calls[0].id == 7 && calls[0].len == 9 && vecs_ok(0));
    memcpy(expected, in, 3);
    expected[3] = 0x52; expected[4] = 0x01;
    expected[5] = 0x52; expected[6] = 0x02;
    expected[7] = 0x52; expected[8] = 0x04;
    CHECK(memcmp(calls[0].buf, expected, 9) == 0);
    CHECK(calls[1].id == 30 && calls[1].len == (int)cn && vecs_ok(1));
    CHECK(memcmp(calls[1].buf, comp, cn) == 0);

    /* 3. any other paramId: byte-for-byte pass-through. */
    reset();
    in[0] = 0x11; in[1] = 0x22; in[2] = 0x33; in[3] = 0x44;
    cp_rg_p_wrapper(in, 4, 0x0a, VID, VPT, VLN);
    CHECK(ncalls == 1);
    CHECK(calls[0].id == 0x0a && calls[0].len == 4 && vecs_ok(0));
    CHECK(memcmp(calls[0].buf, in, 4) == 0);

    /* 4. oversized input would overflow our bounded stack buffer: pass through. */
    reset();
    memset(in, 0x5a, sizeof in);
    cp_rg_p_wrapper(in, (int)sizeof in, 7, VID, VPT, VLN);
    CHECK(ncalls == 1);
    CHECK(calls[0].id == 7 && calls[0].len == (int)sizeof in && vecs_ok(0));
    CHECK(memcmp(calls[0].buf, in, sizeof in) == 0);

    /* 5. real P missing: do nothing, never crash. */
    reset();
    cp_rg_test_real_p = NULL;
    cp_rg_p_wrapper(in, 4, 6, VID, VPT, VLN);
    CHECK(ncalls == 0);

    printf("CP RG wrapper: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
