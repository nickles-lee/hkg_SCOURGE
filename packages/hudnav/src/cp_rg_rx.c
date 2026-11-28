/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_rg_rx.h"
#include "rgd_tlv.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    LINK_HEADER = 9,
    LINK_SYN = 0x80, LINK_ACK = 0x40, LINK_RST = 0x10,
    SYN_FIELDS = 10, SESSION_FIELDS = 3,
    CONTROL_HEADER = 6, MANEUVER_SLOTS = 16
};

typedef struct {
    int index;
    hn_turn turn;
} maneuver_slot;

typedef struct {
    bool supported;
    bool published;
    int state;
    int next_index;
    hn_dist distance;
    int dest_m;
    maneuver_slot maneuvers[MANEUVER_SLOTS];
} route_state;

typedef struct {
    size_t used;
    uint8_t control[UINT16_MAX];
    route_state route;
} session_state;

static uint8_t link_buffer[UINT16_MAX];
static size_t link_used;
static session_state *sessions[UINT8_MAX + 1];
static bool control_sessions[UINT8_MAX + 1];
static bool have_session_table;
static bool have_sequence;
static uint8_t sequence;
static int sock = -1;

static unsigned be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static bool checksum_ok(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += p[i];
    return sum == 0;
}

/* Same Unix socket and line protocol as cp_sock_emit. Keep the stock read
 * path nonblocking even when hudnavd is down or its receive queue is full. */
static void emit(const char *lines)
{
    if (sock == -1) {
        const char *path = getenv("HUDNAV_SOCK");
        if (!path) path = "/tmp/hudnav.sock";
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        if (strlen(path) >= sizeof addr.sun_path) return;
        strcpy(addr.sun_path, path);
        sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (sock < 0) return;
        if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
            close(sock);
            sock = -1;
            return;
        }
    }

    size_t n = strlen(lines);
    if (send(sock, lines, n, MSG_NOSIGNAL) != (ssize_t)n) {
        close(sock);
        sock = -1;
    }
}

static void reset_route(route_state *route)
{
    if (route->published) emit("STATE s=0\n");
    memset(route, 0, sizeof *route);
    route->next_index = -1;
    route->distance.meters = -1;
    route->dest_m = -1;
}

static void drop_fragments(void)
{
    for (unsigned i = 1; i <= UINT8_MAX; i++)
        if (sessions[i]) sessions[i]->used = 0;
}

static void reset_link(void)
{
    for (unsigned i = 1; i <= UINT8_MAX; i++) {
        if (!sessions[i]) continue;
        reset_route(&sessions[i]->route);
        free(sessions[i]);
        sessions[i] = NULL;
    }
    memset(control_sessions, 0, sizeof control_sessions);
    have_session_table = false;
    have_sequence = false;
}

static hn_dist distance_from_update(const hn_rgd_state *update)
{
    static const int units[] = { 1, 2, 0, 4, 3 };  /* km, mi, m, yd, ft */
    static const int tenth_mm_per_unit[] = { 10000000, 16093440, 10000, 9144, 3048 };
    hn_dist dist = { .meters = update->dist_maneuver_m, .display = -1, .unit = 0 };
    int raw_unit = update->dist_display_unit;
    if (raw_unit >= 0 && raw_unit < (int)(sizeof units / sizeof units[0])) {
        dist.unit = units[raw_unit];
        dist.display = update->dist_display_e3;
    } else {
        raw_unit = 2;
    }
    if (dist.display < 0 && dist.meters >= 0) {
        int64_t e3 = (int64_t)dist.meters * 10000000 / tenth_mm_per_unit[raw_unit];
        if (e3 <= INT_MAX) dist.display = (int)e3;
    }
    return dist;
}

