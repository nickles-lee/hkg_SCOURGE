/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host unit tests for the CarPlay route-guidance TLV builders (no car, no VM).
 */
#include "../src/cp_rg_tlv.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define GUARD 0xAA
static void fill_guard(unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) b[i] = GUARD;
}
static void check_guard(const unsigned char *b, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) CHECK(b[i] == GUARD);
}

/* ---- 1. append_ids: exact bytes ----------------------------------------- */
static void test_append_ids(void)
{
    unsigned char out[32];
    static const unsigned char in[4] = { 0x50, 0x00, 0x50, 0x02 };

    fill_guard(out, sizeof out);
    size_t n = cp_rg_append_ids(out, sizeof out, NULL, 0, CP_RG_SENT_IDS, 2);
    CHECK(n == 4);
    CHECK(memcmp(out, "\x52\x00\x52\x03", 4) == 0);
    check_guard(out, 4, sizeof out);

    fill_guard(out, sizeof out);
    n = cp_rg_append_ids(out, sizeof out, in, sizeof in, CP_RG_RECV_IDS, 3);
    CHECK(n == 10);
    CHECK(memcmp(out, "\x50\x00\x50\x02\x52\x01\x52\x02\x52\x04", 10) == 0);
    check_guard(out, 10, sizeof out);

    /* zero ids: pure copy, NULL ids is legal when nids == 0 */
    fill_guard(out, sizeof out);
    n = cp_rg_append_ids(out, sizeof out, in, sizeof in, NULL, 0);
    CHECK(n == 4);
    CHECK(memcmp(out, in, sizeof in) == 0);
    check_guard(out, 4, sizeof out);
}

/* ---- 2. append_ids: capacity and invalid args --------------------------- */
static void test_append_ids_limits(void)
{
    unsigned char out[16];
    static const unsigned char in[4] = { 1, 2, 3, 4 };

    fill_guard(out, sizeof out);
    CHECK(cp_rg_append_ids(out, sizeof out, in, sizeof in, CP_RG_SENT_IDS, 2) == 8);
    CHECK(memcmp(out, "\x01\x02\x03\x04\x52\x00\x52\x03", 8) == 0);
    check_guard(out, 8, sizeof out);

    fill_guard(out, sizeof out);
    CHECK(cp_rg_append_ids(out, 9, in, 4, CP_RG_RECV_IDS, 3) == 0);  /* needs 10 */
    CHECK(cp_rg_append_ids(out, 2, in, 4, CP_RG_SENT_IDS, 2) == 0);  /* inlen > cap */
    CHECK(cp_rg_append_ids(out, sizeof out, in, 4, NULL, 2) == 0);   /* ids NULL */
    CHECK(cp_rg_append_ids(NULL, sizeof out, in, 4, CP_RG_SENT_IDS, 2) == 0);
    CHECK(cp_rg_append_ids(out, sizeof out, NULL, 4, CP_RG_SENT_IDS, 2) == 0);
    check_guard(out, 0, sizeof out);       /* every failure left the buffer untouched */
}

