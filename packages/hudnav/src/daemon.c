/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * hudnavd — forwarder daemon (PROTOTYPE, host/offline only).
 * Reads a line protocol from a unix socket (fed by the projection interceptor),
 * runs the conversion + motion state machine, emits via a pluggable backend.
 *
 * Line protocol (one event per line, key=value, whitespace separated):
 *   HB                                  interceptor heartbeat (link health)
 *   TURN src=cp|aa man=<hn_maneuver> [exit=<deg>] [raw=<n>] [side=<n>] [fourth=<n>]
 *   DIST m=<meters> [disp=<n>] [unit=<0..4>]
 *   DEST m=<meters>                     distance to destination (CarPlay TLV 0x07)
 *   STATE s=0|1|2                       inactive|active|reroute
 *   OWNER src=cp|aa|oem|none            log-only replay fixture
 *
 * The interceptor normalizes source enums via hn_normalize_* (or sends man=
 * directly). Kill switch: daemon emits nothing unless --enabled file exists.
 *
 * Projection+ (2026-11-27, offline-tested only; contract:
 * packages/projection/design/BACKEND_CONTRACT.md):
 *   --kit DIR      once per second: DIR/enabled (master), DIR/aa-nav-enable, DIR/cp-enabled
 *                  (per source) gate output at runtime, and DIR/settings.conf (per-source distance
 *                  steps/units) is re-read when it changes. Without --kit nothing changes.
 *   --status PATH  JSON status for the app (tmpfs only; never personal data), atomic rename,
 *                  on change and at least every 2 s.
 */
#include "hudnav.h"
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>

#define HUDNAV_VERSION "2026-11-28+exits1"

/* ---- Projection+ runtime switches and settings ---- */
typedef struct {
    const char *dir;            /* NULL: no kit gating (legacy behaviour) */
    bool master, aa, cp;
    hn_settings settings;
    struct stat set_st; bool have_set_st;
    int64_t last_check_ms;
} kit_state;

static bool kit_file(const char *dir, const char *name)
{
    char path[512]; struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

static void kit_poll(kit_state *k, int64_t now, bool force)
{
    if (!k->dir) return;
    if (!force && now - k->last_check_ms < 1000) return;
    k->last_check_ms = now;
    k->master = kit_file(k->dir, "enabled");
    k->aa = kit_file(k->dir, "aa-nav-enable");
    k->cp = kit_file(k->dir, "cp-enabled");
    char path[512]; struct stat st;
    snprintf(path, sizeof path, "%s/settings.conf", k->dir);
    if (stat(path, &st) != 0) {
        if (k->have_set_st || force) hn_settings_default(&k->settings);
        k->have_set_st = false;
        return;
    }
    if (force || !k->have_set_st || st.st_mtime != k->set_st.st_mtime ||
        st.st_size != k->set_st.st_size || st.st_ino != k->set_st.st_ino) {
        hn_settings_load(&k->settings, path);
        k->set_st = st; k->have_set_st = true;
        fprintf(stderr, "hudnavd: settings aa=%s/%s cp=%s/%s\n",
                hn_steps_name(k->settings.aa_steps), hn_units_name(k->settings.aa_units),
                hn_steps_name(k->settings.cp_steps), hn_units_name(k->settings.cp_units));
    }
}

static bool kit_source_ok(const kit_state *k, hn_source src)
{
    if (!k->dir) return true;
    if (!k->master) return false;
    if (src == SRC_AA) return k->aa;
    if (src == SRC_CP) return k->cp;
    return true;
}

/* ---- Projection+ status file ---- */
typedef struct {
    const char *path;
    char last[512];
    int64_t last_write_ms;
} status_state;

static int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *src_name(hn_source s)
{
    return s == SRC_AA ? "android_auto" : s == SRC_CP ? "carplay" : "none";
}

/* body = everything except updated_ms, so "changed" ignores the clock.
 * aa_connected / cp_connected = that projection currently owns guidance (AppManager signal);
 * the head unit gives hudnavd no other phone-connection signal. */
static void status_body(char *out, size_t n, const kit_state *k, const hn_motion *m,
                        bool guiding, bool monitor_ok)
{
    const char *health, *err = "";
    hn_source src = guiding && m->have_turn ? m->turn.src :
                    (m->owner == SRC_AA || m->owner == SRC_CP) ? m->owner : SRC_NONE;
    if (k->dir && !k->master) health = "disabled";
    else if (!monitor_ok) { health = "error"; err = "AppManager monitor disconnected"; }
    else if (guiding) health = "guiding";
    else if (src != SRC_NONE) health = "not_guiding";
    else health = "waiting_phone";
    snprintf(out, n,
             "\"v\":1,\"daemon\":\"hudnavd\",\"version\":\"%s\",\"health\":\"%s\","
             "\"source\":\"%s\",\"next_turn_m\":%d,\"dest_m\":%d,"
             "\"aa_connected\":%s,\"cp_connected\":%s,\"last_error\":\"%s\"",
             HUDNAV_VERSION, health, src_name(src),
             guiding && m->have_dist ? m->sent_m : -1,
             guiding && m->dest_m >= 0 ? m->dest_m : -1,
             m->owner == SRC_AA ? "true" : "false",
             m->owner == SRC_CP ? "true" : "false", err);
}

static void status_write(status_state *st, const char *body, int64_t now)
{
    if (!st->path) return;
    if (!strcmp(body, st->last) && now - st->last_write_ms < 2000) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", st->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    char buf[640];
    int len = snprintf(buf, sizeof buf, "{\"updated_ms\":%lld,%s}\n", (long long)wall_ms(), body);
    bool ok = len > 0 && (size_t)len < sizeof buf && write(fd, buf, (size_t)len) == len;
    close(fd);
    if (ok && rename(tmp, st->path) == 0) {
        snprintf(st->last, sizeof st->last, "%s", body);
        st->last_write_ms = now;
    } else {
        unlink(tmp);
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static hn_source parse_src(const char *s)
{
    if (!strcmp(s, "cp")) return SRC_CP;
    if (!strcmp(s, "aa")) return SRC_AA;
    if (!strcmp(s, "oem")) return SRC_OEM;
    return SRC_NONE;
}

/* minimal key=value lookup within a line buffer */
static bool kv(const char *line, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ' ') && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\n' && i + 1 < outsz) out[i++] = *p++;
            out[i] = 0;
            return true;
        }
        p += klen;
    }
    return false;
}

