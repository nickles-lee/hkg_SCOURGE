/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef HUDNAV_RGD_TLV_H
#define HUDNAV_RGD_TLV_H

#include "hudnav.h"
#include <stdint.h>
#include <stddef.h>

#define RGD_MSG_UPDATE   0x5201
#define RGD_MSG_MANEUVER 0x5202
#define RGD_MSG_LANES    0x5204
#define RGD_MAX_LANES    8
#define RGD_MAX_ANGLES   16

typedef struct {
    int      route_state;        /* 0x01 */
    int      maneuver_state;     /* 0x02 */
    uint64_t eta_seconds;        /* 0x05, absolute arrival time */
    uint64_t time_remaining_s;   /* 0x06 */
    int      dist_dest_m;        /* 0x07 */
    int      dist_maneuver_m;    /* 0x0A, exact distance */
    int      dist_display_e3;    /* 0x0B, display value in thousandths */
    int      dist_display_unit;  /* 0x0C, iAP2 units enum */
    int      next_index;         /* 0x0D, head of maneuver index list */
    int      maneuver_count;     /* 0x0E */
    int      visible_in_app;     /* 0x0F */
    int      lane_index;         /* 0x10 */
    int      lane_count;         /* 0x11 */
    int      lanes_showing;      /* 0x12 */
    int      source_supports_rg; /* 0x14 */
    char     current_road[64];   /* 0x03 */
    char     destination[64];    /* 0x04 */
    uint32_t present;            /* bitmask of TLV ids seen */
} hn_rgd_state;

typedef struct {
    int  index;            /* 0x01 */
    int  type;             /* 0x03, EManeuverType 0..53 */
    int  dist_between_m;   /* 0x05 */
    int  driving_side;     /* 0x08, 0 RHT, 1 LHT */
    int  junction_type;    /* 0x09, 0 intersection, 1 roundabout */
    int  junction_angle;   /* 0x0A, first element for legacy callers */
    int  junction_angles[RGD_MAX_ANGLES];
    size_t junction_angle_count;
    int  exit_angle;       /* 0x0B, signed departure bearing */
    char after_road[64];   /* 0x04 */
    char exit_info[64];    /* 0x0D, signpost text */
    uint32_t present;
} hn_rgd_maneuver;

typedef struct {
    int position, status, highlight;
    int angles[RGD_MAX_ANGLES];
    size_t angle_count;
} hn_rgd_lane;

typedef struct {
    int index;
    hn_rgd_lane lanes[RGD_MAX_LANES];
    size_t lane_count;
    char description[64];
} hn_rgd_lanes;

/* Parse a full control message (40 40 + len BE16 + msgid BE16 + TLVs).
 * Returns false on malformed framing. Callers must ignore output on failure. */
bool hn_rgd_parse_update(const uint8_t *msg, size_t len, hn_rgd_state *out);
bool hn_rgd_parse_maneuver(const uint8_t *msg, size_t len, hn_rgd_maneuver *out);
bool hn_rgd_parse_lanes(const uint8_t *msg, size_t len, hn_rgd_lanes *out);

/* Normalize a parsed maneuver to the common hn_turn (uses hn_normalize_cp). */
hn_turn hn_rgd_to_turn(const hn_rgd_maneuver *m);

#endif
