/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CAN output backend — cluster frames via micom D-Bus.
 *
 * CONFIRMED wire format (firmware_verification.md §1, AppNavi SendILNaviCluster):
 *   system bus, dest com.lge.car.micom, path /Can, iface com.lge.car.micom.can,
 *   method SetCANMicomRecv(ay), array = [id>>8, id&0xff, 0x08, data0..data7].
 *   Micom opcode 0x0400 on 260319 (matches 260128).
 *
 * CONFIRMED frame set + positions (firmware_verification.md §1-2, prior_work_gaps.md §2.3):
 *   0x4CC turn:      B0 type/state, B1/B2 side street, B3 direction (7.5-deg units),
 *                    B4/B5 distance to maneuver (metres), B6 state subtype, B7 side/magnitude.
 *   0x63E turn list: d0/d1 maneuver flavour, d2/d3 distance (LE metres), d4-d7 secondary.
 *   0x640 bar:       d0-d6 = ff ff ff 00 ff ff ff, d7 = approach bar as decimal percent
 *                    (0x0a=10% .. 0x5a=90%) — Numen-confirmed.
 *   0x4E8 dest:      B0 = (tenths<<4)|unit, B1=0x15, B2=0x1f, B3 = direction
 *                    (7.5-deg units), B4/B5 = integer distance LE, B6=0x50
 *                    (destination flag), B7=0. See docs/hudnav/HUD_NAV_REPORT.md §5.
 *   0x4EA guidance:  d6 = 00 off / 10 on (Numen + I5 logs).
 *   0x4E9 route:     d7 = 0->1 at nav start (I5 logs).
 *
 * CAN-TP text channel (car-proven 2026-11-25, docs/hudnav/HUD_NAV_REPORT.md):
 *   SetCANTPMicomRecv(ay), array = [0x06, 0xE7, len, sid, payload..]  (len excludes the sid)
 *   F2 = TBT list: count, c0, c1, c2 (icon codes, 00 padded), UTF-16LE name, 00 pad.
 *   The HUD draws its turn arrow from the FIRST F2 icon code (0x4CC byte 3 is ignored for the arrow;
 *   0x4CC/0x63E only drive the distance). An empty F2 (03 f2 00 00 00) clears the arrow.
 *   F0 (street) and F4 (destination) are not needed for the HUD; only blanked at clear like stock.
 *
 * The AU drive capture confirms little-endian metre distances and zero turn flag
 * bytes for the observed plain-turn and LHT roundabout types. The remaining list
 * data and other maneuver types still use provisional values.
 */
#include "output.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <libgen.h>

#define ID_TURN 0x4CC
#define ID_LIST 0x63E
#define ID_BAR  0x640
#define ID_DEST 0x4E8
#define ID_GUID 0x4EA
#define ID_ROUTE 0x4E9

#define NSLOT 8
typedef struct {
    uint16_t id; unsigned char d[8]; long long last_ms; bool used;
} slot_t;
typedef struct {
    FILE *f;
    const char *send_path;   /* NULL = log only (host/bench); else whitelisted helper */
    char tp_path[512];       /* sibling "micom-send-tp" wrapper; empty = TP is log only */
    int last_icon;           /* last F2 icon sent (0 = none/cleared) */
    long long last_f2_ms;
    slot_t slots[NSLOT];
} can_ctx;

#define F2_REFRESH_MS 10000   /* re-send an unchanged F2 list this often (stock sends it once) */

#define RESEND_MS 1000   /* unchanged frames are refreshed at 1 Hz, not 4 Hz */

static long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Hard whitelist: this module may only ever transmit these IDs. */
static bool id_allowed(uint16_t id)
{
    return id == ID_TURN || id == ID_LIST || id == ID_BAR ||
           id == ID_DEST || id == ID_GUID || id == ID_ROUTE;
}

/* Fire the real frame through the vendor-whitelisted helper (basename
 * CANService; see 01_SCOURGE_REPO packages/battery/docs/MICOM_WHITELIST.md).
 * Same mechanism as the car-proven battery daemon: execl(helper, "CANService", arr). */