static void handle_line(hn_motion *m, const char *line, int64_t now, bool fixture)
{
    char v[64];
#ifdef HUDNAV_DIAGNOSTIC
    if (!strncmp(line, "TURN ", 5) || !strncmp(line, "DIST ", 5) || !strncmp(line, "STATE ", 6))
        fprintf(stderr, "RX %s\n", line);
#endif
    if (!strncmp(line, "HB", 2)) { hn_motion_on_heartbeat(m, now); return; }
    if (!strncmp(line, "TURN", 4)) {
        hn_turn t; memset(&t, 0, sizeof t);
        t.exit_angle = -1;
        t.turn_number = -1;
        if (kv(line, "src", v, sizeof v)) t.src = parse_src(v);
        if (kv(line, "man", v, sizeof v)) t.man = (hn_maneuver)atoi(v);
        if (kv(line, "exit", v, sizeof v)) t.exit_angle = atoi(v);
        if (kv(line, "raw", v, sizeof v)) t.raw_type = atoi(v);
        if (kv(line, "side", v, sizeof v)) t.raw_side = atoi(v);
        if (kv(line, "fourth", v, sizeof v)) t.turn_number = atoi(v);
        hn_motion_on_turn(m, &t, now);
        return;
    }
    if (!strncmp(line, "DIST", 4)) {
        hn_dist d; d.meters = -1; d.display = -1; d.unit = 0;
        if (kv(line, "m", v, sizeof v)) d.meters = atoi(v);
        if (kv(line, "disp", v, sizeof v)) d.display = atoi(v);
        if (kv(line, "unit", v, sizeof v)) d.unit = atoi(v);
        hn_motion_on_dist(m, &d, now);
        return;
    }
    if (!strncmp(line, "DEST", 4)) {
        if (kv(line, "m", v, sizeof v))
            hn_motion_on_dest(m, atoi(v), now);
        return;
    }
    if (!strncmp(line, "STATE", 5)) {
        if (kv(line, "s", v, sizeof v))
            hn_motion_on_state(m, (hn_navstate)atoi(v), now);
        return;
    }
    if (fixture && !strncmp(line, "OWNER", 5)) {
        if (kv(line, "src", v, sizeof v))
            hn_motion_on_owner(m, parse_src(v), now);
        return;
    }
}

typedef struct {
    char buf[4096];
    size_t used;
    bool owner_signal;
    bool service_signal;
} input_buffer;

static bool signal_header(const char *line, const char *header)
{
    const char *match = strstr(line, header);
    return !strncmp(line, "signal ", 7) && match && match[strlen(header)] == '\0';
}

