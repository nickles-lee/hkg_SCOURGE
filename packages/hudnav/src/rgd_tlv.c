/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "rgd_tlv.h"
#include <limits.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint64_t tlv_uint(const uint8_t *v, size_t n)
{
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) x = (x << 8) | v[i];
    return x;
}

static int tlv_int(const uint8_t *v, size_t n)
{
    if (!n || n > 4) return -1;
    uint64_t x = tlv_uint(v, n);
    return x <= INT_MAX ? (int)x : -1;
}

static int tlv_angle(const uint8_t *v, size_t n)
{
    /* Retain one-byte compatibility with the original host fixtures. */
    if (n == 1) return v[0] < 128 ? v[0] : v[0] - 256;
    if (n == 2) {
        int x = be16(v);
        return x < 32768 ? x : x - 65536;
    }
    return INT_MIN;
}

static void tlv_str(const uint8_t *v, size_t n, char *out, size_t osz)
{
    if (n >= osz) n = osz - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

static int display_e3(const uint8_t *v, size_t n)
{
    int64_t whole = 0;
    int fraction = 0, scale = 1000;
    bool decimal = false, digits = false;
    for (size_t i = 0; i < n; i++) {
        int c = v[i];
        if (!c && i + 1 == n) break;
        if (!decimal && digits && (c == '.' || c == ',')) {
            decimal = true;
        } else if (c < '0' || c > '9') {
            return -1;
        } else if (decimal) {
            scale /= 10;
            if (!scale) return -1;
            fraction += (c - '0') * scale;
        } else {
            digits = true;
            whole = whole * 10 + c - '0';
            if (whole > INT_MAX / 1000) return -1;
        }
    }
    int64_t result = whole * 1000 + fraction;
    return digits && (!decimal || scale != 1000) && result <= INT_MAX ? (int)result : -1;
}

static const uint8_t *rgd_header(const uint8_t *msg, size_t len, uint16_t expect,
                                 size_t *tlv_len)
{
    if (len < 6 || msg[0] != 0x40 || msg[1] != 0x40) return NULL;
    size_t size = be16(msg + 2);
    if (size < 6 || size > len || be16(msg + 4) != expect) return NULL;
    *tlv_len = size - 6;
    return msg + 6;
}

#define RGD_WALK(p, n) \
    for (const uint8_t *t = (p); (n) != 0; ) { \
        if ((n) < 4) return false; \
        uint16_t tl = be16(t); uint16_t id = be16(t + 2); \
        if (tl < 4 || tl > (n)) return false; \
        const uint8_t *v = t + 4; size_t vn = tl - 4;

#define RGD_END(n) t += tl; (n) -= tl; }

bool hn_rgd_parse_update(const uint8_t *msg, size_t len, hn_rgd_state *out)
{
    memset(out, 0, sizeof *out);
    out->dist_dest_m = -1;
    out->dist_maneuver_m = -1;
    out->dist_display_e3 = -1;
    out->dist_display_unit = -1;
    out->next_index = -1;
    size_t n;
    const uint8_t *p = rgd_header(msg, len, RGD_MSG_UPDATE, &n);
    if (!p) return false;
    RGD_WALK(p, n)
        switch (id) {
        case 0x01: out->route_state = tlv_int(v, vn); break;
        case 0x02: out->maneuver_state = tlv_int(v, vn); break;
        case 0x03: tlv_str(v, vn, out->current_road, sizeof out->current_road); break;
        case 0x04: tlv_str(v, vn, out->destination, sizeof out->destination); break;
        case 0x05:
        case 0x06:
            if (vn != 8) break;
            if (id == 0x05) out->eta_seconds = tlv_uint(v, vn);
            else out->time_remaining_s = tlv_uint(v, vn);
            break;
        case 0x07: out->dist_dest_m = tlv_int(v, vn); break;
        case 0x0A: out->dist_maneuver_m = tlv_int(v, vn); break;
        case 0x0B: out->dist_display_e3 = display_e3(v, vn); break;
        case 0x0C: out->dist_display_unit = tlv_int(v, vn); break;
        case 0x0D:
            if (vn % 2) return false;
            if (vn) out->next_index = be16(v);
            break;
        case 0x0E: out->maneuver_count = tlv_int(v, vn); break;
        case 0x0F: out->visible_in_app = tlv_int(v, vn); break;
        case 0x10: out->lane_index = tlv_int(v, vn); break;
        case 0x11: out->lane_count = tlv_int(v, vn); break;
        case 0x12: out->lanes_showing = tlv_int(v, vn); break;
        case 0x14: out->source_supports_rg = tlv_int(v, vn); break;
        default: break;
        }
        if (id < 32 && ((id != 0x05 && id != 0x06) || vn == 8))
            out->present |= (1u << id);
    RGD_END(n)
    return true;
}

static void append_angle(int *angles, size_t *count, int angle)
{
    if (*count < RGD_MAX_ANGLES && angle != INT_MIN) angles[(*count)++] = angle;
}

bool hn_rgd_parse_maneuver(const uint8_t *msg, size_t len, hn_rgd_maneuver *out)
{
    memset(out, 0, sizeof *out);
    out->type = -1;
    out->exit_angle = -1;
    out->junction_angle = -1;
    out->index = -1;
    size_t n;
    const uint8_t *p = rgd_header(msg, len, RGD_MSG_MANEUVER, &n);
    if (!p) return false;
    RGD_WALK(p, n)
        switch (id) {
        case 0x01: out->index = tlv_int(v, vn); break;
        case 0x03: out->type = tlv_int(v, vn); break;
        case 0x04: tlv_str(v, vn, out->after_road, sizeof out->after_road); break;
        case 0x05: out->dist_between_m = tlv_int(v, vn); break;
        case 0x08: out->driving_side = tlv_int(v, vn); break;
        case 0x09: out->junction_type = tlv_int(v, vn); break;
        case 0x0A:
            if (vn == 1) append_angle(out->junction_angles, &out->junction_angle_count, tlv_angle(v, vn));
            for (size_t i = 0; i + 1 < vn; i += 2)
                append_angle(out->junction_angles, &out->junction_angle_count, tlv_angle(v + i, 2));
            if (out->junction_angle_count) out->junction_angle = out->junction_angles[0];
            break;
        case 0x0B: out->exit_angle = tlv_angle(v, vn); break;
        case 0x0D: tlv_str(v, vn, out->exit_info, sizeof out->exit_info); break;
        default: break;
        }
        if (id < 32) out->present |= (1u << id);
    RGD_END(n)
    return true;
}

static bool parse_lane_group(const uint8_t *p, size_t n, hn_rgd_lanes *out)
{
    hn_rgd_lane *lane = NULL;
    RGD_WALK(p, n)
        if (id <= 3 && (id == 0 || !lane)) {
            if (out->lane_count == RGD_MAX_LANES) return false;
            int position = id == 0 ? tlv_int(v, vn) : (int)out->lane_count;
            lane = &out->lanes[out->lane_count++];
            lane->position = position;
            lane->status = -1;
            lane->highlight = INT_MIN;
        }
        switch (id) {
        case 1: lane->status = tlv_int(v, vn); break;
        case 2:
            if (!vn || vn % 2) return false;
            for (size_t i = 0; i < vn; i += 2)
                append_angle(lane->angles, &lane->angle_count, tlv_angle(v + i, 2));
            break;
        case 3:
            lane->highlight = tlv_angle(v, vn);
            if (lane->highlight == INT_MIN) return false;
            break;
        default: break;
        }
    RGD_END(n)
    return true;
}

bool hn_rgd_parse_lanes(const uint8_t *msg, size_t len, hn_rgd_lanes *out)
{
    memset(out, 0, sizeof *out);
    out->index = -1;
    size_t n;
    const uint8_t *p = rgd_header(msg, len, RGD_MSG_LANES, &n);
    if (!p) return false;
    RGD_WALK(p, n)
        switch (id) {
        case 1: out->index = tlv_int(v, vn); break;
        case 2: if (!parse_lane_group(v, vn, out)) return false; break;
        case 3: tlv_str(v, vn, out->description, sizeof out->description); break;
        default: break;
        }
    RGD_END(n)
    return out->index >= 0 && out->index <= UINT16_MAX;
}

hn_turn hn_rgd_to_turn(const hn_rgd_maneuver *m)
{
    hn_turn t = { .src = SRC_CP, .man = hn_normalize_cp(m->type), .exit_angle = -1,
                  .raw_type = m->type, .raw_side = m->driving_side, .turn_number = -1 };
    bool have_angle = (m->present & (1u << 0x0B)) &&
                      m->exit_angle >= -180 && m->exit_angle <= 180;
    if (have_angle) {
        t.exit_angle = m->exit_angle;
        if (t.man == HN_MAN_ROUNDABOUT) t.exit_angle = (t.exit_angle + 360) % 360;
        /* The daemon uses -1 as "angle absent", so resolve generic exits here. */
        if (t.man == HN_MAN_EXIT_GENERIC && m->exit_angle != 0)
            t.man = m->exit_angle < 0 ? HN_MAN_EXIT_LEFT : HN_MAN_EXIT_RIGHT;
    }
    /* Car drive 2026-11-27: one roundabout came through as a plain right turn (shown as a right
     * arrow) while another showed the roundabout icon. iAP2 JunctionType (TLV 0x09) = 1 means the
     * maneuver happens AT a roundabout, so a left/right/straight/slight/sharp there is drawn as a
     * roundabout with the matching exit angle (the explicit exit angle wins when present). */
    if ((m->present & (1u << 0x09)) && m->junction_type == 1 && t.man != HN_MAN_ROUNDABOUT) {
        int a = -1;
        switch (t.man) {
        case HN_MAN_STRAIGHT:     a = 0;   break;
        case HN_MAN_SLIGHT_RIGHT: a = 45;  break;
        case HN_MAN_TURN_RIGHT:   a = 90;  break;
        case HN_MAN_SHARP_RIGHT:  a = 135; break;
        case HN_MAN_SHARP_LEFT:   a = 225; break;
        case HN_MAN_TURN_LEFT:    a = 270; break;
        case HN_MAN_SLIGHT_LEFT:  a = 315; break;
        case HN_MAN_UTURN_LEFT:
        case HN_MAN_UTURN_RIGHT:  a = 180; break;
        default: break;
        }
        if (a >= 0) {
            t.man = HN_MAN_ROUNDABOUT;
            t.exit_angle = have_angle ? (m->exit_angle + 360) % 360 : a;
        }
    }
    if (t.man == HN_MAN_UTURN_LEFT &&
        ((have_angle && m->exit_angle > 0) ||
         ((!have_angle || m->exit_angle == 0) && m->driving_side == 1)))
        t.man = HN_MAN_UTURN_RIGHT;
    if (m->type >= 28 && m->type <= 46) t.turn_number = m->type - 27;
    return t;
}