/* ---- 3. component value: exact bytes + independent decode -------------- */
static void test_component(void)
{
    unsigned char out[80];
    static const unsigned char expect[] = {
        0x00,0x06,0x00,0x00,0x00,0x2a,                     /* 0: Identifier 42 */
        0x00,0x12,0x00,0x01,'R','o','u','t','e','G','u','i','d','a','n','c','e',0x00,
        0x00,0x06,0x00,0x02,0x00,0x40,                     /* 2: 64 */
        0x00,0x06,0x00,0x04,0x00,0x40,                     /* 4: 64 */
        0x00,0x06,0x00,0x06,0x00,0x08,                     /* 6: 8 */
        0x00,0x06,0x00,0x07,0x00,0x40,                     /* 7: 64 */
        0x00,0x06,0x00,0x08,0x00,0x08                      /* 8: 8 */
    };

    fill_guard(out, sizeof out);
    size_t n = cp_rg_build_component(out, sizeof out);
    CHECK(n == sizeof expect);
    CHECK(memcmp(out, expect, sizeof expect) == 0);
    check_guard(out, n, sizeof out);

    /* decode our own output: check every nested param id, length and value */
    static const char name[] = "RouteGuidance";
    static const unsigned short want[9] = { 42, 0, 64, 0, 64, 0, 8, 64, 8 };
    int seen[9] = { 0 };
    size_t off = 0;
    while (off + 4 <= n) {
        unsigned tl = (unsigned)((out[off] << 8) | out[off + 1]);
        unsigned id = (unsigned)((out[off + 2] << 8) | out[off + 3]);
        CHECK(tl >= 4 && tl <= n - off);
        if (tl < 4 || tl > n - off) break;
        size_t vn = tl - 4;
        CHECK(id < 9);
        if (id >= 9) break;
        CHECK(seen[id] == 0);              /* no duplicates */
        seen[id] = 1;
        if (id == 1) {
            CHECK(vn == sizeof name);      /* 13 chars + NUL */
            CHECK(vn == 14 && memcmp(out + off + 4, name, sizeof name) == 0);
        } else {
            CHECK(vn == 2);                /* U16 */
            unsigned v = (unsigned)((out[off + 4] << 8) | out[off + 5]);
            CHECK(v == (unsigned)want[id]);
        }
        off += tl;
    }
    CHECK(off == n);
    CHECK(seen[0] && seen[1] && seen[2] && seen[4] && seen[6] && seen[7] && seen[8]);
    CHECK(!seen[3] && !seen[5]);           /* IDs 3/5 absent from the public catalog */

    fill_guard(out, sizeof out);
    CHECK(cp_rg_build_component(out, sizeof expect - 1) == 0);
    CHECK(cp_rg_build_component(NULL, sizeof out) == 0);
    check_guard(out, 0, sizeof out);
}

/* ---- 4. 0x5200 start message, with and without the experiment flag ------ */
static void test_start(void)
{
    unsigned char out[24];
    static const unsigned char plain[] = { 0x40,0x40,0x00,0x06,0x52,0x00 };
    static const unsigned char flagged[] = {
        0x40,0x40,0x00,0x0c,0x52,0x00, 0x00,0x06,0x00,0x00,0x00,0x2a
    };

    fill_guard(out, sizeof out);
    size_t n = cp_rg_build_start(out, sizeof out, 0);
    CHECK(n == sizeof plain);
    CHECK(memcmp(out, plain, sizeof plain) == 0);
    CHECK(((out[2] << 8) | out[3]) == (int)n);   /* len from first 0x40 */
    check_guard(out, n, sizeof out);
    CHECK(cp_rg_build_start(out, 6, 0) == 6);    /* exact cap fits */

    fill_guard(out, sizeof out);
    n = cp_rg_build_start(out, sizeof out, 1);
    CHECK(n == sizeof flagged);
    CHECK(memcmp(out, flagged, sizeof flagged) == 0);
    CHECK(((out[2] << 8) | out[3]) == (int)n);
    check_guard(out, n, sizeof out);
    CHECK(cp_rg_build_start(out, 12, 1) == 12);

    /* decode the single experiment TLV */
    size_t off = 6;
    unsigned tl = (unsigned)((out[off] << 8) | out[off + 1]);
    unsigned id = (unsigned)((out[off + 2] << 8) | out[off + 3]);
    CHECK(tl == 6 && id == 0);
    CHECK(out[off + 4] == 0 && out[off + 5] == 42);

    fill_guard(out, sizeof out);
    CHECK(cp_rg_build_start(out, 5, 0) == 0);
    CHECK(cp_rg_build_start(out, 11, 1) == 0);
    CHECK(cp_rg_build_start(NULL, sizeof out, 0) == 0);
    check_guard(out, 0, sizeof out);
}

int main(void)
{
    test_append_ids();
    test_append_ids_limits();
    test_component();
    test_start();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