static void handle_appmanager_line(hn_motion *m, input_buffer *input,
                                   const char *line, int64_t now, bool *guidance)
{
    if (input->owner_signal && !strncmp(line, "   int32 ", 9)) {
        const char *value = line + 9;
        char *end;
        long id = strtol(value, &end, 10);
        if (end != value && !*end) {
            hn_source owner = SRC_OEM;
            switch (id) {
            case 0: owner = SRC_NONE; break;
            case 110: owner = SRC_CP; break;
            case 111: owner = SRC_AA; break;
            }
#ifdef HUDNAV_DIAGNOSTIC
            fprintf(stderr, "APPMANAGER owner=%ld\n", id);
#endif
            hn_motion_on_owner(m, owner, now);
            if (owner == SRC_OEM)
                *guidance = false;
        }
    } else if (input->service_signal && !strcmp(line, "   string \"com.lge.hyundai.avn.appmanager\"")) {
        hn_motion_on_owner(m, SRC_OEM, now);  /* app manager restarted or disappeared */
        *guidance = false;
    }
    input->owner_signal = signal_header(line, "path=/Information; interface=com.lge.avn.appmanager.info; "
                                             "member=navi_route_guide_application");
    input->service_signal = signal_header(line, "path=/org/freedesktop/DBus; interface=org.freedesktop.DBus; "
                                               "member=NameOwnerChanged");
}

static bool read_input(int fd, input_buffer *input, hn_motion *m, int64_t now,
                       bool appmanager, bool fixture, bool *guidance)
{
    ssize_t count = read(fd, input->buf + input->used, sizeof input->buf - input->used);
    if (count <= 0)
        return count < 0 && errno == EINTR;
    input->used += (size_t)count;
    char *start = input->buf;
    char *end = input->buf + input->used;
    for (;;) {
        char *newline = memchr(start, '\n', (size_t)(end - start));
        if (!newline)
            break;
        *newline = '\0';
        if (appmanager)
            handle_appmanager_line(m, input, start, now, guidance);
        else
            handle_line(m, start, now, fixture);
        start = newline + 1;
    }
    input->used = (size_t)(end - start);
    memmove(input->buf, start, input->used);
    return input->used < sizeof input->buf;
}

/* T19: tests/test_core.c includes this file with HUDNAV_NO_MAIN so it can
 * exercise handle_line() (the real TURN-line parser) without an output backend. */
#ifndef HUDNAV_NO_MAIN
int main(int argc, char **argv)
{
    const char *sockpath = "/tmp/hudnav.sock";
    const char *enabled = NULL;
    bool appmanager = false;
    kit_state kit; memset(&kit, 0, sizeof kit); hn_settings_default(&kit.settings);
    kit.master = kit.aa = kit.cp = true;
    status_state status; memset(&status, 0, sizeof status);
#ifndef HUDNAV_DIAGNOSTIC
    bool use_can = false;
    const char *send_path = NULL;
#endif
    hn_config cfg; hn_config_default(&cfg);
    for (int i = 1; i < argc; i++) {
        /* Must exit: this is the read-only smoke check used by the car kit. */
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr, "usage: hudnavd [--socket PATH] [--enabled PATH] "
                            "[--lht|--rht] [--rbt-angle-mirror] [--appmanager-stdin] [--can] "
                            "[--micom-send PATH] [--kit DIR] [--status PATH]\n"
                            "  --rbt-angle-mirror: mirror F2 roundabout exit angle "
                            "(default off; UNVERIFIED, see T14)\n");
            return 0;
        }
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) sockpath = argv[++i];
        else if (!strcmp(argv[i], "--enabled") && i + 1 < argc) enabled = argv[++i];
        else if (!strcmp(argv[i], "--kit") && i + 1 < argc) kit.dir = argv[++i];
        else if (!strcmp(argv[i], "--status") && i + 1 < argc) status.path = argv[++i];
        else if (!strcmp(argv[i], "--rht")) cfg.lht = false;
        else if (!strcmp(argv[i], "--lht")) cfg.lht = true;
        else if (!strcmp(argv[i], "--rbt-angle-mirror")) cfg.rbt_angle_mirror = true;
        else if (!strcmp(argv[i], "--appmanager-stdin")) appmanager = true;
#ifndef HUDNAV_DIAGNOSTIC
        else if (!strcmp(argv[i], "--can")) use_can = true;
        else if (!strcmp(argv[i], "--micom-send") && i + 1 < argc) send_path = argv[++i];
#else
        else { fprintf(stderr, "hudnavd: unsupported diagnostic option: %s\n", argv[i]); return 1; }
#endif
    }
#ifndef HUDNAV_DIAGNOSTIC
    if (send_path && !appmanager) {
        fprintf(stderr, "hudnavd: --micom-send requires --appmanager-stdin\n");
        return 1;
    }
#else
    if (!appmanager) {
        fprintf(stderr, "hudnavd: diagnostic build requires --appmanager-stdin\n");
        return 1;
    }
