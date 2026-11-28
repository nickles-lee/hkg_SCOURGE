/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host unit tests for the CarPlay iAP2 interceptor model (no car, no VM, no USB).
 * Covers: synthetic 0x5201/0x5202 built from the documented TLV map, Identify
 * patch byte diff, message-id registration, emitted line protocol vs the
 * convert.c/motion.c pipeline, and fail-open behaviour.
 */
#include "../src/cp_shim.h"
#include "../src/rgd_tlv.h"
#include "../src/hudnav.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- synthetic iAP2 session-message builders (same map as research docs) ---- */

static size_t put_tlv(uint8_t *b, size_t pos, uint16_t id, const uint8_t *v, size_t vn)
{
    uint16_t tl = (uint16_t)(vn + 4);
    b[pos++] = (uint8_t)(tl >> 8); b[pos++] = (uint8_t)(tl & 0xff);
    b[pos++] = (uint8_t)(id >> 8); b[pos++] = (uint8_t)(id & 0xff);
    memcpy(b + pos, v, vn); pos += vn;
    return pos;
}
static size_t put_int(uint8_t *b, size_t pos, uint16_t id, uint32_t x, int width)
{
    uint8_t v[4];
    for (int i = 0; i < width; i++) v[i] = (uint8_t)((x >> (8 * (width - 1 - i))) & 0xff);
    return put_tlv(b, pos, id, v, (size_t)width);
}
static void hdr(uint8_t *b, size_t total, uint16_t msgid)
{
    b[0] = 0x40; b[1] = 0x40;
    b[2] = (uint8_t)(total >> 8); b[3] = (uint8_t)(total & 0xff);
    b[4] = (uint8_t)(msgid >> 8); b[5] = (uint8_t)(msgid & 0xff);
}