static void publish(route_state *route, bool send_distance, bool send_dest)
{
    if (!route->supported) return;
    if (route->state == 5) {
        emit("STATE s=2\n");
        route->published = true;
        return;
    }

    hn_turn turn = { .src = SRC_CP, .man = HN_MAN_ARRIVED, .exit_angle = -1,
                     .raw_type = -1, .turn_number = -1 };
    if (route->state != 2) {
        if (route->state != 1 && route->state != 6) return;
        const maneuver_slot *slot = route->next_index >= 0
            ? &route->maneuvers[route->next_index % MANEUVER_SLOTS] : NULL;
        if (!slot || slot->turn.man == HN_MAN_UNKNOWN || slot->index != route->next_index) {
            if (route->published) emit("STATE s=0\n");
            route->published = false;
            return;
        }
        turn = slot->turn;
    }

    char dist[80] = "", dest[40] = "", lines[240];
    if (send_distance && route->distance.meters >= 0 && route->state != 2)
        snprintf(dist, sizeof dist, "DIST m=%d disp=%d unit=%d\n",
                 route->distance.meters, route->distance.display, route->distance.unit);
    if (send_dest && route->dest_m >= 0)
        snprintf(dest, sizeof dest, "DEST m=%d\n", route->dest_m);
    snprintf(lines, sizeof lines,
             "STATE s=1\nTURN src=cp man=%d exit=%d raw=%d side=%d fourth=%d\n%s%sHB\n",
             turn.man, turn.exit_angle, turn.raw_type, turn.raw_side, turn.turn_number,
             dist, dest);
    emit(lines);
    route->published = true;
}

static void handle_update(route_state *route, const uint8_t *msg, size_t n)
{
    hn_rgd_state update;
    if (!hn_rgd_parse_update(msg, n, &update)) return;

    if ((update.present & (1u << 0x14)) && update.source_supports_rg == 0) {
        reset_route(route);
        return;
    }
    /* Car 2026-11-27 (real iPhone, Apple Maps): 0x5201 never carries TLV 0x14; the update itself
     * proves support. Only an explicit 0x14 = 0 (handled above) disables the route. */
    route->supported = true;
    if ((update.present & (1u << 0x01)) && update.route_state == 0) {
        bool supported = route->supported;
        reset_route(route);
        route->supported = supported;
        return;
    }
    if (!route->supported) return;

    if (update.present & (1u << 0x01)) {
        if (update.route_state < 1 || update.route_state > 6) return;
        if (update.route_state != route->state &&
            update.route_state >= 3 && update.route_state <= 5) {
            reset_route(route);
            route->supported = true;
        }
        route->state = update.route_state;
    }
    /* Car 2026-11-27: while settling a route the iPhone sends 0x5201 with an EMPTY current-maneuver
     * list (TLV 0x0D, length 0) between full updates; that is "no change", not "no maneuver". */
    if ((update.present & (1u << 0x0D)) && update.next_index >= 0 &&
        update.next_index != route->next_index) {
        route->next_index = update.next_index;
        route->distance.meters = -1;
        if (route->published && update.dist_maneuver_m < 0) {
            emit("STATE s=0\n");
            route->published = false;
        }
    }
    if (update.dist_maneuver_m >= 0) route->distance = distance_from_update(&update);
    if (update.dist_dest_m >= 0) route->dest_m = update.dist_dest_m;
    publish(route, update.dist_maneuver_m >= 0, update.dist_dest_m >= 0);
}

static void handle_control(route_state *route, const uint8_t *msg, size_t n)
{
    switch (be16(msg + 4)) {
    case 0x1D00:   /* StartIdentification marks a fresh accessory session. */
        reset_route(route);
        break;
    case RGD_MSG_UPDATE:
        handle_update(route, msg, n);
        break;
    case RGD_MSG_MANEUVER: {
        hn_rgd_maneuver maneuver;
        if (!hn_rgd_parse_maneuver(msg, n, &maneuver) ||
            maneuver.index < 0 || maneuver.index > UINT16_MAX) return;
        maneuver_slot *slot = &route->maneuvers[maneuver.index % MANEUVER_SLOTS];
        slot->turn = hn_rgd_to_turn(&maneuver);
        slot->index = maneuver.index;
        if (route->next_index == maneuver.index) publish(route, true, true);
        break;
    }
    case RGD_MSG_LANES: {
        hn_rgd_lanes lanes;
        /* Lane events carry independent indices. hudnavd has no lane command. */
        (void)hn_rgd_parse_lanes(msg, n, &lanes);
        break;
    }
    default:
        break;
    }
}

static void feed_control(session_state *session, const uint8_t *p, size_t n)
{
    while (n) {
        if (session->used < 2) {
            session->used = *p == 0x40 ? session->used + 1 : 0;
            p++;
            n--;
            continue;
        }

        size_t need = session->used < 4 ? 4 : be16(session->control + 2);
        size_t take = need - session->used;
        if (take > n) take = n;
        memcpy(session->control + session->used, p, take);
        session->used += take;
        p += take;
        n -= take;

        if (session->used < 4) continue;
        size_t size = be16(session->control + 2);
        if (size < CONTROL_HEADER) {
            session->used = 0;
        } else if (session->used == size) {
            handle_control(&session->route, session->control, size);
            session->used = 0;
        }
    }
}

