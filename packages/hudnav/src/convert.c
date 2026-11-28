/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Maneuver -> native cluster format conversion. Tables derived from the Android
 * Gen5W prior art (Open-HMG cluster-turns.md / ar-hud.md, UWP notes/carplay-ar.md);
 * see docs/hudnav/HUD_NAV_REPORT.md. PROTOTYPE — icon codes and angle coding
 * must be verified against AU stock-nav captures before any car use.
 */
#include "hudnav.h"

void hn_config_default(hn_config *c)
{
    c->lht = true;            /* AU: right-hand drive, left-hand traffic */
    c->street_ar_m = 200;
    c->exit_ar_m = 457;
    c->approach_min_m = 10;
    c->bar_scale_m = 457;
    c->glide_gain = 0.8;
    c->backstep_hold_m = 15;
    c->stale_ms = 4000;
    c->lease_ms = 300000;
    c->rbt_angle_mirror = false; /* keep today's F2 roundabout codes until T14's live test */
}

static int mirror(int angle, bool lht)
{
    if (!lht) return angle % 360;
    return (360 - (angle % 360)) % 360;
}

/* Android Auto legacy NextTurnEnum (AOSP CarNavigationStatusManager). */
hn_maneuver hn_normalize_aa(int event, int side, int angle)
{
    (void)angle;
    bool left = (side == 1), right = (side == 2);
    switch (event) {
    case 1:  return HN_MAN_DEPART;
    case 2:  return HN_MAN_STRAIGHT;      /* name change: continue */
    case 3:  return left ? HN_MAN_SLIGHT_LEFT : right ? HN_MAN_SLIGHT_RIGHT : HN_MAN_STRAIGHT;
    case 4:  return left ? HN_MAN_TURN_LEFT : right ? HN_MAN_TURN_RIGHT : HN_MAN_STRAIGHT;
    case 5:  return left ? HN_MAN_SHARP_LEFT : right ? HN_MAN_SHARP_RIGHT : HN_MAN_STRAIGHT;
    case 6:  return left ? HN_MAN_UTURN_LEFT : HN_MAN_UTURN_RIGHT;
    case 7:  return HN_MAN_ON_RAMP;
    case 8:  return left ? HN_MAN_EXIT_LEFT : right ? HN_MAN_EXIT_RIGHT : HN_MAN_EXIT_GENERIC;
    case 9:  return left ? HN_MAN_FORK_LEFT : right ? HN_MAN_FORK_RIGHT : HN_MAN_STRAIGHT;
    case 10: return HN_MAN_STRAIGHT;      /* merge */
    case 11: case 12: case 13: return HN_MAN_ROUNDABOUT;
    case 14: return HN_MAN_STRAIGHT;
    case 16: case 17: return HN_MAN_FERRY;
    case 19: return HN_MAN_DESTINATION;
    default: return HN_MAN_UNKNOWN;
    }
}

/* CarPlay iAP2 EManeuverType 0..53 (full table from carplay_protocol.md §5,
 * MIB ManeuverMapper / Apple accessoryd). Roundabout exits 28-46 carry the exit
 * angle via the maneuver's junction data (handled by the caller). */