#endif

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr; memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof addr.sun_path - 1);
    unlink(sockpath);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(ls, 1) < 0) { perror("listen"); return 1; }

    hn_motion m; hn_motion_init(&m);
#ifndef HUDNAV_DIAGNOSTIC
    hn_output out = use_can ? hn_output_can(stderr, send_path) : hn_output_log(stderr);
    fprintf(stderr, "hudnavd: prototype, socket=%s lht=%d backend=%s (NOT car-validated)\n",
            sockpath, cfg.lht, use_can ? "can" : "log");
#else
    hn_output out = hn_output_log(stderr);
    fprintf(stderr, "hudnavd: AA diagnostic, socket=%s lht=%d backend=log\n", sockpath, cfg.lht);
#endif

    int conn = -1;
    input_buffer projection_input = {0}, appmanager_input = {0};
    int owner_fd = appmanager ? STDIN_FILENO : -1;
    int64_t last_tick = now_ms();
    bool guidance_emitted = false;
    bool monitor_ok = true;
    int exit_status = 0;
    kit_poll(&kit, now_ms(), true);

    for (;;) {
        struct pollfd pfd[] = {{ls, POLLIN, 0}, {owner_fd, POLLIN, 0}, {conn, POLLIN, 0}};
        int64_t now = now_ms();
        int wait = (int)(250 - (now - last_tick));
        if (wait < 0) wait = 0;
        int pr = poll(pfd, sizeof pfd / sizeof pfd[0], wait);
        if (pr < 0 && errno != EINTR) { perror("poll"); break; }

        now = now_ms();
        if (owner_fd >= 0 && pfd[1].revents) {
            if (!read_input(owner_fd, &appmanager_input, &m, now, true, false, &guidance_emitted) ||
                (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                owner_fd = -1;
                monitor_ok = false;
                hn_motion_on_owner(&m, SRC_OEM, now);  /* unknown owner: inhibit all output */
                guidance_emitted = false;
#ifdef HUDNAV_DIAGNOSTIC
                fprintf(stderr, "hudnavd: AppManager monitor disconnected\n");
                exit_status = 1;
                break;
#endif
            }
        }
        if (conn >= 0 && pfd[2].revents &&
            !read_input(conn, &projection_input, &m, now, false, !appmanager, &guidance_emitted)) {
            close(conn);
            conn = -1;
        }
        /* accept / read interceptor connection */
        if (pfd[0].revents & POLLIN) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                if (conn >= 0) close(conn);
                conn = c;
                projection_input.used = 0;
            }
        }

        /* 4 Hz tick */
        if (now - last_tick >= 250) {
            last_tick = now;
            kit_poll(&kit, now, false);
            if (m.owner == SRC_OEM) {
                guidance_emitted = false;
                char body[512];
                status_body(body, sizeof body, &kit, &m, false, monitor_ok);
                status_write(&status, body, now);
                continue;
            }
            /* Car 2026-11-27: the iPhone sends route guidance only when something changes (none
             * while parked), and the CarPlay receiver has no heartbeat. For CarPlay, an open
             * connection IS the link-health signal; when iAP2Service goes away the socket closes
             * and the normal stale clear follows after stale_ms. */
            if (conn >= 0 && m.have_turn && m.turn.src == SRC_CP) m.last_input_ms = now;
            bool enable_ok = true;
            if (enabled) { struct stat st; enable_ok = (stat(enabled, &st) == 0); }
            hn_native nat; bool clear = false;
            bool emit = hn_motion_tick(&m, now, &cfg, &nat, &clear);
            /* Projection+ runtime switches: master off, or the showing source switched off,
             * takes the normal clear path below and stays quiet. */
            hn_source shown = m.have_turn ? m.turn.src : m.owner;
            if (!kit_source_ok(&kit, shown)) enable_ok = false;
            if (emit) {
                bool cp = m.have_turn && m.turn.src == SRC_CP;
                nat.steps = cp ? kit.settings.cp_steps : kit.settings.aa_steps;
                nat.units = cp ? kit.settings.cp_units : kit.settings.aa_units;
            }
            bool guiding = enable_ok && emit;
            bool was_guiding = guidance_emitted;
            if (guiding != was_guiding) {
                out.emit_guidance(out.ctx, guiding);
                guidance_emitted = guiding;
            }
            if (was_guiding && !guiding) {
                out.emit_clear(out.ctx);
            } else if (guiding) {
                out.emit_turn(out.ctx, &nat);
            }
            char body[512];
            status_body(body, sizeof body, &kit, &m, guiding, monitor_ok);
            status_write(&status, body, now);
        }
    }
    if (conn >= 0) close(conn);
    if (status.path) unlink(status.path);
    close(ls);
    unlink(sockpath);
    return exit_status;
}
#endif /* !HUDNAV_NO_MAIN */
