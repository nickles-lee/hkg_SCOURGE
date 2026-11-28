/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Host tests for the CarPlay iAP2 RouteGuidance TLV parser (synthetic messages). */
#include "../src/rgd_tlv.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* append a TLV (id, value) to buf at *pos; returns new pos */
static size_t put_tlv(uint8_t *b, size_t pos, uint16_t id, const uint8_t *v, size_t vn)
{
    uint16_t tl = (uint16_t)(vn + 4);
    b[pos++] = tl >> 8; b[pos++] = tl & 0xff;
    b[pos++] = id >> 8; b[pos++] = id & 0xff;
    memcpy(b + pos, v, vn); pos += vn;
    return pos;
}
static size_t put_int(uint8_t *b, size_t pos, uint16_t id, uint32_t x, int width)
{
    uint8_t v[4];
    for (int i = 0; i < width; i++) v[i] = (x >> (8 * (width - 1 - i))) & 0xff;
    return put_tlv(b, pos, id, v, width);
}
/* finalize session header: 40 40, len BE16, msgid BE16 */
static void hdr(uint8_t *b, size_t total, uint16_t msgid)
{
    b[0] = 0x40; b[1] = 0x40;
    b[2] = total >> 8; b[3] = total & 0xff;
    b[4] = msgid >> 8; b[5] = msgid & 0xff;
}

static void test_update(void)
{
    uint8_t b[256]; size_t p = 6;
    p = put_int(b, p, 0x01, 1, 1);          /* route_state = 1 guiding */
    p = put_int(b, p, 0x07, 5000, 4);       /* dist_dest 5000 m */
    p = put_int(b, p, 0x0A, 150, 4);        /* dist_maneuver 150 m */
    p = put_int(b, p, 0x0E, 3, 1);          /* maneuver_count 3 */
    p = put_int(b, p, 0x14, 1, 1);          /* source_supports_rg = 1 */
    p = put_tlv(b, p, 0x03, (const uint8_t *)"TEST_ROAD_A", 11);
    hdr(b, p, RGD_MSG_UPDATE);

    hn_rgd_state s;
    CHECK(hn_rgd_parse_update(b, p, &s));
    CHECK(s.route_state == 1);
    CHECK(s.dist_dest_m == 5000);
    CHECK(s.dist_maneuver_m == 150);
    CHECK(s.maneuver_count == 3);
    CHECK(s.source_supports_rg == 1);
    CHECK(strcmp(s.current_road, "TEST_ROAD_A") == 0);

    /* malformed: bad sync */
    b[0] = 0x00;
    CHECK(!hn_rgd_parse_update(b, p, &s));
    b[0] = 0x40;
    /* malformed: TLV length overruns frame */
    uint8_t bad[16]; hdr(bad, 12, RGD_MSG_UPDATE);
    bad[6] = 0x00; bad[7] = 0x40; bad[8] = 0x00; bad[9] = 0x01; /* tl=64 > remaining */
    CHECK(!hn_rgd_parse_update(bad, 12, &s));
    /* wrong msgid */
    CHECK(!hn_rgd_parse_maneuver(b, p, &(hn_rgd_maneuver){0}));
}

static void test_maneuver(void)
{
    uint8_t b[256]; size_t p = 6;
    p = put_int(b, p, 0x01, 0, 1);          /* index 0 */
    p = put_int(b, p, 0x03, 2, 1);          /* type 2 = RIGHT_TURN */
    p = put_int(b, p, 0x08, 2, 1);          /* driving_side 2 = right */
    p = put_int(b, p, 0x09, 0, 1);          /* junction_type 0 */
    p = put_int(b, p, 0x0B, 90, 1);         /* exit_angle 90 */
    p = put_tlv(b, p, 0x04, (const uint8_t *)"TEST_ROAD_B", 11);
    hdr(b, p, RGD_MSG_MANEUVER);

    hn_rgd_maneuver m;
    CHECK(hn_rgd_parse_maneuver(b, p, &m));
    CHECK(m.type == 2 && m.driving_side == 2 && m.exit_angle == 90);
    CHECK(strcmp(m.after_road, "TEST_ROAD_B") == 0);
    hn_turn t = hn_rgd_to_turn(&m);
    CHECK(t.src == SRC_CP && t.man == HN_MAN_TURN_RIGHT);

    /* roundabout exit type 30 -> ROUNDABOUT */
    p = 6; p = put_int(b, p, 0x03, 30, 1); p = put_int(b, p, 0x09, 1, 1);
    hdr(b, p, RGD_MSG_MANEUVER);
    CHECK(hn_rgd_parse_maneuver(b, p, &m));
    CHECK(hn_rgd_to_turn(&m).man == HN_MAN_ROUNDABOUT);
}

int main(void)
{
    test_update();
    test_maneuver();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
