/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Host unit tests for the converter + motion state machine (no car, no D-Bus). */
#include "../src/hudnav.h"
#include <stdio.h>
#include <string.h>

/* T19: include the REAL daemon line parser so turn_number parsing is unit-tested.
 * HUDNAV_NO_MAIN drops main(), so test_core keeps its own main and needs no
 * output backend. No script/build changes required. */
/* the included file has helpers this test does not call (now_ms, read_input). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define HUDNAV_NO_MAIN
#include "../src/daemon.c"
#undef HUDNAV_NO_MAIN
#pragma GCC diagnostic pop

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_convert(void)
{
    hn_config c; hn_config_default(&c);   /* LHT (AU) */
    hn_native n;

    /* far: icons, side_street 65535, no native */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 1000, &c);
    CHECK(n.type == HN_TYPE_ICON && n.icon == 67 && n.side_street == 65535 && !n.near);

    /* near right turn: angle is physical (90), NOT traffic-side mirrored. A single
     * rotated-arrow icon (0x0d) must point the same way regardless of LHT/RHT; see
     * convert.c's comment + docs/hudnav/HUD_NAV_REPORT.md §9. Same under LHT and RHT. */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 100, &c);
    CHECK(n.type == HN_TYPE_TURN && n.angle == 90 && n.side_street == 0 && n.near);

    c.lht = false;
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 100, &c);
    CHECK(n.type == HN_TYPE_TURN && n.angle == 90);
    c.lht = true;

    /* U-turn IS traffic-side mirrored (confirmed: LHT sends 90, RHT sends 270). */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_UTURN_RIGHT, -1, 0, 0, -1}, 100, &c);
    CHECK(n.type == HN_TYPE_UTURN && n.angle == 90);
    c.lht = false;
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_UTURN_RIGHT, -1, 0, 0, -1}, 100, &c);
    CHECK(n.type == HN_TYPE_UTURN && n.angle == 270);
    c.lht = true;

    /* exit family uses 457 window: native at 400 m */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_EXIT_RIGHT, -1, 8, 2, -1}, 400, &c);
    CHECK(n.type == HN_TYPE_EXIT_R && n.near);
    CHECK(n.f2_icon == 0xA4);   /* HUD sweep 2026-11-28: 45-deg right exit */
    n = hn_convert(&(hn_turn){SRC_CP, HN_MAN_EXIT_LEFT, -1, -1, -1, -1}, 400, &c);
    CHECK(n.f2_icon == 0xA5);
    n = hn_convert(&(hn_turn){SRC_CP, HN_MAN_EXIT_GENERIC, -1, -1, -1, -1}, 400, &c);
    CHECK(n.f2_icon == 0xA5);   /* sideless OFF_RAMP, LHT -> left exit */
    /* street turn beyond 200 m -> icon */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 300, &c);
    CHECK(n.type == HN_TYPE_ICON && !n.near);

    /* roundabout LHT type 33, icon family +96, exit angle mirrored (confirmed: 90 -> 270) */
    /* AA right exit = AA angle 270 (clockwise from entry, car 2026-11-26) -> converter 90 */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 270, 13, 0, -1}, 100, &c);
    CHECK(n.type == HN_TYPE_ROUND_L && n.angle == 270);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 270, 13, 0, -1}, 1000, &c);
    CHECK(n.type == HN_TYPE_ICON && n.icon == 195);  /* 99 + 96 */

    /* never icon 64; unknown -> 65 */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_UNKNOWN, -1, 99, 0, -1}, 1000, &c);
    CHECK(n.icon == 65);

    /* CAN-TP F2 icon codes (the HUD draws its arrow from these; HUD sweep on the car 2026-11-25):
     * the same code near and far, no mirroring for the plain turns */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 100, &c);   /* near */
    CHECK(n.f2_icon == 0x43);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 1000, &c);  /* far */
    CHECK(n.f2_icon == 0x43);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_TURN_LEFT, -1, 4, 1, -1}, 100, &c);
    CHECK(n.f2_icon == 0x46);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_STRAIGHT, -1, 2, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x41);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_SLIGHT_RIGHT, -1, 3, 2, -1}, 100, &c);
    CHECK(n.f2_icon == 0x42);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_SHARP_RIGHT, -1, 5, 2, -1}, 100, &c);
    CHECK(n.f2_icon == 0x44);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_SHARP_LEFT, -1, 5, 1, -1}, 100, &c);
    CHECK(n.f2_icon == 0x45);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_SLIGHT_LEFT, -1, 3, 1, -1}, 100, &c);
    CHECK(n.f2_icon == 0x47);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_UTURN_LEFT, -1, 6, 1, -1}, 100, &c);
    CHECK(n.f2_icon == 0x48);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_UTURN_RIGHT, -1, 6, 2, -1}, 100, &c);
    CHECK(n.f2_icon == 0x49);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_FORK_RIGHT, -1, 9, 2, -1}, 100, &c);
    CHECK(n.f2_icon == 0x93);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_FORK_LEFT, -1, 9, 1, -1}, 100, &c);
    CHECK(n.f2_icon == 0x95);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_DESTINATION, -1, 19, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x70);
    /* LHT roundabout exits (stock 0xC3 = right; sweep: C0 straight, C1 slight R, C4 sharp R, C6 180,
     * C8 sharp L, C9 left, CB slight L) */
    /* real AA values from the 2026-11-26 drive: 90 = exit 1 (left), 180 = straight, 270 = right, 360 = back */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 270, 13, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC3);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 90, 13, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC9);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 180, 13, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC0);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 360, 13, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC6);

    /* arrived / recalc: no F2 icon (the CAN output clears the list instead) */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ARRIVED, -1, 0, 0, -1}, 5, &c);
    CHECK(n.type == HN_TYPE_ARRIVED && n.f2_icon == 0);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_RECALC, -1, 0, 0, -1}, -1, &c);
    CHECK(n.type == HN_TYPE_RECALC);

    /* bar counts DOWN toward the turn: 90 far, 10 near (Numen: 250m -> 80%).
     * 0 at/beyond the window edge. */
    CHECK(hn_bar(0, 457) == 10);
    CHECK(hn_bar(457, 457) == 0);
    CHECK(hn_bar(1000, 457) == 0);
    CHECK(hn_bar(400, 457) >= 75 && hn_bar(400, 457) <= 85);   /* ~80% far */
    CHECK(hn_bar(228, 457) >= 40 && hn_bar(228, 457) <= 60);

    /* normalization */
    CHECK(hn_normalize_aa(4, 2, -1) == HN_MAN_TURN_RIGHT);
    CHECK(hn_normalize_aa(8, 3, -1) == HN_MAN_EXIT_GENERIC);
    CHECK(hn_normalize_aa(13, 0, 90) == HN_MAN_ROUNDABOUT);
    CHECK(hn_normalize_cp(23) == HN_MAN_EXIT_RIGHT);
    CHECK(hn_normalize_cp(2) == HN_MAN_TURN_RIGHT);
    CHECK(hn_normalize_cp(8) == HN_MAN_EXIT_GENERIC);
}