static void send_real(can_ctx *c, uint16_t id, const unsigned char d[8])
{
    if (!c->send_path || !id_allowed(id)) return;
    long long now = mono_ms();
    slot_t *s = NULL;
    for (int i = 0; i < NSLOT; i++) {
        if (c->slots[i].used && c->slots[i].id == id) { s = &c->slots[i]; break; }
        if (!c->slots[i].used && !s) s = &c->slots[i];
    }
    if (!s) return;
    if (s->used && !memcmp(s->d, d, 8) && now - s->last_ms < RESEND_MS) return;
    s->used = true; s->id = id; memcpy(s->d, d, 8); s->last_ms = now;

    char arg[96];
    snprintf(arg, sizeof arg,
             "array:byte:0x%02x,0x%02x,0x08,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
             id >> 8, id & 0xff, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    pid_t pid = fork();
    if (pid == 0) { execl(c->send_path, "CANService", arg, (char *)NULL); _exit(127); }
    if (pid < 0) fprintf(c->f, "hudnavd: fork failed for 0x%03x\n", id);
}

/* CAN-TP (ISO-TP multi-frame) messages, cluster text channel 0x6E7 only, SIDs F0/F2/F4 only.
 * `n` messages are sent in order by ONE helper process (so they cannot reorder), ~0.15 s apart.
 * The helper is a separate wrapper so an old micom-send can never send a TP array as a CAN frame. */
#define TP_MAX 3
static bool tp_allowed(const unsigned char *m, int n)
{
    if (n < 4 || n > 4 + 0x7f) return false;
    if (m[0] != 0x06 || m[1] != 0xE7) return false;
    if (m[3] != 0xF0 && m[3] != 0xF2 && m[3] != 0xF4) return false;
    return n == 4 + m[2];          /* len excludes the sid */
}

static void emit_tp(can_ctx *c, const unsigned char *const msgs[], const int lens[], int count)
{
    char args[TP_MAX][700];
    int na = 0;
    for (int k = 0; k < count && na < TP_MAX; k++) {
        if (!tp_allowed(msgs[k], lens[k])) { fprintf(c->f, "hudnavd: TP message refused\n"); continue; }
        char *a = args[na]; size_t o = 0;
        o += (size_t)snprintf(a + o, sizeof args[0] - o, "array:byte:");
        for (int i = 0; i < lens[k]; i++)
            o += (size_t)snprintf(a + o, sizeof args[0] - o, "%s0x%02x", i ? "," : "", msgs[k][i]);
        fprintf(c->f, "MICOMTP SetCANTPMicomRecv %s\n", a + 11);
        na++;
    }
    if (na == 0 || !c->tp_path[0]) return;
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[TP_MAX + 2]; argv[0] = "CANService";
        for (int i = 0; i < na; i++) argv[i + 1] = args[i];
        argv[na + 1] = NULL;
        execv(c->tp_path, argv); _exit(127);
    }
    if (pid < 0) fprintf(c->f, "hudnavd: fork failed for TP\n");
}

static void tp_clear_all(can_ctx *c)
{
    static const unsigned char f0[] = {0x06, 0xE7, 0x03, 0xF0, 0, 0, 0};
    static const unsigned char f2[] = {0x06, 0xE7, 0x03, 0xF2, 0, 0, 0};
    static const unsigned char f4[] = {0x06, 0xE7, 0x03, 0xF4, 0, 0, 0};
    const unsigned char *m[3] = { f4, f2, f0 };
    int l[3] = { 7, 7, 7 };
    emit_tp(c, m, l, 3);
    c->last_icon = 0;
}

/* F2 TBT list with one icon code, no lane/street data (the proven minimum: count 01, code, 00 00). */
static void tp_f2_icon(can_ctx *c, int icon, bool send_empty_first)
{
    static const unsigned char empty[] = {0x06, 0xE7, 0x03, 0xF2, 0, 0, 0};
    unsigned char list[12] = {0x06, 0xE7, 0x08, 0xF2, 0x01, 0, 0, 0, 0, 0, 0, 0};
    list[5] = (unsigned char)icon;
    const unsigned char *m[2]; int l[2], n = 0;
    if (send_empty_first) { m[n] = empty; l[n++] = 7; }
    m[n] = list; l[n++] = 12;
    emit_tp(c, m, l, n);
    c->last_icon = icon;
    c->last_f2_ms = mono_ms();
}