static void handle_packet(const uint8_t *packet, size_t n)
{
    unsigned flags = packet[4], id = packet[7];
    const uint8_t *payload = packet + LINK_HEADER;
    size_t payload_size = n > LINK_HEADER ? n - LINK_HEADER - 1 : 0;

    if (flags & (LINK_SYN | LINK_RST)) {
        reset_link();
        if ((flags & LINK_SYN) && payload_size >= SYN_FIELDS && payload[0] == 1 &&
            (payload_size - SYN_FIELDS) % SESSION_FIELDS == 0) {
            have_session_table = true;
            for (size_t i = SYN_FIELDS; i < payload_size; i += SESSION_FIELDS)
                control_sessions[payload[i]] = payload[i + 1] == 0;
        }
        return;
    }
    if (!payload_size || !id || flags != LINK_ACK) return;

    uint8_t seq = packet[5];
    if (have_sequence) {
        if (seq == sequence) return;  /* immediate retransmission */
        if (seq != (uint8_t)(sequence + 1)) drop_fragments();
    }
    sequence = seq;
    have_sequence = true;

    if (have_session_table && !control_sessions[id]) return;
    session_state *session = sessions[id];
    if (!session) {
        session = calloc(1, sizeof *session);
        if (!session) return;
        reset_route(&session->route);
        session->control[0] = session->control[1] = 0x40;
        sessions[id] = session;
    }
    feed_control(session, payload, payload_size);
}

static void drain_link(void)
{
    static const uint8_t detect[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    size_t off = 0;
    while (link_used - off >= 2) {
        const uint8_t *p = link_buffer + off;
        size_t available = link_used - off;
        if (p[0] == 0xff && p[1] == 0x55) {
            size_t check = available < sizeof detect ? available : sizeof detect;
            if (!memcmp(p, detect, check)) {
                if (check < sizeof detect) break;
                reset_link();
                off += sizeof detect;
                continue;
            }
        }
        if (p[0] != 0xff || p[1] != 0x5a) {
            off++;
            continue;
        }
        if (available < LINK_HEADER) break;
        size_t size = be16(p + 2);
        if (size < LINK_HEADER || size == LINK_HEADER + 1 || !checksum_ok(p, LINK_HEADER)) {
            off++;
            continue;
        }
        if (size > available) break;
        if (!checksum_ok(p + LINK_HEADER, size - LINK_HEADER)) {
            drop_fragments();
            off++;
            continue;
        }

        handle_packet(p, size);
        off += size;
    }
    if (off) {
        link_used -= off;
        memmove(link_buffer, link_buffer + off, link_used);
    }
}

void cp_rx_feed(const unsigned char *buf, long n)
{
    if (!buf || n <= 0) return;
    int saved_errno = errno;
    while (n > 0) {
        size_t take = sizeof link_buffer - link_used;
        if (take > (size_t)n) take = (size_t)n;
        memcpy(link_buffer + link_used, buf, take);
        link_used += take;
        buf += take;
        n -= (long)take;
        drain_link();
    }
    errno = saved_errno;
}

/* Whole iAP2 control message (`40 40 len id ...`) already assembled by the stock stack (the dispatcher
 * handleReceivedData 0x13afa of iAP2Service-2.0). Same handling as a message that arrived over the link
 * path, without the link layer. `session` is the link session id the stock code passes. */
void cp_rx_feed_message(int session, const unsigned char *msg, long n)
{
    if (!msg || n < (long)CONTROL_HEADER || msg[0] != 0x40 || msg[1] != 0x40) return;
    size_t size = be16(msg + 2);
    if (size < CONTROL_HEADER || size > (size_t)n) return;
    int saved_errno = errno;
    unsigned id = (unsigned)session & 0xff;
    session_state *st = sessions[id];
    if (!st) {
        st = calloc(1, sizeof *st);
        if (!st) { errno = saved_errno; return; }
        reset_route(&st->route);
        st->control[0] = st->control[1] = 0x40;
        sessions[id] = st;
    }
    handle_control(&st->route, msg, size);
    errno = saved_errno;
}