static void test_motion(void)
{
    hn_config c; hn_config_default(&c);
    hn_motion m; hn_motion_init(&m);
    hn_native n; bool clear;

    /* inactive -> one clear, no frames */
    CHECK(!hn_motion_tick(&m, 1000, &c, &n, &clear));
    hn_motion_on_owner(&m, SRC_AA, 1000);
    hn_motion_on_state(&m, HN_NAV_ACTIVE, 1000);
    hn_motion_on_turn(&m, &(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 1000);
    hn_motion_on_heartbeat(&m, 1000);
    hn_motion_on_dist(&m, &(hn_dist){100, 100, 0}, 1000);
    CHECK(hn_motion_tick(&m, 1000, &c, &n, &clear));
    CHECK(n.type == HN_TYPE_TURN && n.dist_m == 100);

    /* destination distance is carried independently of the maneuver */
    hn_motion_on_dest(&m, 1600, 1000);
    CHECK(hn_motion_tick(&m, 1000, &c, &n, &clear));
    CHECK(n.dest_m == 1600 && n.dist_m == 100);

    /* glide: 1 s later at 10 m/s closing, gain 0.8 -> ~92 m */
    hn_motion_on_dist(&m, &(hn_dist){90, 90, 0}, 2000);
    CHECK(hn_motion_tick(&m, 2000, &c, &n, &clear));
    CHECK(n.dist_m <= 90 && n.dist_m >= 88);

    /* new maneuver cannot borrow old distance: dist resets to unknown */
    hn_motion_on_turn(&m, &(hn_turn){SRC_AA, HN_MAN_TURN_LEFT, -1, 4, 1, -1}, 3000);
    CHECK(hn_motion_tick(&m, 3000, &c, &n, &clear));
    CHECK(n.dist_m == -1 && n.type == HN_TYPE_ICON);  /* far until new sample */
    CHECK(n.dest_m == 1600);                          /* dest survives maneuver change */

    /* link stale -> clear once */
    hn_motion_on_dist(&m, &(hn_dist){50, 50, 0}, 3000);
    hn_motion_on_heartbeat(&m, 3000);
    CHECK(hn_motion_tick(&m, 3100, &c, &n, &clear));
    bool got_clear = false;
    for (int64_t t = 8000; t <= 9000; t += 250)
        if (!hn_motion_tick(&m, t, &c, &n, &clear) && clear) got_clear = true;
    CHECK(got_clear);
    CHECK(!clear);

    /* guidance end -> clear */
    hn_motion_on_turn(&m, &(hn_turn){SRC_AA, HN_MAN_TURN_RIGHT, -1, 4, 2, -1}, 10000);
    hn_motion_on_dist(&m, &(hn_dist){100, 100, 0}, 10000);
    CHECK(hn_motion_tick(&m, 10000, &c, &n, &clear));
    hn_motion_on_heartbeat(&m, 10000);
    hn_motion_on_state(&m, HN_NAV_INACTIVE, 10000);
    CHECK(!hn_motion_tick(&m, 10000, &c, &n, &clear));
    CHECK(clear);
    CHECK(m.dest_m == -1);                            /* route end clears dest */
}

/* T19: roundabout angle-mirror option, turnNumber plumbing, exit-icon reuse. */
static void test_roundabout_t19(void)
{
    hn_config c; hn_config_default(&c);
    hn_native n;
    const int angles[5] = {0, 45, 90, 180, 270};
    /* AA angles are rotated by 180 first (car 2026-11-26) */
    const int plain[5] = {0xC6, 0xC8, 0xC9, 0xC0, 0xC3};   /* mirror off (default) */
    const int mirrored[5] = {0xC6, 0xC4, 0xC3, 0xC0, 0xC9}; /* mirror(angle, LHT) after rotation */

    CHECK(!c.rbt_angle_mirror);   /* default keeps today's F2 codes */
    for (int i = 0; i < 5; i++) {
        n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, angles[i], 13, 0, -1}, 100, &c);
        CHECK(n.f2_icon == plain[i]);
    }
    c.rbt_angle_mirror = true;
    for (int i = 0; i < 5; i++) {
        n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, angles[i], 13, 0, -1}, 100, &c);
        CHECK(n.f2_icon == mirrored[i]);
    }
    c.rbt_angle_mirror = false;

    /* ROUNDABOUT_EXIT (event 12, no angle) reuses the icon from ROUNDABOUT_ENTER (11). */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 270, 11, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC3);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, -1, 12, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0xC3);
    CHECK(n.dist_m == 100);   /* reuse must not reset the distance */

    /* Non-roundabout, ARRIVED and RECALC all drop the remembered icon. */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_STRAIGHT, -1, 14, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x41);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, -1, 12, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x41);   /* no prior roundabout: old fallback */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 90, 13, 0, -1}, 100, &c);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ARRIVED, -1, 0, 0, -1}, 5, &c);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, -1, 12, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x41);   /* ARRIVED reset */
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, 90, 13, 0, -1}, 100, &c);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_RECALC, -1, 0, 0, -1}, -1, &c);
    n = hn_convert(&(hn_turn){SRC_AA, HN_MAN_ROUNDABOUT, -1, 12, 0, -1}, 100, &c);
    CHECK(n.f2_icon == 0x41);   /* RECALC reset */

    /* Real daemon TURN-line parser: fourth= -> turn_number, -1 when absent. */
    hn_motion m; hn_motion_init(&m);
    hn_motion_on_owner(&m, SRC_AA, 1000);
    handle_line(&m, "TURN src=aa man=15 exit=90 raw=13 side=0 fourth=3", 1000, false);
    CHECK(m.turn.turn_number == 3);
    CHECK(m.turn.man == HN_MAN_ROUNDABOUT && m.turn.exit_angle == 90 &&
          m.turn.raw_type == 13 && m.turn.raw_side == 0 && m.turn.src == SRC_AA);
    hn_motion m2; hn_motion_init(&m2);
    hn_motion_on_owner(&m2, SRC_AA, 1000);
    handle_line(&m2, "TURN src=aa man=15 exit=-1 raw=12 side=0", 1000, false);
    CHECK(m2.turn.turn_number == -1);
    CHECK(m2.turn.exit_angle == -1);
}

int main(void)
{
    test_convert();
    test_motion();
    test_roundabout_t19();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
