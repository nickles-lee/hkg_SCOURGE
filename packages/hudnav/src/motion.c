/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Distance glide / expiry / clear state machine. Rules adopted from the Android
 * mod's road-tested behavior (docs/hudnav/HUD_NAV_REPORT.md §6): 1 Hz phone
 * samples glided at 4 Hz with gain, back-step hold, maneuver-bound distances,
 * passed-maneuver and lease expiry with a one-time clear, link-stale clear.
 * PROTOTYPE — host-tested only.
 */
#include "hudnav.h"

static void reset_motion(hn_motion *m, int64_t now_ms)
{
    m->have_dist = false;
    m->last_sample_m = -1;
    m->last_sample_ms = now_ms;
    m->sent_m = -1;
    m->rate_mps = 0.0;
    m->maneuver_start_ms = now_ms;
}

static bool projection_owns(const hn_motion *m)
{
    return m->owner == SRC_CP || m->owner == SRC_AA;
}

void hn_motion_init(hn_motion *m)
{
    m->have_turn = false;
    m->have_dist = false;
    m->last_sample_m = -1;
    m->last_sample_ms = 0;
    m->sent_m = -1;
    m->rate_mps = 0.0;
    m->maneuver_start_ms = 0;
    m->dest_m = -1;
    m->dest_ms = 0;
    m->last_input_ms = 0;
    m->cleared = true;   /* nothing shown yet */
    m->navstate = HN_NAV_INACTIVE;
    m->owner = SRC_NONE;
    m->turn.src = SRC_NONE;
    m->turn.man = HN_MAN_UNKNOWN;
    m->turn.exit_angle = -1;
    m->turn.raw_type = 0;
    m->turn.raw_side = 0;
}

void hn_motion_on_heartbeat(hn_motion *m, int64_t now_ms)
{
    m->last_input_ms = now_ms;
}

static bool same_maneuver(const hn_turn *a, const hn_turn *b)
{
    return a->src == b->src && a->man == b->man &&
           a->raw_type == b->raw_type && a->exit_angle == b->exit_angle;
}

void hn_motion_on_turn(hn_motion *m, const hn_turn *t, int64_t now_ms)
{
    if (!projection_owns(m) || t->src != m->owner)
        return;
    m->last_input_ms = now_ms;
    if (!m->have_turn || !same_maneuver(&m->turn, t)) {
        m->turn = *t;
        m->have_turn = true;
        reset_motion(m, now_ms);   /* new maneuver cannot borrow old distance */
    }
}

void hn_motion_on_dist(hn_motion *m, const hn_dist *d, int64_t now_ms)
{
    m->last_input_ms = now_ms;
    if (!m->have_turn || d->meters < 0) return;

    if (!m->have_dist) {
        m->have_dist = true;
        m->last_sample_m = d->meters;
        m->last_sample_ms = now_ms;
        m->rate_mps = 0.0;
        return;
    }
    int64_t dt = now_ms - m->last_sample_ms;
    int jump = d->meters - m->last_sample_m;
    if (jump >= 50 || jump <= -50) {
        /* reroute / reverse / big correction: reset the glide base. */
        m->last_sample_m = d->meters;
        m->last_sample_ms = now_ms;
        m->rate_mps = 0.0;
        return;
    }
    if (dt > 0) {
        double inst = (double)(m->last_sample_m - d->meters) * 1000.0 / (double)dt;
        if (inst < 0.0) inst = 0.0;          /* increasing/stationary: no closing */
        if (inst > 60.0) inst = 60.0;        /* implausible */
        m->rate_mps = 0.5 * m->rate_mps + 0.5 * inst;
    }
    m->last_sample_m = d->meters;
    m->last_sample_ms = now_ms;
}

void hn_motion_on_dest(hn_motion *m, int meters, int64_t now_ms)
{
    m->last_input_ms = now_ms;
    m->dest_m = meters;
    m->dest_ms = now_ms;
}

void hn_motion_on_state(hn_motion *m, hn_navstate s, int64_t now_ms)
{
    m->last_input_ms = now_ms;
    if (s != m->navstate) {
        m->navstate = s;
        if (s == HN_NAV_INACTIVE) {
            m->have_turn = false;
            m->dest_m = -1;
            reset_motion(m, now_ms);
        }
    }
}

void hn_motion_on_owner(hn_motion *m, hn_source owner, int64_t now_ms)
{
    if (owner != m->owner) {
        if (projection_owns(m) || owner == SRC_OEM)
            m->navstate = HN_NAV_INACTIVE;
        m->owner = owner;
        m->have_turn = false;
        m->dest_m = -1;
        reset_motion(m, now_ms);
        if (owner == SRC_OEM)
            m->cleared = true;
    }
}

bool hn_motion_tick(hn_motion *m, int64_t now_ms, const hn_config *c,
                    hn_native *out, bool *clear)
{
    *clear = false;
    if (m->owner == SRC_OEM)
        return false;
    bool guiding = m->have_turn && m->navstate == HN_NAV_ACTIVE &&
                   projection_owns(m);

    /* link stale (interceptor heartbeat) -> immediate clear, Numen-style. */
    if (m->last_input_ms > 0 && now_ms - m->last_input_ms > c->stale_ms) {
        if (!m->cleared) { m->cleared = true; *clear = true; }
        return false;
    }
    if (!guiding) {
        if (!m->cleared) { m->cleared = true; *clear = true; }
        return false;
    }
    if (m->navstate == HN_NAV_REROUTE) return false;  /* pass-through, no glide */

    /* stuck-sender lease expiry. */
    if (m->have_dist && now_ms - m->last_sample_ms > c->lease_ms) {
        if (!m->cleared) { m->cleared = true; *clear = true; }
        return false;
    }
    /* passed-maneuver detection: closing >= 12 m/s, passed at
     * last sample + 2*remaining + 10 s, at least 20 s after the sample. */
    if (m->have_dist && m->rate_mps >= 12.0 && m->last_sample_m >= 0) {
        int64_t remain_ms = (int64_t)((double)m->last_sample_m / m->rate_mps * 1000.0);
        int64_t passed_at = m->last_sample_ms + 2 * remain_ms + 10000;
        if (passed_at < m->last_sample_ms + 20000)
            passed_at = m->last_sample_ms + 20000;
        if (now_ms > passed_at) {
            if (!m->cleared) { m->cleared = true; *clear = true; }
            return false;
        }
    }

    /* distance to emit: glide between samples, hold after 1.5 s, never < 0. */
    int display = -1;
    if (m->have_dist) {
        int64_t el = now_ms - m->last_sample_ms;
        if (el <= 1500 && m->rate_mps > 0.0) {
            double p = (double)m->last_sample_m -
                       m->rate_mps * c->glide_gain * (double)el / 1000.0;
            display = (p < 0.0) ? 0 : (int)p;
        } else {
            display = m->last_sample_m;   /* hold */
        }
        if (display < 0) display = 0;
        /* back-step hold: a sample slightly above the sent value holds. */
        if (m->sent_m >= 0 && display > m->sent_m &&
            display - m->sent_m <= c->backstep_hold_m)
            display = m->sent_m;
        m->sent_m = display;
    }

    *out = hn_convert(&m->turn, display, c);
    /* 0x4E8 destination distance: independent of the maneuver, expires on lease. */
    if (m->dest_m >= 0 && m->dest_ms > 0 && now_ms - m->dest_ms > c->lease_ms)
        m->dest_m = -1;
    out->dest_m = m->dest_m;
    m->cleared = false;
    return true;
}
