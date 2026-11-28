/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Projection+ settings: parser defaults/validation and per-preset distance rounding.
 * "balanced" must stay byte-identical to the car-proven 2026-11-27 can_turn encoding.
 */
#include "../src/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect(int line, int m, hn_steps s, hn_units u, int turn_m, int b0, int val)
{
    hn_distance d = hn_round_distance(m, s, u);
    if (d.turn_m != turn_m || d.list_b0 != b0 || d.list_val != val) {
        fprintf(stderr, "line %d: %d m %s/%s -> turn %d b0 %02x val %d; want turn %d b0 %02x val %d\n",
                line, m, hn_steps_name(s), hn_units_name(u), d.turn_m, d.list_b0, d.list_val,
                turn_m, b0, val);
        fails++;
    }
}
#define E(m, s, u, t, b, v) expect(__LINE__, m, s, u, t, b, v)

/* The exact pre-Projection+ can_turn logic, kept here as the reference for "balanced". */
static void legacy(int dist, int *turn, int *b0, int *val)
{
    if (dist < 0) dist = 0;
    if (dist > 0xffff) dist = 0xffff;
    *turn = dist;
    if (dist >= 1000) {
        int tenths_total = (dist + 50) / 100;
        *b0 = ((tenths_total % 10) << 4) | 0x01;
        *val = tenths_total / 10;
        *turn = tenths_total * 100;
    } else {
        if (dist >= 100) { dist = ((dist + 5) / 10) * 10; if (dist > 990) dist = 990; *turn = dist; }
        *b0 = 0; *val = dist;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    const hn_steps B = HN_STEPS_BALANCED, P = HN_STEPS_PRECISE, C = HN_STEPS_CALM;
    const hn_units CAR = HN_UNITS_CAR, MET = HN_UNITS_METRIC, IMP = HN_UNITS_IMPERIAL;

    /* balanced == legacy for every distance 0..70000, in both car and metric units */
    for (int m = -5; m <= 70000; m++) {
        int t, b, v;
        legacy(m, &t, &b, &v);
        hn_distance d1 = hn_round_distance(m, B, CAR), d2 = hn_round_distance(m, B, MET);
        if (d1.turn_m != t || d1.list_b0 != b || d1.list_val != v ||
            memcmp(&d1, &d2, sizeof d1)) {
            fprintf(stderr, "balanced differs from legacy at %d m\n", m);
            fails++;
            break;
        }
    }

    /* boundary table */
    E(0, P, CAR, 0, 0x00, 0);       E(0, B, CAR, 0, 0x00, 0);       E(0, C, CAR, 0, 0x00, 0);
    E(99, P, CAR, 99, 0x00, 99);    E(99, B, CAR, 99, 0x00, 99);    E(99, C, CAR, 100, 0x00, 100);
    E(100, P, CAR, 100, 0x00, 100); E(100, B, CAR, 100, 0x00, 100); E(100, C, CAR, 100, 0x00, 100);
    E(104, P, CAR, 104, 0x00, 104); E(104, B, CAR, 100, 0x00, 100); E(104, C, CAR, 100, 0x00, 100);
    E(105, P, CAR, 105, 0x00, 105); E(105, B, CAR, 110, 0x00, 110); E(105, C, CAR, 110, 0x00, 110);
    E(347, P, CAR, 347, 0x00, 347); E(347, B, CAR, 350, 0x00, 350); E(347, C, CAR, 350, 0x00, 350);
    E(994, P, CAR, 994, 0x00, 994); E(994, B, CAR, 990, 0x00, 990); E(994, C, CAR, 990, 0x00, 990);
    E(995, P, CAR, 995, 0x00, 995); E(995, B, CAR, 990, 0x00, 990); E(995, C, CAR, 990, 0x00, 990);
    E(999, P, CAR, 999, 0x00, 999); E(999, B, CAR, 990, 0x00, 990); E(999, C, CAR, 990, 0x00, 990);
    E(1000, P, CAR, 1000, 0x01, 1); E(1000, B, CAR, 1000, 0x01, 1); E(1000, C, CAR, 1000, 0x01, 1);
    E(1049, P, CAR, 1000, 0x01, 1); E(1049, B, CAR, 1000, 0x01, 1); E(1049, C, CAR, 1000, 0x01, 1);
    E(1050, P, CAR, 1100, 0x11, 1); E(1050, B, CAR, 1100, 0x11, 1); E(1050, C, CAR, 1100, 0x11, 1);
    E(12360, P, CAR, 12400, 0x41, 12); E(12360, B, CAR, 12400, 0x41, 12);
    E(12360, C, CAR, 12400, 0x41, 12);
    E(-7, C, CAR, 0, 0x00, 0);
    E(5, C, CAR, 10, 0x00, 10);
    E(4, C, CAR, 0, 0x00, 0);

    /* imperial (car-UNVERIFIED): feet (unit 3) below 161 m, metric above */
    E(0, B, IMP, 0, 0x03, 0);
    E(30, P, IMP, 30, 0x03, 100);   /* 98.4 ft -> 100 ft (10 ft steps) */
    E(30, C, IMP, 30, 0x03, 100);   /* 98.4 ft -> 100 ft (50 ft steps) */
    E(100, B, IMP, 101, 0x03, 330); /* 328.1 ft -> 330 ft */
    E(100, C, IMP, 107, 0x03, 350); /* 328.1 ft -> 350 ft */
    E(160, B, IMP, 162, 0x03, 530); /* 524.9 ft -> 530 ft */
    E(161, B, IMP, 160, 0x00, 160); /* from 0.1 mi: metric until a mile code is known */
    E(2000, B, IMP, 2000, 0x01, 2);

    /* parser */
    const char *path = "/tmp/hudnav-test-settings.conf";
    hn_settings s;
    remove(path);
    if (hn_settings_load(&s, path) != -1 || s.aa_steps != B || s.cp_units != CAR) {
        fprintf(stderr, "missing file: want -1 and defaults\n"); fails++;
    }
    write_file(path, "# Projection+\n aa_steps = calm \ncp_steps=precise\r\naa_units=imperial # c\n"
                     "cp_units=metric\nbogus=1\nnoequals\n");
    if (hn_settings_load(&s, path) != 0 || s.aa_steps != C || s.cp_steps != P ||
        s.aa_units != IMP || s.cp_units != MET) {
        fprintf(stderr, "valid file not parsed\n"); fails++;
    }
    write_file(path, "aa_steps=CALM\ncp_steps=\naa_units=miles\ncp_units=imperial extra\n");
    if (hn_settings_load(&s, path) != 0 || s.aa_steps != B || s.cp_steps != B ||
        s.aa_units != CAR || s.cp_units != CAR) {
        fprintf(stderr, "invalid values must fall back to defaults\n"); fails++;
    }
    write_file(path, "");
    if (hn_settings_load(&s, path) != 0 || s.aa_steps != B || s.cp_units != CAR) {
        fprintf(stderr, "empty file: defaults\n"); fails++;
    }
    remove(path);

    if (fails) { fprintf(stderr, "test_settings: %d failure(s)\n", fails); return 1; }
    printf("settings parser and distance presets passed\n");
    return 0;
}