hn_maneuver hn_normalize_cp(int mt)
{
    switch (mt) {
    case 0:  case 5:  case 3:  return HN_MAN_STRAIGHT;   /* NO_TURN/FOLLOW_ROAD/STRAIGHT */
    case 1:  case 20:          return HN_MAN_TURN_LEFT;   /* LEFT_TURN / LEFT_TURN_AT_END */
    case 2:  case 21:          return HN_MAN_TURN_RIGHT;
    case 4:  case 18: case 26: return HN_MAN_UTURN_LEFT;  /* U_TURN / START_ROUTE_UTURN / U_TURN_WHEN_POSSIBLE */
    case 6:  case 7:  case 19: return HN_MAN_ROUNDABOUT;  /* ENTER/EXIT_ROUNDABOUT / U_TURN_AT_ROUNDABOUT */
    case 8:                    return HN_MAN_EXIT_GENERIC;/* OFF_RAMP */
    case 9:                    return HN_MAN_ON_RAMP;
    case 10: case 12: case 27:
    case 24: case 25:          return HN_MAN_ARRIVED;     /* ARRIVE_* / ARRIVE_DESTINATION_L/R */
    case 11:                   return HN_MAN_DEPART;      /* START_ROUTE */
    case 13: case 49:          return HN_MAN_SLIGHT_LEFT; /* KEEP_LEFT / SLIGHT_LEFT */
    case 14: case 50:          return HN_MAN_SLIGHT_RIGHT;
    case 15: case 16: case 17: return HN_MAN_FERRY;       /* ENTER/EXIT/CHANGE_FERRY */
    case 22:                   return HN_MAN_EXIT_LEFT;   /* HWY_OFF_RAMP_LEFT */
    case 23:                   return HN_MAN_EXIT_RIGHT;
    case 47:                   return HN_MAN_SHARP_LEFT;
    case 48:                   return HN_MAN_SHARP_RIGHT;
    case 52:                   return HN_MAN_FORK_LEFT;   /* CHANGE_HIGHWAY_LEFT */
    case 53:                   return HN_MAN_FORK_RIGHT;
    case 51:                   return HN_MAN_STRAIGHT;    /* CHANGE_HIGHWAY (no side) -> cue */
    case 255:                  return HN_MAN_UNKNOWN;    /* NOT_SET */
    default:
        if (mt >= 28 && mt <= 46) return HN_MAN_ROUNDABOUT; /* ROUNDABOUT_EXIT_1..19 */
        return HN_MAN_UNKNOWN;
    }
}


int hn_bar(int dist_m, int scale_m)
{
    if (dist_m < 0 || scale_m <= 0 || dist_m >= scale_m) return 0;
    /* Stock behavior (road-tested Android mod + Numen IONIQ6 example): the bar
     * is HIGH far from the maneuver (90) and LOW near it (10) — it counts DOWN
     * as you approach. Numen's worked example: right turn at 250 m -> bar 0x50
     * (80%), consistent with dist/scale on a ~300 m near-view window.
     * (A previous version had this inverted; corrected 2026-11-24.) */
    int v = 10 + (int)((int64_t)dist_m * 80 / scale_m);
    return v > 90 ? 90 : v;
}

/* Car 2026-11-26 (owner drive, AU LHT): with the raw AA turnAngle every roundabout arrow was rotated
 * by half a turn (straight <-> U-turn, left <-> right). Car log: AA angle = degrees travelled clockwise
 * from the entry (exit 1 left = 90, straight = 180, right = 270, back = 360). The converter's tables use
 * straight on = 0, right = 90: rotate AA by 180. Returns -1 when unknown; other sources unchanged. */
static int rbt_exit_angle(const hn_turn *t)
{
    if (t->exit_angle < 0) return -1;
    int a = t->exit_angle % 360;
    if (t->src == SRC_AA) a = (a + 180) % 360;
    return a;
}

/* T19: last roundabout F2 icon seen in the current maneuver. A ROUNDABOUT_EXIT
 * with no angle reuses it; hn_convert clears it on any non-roundabout maneuver. */
static int last_rbt_f2_icon = 0;

/* Far projection icons (>=64). RHT codes; LHT roundabout family is +96.
 * Codes marked verified-in-prior-art; others are best-effort (INFERENCE). */