/* ---- line-emitter sink ---- */
typedef struct { char buf[1024]; size_t n; int lines; } sink;
static void collect(void *ctx, const char *line)
{
    sink *s = (sink *)ctx;
    if (!s || !line) return;
    size_t l = strlen(line);
    if (s->n + l + 1 < sizeof s->buf) {
        memcpy(s->buf + s->n, line, l);
        s->n += l; s->buf[s->n] = 0;
    }
    s->lines++;
}
static const char *find_line(const sink *s, const char *prefix)
{
    static char line[128];
    if (!s) return NULL;
    const char *p = s->buf;
    size_t plen = strlen(prefix);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= plen && !strncmp(p, prefix, plen)) {
            size_t n = l < sizeof line - 1 ? l : sizeof line - 1;
            memcpy(line, p, n); line[n] = 0;
            return line;
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

/* ---- 1. TLV parse from the documented map -------------------------------- */
static void test_tlv_from_map(void)
{
    /* 0x5201 map: 0x01 state, 0x03 road, 0x04 dest, 0x07 dist_dest,
     * 0x0A exact dist to next maneuver, 0x0E count, 0x0F visible, 0x14 gate. */
    uint8_t u[256]; size_t p = 6;
    p = put_int(u, p, 0x01, 1, 1);
    p = put_int(u, p, 0x02, 2, 1);                       /* maneuver_state */
    p = put_tlv(u, p, 0x03, (const uint8_t *)"TEST_ROAD_A", 11);
    p = put_tlv(u, p, 0x04, (const uint8_t *)"Home", 4);
    p = put_int(u, p, 0x05, 600, 2);                     /* ETA seconds */
    p = put_int(u, p, 0x06, 580, 2);                     /* time remaining */
    p = put_int(u, p, 0x07, 5000, 4);
    p = put_int(u, p, 0x0A, 150, 4);
    p = put_int(u, p, 0x0E, 3, 1);
    p = put_int(u, p, 0x0F, 1, 1);
    p = put_int(u, p, 0x13, 0, 1);                       /* unknown-to-parser source name */
    p = put_int(u, p, 0x14, 1, 1);
    hdr(u, p, RGD_MSG_UPDATE);

    hn_rgd_state s;
    CHECK(hn_rgd_parse_update(u, p, &s));
    CHECK(s.route_state == 1 && s.maneuver_state == 2);
    CHECK(s.dist_dest_m == 5000 && s.dist_maneuver_m == 150);
    CHECK(s.maneuver_count == 3 && s.visible_in_app == 1);
    CHECK(s.source_supports_rg == 1);
    CHECK(strcmp(s.current_road, "TEST_ROAD_A") == 0);
    CHECK(strcmp(s.destination, "Home") == 0);
    CHECK((s.present & (1u << 0x14)) != 0);
    CHECK((s.present & (1u << 0x13)) != 0);              /* forward-compatible skip */

    /* 0x5202 map: 0x01 index, 0x02 description, 0x03 type, 0x04 after road,
     * 0x05 distance between, 0x08 driving side, 0x09 junction type,
     * 0x0A junction element angle, 0x0B signed exit angle, 0x0D exit info. */
    uint8_t m[256]; size_t q = 6;
    q = put_int(m, q, 0x01, 0, 1);
    q = put_tlv(m, q, 0x02, (const uint8_t *)"Turn right", 10);
    q = put_int(m, q, 0x03, 2, 1);                       /* RIGHT_TURN */
    q = put_tlv(m, q, 0x04, (const uint8_t *)"TEST_ROAD_B", 11);
    q = put_int(m, q, 0x05, 200, 2);
    q = put_int(m, q, 0x08, 2, 1);                       /* driving side R */
    q = put_int(m, q, 0x09, 0, 1);                       /* intersection */
    q = put_int(m, q, 0x0A, 45, 1);
    q = put_tlv(m, q, 0x0B, (const uint8_t *)"\xa6", 1); /* signed -90 degrees */
    q = put_tlv(m, q, 0x0D, (const uint8_t *)"E1", 2);
    hdr(m, q, RGD_MSG_MANEUVER);

    hn_rgd_maneuver mv;
    CHECK(hn_rgd_parse_maneuver(m, q, &mv));
    CHECK(mv.index == 0 && mv.type == 2 && mv.driving_side == 2);
    CHECK(mv.junction_type == 0 && mv.junction_angle == 45);
    CHECK(mv.exit_angle == -90);                        /* signed decode */
    CHECK(mv.dist_between_m == 200);
    CHECK(strcmp(mv.after_road, "TEST_ROAD_B") == 0);
    CHECK(hn_rgd_to_turn(&mv).src == SRC_CP);
    CHECK(hn_rgd_to_turn(&mv).man == HN_MAN_TURN_RIGHT);
    CHECK(hn_rgd_to_turn(&mv).raw_type == 2 && hn_rgd_to_turn(&mv).raw_side == 2);
}

/* ---- 2. Identify patch byte diff ----------------------------------------- */
static void test_identify_patch(void)
{
    uint8_t b[64]; size_t p = 6;
    p = put_tlv(b, p, 0x0001, (const uint8_t *)"acc", 3);
    p = put_int(b, p, 0x0005, 2, 1);
    hdr(b, p, CP_MSG_IDENTIFY);
    size_t orig = p;

    size_t n = cp_patch_identify(b, sizeof b, orig);
    CHECK(n == orig + 5);
    CHECK(b[2] == 0x00 && b[3] == (uint8_t)(orig + 5));    /* header length updated */
    CHECK(b[orig] == 0x00 && b[orig + 1] == 0x05);
    CHECK(b[orig + 2] == 0x00 && b[orig + 3] == 0x1e);    /* param 30 = 0x001E */
    CHECK(b[orig + 4] == 0x01);                           /* PROVISIONAL "supported" */
    CHECK(b[6] == 0x00 && b[7] == 0x07);                  /* original TLVs untouched */

    /* idempotence: second call must not append again */
    CHECK(cp_patch_identify(b, sizeof b, n) == n);

    /* already carrying 0x001E -> unchanged */
    uint8_t c[64]; size_t q = 6;
    q = put_int(c, q, CP_PARAM_RG_COMP, 1, 1);
    hdr(c, q, CP_MSG_IDENTIFY);
    CHECK(cp_patch_identify(c, sizeof c, q) == q);

    /* fail-open: bad sync, insufficient capacity, wrong message id */
    uint8_t d[64]; memcpy(d, b, orig); d[0] = 0x00; hdr(d, orig, CP_MSG_IDENTIFY); d[0] = 0x00;
    CHECK(cp_patch_identify(d, sizeof d, orig) == orig);
    uint8_t e[64]; size_t r = 6; r = put_int(e, r, 0x0001, 0, 1); hdr(e, r, CP_MSG_IDENTIFY);
    CHECK(cp_patch_identify(e, r, r) == r);                 /* cap == len: no room */
    uint8_t f[64]; size_t t = 6; t = put_int(f, t, 0x0001, 0, 1); hdr(f, t, 0x5001);
    CHECK(cp_patch_identify(f, sizeof f, t) == t);
}

/* ---- 3. message-id registration model ------------------------------------ */
static void test_registration(void)
{
    cp_msgid_reg r;
    CHECK(cp_reg_rgd_defaults(&r) == 5);
    CHECK(cp_reg_has(&r, CP_MSG_START_RGD, true));
    CHECK(!cp_reg_has(&r, CP_MSG_START_RGD, false));
    CHECK(cp_reg_has(&r, CP_MSG_STOP_RGD, true));
    CHECK(cp_reg_has(&r, CP_MSG_UPDATE, false));
    CHECK(!cp_reg_has(&r, CP_MSG_UPDATE, true));
    CHECK(cp_reg_has(&r, CP_MSG_MANEUVER, false));
    CHECK(cp_reg_has(&r, CP_MSG_LANES, false));
    CHECK(!cp_reg_has(&r, 0x1234, true));

    cp_reg_init(&r);
    for (uint16_t i = 0; i < 12; i++) CHECK(cp_reg_set(&r, (uint16_t)(0x1000 + i), true, false));
    CHECK(!cp_reg_set(&r, 0x2000, true, false));          /* full: fail-open */

    CHECK(cp_shim_register_ids() == 5);
    CHECK(cp_shim_id_registered(CP_MSG_UPDATE, false));
    CHECK(cp_shim_id_registered(CP_MSG_START_RGD, true));
}

/* ---- 4. normalized record output -> convert.c / motion.c ----------------- */
static void test_normalized_output(void)
{
    uint8_t u[128]; size_t p = 6;
    p = put_int(u, p, 0x01, 1, 1);
    p = put_int(u, p, 0x0A, 150, 4);
    p = put_int(u, p, 0x14, 1, 1);
    hdr(u, p, RGD_MSG_UPDATE);
    sink su = {0};
    CHECK(cp_handle_message(u, p, collect, &su) == 3);
    char expect[512];
    snprintf(expect, sizeof expect,
             "STATE s=1\nDIST m=150 disp=150 unit=0\nHB\n");
    CHECK(strcmp(su.buf, expect) == 0);

    uint8_t m[128]; size_t q = 6;
    q = put_int(m, q, 0x01, 0, 1);
    q = put_int(m, q, 0x03, 2, 1);                        /* RIGHT_TURN */
    q = put_int(m, q, 0x08, 2, 1);                        /* side R */
    q = put_int(m, q, 0x0B, 90, 1);                       /* exit angle 90 */
    hdr(m, q, RGD_MSG_MANEUVER);
    sink sm = {0};
    CHECK(cp_handle_message(m, q, collect, &sm) == 2);
    snprintf(expect, sizeof expect, "TURN src=cp man=%d exit=90 raw=2 side=2\nHB\n",
             (int)HN_MAN_TURN_RIGHT);
    CHECK(strcmp(sm.buf, expect) == 0);

    /* Feed the emitted lines through the same key=value fields the daemon uses,
     * then verify the converter/motion output hn_turn produces. */
    int meters = -1, st = -1, man = -1, exit_a = -999, raw = -1, side = -1;
    CHECK(sscanf(find_line(&su, "DIST "), "DIST m=%d disp=%*d unit=%*d", &meters) == 1);
    CHECK(sscanf(find_line(&su, "STATE "), "STATE s=%d", &st) == 1);
    CHECK(sscanf(find_line(&sm, "TURN "),
                 "TURN src=cp man=%d exit=%d raw=%d side=%d",
                 &man, &exit_a, &raw, &side) == 4);
    CHECK(meters == 150 && st == 1);
    CHECK(man == (int)HN_MAN_TURN_RIGHT && exit_a == 90 && raw == 2 && side == 2);

    hn_config cfg; hn_config_default(&cfg);               /* LHT (AU) */
    hn_motion mo; hn_motion_init(&mo);
    hn_motion_on_owner(&mo, SRC_CP, 0);
    hn_motion_on_state(&mo, (hn_navstate)st, 0);
    hn_turn t; memset(&t, 0, sizeof t);
    t.src = SRC_CP; t.man = (hn_maneuver)man; t.exit_angle = exit_a;
    t.raw_type = raw; t.raw_side = side;
    hn_motion_on_turn(&mo, &t, 0);
    hn_motion_on_dist(&mo, &(hn_dist){meters, meters, 0}, 0);
    hn_native nat; bool clear = false;
    CHECK(hn_motion_tick(&mo, 0, &cfg, &nat, &clear));
    CHECK(nat.type == HN_TYPE_TURN && nat.dist_m == 150);
    /* Physical angle, NOT traffic-side mirrored: a single rotated-arrow icon (0x0d)
     * must point the same way under LHT and RHT. See convert.c's comment +
     * docs/hudnav/HUD_NAV_REPORT.md §9 (mirroring is confirmed only for the
     * roundabout exit angle and U-turn, not the plain turn arrow). */
    CHECK(nat.angle == 90 && nat.side_street == 0 && nat.near);
    CHECK(nat.bar > 0 && nat.bar <= 90);

    /* TLV 0x07 -> DEST line (destination distance for the 0x4E8 cluster frame) */
    uint8_t du[128]; size_t dp = 6;
    dp = put_int(du, dp, 0x07, 1600, 4);
    dp = put_int(du, dp, 0x0A, 150, 4);
    dp = put_int(du, dp, 0x14, 1, 1);
    hdr(du, dp, RGD_MSG_UPDATE);
    sink sd = {0};
    CHECK(cp_handle_message(du, dp, collect, &sd) == 4);
    CHECK(find_line(&sd, "DEST m=1600") != NULL);
    CHECK(find_line(&sd, "DIST m=150 ") != NULL);
}

/* ---- 5. fail-open -------------------------------------------------------- */
static void test_fail_open(void)
{
    uint8_t u[128]; size_t p = 6;
    p = put_int(u, p, 0x01, 1, 1);
    p = put_int(u, p, 0x0A, 150, 4);
    p = put_int(u, p, 0x14, 1, 1);
    hdr(u, p, RGD_MSG_UPDATE);
    sink s = {0};

    /* malformed framing / msgid / TLV length */
    uint8_t b[128]; memcpy(b, u, p);
    b[0] = 0x00; CHECK(cp_handle_message(b, p, collect, &s) == 0); b[0] = 0x40;
    CHECK(cp_handle_message(u, 5, collect, &s) == 0);
    hdr(b, p, 0x1234); CHECK(cp_handle_message(b, p, collect, &s) == 0);
    memcpy(b, u, p); b[6] = 0x00; b[7] = 0x40;            /* TLV len overruns */
    CHECK(cp_handle_message(b, p, collect, &s) == 0);
    CHECK(s.lines == 0);
    CHECK(cp_handle_message(u, p, NULL, &s) == 0);

    /* app gate: 0 or absent -> forward nothing (§6) */
    uint8_t g[128]; size_t q = 6;
    q = put_int(g, q, 0x0A, 150, 4); q = put_int(g, q, 0x14, 0, 1);
    hdr(g, q, RGD_MSG_UPDATE);
    CHECK(cp_handle_message(g, q, collect, &s) == 0);
    q = 6; q = put_int(g, q, 0x0A, 150, 4); hdr(g, q, RGD_MSG_UPDATE);
    CHECK(cp_handle_message(g, q, collect, &s) == 0);

    /* maneuver type out of documented 0..53 range (255 NOT_SET, 54) */
    uint8_t m[128]; size_t r = 6;
    r = put_int(m, r, 0x03, 255, 1); hdr(m, r, RGD_MSG_MANEUVER);
    CHECK(cp_handle_message(m, r, collect, &s) == 0);
    r = 6; r = put_int(m, r, 0x03, 54, 1); hdr(m, r, RGD_MSG_MANEUVER);
    CHECK(cp_handle_message(m, r, collect, &s) == 0);
    CHECK(s.lines == 0);

    /* 0x5200 / 0x5203 / 0x5204 / unknown -> no normalized record */
    uint8_t z[16]; size_t zlen = 6;
    hdr(z, zlen, CP_MSG_START_RGD); CHECK(cp_handle_message(z, zlen, collect, &s) == 0);
    hdr(z, zlen, CP_MSG_STOP_RGD);  CHECK(cp_handle_message(z, zlen, collect, &s) == 0);
    hdr(z, zlen, CP_MSG_LANES);     CHECK(cp_handle_message(z, zlen, collect, &s) == 0);

    /* 0x5200 builder and raw-link frame scan */
    uint8_t st[8];
    CHECK(cp_build_start(st, 5) == 0);
    CHECK(cp_build_start(st, sizeof st) == 6);
    CHECK(st[0] == 0x40 && st[1] == 0x40 && st[2] == 0x00 && st[3] == 0x06);
    CHECK(st[4] == 0x52 && st[5] == 0x00);
    size_t ml = 0;
    CHECK(cp_find_session(u, p, &ml) == 0 && ml == p);
    /* wrap a session message in a synthetic FF5A + 7 link header (§3) */
    uint8_t link[256];
    link[0] = 0xff; link[1] = 0x5a;
    for (size_t i = 2; i < 9; i++) link[i] = 0x00;
    memcpy(link + 9, u, p);
    CHECK(cp_find_session(link, 9 + p, &ml) == 9 && ml == p);
    CHECK(cp_find_session(link, 5, &ml) == -1);
    uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(cp_find_session(junk, sizeof junk, &ml) == -1);
}

/* ---- 2b. Identify registration TLVs (cp_iap2_impl_spec.md §3.3) ---------- */
static int find_tlv(const uint8_t *b, size_t len, uint16_t id, size_t *off)
{
    size_t o = 6;
    while (o + 4 <= len) {
        uint16_t tl = (uint16_t)((b[o] << 8) | b[o + 1]);
        if (tl < 4 || tl > len - o) return 0;
        if (((b[o + 2] << 8) | b[o + 3]) == id) { *off = o; return 1; }
        o += tl;
    }
    return 0;
}

static void test_identify_registration(void)
{
    uint8_t b[256]; size_t p = 6;
    const uint8_t sent[4] = { 0x50, 0x00, 0x50, 0x02 };
    const uint8_t recv[2] = { 0x50, 0x01 };
    p = put_tlv(b, p, 6, sent, sizeof sent);
    p = put_tlv(b, p, 7, recv, sizeof recv);
    p = put_int(b, p, 0x0002, 8, 1);
    hdr(b, p, CP_MSG_IDENTIFY);
    size_t orig = p;

    /* +4 bytes to TLV 6, +6 to TLV 7, +5 for param 30 */
    size_t n = cp_patch_identify(b, sizeof b, orig);
    CHECK(n == orig + 15);
    CHECK(((b[2] << 8) | b[3]) == (int)n);

    size_t o6 = 0, o7 = 0, o30 = 0;
    CHECK(find_tlv(b, n, 6, &o6));
    CHECK(find_tlv(b, n, 7, &o7));
    CHECK(find_tlv(b, n, CP_PARAM_RG_COMP, &o30));
    CHECK(((b[o6] << 8) | b[o6 + 1]) == 12);          /* 8 + 4 */
    CHECK(((b[o7] << 8) | b[o7 + 1]) == 12);          /* 6 + 6 */
    /* appended ids are 2-byte big-endian, after the stock entries */
    CHECK(memcmp(b + o6 + 4, "\x50\x00\x50\x02\x52\x00\x52\x03", 8) == 0);
    CHECK(memcmp(b + o7 + 4, "\x50\x01\x52\x01\x52\x02\x52\x04", 8) == 0);
    CHECK(b[o30] == 0x00 && b[o30 + 1] == 0x05);
    CHECK(b[o30 + 2] == 0x00 && b[o30 + 3] == 0x1e && b[o30 + 4] == 0x01);

    /* idempotent: all five ids and param 30 are present now */
    CHECK(cp_patch_identify(b, sizeof b, n) == n);

    /* real link-body form: [0x02][session][40 40 …] */
    uint8_t lb[256]; lb[0] = 0x02; lb[1] = 0x33;
    size_t q = 2 + 6;
    q = put_tlv(lb, q, 6, sent, sizeof sent);
    q = put_tlv(lb, q, 7, recv, sizeof recv);
    hdr(lb + 2, q - 2, CP_MSG_IDENTIFY);
    size_t ln = cp_patch_identify(lb, sizeof lb, q);
    CHECK(ln == q + 15);
    CHECK(lb[0] == 0x02 && lb[1] == 0x33);
    CHECK(lb[2] == 0x40 && lb[3] == 0x40);
    CHECK(((lb[4] << 8) | lb[5]) == (int)((q - 2) + 15));
    CHECK(((lb[6] << 8) | lb[7]) == CP_MSG_IDENTIFY);

    /* insufficient capacity with registration TLVs -> unchanged */
    uint8_t c[256]; memcpy(c, b, orig);
    CHECK(cp_patch_identify(c, orig + 14, orig) == orig);

    /* 0x5200 link body */
    uint8_t st[8];
    CHECK(cp_build_start_link(st, 7, 0x33) == 0);
    CHECK(cp_build_start_link(st, sizeof st, 0x33) == 8);
    CHECK(memcmp(st, "\x02\x33\x40\x40\x00\x06\x52\x00", 8) == 0);
}

int main(void)
{
    test_tlv_from_map();
    test_identify_patch();
    test_identify_registration();
    test_registration();
    test_normalized_output();
    test_fail_open();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
