/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * hudnav — CarPlay/Android Auto turn-by-turn forwarder for Linux Gen5W (Ioniq 5 AU).
 * PROTOTYPE / WIP — offline host-tested only; NOT car-validated. Separate project;
 * SCOURGE is access/reference only. See docs/hudnav/HUD_NAV_REPORT.md.
 *
 * Core types shared by the converter, motion state machine and daemon.
 */
#ifndef HUDNAV_H
#define HUDNAV_H

#include <stdint.h>
#include <stdbool.h>
#include "settings.h"

/* ---- input events (line protocol over unix socket, see daemon.c) ---- */

typedef enum { SRC_NONE = 0, SRC_CP, SRC_AA, SRC_OEM } hn_source;

/* Android Auto NextTurnEnum (legacy) / CarPlay ManeuverType are kept as raw
 * values plus a normalized maneuver kind, so the converter is source-agnostic. */
typedef enum {
    HN_MAN_UNKNOWN = 0,
    HN_MAN_STRAIGHT,        /* continue / straight */
    HN_MAN_TURN_LEFT, HN_MAN_TURN_RIGHT,
    HN_MAN_SLIGHT_LEFT, HN_MAN_SLIGHT_RIGHT,
    HN_MAN_SHARP_LEFT, HN_MAN_SHARP_RIGHT,
    HN_MAN_UTURN_LEFT, HN_MAN_UTURN_RIGHT,
    HN_MAN_FORK_LEFT, HN_MAN_FORK_RIGHT,
    HN_MAN_EXIT_LEFT, HN_MAN_EXIT_RIGHT, HN_MAN_EXIT_GENERIC,
    HN_MAN_ROUNDABOUT,      /* exit angle carried separately */
    HN_MAN_ON_RAMP,
    HN_MAN_FERRY,
    HN_MAN_DESTINATION,     /* destination ahead */
    HN_MAN_ARRIVED,
    HN_MAN_DEPART,          /* start-route / head-for-route */
    HN_MAN_RECALC           /* rerouting */
} hn_maneuver;

typedef struct {
    hn_source  src;
    hn_maneuver man;
    int        exit_angle;   /* roundabout/exit angle, degrees; -1 = unknown */
    int        raw_type;     /* source enum value, for diagnostics */
    int        raw_side;     /* source side value */
    int        turn_number;  /* AA turnNumber (fourth= line key); -1 = unknown */
} hn_turn;

typedef struct {
    int meters;              /* exact distance to maneuver, meters; -1 unknown */
    int display;             /* rounded display distance (source units) */
    int unit;                /* 0 m, 1 km, 2 mi, 3 ft, 4 yd (cluster units) */
} hn_dist;

typedef enum { HN_NAV_INACTIVE = 0, HN_NAV_ACTIVE, HN_NAV_REROUTE } hn_navstate;

/* ---- native output (car's own format; Android DisplayType semantics) ---- */

typedef enum {
    HN_TYPE_NONE      = 0,   /* clear */
    HN_TYPE_DIRECT    = 2,   /* destination compass */
    HN_TYPE_ARRIVED   = 3,
    HN_TYPE_RECALC    = 10,
    HN_TYPE_TURN      = 13,
    HN_TYPE_EXIT_R    = 17,
    HN_TYPE_EXIT_L    = 18,
    HN_TYPE_FORK      = 25,
    HN_TYPE_UTURN     = 31,
    HN_TYPE_ROUND_R   = 32,  /* RHT circulation */
    HN_TYPE_ROUND_L   = 33,  /* LHT circulation */
    HN_TYPE_ICON      = 64   /* >=64: projection icon carried in hn_native.icon */
} hn_ntype;

typedef struct {
    hn_ntype type;
    int      angle;          /* degrees, 45-step for native; mirrored for LHT only on
                               * roundabout exit angle + U-turn (see convert.c), not on
                               * plain turn/slight/sharp/on-ramp/fork/exit */
    int      icon;           /* projection icon (>=64) when type == HN_TYPE_ICON */
    uint16_t side_street;    /* 0 with native types, 65535 with icons */
    int      dist_m;         /* meters, -1 unknown */
    int      dest_m;         /* meters to destination, -1 unknown (0x4E8) */
    int      unit;           /* cluster unit for display */
    int      bar;            /* 0..90 distance bar (0 = off) */
    bool     near;           /* near-maneuver native view active */
    int      f2_icon;        /* HUD icon code for the CAN-TP "F2" list (0x6E7); 0 = none.
                              * The HUD draws its turn arrow from THIS, not from 0x4CC byte 3
                              * (car-proven 2026-11-25, docs/hudnav/HUD_NAV_REPORT.md) */
    hn_steps steps;          /* Projection+ distance steps for this source (0 = balanced) */
    hn_units units;          /* Projection+ units for this source (0 = car) */
} hn_native;