static int far_icon(const hn_turn *t, const hn_config *c)
{
    switch (t->man) {
    case HN_MAN_STRAIGHT:      return 65;
    case HN_MAN_SLIGHT_RIGHT:  return 66;
    case HN_MAN_SLIGHT_LEFT:   return 71;
    case HN_MAN_TURN_RIGHT:    return 67;
    case HN_MAN_TURN_LEFT:     return 70;
    case HN_MAN_SHARP_RIGHT:   return 68;
    case HN_MAN_SHARP_LEFT:    return 69;
    case HN_MAN_UTURN_LEFT:    return 72;   /* 0x48 left U-turn, 0x49 right: HUD sweep 2026-11-25 */
    case HN_MAN_UTURN_RIGHT:   return 73;
    /* Highway exits: AppNavi hw_ic_exit_right/left (RG 28/27), HUD sweep 2026-11-28:
     * 0xA4 = 45-deg right exit, 0xA5 = 45-deg left exit, 0xA3 = straight. 0x51/0x52 are
     * overpass-diverge glyphs and 0x56 drew a plain straight arrow on a motorway exit. A
     * sideless OFF_RAMP takes the traffic-side exit (left in LHT). */
    case HN_MAN_EXIT_RIGHT:    return 164;
    case HN_MAN_EXIT_LEFT:     return 165;
    case HN_MAN_EXIT_GENERIC:  return c->lht ? 165 : 164;
    case HN_MAN_FORK_RIGHT:    return 147;  /* 0x93 right at a Y fork (HUD sweep 2026-11-25) */
    case HN_MAN_FORK_LEFT:     return 149;  /* 0x95 left at a Y fork */
    case HN_MAN_ON_RAMP:       return 172;
    case HN_MAN_FERRY:         return 176;
    case HN_MAN_DESTINATION:   return 112;
    case HN_MAN_DEPART:        return 65;   /* start-route cue, no AR */
    case HN_MAN_ROUNDABOUT: {
        /* exit angle -> icon, 45-deg steps. RHT 96..107, LHT 192..203.
         * Avoid 103/106 (and LHT 202/0xca) — spinner codes. */
        static const int rht[8] = {96, 97, 99, 100, 102, 104, 105, 107};
        int a = rbt_exit_angle(t);
        if (a < 0) return last_rbt_f2_icon > 0 ? last_rbt_f2_icon : 65;
        /* UNVERIFIED: settle with one live AU roundabout (T14) */
        if (c->rbt_angle_mirror) a = mirror(a, c->lht);
        int idx = ((a % 360) + 22) / 45 % 8;   /* nearest 45-deg step */
        int code = rht[idx];
        if (code == 103 || code == 106) code = 104; /* fallback, unverified */
        int icon = c->lht ? code + 96 : code;
        last_rbt_f2_icon = icon;
        return icon;
    }
    default:                   return 65;   /* never 64 (spinner) */
    }
}