static void emit_frame(can_ctx *c, uint16_t id, const unsigned char d[8], const char *note)
{
    fprintf(c->f, "MICOM SetCANMicomRecv array:byte:0x%02x,0x%02x,0x08,"
                  "0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
            id >> 8, id & 0xff, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    if (note) fprintf(c->f, "  %s", note);
    fprintf(c->f, "\n");
    send_real(c, id, d);
}

static void emit_raw(can_ctx *c, uint16_t id, const char *bytes, const char *note)
{
    fprintf(c->f, "MICOM SetCANMicomRecv array:byte:0x%02x,0x%02x,0x08,%s  %s\n",
            id >> 8, id & 0xff, bytes, note);
    unsigned char d[8]; const char *p = bytes;
    for (int i = 0; i < 8 && p; i++) {
        d[i] = (unsigned char)strtoul(p, NULL, 16);
        p = strchr(p, ','); if (p) p++;
    }
    send_real(c, id, d);
}

/* 0x4E8 destination distance. Layout from the 2026-11-24 AU drive log
 * (docs/hudnav/HUD_NAV_REPORT.md §5): tenths|unit nibbles in B0, integer
 * distance LE in B4/B5, destination flag 0 in B6 low nibble. Metric car: km
 * with one decimal at >= 1 km, metres below. dist_m < 0 sends the stock
 * "hidden" form. B2/B3 are stock-observed but their exact meaning is PROVISIONAL. */
static void emit_dest(can_ctx *c, int dist_m, int dir_deg)
{
    unsigned char d[8] = { 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00 };
    bool show = dist_m >= 0;
    if (show) {
        int unit = 0, whole = dist_m, tenths = 0;
        if (dist_m >= 1000) {
            unit = 1;                       /* km */
            whole = dist_m / 1000;
            tenths = (dist_m % 1000) / 100;
        }
        if (unit != 0 && whole > 99) {
            show = false;                   /* stock hides >99 whole km/mi */
        } else {
            d[0] = (unsigned char)(((tenths & 0xf) << 4) | (unit & 0xf));
            d[1] = 0x15;
            d[2] = 0x1f;
            d[3] = (unsigned char)(((dir_deg % 360) * 2 / 15) & 0xff);
            d[4] = (unsigned char)(whole & 0xff);
            d[5] = (unsigned char)((whole >> 8) & 0xff);
            d[6] = 0x50;                    /* flag nibble 0 = destination */
            d[7] = 0x00;
        }
    }
    emit_frame(c, ID_DEST, d, show ? "(0x4E8 destination)" : "(0x4E8 hidden)");
}

static bool can_turn(void *ctx, const hn_native *n)
{
    can_ctx *c = ctx;
    unsigned char t[8], l[8], b[8];
    memset(t, 0, sizeof t); memset(l, 0, sizeof l);
    memset(b, 0xff, sizeof b); b[3] = 0x00;  /* 0x640 idle pattern d0..d6 */
    int type = (n->type == HN_TYPE_ICON) ? n->icon : (int)n->type;
    int dist = n->dist_m < 0 ? 0 : n->dist_m;
    if (dist > 0xffff) dist = 0xffff;
    uint16_t side = n->side_street;

    /* 0x4CC: type, side street, direction in 7.5-degree units, distance,
     * then the last two bytes as observed in the AU drive capture. */
    bool captured_type = n->type == HN_TYPE_TURN || n->type == HN_TYPE_ROUND_L;
    t[0] = (unsigned char)(type & 0xff);
    t[1] = (unsigned char)(side >> 8);
    t[2] = (unsigned char)(side & 0xff);
    t[3] = (unsigned char)(((n->angle % 360) * 2 / 15) & 0xff);
    t[4] = dist & 0xff; t[5] = dist >> 8;
    /* Car 2026-11-26: B6/B7 = 02 20 made the HUD show the distance in FEET. Every stock 0x4CC
     * maneuver frame in the 2026-11-25 AU capture (types 0d/70/08/21/02/43) has B6/B7 = 00 00
     * (only the 09/03 clear frames differ), so send 00 00 for all maneuver types. */
    (void)captured_type;
    t[6] = 0x00;
    t[7] = 0x00;

    /* The two matched stock list frames have a zero prefix. B4..B7 are the
     * route leg lengths (current->next, next->next); projections do not give us
     * that lookahead, and stock uses ff ff for "no further maneuver" (AU drive
     * log, docs/hudnav/HUD_NAV_REPORT.md §3). Send that rather than a
     * misleading 0 m leg. */
    /* Car 2026-11-26: l[0] = 0x03 made the HUD show the distance in FEET (HUD stayed in ft after
     * fixing 0x4CC B6/B7). Every stock 0x63E frame starts 00 (AU capture + the replayed left block
     * that showed "200 m"), so B0 is a unit field: 0 = metres. Always send 0. */
    /* Drive 2026-11-27: nothing showed on the HUD until the turn was < 1 km away. Stock (and
     * 0x4E8) encode B0 = (tenths << 4) | unit with unit 0 m / 1 km, so a metre count >= 1000 is
     * most likely outside what the HUD will draw. UNVERIFIED on the car: from 1 km send km with
     * one decimal. Rounding (owner request): 1 m under 100 m, 10 m to 999 m, 0.1 km above. */
    /* Projection+ (2026-11-27): rounding/units per source from settings.conf; the defaults
     * (balanced, car units) are byte-identical to the car-proven build: 1 m under 100 m, 10 m to
     * 990 m, then km with one decimal in 0x63E B0 = (tenths << 4) | unit. See settings.c. */
    hn_distance rd = hn_round_distance(dist, n->steps, n->units);
    t[4] = rd.turn_m & 0xff; t[5] = (rd.turn_m >> 8) & 0xff;
    l[0] = rd.list_b0;
    l[2] = rd.list_val & 0xff; l[3] = (rd.list_val >> 8) & 0xff;
    l[4] = 0xff; l[5] = 0xff; l[6] = 0xff; l[7] = 0xff;

    /* 0x640: d7 bar as decimal percent (10..90 = 0x0a..0x5a). */
    b[7] = (unsigned char)(n->bar > 0 ? n->bar : 0);

    emit_frame(c, ID_TURN, t, "(0x4CC turn)");
    emit_frame(c, ID_LIST, l, "(0x63E turn-list; trailing fields provisional)");
    emit_frame(c, ID_BAR, b, "(0x640 approach bar)");
    emit_dest(c, n->dest_m, n->angle);

    /* The arrow itself: F2 list, empty first then the icon when the maneuver changes (stock order),
     * and the icon alone as a slow refresh. */
    if (n->f2_icon > 0 && n->f2_icon < 256) {
        long long now = mono_ms();
        if (n->f2_icon != c->last_icon) tp_f2_icon(c, n->f2_icon, true);
        else if (now - c->last_f2_ms >= F2_REFRESH_MS) tp_f2_icon(c, n->f2_icon, false);
    } else if (c->last_icon != 0) {
        tp_clear_all(c);
    }
    return true;
}

static bool can_clear(void *ctx)
{
    can_ctx *c = ctx;
    /* 0x4CC clear = type 03 (03 ff ff ff ff ff ff f0): what AppNavi sends at ROUTE END on the
     * AU 260319 unit (2026-11-25 10:36:11, docs/hudnav/HUD_NAV_REPORT.md).
     * NOT type 09: that is the stock "Calculating" state, sent at route START; sending it as
     * "clear" leaves the HUD showing a spinner (observed on the car 2026-11-25). */
    /* Car 2026-11-27 (CarPlay drive): after type 03 the checkered flag STAYED on the HUD with the car
     * parked, despite the idle compass frames below. A clear is not an arrival, so 03 is no longer
     * sent; the idle compass 0x4CC frame below is now the first 0x4CC of the clear set. */
    emit_raw(c, ID_LIST, "0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff", "(0x63E clear)");
    emit_raw(c, ID_BAR,  "0xff,0xff,0xff,0x00,0xff,0xff,0xff,0x00", "(0x640 clear, bar 0)");
    emit_raw(c, ID_DEST, "0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00", "(0x4E8 clear/hidden)");
    /* Type 03 alone means "arrived": the HUD draws the checkered flag and stays there (observed
     * on the car, 2026-11-25). The stock nav follows it immediately with idle compass frames
     * (08 .. heading), which take the display back to normal. Heading 0x21 is the value used
     * when the HUD was cleared on the car; the stock idle frame carries a compass heading. */
    emit_raw(c, ID_TURN, "0x08,0x00,0x00,0x21,0x00,0x00,0x00,0x00", "(0x4CC idle compass)");
    emit_raw(c, ID_DEST, "0xff,0xff,0xff,0x21,0xff,0xff,0xff,0x00", "(0x4E8 idle compass)");
    /* Blank F4/F2/F0 (stock end-of-route): empty F2 removes the HUD arrow. */
    tp_clear_all(c);
    return true;
}

static bool can_guidance(void *ctx, bool active)
{
    can_ctx *c = ctx;
    unsigned char g[8], r[8];
    memset(g, 0, sizeof g); memset(r, 0, sizeof r);
    g[6] = active ? 0x10 : 0x00;      /* 0x4EA guidance flag (H-confidence) */
    r[7] = active ? 0x01 : 0x00;      /* 0x4E9 route active (M-confidence) */
    emit_frame(c, ID_GUID, g, "(0x4EA guidance flag)");
    emit_frame(c, ID_ROUTE, r, "(0x4E9 route active; other bytes PROVISIONAL)");
    return true;
}

static void can_destroy(void *ctx) { (void)ctx; }

hn_output hn_output_can(FILE *f, const char *send_path)
{
    static can_ctx ctx;
    signal(SIGCHLD, SIG_IGN);        /* helper children are fire-and-forget */
    ctx.send_path = send_path;
    ctx.f = f;
    ctx.tp_path[0] = 0;
    ctx.last_icon = 0; ctx.last_f2_ms = 0;
    if (send_path) {
        /* TP helper = sibling "micom-send-tp" next to the CAN helper; absent = TP is not transmitted. */
        char tmp[400]; snprintf(tmp, sizeof tmp, "%s", send_path);
        char cand[512]; snprintf(cand, sizeof cand, "%s/micom-send-tp", dirname(tmp));
        if (access(cand, X_OK) == 0) snprintf(ctx.tp_path, sizeof ctx.tp_path, "%s", cand);
        else fprintf(f, "hudnavd: no %s: CAN-TP (turn arrow) NOT transmitted\n", cand);
    }
    hn_output o = { &ctx, can_turn, can_clear, can_guidance, can_destroy };
    return o;
}