/* ---- configuration ---- */

typedef struct {
    bool lht;                /* left-hand traffic (AU default true) */
    int  street_ar_m;        /* native street-turn window, meters (default 200) */
    int  exit_ar_m;          /* exit/fork/uturn window (default 457) */
    int  approach_min_m;     /* min distance before native AR (default 10) */
    int  bar_scale_m;        /* bar scale window (default 457) */
    double glide_gain;       /* extrapolation gain (default 0.8) */
    int  backstep_hold_m;    /* hold sent dist when sample <= this above (15) */
    int  stale_ms;           /* input stale -> clear (default 4000, Numen) */
    int  lease_ms;           /* stuck-sender lease (default 300000) */
    bool rbt_angle_mirror;   /* mirror F2 roundabout exit angle (default false; T14 UNVERIFIED) */
} hn_config;

void hn_config_default(hn_config *c);

/* ---- converter (convert.c) ---- */

/* Source-specific normalization to hn_maneuver.
 * AA: legacy NextTurnEnum event + TurnSide (1 left, 2 right, 3 unspecified).
 * CP: iAP2 ManeuverType. exit_angle degrees or -1. */
hn_maneuver hn_normalize_aa(int event, int side, int angle);
hn_maneuver hn_normalize_cp(int maneuver_type);

/* Map a normalized maneuver to the native near/far representation.
 * dist_m < 0 means unknown. Returns the frame content; `near` selects native
 * types inside the activation window, icons outside. */
hn_native hn_convert(const hn_turn *t, int dist_m, const hn_config *c);

/* bar level 0..90 from distance over the scale window (stock 457 m behavior). */
int hn_bar(int dist_m, int scale_m);

/* ---- motion state machine (motion.c) ---- */

typedef struct {
    /* current maneuver binding */
    hn_turn  turn;
    bool     have_turn;
    bool     have_dist;       /* a distance sample is bound to THIS maneuver */
    int      last_sample_m;   /* last exact phone sample */
    int64_t  last_sample_ms;
    int      sent_m;          /* last emitted distance */
    double   rate_mps;        /* smoothed closing rate */
    int64_t  maneuver_start_ms;
    /* destination distance (0x4E8); slow-moving, independent of the maneuver */
    int      dest_m;
    int64_t  dest_ms;
    /* lease / expiry */
    int64_t  last_input_ms;   /* last interceptor heartbeat/event (link health) */
    bool     cleared;
    hn_navstate navstate;
    hn_source owner;          /* guidance owner from ownership signal */
} hn_motion;

void hn_motion_init(hn_motion *m);
/* Feed events; now_ms is monotonic. */
void hn_motion_on_turn(hn_motion *m, const hn_turn *t, int64_t now_ms);
void hn_motion_on_dist(hn_motion *m, const hn_dist *d, int64_t now_ms);
/* Destination distance in metres (CarPlay TLV 0x07); -1 clears it. */
void hn_motion_on_dest(hn_motion *m, int meters, int64_t now_ms);
void hn_motion_on_state(hn_motion *m, hn_navstate s, int64_t now_ms);
void hn_motion_on_owner(hn_motion *m, hn_source owner, int64_t now_ms);
/* Interceptor link heartbeat (independent of phone nav silence at lights). */
void hn_motion_on_heartbeat(hn_motion *m, int64_t now_ms);

/* 4 Hz tick: compute the distance to emit (glide/backstep/hold), detect
 * expiry/passed. Returns true if a frame should be emitted; `out` filled.
 * `clear` set true when a one-time clear must be sent. */
bool hn_motion_tick(hn_motion *m, int64_t now_ms, const hn_config *c,
                    hn_native *out, bool *clear);

#endif /* HUDNAV_H */