hn_native hn_convert(const hn_turn *t, int dist_m, const hn_config *c)
{
    hn_native n;
    n.type = HN_TYPE_ICON; n.angle = 0; n.icon = 65;
    n.side_street = 65535; n.dist_m = dist_m; n.dest_m = -1; n.unit = 0;
    n.bar = 0; n.near = false; n.f2_icon = 0;
    n.steps = HN_STEPS_BALANCED; n.units = HN_UNITS_CAR;

    /* T19: roundabout icon reuse is scoped to the current roundabout maneuver;
     * ARRIVED, RECALC and every other maneuver drop the remembered icon. */
    if (t->man != HN_MAN_ROUNDABOUT) last_rbt_f2_icon = 0;

    if (t->man == HN_MAN_ARRIVED) {
        n.type = HN_TYPE_ARRIVED; n.angle = 0; n.side_street = 0;
        n.near = true; n.bar = 0;
        return n;
    }
    if (t->man == HN_MAN_RECALC) {
        n.type = HN_TYPE_RECALC; n.angle = 0; n.side_street = 0;
        n.near = true;
        return n;
    }

    /* The HUD draws the arrow from the CAN-TP F2 list, so every real maneuver carries its icon
     * code, near or far (stock: near right turn -> 4cc type 0d, list 43; far -> type 43). */
    n.f2_icon = far_icon(t, c);

    bool is_exit_family = (t->man == HN_MAN_EXIT_LEFT || t->man == HN_MAN_EXIT_RIGHT ||
                           t->man == HN_MAN_EXIT_GENERIC || t->man == HN_MAN_FORK_LEFT ||
                           t->man == HN_MAN_FORK_RIGHT || t->man == HN_MAN_UTURN_LEFT ||
                           t->man == HN_MAN_UTURN_RIGHT || t->man == HN_MAN_ROUNDABOUT);
    int window = is_exit_family ? c->exit_ar_m : c->street_ar_m;

    /* Far / unknown distance: projection icon card, bar off. */
    if (dist_m < 0 || dist_m > window || t->man == HN_MAN_DEPART ||
        t->man == HN_MAN_FERRY || t->man == HN_MAN_DESTINATION) {
        n.icon = far_icon(t, c);
        n.side_street = 65535;
        if (t->man == HN_MAN_DESTINATION) n.bar = 0;
        else if (dist_m >= 0) n.bar = hn_bar(dist_m, c->bar_scale_m);
        return n;
    }

    /* Near: native types, side_street 0, angle in 45-deg steps.
     *
     * LHT mirroring is NOT a blanket angle transform. docs/hudnav/HUD_NAV_REPORT.md
     * §9 (Creta Electric India, RHD/LHT, byte-measured) found traffic-side mirroring
     * on exactly two things: the roundabout icon family (RHT 0x20/96-107 <->
     * LHT 0x21/192-203, exit angle = 360-angle) and the U-turn angle (LHT sends 90,
     * not 270). Everything else -- including the plain turn arrow (0x0d, confirmed
     * to have no L/R variant: "0x0d turn arrow", single code for both directions) --
     * is "cluster-dependent, not traffic-dependent" per that same measurement: a
     * single rotated-arrow icon encodes a physical turn direction that does not
     * change with which side of the road you drive on. Mirroring TURN_RIGHT/LEFT
     * (or SLIGHT/SHARP/ON_RAMP) here would send a right turn as a left arrow on
     * every LHT (AU) car -- do not reintroduce that. FORK/EXIT angle mirroring is
     * unconfirmed either way; left un-mirrored (the physically-correct default)
     * pending a parked symbol sweep -- see CAR_READY_SANITY_CHECK.md §2. */
    n.near = true;
    n.side_street = 0;
    n.bar = hn_bar(dist_m, c->bar_scale_m);
    int a = 0;
    bool mirror_angle = false;
    switch (t->man) {
    case HN_MAN_STRAIGHT:     n.type = HN_TYPE_TURN; a = 0; break;
    case HN_MAN_TURN_RIGHT:   n.type = HN_TYPE_TURN; a = 90; break;
    case HN_MAN_TURN_LEFT:    n.type = HN_TYPE_TURN; a = 270; break;
    case HN_MAN_SLIGHT_RIGHT: n.type = HN_TYPE_TURN; a = 45; break;
    case HN_MAN_SLIGHT_LEFT:  n.type = HN_TYPE_TURN; a = 315; break;
    case HN_MAN_SHARP_RIGHT:  n.type = HN_TYPE_TURN; a = 135; break;
    case HN_MAN_SHARP_LEFT:   n.type = HN_TYPE_TURN; a = 225; break;
    case HN_MAN_UTURN_LEFT:
    case HN_MAN_UTURN_RIGHT:
        n.type = HN_TYPE_UTURN; a = 270; mirror_angle = true; break; /* LHT->90, confirmed */
    case HN_MAN_FORK_RIGHT:   n.type = HN_TYPE_FORK; a = 90; break;
    case HN_MAN_FORK_LEFT:    n.type = HN_TYPE_FORK; a = 270; break;
    case HN_MAN_EXIT_RIGHT:   n.type = HN_TYPE_EXIT_R; a = 90; break;
    case HN_MAN_EXIT_LEFT:    n.type = HN_TYPE_EXIT_L; a = 270; break;
    case HN_MAN_EXIT_GENERIC:
        /* side from exit-angle sign else traffic side (UWP 108 policy). */
        if (t->exit_angle != 0 && t->exit_angle != -1)
            n.type = (t->exit_angle > 0) ? HN_TYPE_EXIT_R : HN_TYPE_EXIT_L;
        else
            n.type = c->lht ? HN_TYPE_EXIT_L : HN_TYPE_EXIT_R;
        a = (n.type == HN_TYPE_EXIT_R) ? 90 : 270;
        break;
    case HN_MAN_ON_RAMP:      n.type = HN_TYPE_TURN; a = 45; break;
    case HN_MAN_ROUNDABOUT:
        n.type = c->lht ? HN_TYPE_ROUND_L : HN_TYPE_ROUND_R;
        a = (rbt_exit_angle(t) >= 0) ? rbt_exit_angle(t) : 0; /* straight-on = 0 convention */
        mirror_angle = true; /* confirmed: RHT angle, LHT = 360 - angle */
        break;
    default:
        n.type = HN_TYPE_ICON; n.icon = far_icon(t, c);
        n.side_street = 65535; n.near = false;
        return n;
    }
    n.angle = mirror_angle ? mirror(a, c->lht) : (a % 360);
    return n;
}
