/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TARGET: Hyundai/Kia Gen5W head unit (AU 260319), run as root by projection-daemon.service.
 *
 * projection_daemon — settings service for the Projection+ home-screen page.
 * Contract: ../design/BACKEND_CONTRACT.md §4. WIP; car-run once on one AU 260319 car (install, tile, status); restore VM-only.
 *
 *   GET  /status   this daemon's own timestamp (so the page can tell "service up" apart from
 *                  "HUD Nav quiet"), the switch files and settings.conf, merged with hudnavd's
 *                  /tmp/hudnav-status.json when that is fresh (< 8 s).
 *   POST /config   flat JSON object; every key and value is validated before anything is
 *                  written (400 and no change otherwise). Switches = create/unlink empty files;
 *                  settings.conf = tmp + fsync + rename. Never starts/stops other services.
 *
 * Loopback only (127.0.0.1). One request per connection, bounded sizes, 2 s socket timeouts.
 * Options: --port N (8092), --kit DIR (/rw_data/hudnav), --status PATH (/tmp/hudnav-status.json),
 *          --guard DIR (/rw_data/hudnav-bootguard).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef PROJECTION_BUILD_ID
#define PROJECTION_BUILD_ID "dev"
#endif
#define APP_VERSION "1.0"
#define MAX_REQ 4096
#define MAX_OUT 2048
#define STALE_MS 8000

static const char *kit = "/rw_data/hudnav";
static const char *status_path = "/tmp/hudnav-status.json";
static const char *guard = "/rw_data/hudnav-bootguard";

/* ---------------------------------------------------------------- helpers */

static long long wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool regular_file(const char *dir, const char *name)
{
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return lstat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool kit_present(void)
{
    struct stat st;
    return lstat(kit, &st) == 0 && S_ISDIR(st.st_mode);
}

static int read_small(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

/* ---------------------------------------------------------------- settings */

static const char *const STEPS[] = {"precise", "balanced", "calm", NULL};
static const char *const UNITS[] = {"car", "metric", "imperial", NULL};

typedef struct { char aa_steps[16], cp_steps[16], aa_units[16], cp_units[16]; } settings;

static bool one_of(const char *v, const char *const *opts)
{
    for (; *opts; opts++)
        if (!strcmp(v, *opts)) return true;
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

/* Same rules as hudnavd (settings.c): unknown keys ignored, invalid values -> default. */
static void settings_load(settings *s)
{
    strcpy(s->aa_steps, "balanced"); strcpy(s->cp_steps, "balanced");
    strcpy(s->aa_units, "car");      strcpy(s->cp_units, "car");
    char p[600], buf[2048];
    snprintf(p, sizeof p, "%s/settings.conf", kit);
    if (read_small(p, buf, sizeof buf) < 0) return;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "aa_steps") && one_of(v, STEPS)) snprintf(s->aa_steps, 16, "%s", v);
        else if (!strcmp(k, "cp_steps") && one_of(v, STEPS)) snprintf(s->cp_steps, 16, "%s", v);
        else if (!strcmp(k, "aa_units") && one_of(v, UNITS)) snprintf(s->aa_units, 16, "%s", v);
        else if (!strcmp(k, "cp_units") && one_of(v, UNITS)) snprintf(s->cp_units, 16, "%s", v);
    }
}

static int settings_save(const settings *s)
{
    int d = open(kit, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return -1;
    int f = openat(d, "settings.conf.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (f < 0) { close(d); return -1; }
    char b[256];
    int n = snprintf(b, sizeof b,
                     "# Projection+ settings (written by projection_daemon)\n"
                     "aa_steps=%s\ncp_steps=%s\naa_units=%s\ncp_units=%s\n",
                     s->aa_steps, s->cp_steps, s->aa_units, s->cp_units);
    bool ok = write(f, b, (size_t)n) == n && fsync(f) == 0;
    close(f);
    if (!ok || renameat(d, "settings.conf.tmp", d, "settings.conf") != 0) {
        unlinkat(d, "settings.conf.tmp", 0);
        close(d);
        return -1;
    }
    fsync(d);
    close(d);
    return 0;
}

static int switch_set(const char *name, bool on)
{
    int d = open(kit, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0) return -1;
    int r = 0;
    if (on) {
        int f = openat(d, name, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (f < 0) r = -1; else { fsync(f); close(f); }
    } else if (unlinkat(d, name, 0) != 0 && errno != ENOENT) {
        r = -1;
    }
    if (r == 0) fsync(d);
    close(d);
    return r;
}

/* ---------------------------------------------------------------- hudnavd status */

/* Tiny extractors for hudnavd's flat, trusted-format status JSON. Values are re-emitted only
 * after validation (enums from a fixed list, ints, bools), never copied through raw. */
static const char *json_find(const char *j, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? p + strlen(pat) : NULL;
}

static bool json_str(const char *j, const char *key, char *out, size_t n)
{
    const char *p = json_find(j, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        char c = *p++;
        out[i++] = (c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
    }
    out[i] = 0;
    return *p == '"';
}

static bool json_ll(const char *j, const char *key, long long *out)
{
    const char *p = json_find(j, key);
    if (!p) return false;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool json_bool(const char *j, const char *key)
{
    const char *p = json_find(j, key);
    return p && !strncmp(p, "true", 4);
}

static const char *const HEALTH[] = {"disabled", "waiting_phone", "not_guiding", "guiding", "error", NULL};
static const char *const SOURCE[] = {"android_auto", "carplay", "none", NULL};

typedef struct {
    bool fresh;
    char health[16], source[16], version[48], last_error[96];
    long long next_turn_m, dest_m;
    bool aa_connected, cp_connected;
} hudnav_status;

static void hudnav_read(hudnav_status *h)
{
    memset(h, 0, sizeof *h);
    h->next_turn_m = h->dest_m = -1;
    char buf[2048];
    long long upd = 0;
    if (read_small(status_path, buf, sizeof buf) < 0) return;
    if (!json_ll(buf, "updated_ms", &upd)) return;
    long long age = wall_ms() - upd;
    if (age < -STALE_MS || age > STALE_MS) return;
    if (!json_str(buf, "health", h->health, sizeof h->health) || !one_of(h->health, HEALTH)) return;
    if (!json_str(buf, "source", h->source, sizeof h->source) || !one_of(h->source, SOURCE))
        strcpy(h->source, "none");
    if (!json_str(buf, "version", h->version, sizeof h->version)) strcpy(h->version, "unknown");
    if (!json_str(buf, "last_error", h->last_error, sizeof h->last_error)) h->last_error[0] = 0;
    json_ll(buf, "next_turn_m", &h->next_turn_m);
    json_ll(buf, "dest_m", &h->dest_m);
    h->aa_connected = json_bool(buf, "aa_connected");
    h->cp_connected = json_bool(buf, "cp_connected");
    h->fresh = true;
}

/* ---------------------------------------------------------------- /status */

static void status_json(char *out, size_t n)
{
    bool present = kit_present();
    bool master = present && regular_file(kit, "enabled");
    bool aa = present && regular_file(kit, "aa-nav-enable");
    bool cp = present && regular_file(kit, "cp-enabled");
    settings s;
    settings_load(&s);
    hudnav_status h;
    hudnav_read(&h);

    const char *health, *source = "none", *err = "";
    if (!present) { health = "disabled"; err = "HUD Nav is not installed"; }
    else if (h.fresh) { health = h.health; source = h.source; err = h.last_error; }
    else if (!master) health = "disabled";
    else { health = "error"; err = "HUD Nav is not running; it starts with the car"; }

    const char *guard_state = regular_file(guard, "rolled_back") ? "rolled_back"
                            : regular_file(guard, "armed") ? "armed" : "off";
    char fails_buf[32], p[600];
    long fails = 0;
    snprintf(p, sizeof p, "%s/fails", guard);
    if (read_small(p, fails_buf, sizeof fails_buf) > 0) fails = strtol(fails_buf, NULL, 10);

    snprintf(out, n,
        "{\"updated_ms\":%lld,\"stale\":%s,\"feature_known\":true,\"supported\":%s,"
        "\"hudnav_enabled\":%s,\"aa_enabled\":%s,\"cp_enabled\":%s,"
        "\"aa_distance_steps\":\"%s\",\"cp_distance_steps\":\"%s\","
        "\"aa_units\":\"%s\",\"cp_units\":\"%s\","
        "\"health\":\"%s\",\"source\":\"%s\",\"source_known\":%s,"
        "\"guidance_active\":%s,\"guidance_known\":%s,"
        "\"next_turn_m\":%lld,\"dest_m\":%lld,\"aa_connected\":%s,\"cp_connected\":%s,"
        "\"last_error\":\"%s\",\"boot_guard\":\"%s\",\"boot_failures\":%ld,"
        "\"version_app\":\"%s\",\"version_daemon\":\"%s\",\"version_hudnav\":\"%s\","
        "\"version_firmware\":\"260319\"}",
        wall_ms(), h.fresh ? "false" : "true", present ? "true" : "false",
        master ? "true" : "false", aa ? "true" : "false", cp ? "true" : "false",
        s.aa_steps, s.cp_steps, s.aa_units, s.cp_units,
        health, source, h.fresh ? "true" : "false",
        h.fresh && !strcmp(h.health, "guiding") ? "true" : "false", h.fresh ? "true" : "false",
        h.fresh ? h.next_turn_m : -1, h.fresh ? h.dest_m : -1,
        h.aa_connected ? "true" : "false", h.cp_connected ? "true" : "false",
        err, guard_state, fails, APP_VERSION, PROJECTION_BUILD_ID,
        h.fresh ? h.version : "unknown");
}

/* ---------------------------------------------------------------- /config */

enum { K_MASTER, K_AA, K_CP, K_AA_STEPS, K_CP_STEPS, K_AA_UNITS, K_CP_UNITS, K_RESTORE, K_COUNT };
static const char *const KEYS[K_COUNT] = {
    "hudnav_enabled", "aa_enabled", "cp_enabled", "aa_distance_steps", "cp_distance_steps",
    "aa_units", "cp_units", "restore_stock"};

typedef struct { bool seen[K_COUNT]; bool b[K_COUNT]; char s[K_COUNT][16]; } config_req;

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Strict flat-object parser: string values for the four setting keys, JSON booleans for the
 * rest. Duplicates, unknown keys, other types or trailing bytes -> error. */
static const char *parse_config(const char *p, config_req *r)
{
    memset(r, 0, sizeof *r);
    p = skip_ws(p);
    if (*p++ != '{') return "expected an object";
    p = skip_ws(p);
    if (*p == '}') { p = skip_ws(p + 1); return *p ? "trailing data" : NULL; }
    for (;;) {
        if (*p++ != '"') return "expected a key";
        char key[32]; size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof key) key[i++] = *p++;
        key[i] = 0;
        if (*p++ != '"') return "bad key";
        int k = -1;
        for (int j = 0; j < K_COUNT; j++) if (!strcmp(key, KEYS[j])) k = j;
        if (k < 0) return "unknown key";
        if (r->seen[k]) return "duplicate key";
        r->seen[k] = true;
        p = skip_ws(p);
        if (*p++ != ':') return "expected ':'";
        p = skip_ws(p);
        bool is_string_key = k >= K_AA_STEPS && k <= K_CP_UNITS;
        if (is_string_key) {
            if (*p++ != '"') return "expected a string";
            i = 0;
            while (*p && *p != '"' && i + 1 < sizeof r->s[k]) r->s[k][i++] = *p++;
            r->s[k][i] = 0;
            if (*p++ != '"') return "bad string";
            const char *const *opts = (k == K_AA_STEPS || k == K_CP_STEPS) ? STEPS : UNITS;
            if (!one_of(r->s[k], opts)) return "invalid value";
        } else if (!strncmp(p, "true", 4)) {
            r->b[k] = true; p += 4;
        } else if (!strncmp(p, "false", 5)) {
            r->b[k] = false; p += 5;
        } else {
            return "expected true or false";
        }
        if (k == K_RESTORE && !r->b[k]) return "restore_stock must be true";
        p = skip_ws(p);
        if (*p == ',') { p = skip_ws(p + 1); continue; }
        if (*p++ != '}') return "expected ',' or '}'";
        p = skip_ws(p);
        return *p ? "trailing data" : NULL;
    }
}

static const char *apply_config(const config_req *r)
{
    if (!kit_present()) return "HUD Nav is not installed";
    if (r->seen[K_AA_STEPS] || r->seen[K_CP_STEPS] || r->seen[K_AA_UNITS] || r->seen[K_CP_UNITS]) {
        settings s;
        settings_load(&s);
        if (r->seen[K_AA_STEPS]) snprintf(s.aa_steps, 16, "%s", r->s[K_AA_STEPS]);
        if (r->seen[K_CP_STEPS]) snprintf(s.cp_steps, 16, "%s", r->s[K_CP_STEPS]);
        if (r->seen[K_AA_UNITS]) snprintf(s.aa_units, 16, "%s", r->s[K_AA_UNITS]);
        if (r->seen[K_CP_UNITS]) snprintf(s.cp_units, 16, "%s", r->s[K_CP_UNITS]);
        if (settings_save(&s) != 0) return "could not save settings";
    }
    /* restore_stock wins over anything else in the same request: everything off. */
    static const char *const files[3] = {"enabled", "aa-nav-enable", "cp-enabled"};
    for (int k = K_MASTER; k <= K_CP; k++) {
        if (!r->seen[k] && !r->seen[K_RESTORE]) continue;
        bool on = r->seen[K_RESTORE] ? false : r->b[k];
        if (switch_set(files[k], on) != 0) return "could not change a switch";
    }
    return NULL;
}

/* ---------------------------------------------------------------- HTTP */

static void reply(int c, int code, const char *body)
{
    const char *why = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                    : code == 413 ? "Payload Too Large" : "Service Unavailable";
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\nContent-Length: %zu\r\n\r\n", code, why, strlen(body));
    if (write(c, head, (size_t)n) < 0) return;
    if (write(c, body, strlen(body)) < 0) return;
}

static void reply_error(int c, int code, const char *msg)
{
    char b[160];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    reply(c, code, b);
}

static void handle(int c)
{
    char req[MAX_REQ + 1];
    size_t used = 0;
    char *body = NULL;
    long clen = 0;
    for (;;) {   /* read headers, then exactly Content-Length body bytes */
        if (used >= MAX_REQ) { reply_error(c, 413, "request too large"); return; }
        ssize_t r = read(c, req + used, MAX_REQ - used);
        if (r <= 0) return;
        used += (size_t)r;
        req[used] = 0;
        if (!body) {
            char *end = strstr(req, "\r\n\r\n");
            if (!end) continue;
            body = end + 4;
            char *cl = strcasestr(req, "\r\nContent-Length:");
            if (cl && cl < end) clen = strtol(cl + 17, NULL, 10);
            if (clen < 0 || clen > MAX_REQ / 2) { reply_error(c, 413, "request too large"); return; }
        }
        if ((long)(req + used - body) >= clen) break;
    }
    body[clen] = 0;
    char method[8], path[64];
    if (sscanf(req, "%7s %63s", method, path) != 2) { reply_error(c, 400, "bad request line"); return; }

    char out[MAX_OUT];
    if (!strcmp(path, "/status")) {
        if (strcmp(method, "GET")) { reply_error(c, 400, "use GET"); return; }
        status_json(out, sizeof out);
        reply(c, 200, out);
        return;
    }
    if (!strcmp(path, "/config")) {
        if (strcmp(method, "POST")) { reply_error(c, 400, "use POST"); return; }
        config_req cr;
        const char *err = parse_config(body, &cr);
        if (err) { reply_error(c, 400, err); return; }
        err = apply_config(&cr);
        if (err) { reply_error(c, 503, err); return; }
        status_json(out, sizeof out);
        reply(c, 200, out);
        return;
    }
    reply_error(c, 404, "not found");
}

int main(int argc, char **argv)
{
    int port = 8092;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kit") && i + 1 < argc) kit = argv[++i];
        else if (!strcmp(argv[i], "--status") && i + 1 < argc) status_path = argv[++i];
        else if (!strcmp(argv[i], "--guard") && i + 1 < argc) guard = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("projection_daemon %s (%s)\n", APP_VERSION, PROJECTION_BUILD_ID); return 0; }
        else { fprintf(stderr, "usage: projection_daemon [--port N] [--kit DIR] [--status PATH] [--guard DIR]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) { perror("bind"); return 1; }
    fprintf(stderr, "projection_daemon %s (%s) on 127.0.0.1:%d kit=%s\n",
            APP_VERSION, PROJECTION_BUILD_ID, port, kit);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle(c);
        close(c);
    }
}
