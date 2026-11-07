/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/*
 * wifi_daemon — WiFi / CarPlay AP / SSH manager for Ioniq 5 Gen5W (260128)
 *
 * Runs as root on the head unit.  Exposes a tiny HTTP/JSON API on
 * 127.0.0.1:8090 for the QML WiFi page and mirrors state to
 * /rw_data/wifi/status.json.
 *
 * Modes:
 *   off      - wlan0 down, OEM WiFi managers stopped, SSH stopped
 *   client   - ConnMan station mode using imported wifi.txt credentials
 *   carplay  - ConnMan AP/tether + CarPlay IE + CarPlay services
 *
 * It deliberately owns wlan0 against the OEM supplicantmanager/ConnMan
 * race observed in the earlier owner-access field tests.
 *
 * This is research tooling for the owner's own head unit.  It does not
 * flash firmware, touch the updater, bootloader, or partitions.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_DIR        "/rw_data/wifi"
#define CONFIG_PATH    APP_DIR "/config.conf"
#define STATUS_PATH    APP_DIR "/status.json"
#define LOG_PATH       APP_DIR "/daemon.log"
#define WIFI_TXT_COPY  APP_DIR "/wifi.txt"
#define SSH_DIR        APP_DIR "/ssh"
#define SSH_HOME       APP_DIR "/home"
#define SSH_KEYS       SSH_DIR "/authorized_keys"
#define SSH_PID        SSH_DIR "/dropbear.pid"
#define DROPBEAR_BIN   APP_DIR "/bin/dropbear"
#define DROPBEAR_KEY   APP_DIR "/bin/dropbearkey"
#define DROPBEAR_MULTI APP_DIR "/bin/dropbearmulti"
#define VBT_BAK_DIR    APP_DIR "/backup"
#define WPA_CONF       APP_DIR "/wpa_supplicant.conf"
#define WPA_LOG        APP_DIR "/wpa_supplicant.log"
#define WPA_PIDFILE    APP_DIR "/wpa_supplicant.pid"
#define APPLY_RESULT   APP_DIR "/apply.result"
#define BT_STATE       APP_DIR "/bt.state"
#define SSH_BOUND      APP_DIR "/ssh.bound"
#define HOME_STATE     APP_DIR "/home.json"
#define SESSION_ENV    "/tmp/session_appmgr.env"
#define SESSION_ADDR   "/tmp/session_appmgr"
#define WPA_CTRL_DIR   "/var/run/wpa_supplicant"
#define HOME_BUS_FILE  "/tmp/session_appmgr"
#define DBUS_MONITOR   "/usr/bin/dbus-monitor"

#define BCMDHD_KO \
    "/lib/modules/4.1.27-abl/kernel/drivers/net/wireless/bcmdhd/bcmdhd.ko"
#define CFG80211_KO \
    "/lib/modules/4.1.27-abl/kernel/net/wireless/cfg80211.ko"

#define DEFAULT_HTTP_PORT 8090
#define SSH_DEFAULT_PORT  2222
#define SSH_FALLBACK_LAST 2230

#define CAR_PASSWD_LINE \
    "car:x:0:0:Car User:/rw_data/wifi/home:/bin/sh"
#define CAR_SHADOW_LINE \
    "car:$6$ioniq5$/BDNqdHDOkg.wPr3W8PkQx0vbdQWmJBPkU9/.pwGNtL.v/IyXbR20nEEY/CYImn04cI8NlGDKK6A6kPA5noPZ.:19000:0:99999:7:::"

typedef enum { MODE_OFF = 0, MODE_CLIENT, MODE_CARPLAY } wifi_mode_t;

typedef struct {
    wifi_mode_t mode;
    bool wlan_on;

    char ssid[80];
    char psk[160];

    char cp_ssid[80];
    char cp_psk[160];
    int  cp_band;          /* 0 auto, 1 = 2.4 GHz, 2 = 5 GHz */
    int  cp_channel;
    bool cp_hidden;
    int  cp_max_sta;

    char cp_manufacturer[64];
    char cp_model[64];
    char cp_name[64];
    char cp_id[64];
    char cp_btaddr[32];

    bool ssh_enabled;
    int  ssh_port;

    char imported_from[256];
    bool imported;
} config_t;

static config_t g_cfg;
static volatile sig_atomic_t g_stop;
static const char *g_status_path = STATUS_PATH;
static char g_last_msg[256] = "starting";
static int64_t g_last_apply_ms;
static int64_t g_last_watchdog_ms;
static int64_t g_last_status_ms;
static bool g_apply_failed;
static int64_t g_start_ms;
static pid_t g_apply_pid;
static int64_t g_apply_start_ms;
static bool g_in_child;
static int64_t g_last_autodump_ms;
static int64_t g_last_dump_try_ms;
static bool g_boot_dumped;
static int g_ssh_active_port;
static bool g_ssh_port_fallback;
static char g_ssh_bound_ip[64];
static bool g_config_migrated;
static bool g_bt_discoverable;
static bool g_bt_pairable;
static int64_t g_bt_fail_ms;
static char g_pair_msg[256];
static pid_t g_home_pid = -1;
static int g_home_fd = -1;
static int64_t g_home_retry_ms;
static int g_home_seq;
static long g_home_epoch;
static char g_home_source[12] = "none";
static int g_home_last_key = -1;
static bool g_home_monitor;
static char g_home_line[512];
static size_t g_home_line_len;
static char g_home_member[48];

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void str_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    if (!src) src = "";
    snprintf(dst, n, "%s", src);
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
        fclose(f);
    }
    fprintf(stderr, "%s\n", buf);
}

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_msg, sizeof(g_last_msg), fmt, ap);
    va_end(ap);
    log_msg("msg: %s", g_last_msg);
}

/* Run a shell command, discard output.  Returns exit status or -1. */
static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) { /* drain */ }
    int rc = pclose(p);
    if (rc < 0) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Run a shell command and capture stdout+stderr. */
static int sh_out(char *out, size_t outsz, const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    if (out && outsz) out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    size_t used = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        size_t n = strlen(buf);
        if (out && used + 1 < outsz) {
            size_t room = outsz - used - 1;
            if (n > room) n = room;
            memcpy(out + used, buf, n);
            used += n;
            out[used] = 0;
        }
    }
    int rc = pclose(p);
    if (rc < 0) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Single-quote a value for /bin/sh. */
static void shq(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    if (!n) return;
    if (j + 1 < n) dst[j++] = '\'';
    for (const char *p = src ? src : ""; *p && j + 5 < n; p++) {
        if (*p == '\'') {
            memcpy(dst + j, "'\\''", 4);
            j += 4;
        } else {
            dst[j++] = *p;
        }
    }
    if (j + 1 < n) dst[j++] = '\'';
    dst[j] = 0;
}

/* Overwrite every occurrence of a secret in a captured command output.
 * connmanctl's interactive loop may print an unrecognised line verbatim;
 * the passphrase is one of those lines when no RequestInput was needed. */
static void redact_secret(char *buf, const char *secret)
{
    if (!buf || !secret || !*secret) return;
    size_t sl = strlen(secret);
    char *p = buf;
    while ((p = strstr(p, secret)) != NULL) {
        memset(p, '*', sl);
        p += sl;
    }
}

static void json_escape(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    if (!n) return;
    for (const unsigned char *p = (const unsigned char *)(src ? src : "");
         *p && j + 7 < n; p++) {
        switch (*p) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"'; break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n'; break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r'; break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't'; break;
        default:
            if (*p < 0x20) {
                snprintf(dst + j, n - j, "\\u%04x", *p);
                j += 6;
            } else {
                dst[j++] = (char)*p;
            }
            break;
        }
    }
    dst[j] = 0;
}

static bool file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

static int write_file_atomic(const char *path, const char *data, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t len = strlen(data);
    ssize_t w = write(fd, data, len);
    close(fd);
    if (w != (ssize_t)len) {
        unlink(tmp);
        return -1;
    }
    chmod(tmp, mode);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Read the last n-1 bytes of a file (whole file if smaller), trimming a
 * partial first line. */
static int read_tail(const char *path, char *buf, size_t n)
{
    if (!buf || n < 2) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) { close(fd); return -1; }
    size_t len = n - 1;
    bool partial = false;
    if ((off_t)len > sz) len = (size_t)sz;
    else partial = true;
    lseek(fd, sz - (off_t)len, SEEK_SET);
    ssize_t r = read(fd, buf, len);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    if (partial) {
        char *nl = strchr(buf, '\n');
        if (nl) memmove(buf, nl + 1, strlen(nl + 1) + 1);
    }
    return (int)strlen(buf);
}

/* Copy the last non-empty line of a log file into out. */
static void log_tail(const char *path, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[1024];
    size_t len = sizeof(buf) - 1;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) { close(fd); return; }
    if (sz > (off_t)len)
        lseek(fd, sz - (off_t)len, SEEK_SET);
    else {
        len = (size_t)sz;
        lseek(fd, 0, SEEK_SET);
    }
    ssize_t r = read(fd, buf, len);
    close(fd);
    if (r <= 0) return;
    buf[r] = 0;

    char *p = buf + r - 1;
    while (p >= buf && (*p == '\n' || *p == '\r' || *p == ' ')) *p-- = 0;
    char *nl = NULL;
    for (char *q = buf; *q; q++)
        if (*q == '\n') nl = q;
    str_copy(out, n, nl ? nl + 1 : buf);
}

/* Find a process by /proc/<pid>/comm.  Returns first pid or -1. */
static int pid_by_comm(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64], comm[128];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        if (read_file(path, comm, sizeof(comm)) <= 0) continue;
        char *nl = strchr(comm, '\n');
        if (nl) *nl = 0;
        if (!strcmp(comm, name)) {
            int pid = atoi(de->d_name);
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;
}

/* Find a process whose /proc/<pid>/cmdline contains needle. */
static int pid_by_cmdline(const char *needle)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
        char cmd[1024];
        int n = read_file(path, cmd, sizeof(cmd));
        if (n <= 0) continue;
        /* cmdline has NUL separators; replace them for strstr */
        for (int i = 0; i < n; i++)
            if (cmd[i] == 0) cmd[i] = ' ';
        if (strstr(cmd, needle)) {
            int pid = atoi(de->d_name);
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;
}

static void kill_pid(int pid)
{
    if (pid > 1) kill(pid, SIGTERM);
}

static bool iface_present(void)
{
    return file_exists("/sys/class/net/wlan0");
}

static void iface_up(void)
{
    sh("ifconfig wlan0 up 2>/dev/null");
}

static void iface_down(void)
{
    sh("ifconfig wlan0 down 2>/dev/null");
}

/* Get IPv4 address of wlan0 into out. */
static bool wlan_ip(char *out, size_t n)
{
    char buf[2048];
    if (sh_out(buf, sizeof(buf), "ifconfig wlan0 2>/dev/null") != 0)
        return false;

    const char *p = strstr(buf, "inet addr:");
    if (p) {
        p += 10;
    } else {
        p = strstr(buf, "inet ");
        if (!p) return false;
        p += 5;
    }
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\t' && i + 1 < n)
        out[i++] = *p++;
    out[i] = 0;
    return i > 0 && strcmp(out, "127.0.0.1") != 0;
}

static void wlan_link(char *ssid, size_t ssid_n, int *signal)
{
    if (ssid && ssid_n) ssid[0] = 0;
    if (signal) *signal = 0;
    char buf[4096];
    if (sh_out(buf, sizeof(buf), "iw dev wlan0 link 2>/dev/null") != 0)
        return;

    char *line = strtok(buf, "\n");
    while (line) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "SSID:", 5)) {
            p += 5;
            while (*p && isspace((unsigned char)*p)) p++;
            if (ssid) str_copy(ssid, ssid_n, p);
        } else if (!strncmp(p, "signal:", 7)) {
            p += 7;
            while (*p && isspace((unsigned char)*p)) p++;
            if (signal) *signal = atoi(p);
        }
        line = strtok(NULL, "\n");
    }
}

/* Count AP clients. */
static int ap_clients(void)
{
    char buf[8192];
    if (sh_out(buf, sizeof(buf), "iw dev wlan0 station dump 2>/dev/null") != 0)
        return 0;
    int n = 0;
    char *p = buf;
    while ((p = strstr(p, "Station "))) {
        n++;
        p += 8;
    }
    return n;
}

/* Parse `connmanctl technologies` for wifi Tethering = True. */
static bool tethering_on(char *ap_ssid, size_t ap_ssid_n)
{
    if (ap_ssid && ap_ssid_n) ap_ssid[0] = 0;
    char buf[8192];
    if (sh_out(buf, sizeof(buf), "connmanctl technologies 2>/dev/null") != 0)
        return false;

    bool in_wifi = false;
    bool tether = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (!strncmp(p, "/net/connman/technology/", 24)) {
            in_wifi = !strcmp(p + 24, "wifi");
            continue;
        }
        if (!in_wifi) continue;
        if (!strncmp(p, "Tethering = ", 12)) {
            tether = !strcmp(trim(p + 12), "True");
        } else if (!strncmp(p, "TetheringIdentifier = ", 22)) {
            if (ap_ssid) str_copy(ap_ssid, ap_ssid_n, trim(p + 22));
        }
    }
    return tether;
}

static bool ssid_md5(const char *ssid, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char ssid_q[256], raw[128];
    shq(ssid_q, sizeof(ssid_q), ssid);
    /* This firmware ships md5sum only as a busybox applet; the coreutils
     * binary is /usr/bin/md5sum.coreutils.  Try both so the service-id
     * hash never depends on the applet alone. */
    if (sh_out(raw, sizeof(raw),
               "(printf %%s %s | md5sum) 2>/dev/null || "
               "(printf %%s %s | md5sum.coreutils) 2>/dev/null",
               ssid_q, ssid_q) != 0)
        return false;
    char *p = trim(raw);
    if (strlen(p) < 32) return false;
    char hash[33];
    memcpy(hash, p, 32);
    hash[32] = 0;
    str_copy(out, n, hash);
    return true;
}

/* Find a ConnMan service token for an SSID. */
static bool find_service(const char *services, const char *ssid,
                         char *service, size_t service_n)
{
    char *save = NULL;
    char *copy = strdup(services ? services : "");
    if (!copy) return false;
    bool found = false;
    char hash[40] = "";
    bool have_hash = ssid_md5(ssid, hash, sizeof(hash));

    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strstr(line, "wifi_")) continue;

        bool name_hit = strstr(line, ssid) != NULL;
        bool hash_hit = have_hash && strstr(line, hash) != NULL;
        if (!name_hit && !hash_hit) continue;

        /* The service id is the token starting with wifi_; connmanctl
         * prints a trailing state word, so the last token is not it.
         * Prefer the token carrying the SSID hash. */
        char *tok = strtok(line, " \t");
        while (tok) {
            if (!strncmp(tok, "wifi_", 5)) {
                if (hash_hit && !strstr(tok, hash)) {
                    tok = strtok(NULL, " \t");
                    continue;
                }
                str_copy(service, service_n, tok);
                found = true;
                break;
            }
            tok = strtok(NULL, " \t");
        }
        if (found) break;
    }
    free(copy);
    return found;
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

static void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mode = MODE_OFF;
    g_cfg.wlan_on = false;
    g_cfg.cp_band = 2;
    g_cfg.cp_channel = 36;
    g_cfg.cp_hidden = false;
    g_cfg.cp_max_sta = 2;
    str_copy(g_cfg.cp_manufacturer, sizeof(g_cfg.cp_manufacturer), "Hyundai");
    str_copy(g_cfg.cp_model, sizeof(g_cfg.cp_model), "Ioniq 5");
    str_copy(g_cfg.cp_name, sizeof(g_cfg.cp_name), "Ioniq 5");
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay");
    g_cfg.ssh_enabled = false;
    g_cfg.ssh_port = SSH_DEFAULT_PORT;
}

static void config_load(void)
{
    config_defaults();
    char buf[8192];
    if (read_file(CONFIG_PATH, buf, sizeof(buf)) <= 0) return;

    bool has_version = false;
    bool has_ssh_port = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p);
        char *v = trim(eq + 1);

        if (!strcmp(k, "mode")) {
            if (!strcmp(v, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(v, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else g_cfg.mode = MODE_OFF;
        } else if (!strcmp(k, "wlan_on")) {
            g_cfg.wlan_on = atoi(v) != 0;
        } else if (!strcmp(k, "ssid")) {
            str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), v);
        } else if (!strcmp(k, "psk")) {
            str_copy(g_cfg.psk, sizeof(g_cfg.psk), v);
        } else if (!strcmp(k, "carplay_ssid")) {
            str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), v);
        } else if (!strcmp(k, "carplay_psk")) {
            str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), v);
        } else if (!strcmp(k, "carplay_band")) {
            if (!strcmp(v, "2.4")) g_cfg.cp_band = 1;
            else if (!strcmp(v, "5")) g_cfg.cp_band = 2;
            else g_cfg.cp_band = 0;
        } else if (!strcmp(k, "carplay_channel")) {
            g_cfg.cp_channel = atoi(v);
        } else if (!strcmp(k, "carplay_hidden")) {
            g_cfg.cp_hidden = atoi(v) != 0;
        } else if (!strcmp(k, "carplay_max_sta")) {
            g_cfg.cp_max_sta = atoi(v);
            if (g_cfg.cp_max_sta < 1) g_cfg.cp_max_sta = 1;
            if (g_cfg.cp_max_sta > 128) g_cfg.cp_max_sta = 128;
        } else if (!strcmp(k, "carplay_manufacturer")) {
            str_copy(g_cfg.cp_manufacturer, sizeof(g_cfg.cp_manufacturer), v);
        } else if (!strcmp(k, "carplay_model")) {
            str_copy(g_cfg.cp_model, sizeof(g_cfg.cp_model), v);
        } else if (!strcmp(k, "carplay_name")) {
            str_copy(g_cfg.cp_name, sizeof(g_cfg.cp_name), v);
        } else if (!strcmp(k, "carplay_id")) {
            str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), v);
        } else if (!strcmp(k, "carplay_btaddr")) {
            str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), v);
        } else if (!strcmp(k, "ssh")) {
            g_cfg.ssh_enabled = atoi(v) != 0;
        } else if (!strcmp(k, "ssh_port")) {
            has_ssh_port = true;
            g_cfg.ssh_port = atoi(v);
            if (g_cfg.ssh_port < 1 || g_cfg.ssh_port > 65535)
                g_cfg.ssh_port = SSH_DEFAULT_PORT;
        } else if (!strcmp(k, "config_version")) {
            has_version = true;
        } else if (!strcmp(k, "imported")) {
            g_cfg.imported = atoi(v) != 0;
        } else if (!strcmp(k, "imported_from")) {
            str_copy(g_cfg.imported_from, sizeof(g_cfg.imported_from), v);
        }
    }

    /* Old builds defaulted to 22.  Migrate once to the non-conflicting
     * default; a config with config_version=2 is left alone. */
    if (!has_version && has_ssh_port && g_cfg.ssh_port == 22) {
        g_cfg.ssh_port = SSH_DEFAULT_PORT;
        g_config_migrated = true;
        log_msg("config migration: SSH port 22 -> %d", SSH_DEFAULT_PORT);
    }
}

static void config_save(void)
{
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "# wifi_daemon config\n"
             "config_version=2\n"
             "mode=%s\n"
             "wlan_on=%d\n"
             "ssid=%s\n"
             "psk=%s\n"
             "carplay_ssid=%s\n"
             "carplay_psk=%s\n"
             "carplay_band=%s\n"
             "carplay_channel=%d\n"
             "carplay_hidden=%d\n"
             "carplay_max_sta=%d\n"
             "carplay_manufacturer=%s\n"
             "carplay_model=%s\n"
             "carplay_name=%s\n"
             "carplay_id=%s\n"
             "carplay_btaddr=%s\n"
             "ssh=%d\n"
             "ssh_port=%d\n"
             "imported=%d\n"
             "imported_from=%s\n",
             g_cfg.mode == MODE_CLIENT ? "client" :
             g_cfg.mode == MODE_CARPLAY ? "carplay" : "off",
             g_cfg.wlan_on ? 1 : 0,
             g_cfg.ssid, g_cfg.psk,
             g_cfg.cp_ssid, g_cfg.cp_psk,
             g_cfg.cp_band == 1 ? "2.4" : g_cfg.cp_band == 2 ? "5" : "auto",
             g_cfg.cp_channel, g_cfg.cp_hidden ? 1 : 0, g_cfg.cp_max_sta,
             g_cfg.cp_manufacturer, g_cfg.cp_model, g_cfg.cp_name,
             g_cfg.cp_id, g_cfg.cp_btaddr,
             g_cfg.ssh_enabled ? 1 : 0, g_cfg.ssh_port,
             g_cfg.imported ? 1 : 0, g_cfg.imported_from);

    mkdir(APP_DIR, 0755);
    write_file_atomic(CONFIG_PATH, buf, 0600);
}

/* ------------------------------------------------------------------ */
/* USB import                                                          */
/* ------------------------------------------------------------------ */

static bool newest_match(const char *pattern, char *out, size_t n)
{
    glob_t g;
    bool found = false;
    time_t best = 0;
    if (glob(pattern, 0, NULL, &g) != 0) return false;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        struct stat st;
        if (stat(g.gl_pathv[i], &st) != 0) continue;
        if (!found || st.st_mtime > best) {
            best = st.st_mtime;
            str_copy(out, n, g.gl_pathv[i]);
            found = true;
        }
    }
    globfree(&g);
    return found;
}

static bool find_usb_file(const char *name, char *out, size_t n)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "/media/usb/*/*/%s", name);
    if (newest_match(pat, out, n)) return true;
    snprintf(pat, sizeof(pat), "/media/usb/*/%s", name);
    if (newest_match(pat, out, n)) return true;
    snprintf(pat, sizeof(pat), "/media/usb/%s", name);
    if (newest_match(pat, out, n)) return true;
    snprintf(pat, sizeof(pat), "/run/media/*/%s", name);
    if (newest_match(pat, out, n)) return true;
    snprintf(pat, sizeof(pat), "/run/media/*/*/%s", name);
    if (newest_match(pat, out, n)) return true;
    snprintf(pat, sizeof(pat), "/mnt/*/%s", name);
    return newest_match(pat, out, n);
}

static void random_psk(char *out, size_t n)
{
    static const char chars[] =
        "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned char rnd[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, rnd, sizeof(rnd));
        close(fd);
        if (r <= 0) memset(rnd, 0x5a, sizeof(rnd));
    } else {
        memset(rnd, 0x5a, sizeof(rnd));
    }
    size_t j = 0;
    for (size_t i = 0; i < sizeof(rnd) && j + 1 < n; i++)
        out[j++] = chars[rnd[i] % (sizeof(chars) - 1)];
    out[j] = 0;
}

/* Parse wifi.txt.  Supports key=value and a two-line SSID/PSK fallback. */
static bool parse_wifi_txt(const char *path)
{
    char buf[8192];
    if (read_file(path, buf, sizeof(buf)) <= 0) return false;

    char ssid[80] = {0}, psk[160] = {0};
    char cp_ssid[80] = {0}, cp_psk[160] = {0};
    int cp_band = -1, cp_channel = -1, cp_hidden = -1, cp_max = -1;
    int ssh = -1, ssh_port = -1;
    char cp_manufacturer[64] = {0}, cp_model[64] = {0};
    char cp_name[64] = {0}, cp_id[64] = {0}, cp_btaddr[32] = {0};
    int plain_lines = 0;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            if (plain_lines == 0) str_copy(ssid, sizeof(ssid), p);
            else if (plain_lines == 1) str_copy(psk, sizeof(psk), p);
            plain_lines++;
            continue;
        }
        *eq = 0;
        char *k = trim(p);
        char *v = trim(eq + 1);
        if (!strcmp(k, "ssid")) str_copy(ssid, sizeof(ssid), v);
        else if (!strcmp(k, "psk") || !strcmp(k, "key") || !strcmp(k, "password"))
            str_copy(psk, sizeof(psk), v);
        else if (!strcmp(k, "carplay_ssid")) str_copy(cp_ssid, sizeof(cp_ssid), v);
        else if (!strcmp(k, "carplay_psk") || !strcmp(k, "carplay_key"))
            str_copy(cp_psk, sizeof(cp_psk), v);
        else if (!strcmp(k, "carplay_band")) {
            if (!strcmp(v, "2.4")) cp_band = 1;
            else if (!strcmp(v, "5")) cp_band = 2;
            else cp_band = 0;
        } else if (!strcmp(k, "carplay_channel")) cp_channel = atoi(v);
        else if (!strcmp(k, "carplay_hidden")) cp_hidden = atoi(v) != 0;
        else if (!strcmp(k, "carplay_max_sta")) cp_max = atoi(v);
        else if (!strcmp(k, "carplay_manufacturer")) str_copy(cp_manufacturer, sizeof(cp_manufacturer), v);
        else if (!strcmp(k, "carplay_model")) str_copy(cp_model, sizeof(cp_model), v);
        else if (!strcmp(k, "carplay_name")) str_copy(cp_name, sizeof(cp_name), v);
        else if (!strcmp(k, "carplay_id")) str_copy(cp_id, sizeof(cp_id), v);
        else if (!strcmp(k, "carplay_btaddr")) str_copy(cp_btaddr, sizeof(cp_btaddr), v);
        else if (!strcmp(k, "ssh")) ssh = atoi(v) != 0;
        else if (!strcmp(k, "ssh_port")) ssh_port = atoi(v);
        else if (!strcmp(k, "mode")) {
            if (!strcmp(v, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(v, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else if (!strcmp(v, "off")) g_cfg.mode = MODE_OFF;
        }
    }

    if (!ssid[0]) return false;
    str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), ssid);
    str_copy(g_cfg.psk, sizeof(g_cfg.psk), psk);
    if (cp_ssid[0]) str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), cp_ssid);
    if (cp_psk[0]) str_copy(g_cfg.cp_psk, sizeof(g_cfg.cp_psk), cp_psk);
    if (cp_band >= 0) g_cfg.cp_band = cp_band;
    if (cp_channel > 0) g_cfg.cp_channel = cp_channel;
    if (cp_hidden >= 0) g_cfg.cp_hidden = cp_hidden != 0;
    if (cp_max > 0) g_cfg.cp_max_sta = cp_max;
    if (cp_manufacturer[0]) str_copy(g_cfg.cp_manufacturer, sizeof(g_cfg.cp_manufacturer), cp_manufacturer);
    if (cp_model[0]) str_copy(g_cfg.cp_model, sizeof(g_cfg.cp_model), cp_model);
    if (cp_name[0]) str_copy(g_cfg.cp_name, sizeof(g_cfg.cp_name), cp_name);
    if (cp_id[0]) str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), cp_id);
    if (cp_btaddr[0]) str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), cp_btaddr);
    if (ssh >= 0) g_cfg.ssh_enabled = ssh != 0;
    if (ssh_port > 0 && ssh_port < 65536) g_cfg.ssh_port = ssh_port;

    if (!g_cfg.cp_psk[0]) random_psk(g_cfg.cp_psk, sizeof(g_cfg.cp_psk));
    if (g_cfg.cp_max_sta < 1) g_cfg.cp_max_sta = 2;
    if (g_cfg.cp_channel <= 0)
        g_cfg.cp_channel = g_cfg.cp_band == 1 ? 6 : 36;

    str_copy(g_cfg.imported_from, sizeof(g_cfg.imported_from), path);
    g_cfg.imported = true;
    config_save();

    /* Keep a private copy for the UI / later use. */
    char copy[8192];
    if (read_file(path, copy, sizeof(copy)) > 0)
        write_file_atomic(WIFI_TXT_COPY, copy, 0600);

    set_msg("imported WiFi config from %s (ssid=%s)", path, g_cfg.ssid);
    return true;
}

static bool import_wifi_path(const char *path)
{
    return parse_wifi_txt(path);
}

static bool import_wifi(void)
{
    char path[512];
    if (!find_usb_file("wifi.txt", path, sizeof(path))) {
        set_msg("wifi.txt not found on USB");
        return false;
    }
    return parse_wifi_txt(path);
}

static bool import_authorized_keys(void)
{
    char path[512];
    if (!find_usb_file("authorized_keys", path, sizeof(path))) {
        set_msg("authorized_keys not found on USB");
        return false;
    }
    char buf[16384];
    if (read_file(path, buf, sizeof(buf)) <= 0) {
        set_msg("cannot read %s", path);
        return false;
    }
    mkdir(SSH_DIR, 0700);
    if (write_file_atomic(SSH_KEYS, buf, 0600) != 0) {
        set_msg("cannot write %s", SSH_KEYS);
        return false;
    }
    set_msg("imported SSH keys from %s", path);
    return true;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi / ConnMan                                                     */
/* ------------------------------------------------------------------ */

static void stop_oem_wifi(void)
{
    sh("systemctl stop supplicantmanager.path 2>/dev/null");
    sh("systemctl stop supplicantmanager.service 2>/dev/null");
    sh("rm -f /tmp/no_unece");

    int pid = pid_by_comm("supplicantmanager");
    if (pid > 0) kill_pid(pid);

    pid = pid_by_cmdline("/rw_data/wifi/entropy.bin");
    if (pid > 0) kill_pid(pid);
}

static bool ensure_connman(void)
{
    if (sh("systemctl is-active --quiet connman.service 2>/dev/null") == 0)
        return true;
    sh("systemctl start connman.service 2>/dev/null");
    for (int i = 0; i < 10; i++) {
        if (sh("systemctl is-active --quiet connman.service 2>/dev/null") == 0)
            return true;
        usleep(300000);
    }
    return false;
}

static bool load_driver(void)
{
    if (iface_present()) return true;

    sh("modprobe cfg80211 2>/dev/null");
    sh("modprobe bcmdhd firmware_path=/lib/firmware/bcmdhd/rtecdc.bin "
       "nvram_path=/lib/firmware/bcmdhd/nvram.txt iface_name=wlan0 2>/dev/null");
    for (int i = 0; i < 10; i++) {
        if (iface_present()) return true;
        usleep(300000);
    }

    sh("insmod " BCMDHD_KO " firmware_path=/lib/firmware/bcmdhd/rtecdc.bin "
       "nvram_path=/lib/firmware/bcmdhd/nvram.txt iface_name=wlan0 2>/dev/null");
    for (int i = 0; i < 10; i++) {
        if (iface_present()) return true;
        usleep(300000);
    }
    return iface_present();
}

/* ------------------------------------------------------------------ */
/* WiFi supplicant                                                     */
/* ------------------------------------------------------------------ */

static bool wpa_ping(void)
{
    if (sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 ping 2>/dev/null | grep -q PONG") == 0)
        return true;

    /* A running supplicant with a dead/unreadable socket is the classic
     * SMACK-label mismatch seen on this car.  Log the socket state (at most
     * once a minute) so the reason is in the console and the log bundle. */
    static int64_t last_log;
    int64_t now = now_ms();
    if (now - last_log > 60000) {
        last_log = now;
        char ls[1024] = "";
        sh_out(ls, sizeof(ls), "ls -la " WPA_CTRL_DIR " 2>&1");
        log_msg("wpa_cli ping failed; ctrl dir: %s", trim(ls));
    }
    return false;
}

static void stop_wpa_supplicant(void)
{
    char buf[64];
    if (read_file(WPA_PIDFILE, buf, sizeof(buf)) > 0) {
        int pid = atoi(trim(buf));
        if (pid > 1) {
            char path[64], comm[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            if (read_file(path, comm, sizeof(comm)) > 0 &&
                !strcmp(trim(comm), "wpa_supplicant"))
                kill_pid(pid);
        }
    }
    int pid = pid_by_comm("wpa_supplicant");
    if (pid > 0) kill_pid(pid);
    unlink(WPA_PIDFILE);
}

static void ensure_wpa_conf(void)
{
    /* Deliberately no network blocks: association is driven by ConnMan or
     * wpa_cli.  The OEM /etc/wpa_supplicant.conf has key_mgmt=NONE, which
     * makes the supplicant auto-associate to any open network. */
    if (!file_exists(WPA_CONF))
        write_file_atomic(WPA_CONF,
                          "ctrl_interface=/var/run/wpa_supplicant\n"
                          "ctrl_interface_group=0\n"
                          "update_config=0\n"
                          "ap_scan=1\n", 0644);
}

static bool start_wpa_supplicant_args(const char *driver)
{
    ensure_wpa_conf();
    sh("mkdir -p /var/run/wpa_supplicant /run/wpa_supplicant 2>/dev/null");
    sh("chmod 755 /var/run/wpa_supplicant 2>/dev/null");
    sh("rm -f /var/run/wpa_supplicant/wlan0 2>/dev/null");

    /* This build has no CONFIG_DEBUG_FILE: -f is rejected.  Do not use -B:
     * daemon(0,0) would redirect output to /dev/null and the log would stay
     * empty.  Background via the shell instead so wpa_supplicant.log keeps
     * the real output.  Do not pass -e /rw_data/wifi/entropy.bin: the OEM
     * supplicant is identified by that cmdline argument in stop_oem_wifi,
     * and our own instance would be killed by mistake. */
    if (driver && *driver)
        sh("nohup /usr/sbin/wpa_supplicant -u -D%s -iwlan0 -c%s -P%s >>%s 2>&1 &",
           driver, WPA_CONF, WPA_PIDFILE, WPA_LOG);
    else
        sh("nohup /usr/sbin/wpa_supplicant -u -iwlan0 -c%s -P%s >>%s 2>&1 &",
           WPA_CONF, WPA_PIDFILE, WPA_LOG);

    for (int i = 0; i < 32; i++) {
        if (wpa_ping()) return true;
        usleep(250000);
    }
    return false;
}

static bool ensure_wpa_supplicant(void)
{
    if (wpa_ping()) return true;
    if (!iface_present()) return false;

    /* A stale instance with a broken control socket blocks the new one. */
    if (pid_by_comm("wpa_supplicant") > 0) {
        log_msg("wpa_supplicant running but control socket is dead; restarting");
        stop_wpa_supplicant();
        usleep(500000);
    }

    /* One start with the driver list: wpa_supplicant falls back from
     * nl80211 to wext internally. */
    if (start_wpa_supplicant_args("nl80211,wext")) return true;
    stop_wpa_supplicant();
    log_msg("wpa_supplicant nl80211,wext failed; trying compiled-in default");
    if (start_wpa_supplicant_args(NULL)) return true;

    char tail[256];
    log_tail(WPA_LOG, tail, sizeof(tail));
    if (tail[0])
        set_msg("wpa_supplicant failed: %s", tail);
    else
        set_msg("wpa_supplicant failed to start (no output; see wpa_supplicant.log)");
    return false;
}

static bool wifi_powered(void)
{
    char buf[8192];
    if (sh_out(buf, sizeof(buf), "connmanctl technologies 2>/dev/null") != 0)
        return false;
    bool in_wifi = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (!strncmp(p, "/net/connman/technology/", 24)) {
            in_wifi = !strcmp(p + 24, "wifi");
            continue;
        }
        if (!in_wifi) continue;
        if (!strncmp(p, "Powered = ", 10))
            return !strcmp(trim(p + 10), "True");
    }
    return false;
}

/* Load the driver, evict the OEM manager, and leave a supplicant under our
 * control.  Used by scan, client connect and CarPlay AP start. */
static bool ensure_wifi_ready(void)
{
    if (!iface_present() && !load_driver()) {
        set_msg("bcmdhd/wlan0 not available");
        return false;
    }
    sh("rfkill unblock all 2>/dev/null");
    stop_oem_wifi();
    if (!ensure_connman())
        log_msg("connman not active; continuing");
    iface_up();
    if (!ensure_wpa_supplicant())
        return false;
    sh("connmanctl enable wifi 2>/dev/null");
    for (int i = 0; i < 20; i++) {
        if (wifi_powered()) return true;
        usleep(250000);
    }
    log_msg("wifi technology not powered after enable");
    return wpa_ping();
}

static bool wpa_state_completed(void)
{
    char buf[2048];
    if (sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") != 0)
        return false;
    return strstr(buf, "wpa_state=COMPLETED") != NULL;
}

static int channel_to_freq(void)
{
    if (g_cfg.cp_band == 1) {
        int ch = g_cfg.cp_channel;
        if (ch < 1 || ch > 13) ch = 6;
        return 2407 + ch * 5;
    }
    int ch = g_cfg.cp_channel;
    if (ch < 36 || ch > 165) ch = 36;
    return 5000 + ch * 5;
}

static bool ap_start(void)
{
    char ssid_q[256], psk_q[256];
    shq(ssid_q, sizeof(ssid_q), g_cfg.cp_ssid);
    shq(psk_q, sizeof(psk_q), g_cfg.cp_psk);
    int freq = channel_to_freq();

    if (!ensure_wifi_ready()) {
        set_msg("CarPlay AP: WiFi stack not ready");
        return false;
    }

    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    sh("connmanctl tether wifi off 2>/dev/null");
    sh("connmanctl enable wifi 2>/dev/null");
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 disconnect 2>/dev/null");
    iface_up();

    /* This vendor connmanctl parses tether options, not positionals.
     * Verified by executing the car binary with its own loader:
     *   tether wifi on <ssid> <psk> --hidden no --frequency 5180 --maxsta 2
     *            --appinfo 1
     * Positional "<hidden> <frequency> <maxsta>" yields
     * "unknown option [?][no]" and "unknown option [?][5180]" - the exact
     * errors in the on-car log.  --appinfo 1 marks the AP as CarPlay. */
    char out[4096] = "";
    int rc = sh_out(out, sizeof(out),
                    "connmanctl tether wifi on %s %s --hidden %s "
                    "--frequency %d --maxsta %d --appinfo 1 2>&1",
                    ssid_q, psk_q, g_cfg.cp_hidden ? "yes" : "no", freq,
                    g_cfg.cp_max_sta);
    log_msg("tether 5GHz rc=%d out=%s", rc, trim(out));

    bool up = false;
    for (int i = 0; i < 20 && !up; i++) {
        if (tethering_on(NULL, 0)) up = true;
        else usleep(500000);
    }

    if (!up) {
        /* 5 GHz / channel 36 may be rejected by the driver or regdomain.
         * Retry on 2.4 GHz channel 6 and remember it if that works. */
        rc = sh_out(out, sizeof(out),
                    "connmanctl tether wifi on %s %s --hidden %s "
                    "--frequency %d --maxsta %d --appinfo 1 2>&1",
                    ssid_q, psk_q, g_cfg.cp_hidden ? "yes" : "no", 2437,
                    g_cfg.cp_max_sta);
        log_msg("tether 2.4GHz rc=%d out=%s", rc, trim(out));
        for (int i = 0; i < 20 && !up; i++) {
            if (tethering_on(NULL, 0)) up = true;
            else usleep(500000);
        }
        if (up) {
            g_cfg.cp_band = 1;
            g_cfg.cp_channel = 6;
            config_save();
            log_msg("CarPlay AP fell back to 2.4 GHz channel 6");
        }
    }
    if (up) return true;

    char tech[4096];
    if (sh_out(tech, sizeof(tech), "connmanctl technologies 2>/dev/null") == 0)
        log_msg("technologies after tether failure: %s", trim(tech));
    set_msg("CarPlay AP failed: %s",
            out[0] ? trim(out) : "tether did not come up");
    return false;
}

static void carplay_fw_close(void);

static void ap_stop(void)
{
    carplay_fw_close();
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    sh("connmanctl tether wifi off 2>/dev/null");
}

static bool json_get_string_value(const char *path, const char *key,
                                  char *out, size_t n);

/* ------------------------------------------------------------------ */
/* Bluetooth (vendor vbtd, CommonAPI on the session bus)               */
/* ------------------------------------------------------------------ */

/* Dbuslaunch has shipped both shell-quoted (`ADDRESS='unix:...';`) and
 * bare (`ADDRESS=unix:...`) forms.  Normalise before handing the string to
 * dbus-send/dbus-monitor. */
static void sanitize_bus_address(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\'' || *p == '"') p++;
    char *e = p + strlen(p);
    while (e > p &&
           (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
            e[-1] == '\r' || e[-1] == '\'' || e[-1] == '"' ||
            e[-1] == ';'))
        *--e = 0;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
}

static bool session_bus_address(char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char buf[4096];

    if (read_file(SESSION_ENV, buf, sizeof(buf)) > 0) {
        char *p = strstr(buf, "DBUS_SESSION_BUS_ADDRESS=");
        if (p) {
            p += strlen("DBUS_SESSION_BUS_ADDRESS=");
            char *e = p;
            while (*e && *e != '\n' && *e != '\r') e++;
            size_t len = (size_t)(e - p);
            if (len >= n) len = n - 1;
            memcpy(out, p, len);
            out[len] = 0;
            sanitize_bus_address(out);
            if (out[0]) return true;
        }
    }
    if (read_file(SESSION_ADDR, buf, sizeof(buf)) > 0) {
        char *v = trim(buf);
        if (*v) {
            str_copy(out, n, v);
            sanitize_bus_address(out);
            return true;
        }
    }

    /* Last resort: the AppLauncher environment (same session bus). */
    int pid = pid_by_comm("AppLauncher");
    if (pid > 1) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/environ", pid);
        int len = read_file(path, buf, sizeof(buf));
        if (len > 0) {
            for (int i = 0; i < len; i++)
                if (buf[i] == 0) buf[i] = '\n';
            char *p = strstr(buf, "DBUS_SESSION_BUS_ADDRESS=");
            if (p) {
                p += strlen("DBUS_SESSION_BUS_ADDRESS=");
                char *e = p;
                while (*e && *e != '\n') e++;
                size_t l = (size_t)(e - p);
                if (l >= n) l = n - 1;
                memcpy(out, p, l);
                out[l] = 0;
            }
        }
    }
    return out && out[0];
}

/* ------------------------------------------------------------------ */
/* Home/Star watcher (session bus)                                     */
/*                                                                    */
/* The steering-wheel Home/Star key never reaches QML: the app        */
/* manager delivers NotifyRequestHomeScreen (method call) or          */
/* custom_key_value (signal) on the session bus.  Tail dbus-monitor   */
/* and expose home_seq so the overlay can close itself.  This is the  */
/* same proven pattern as the Battery daemon.                          */
/* ------------------------------------------------------------------ */

static void home_state_write(void)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"seq\":%d,\"epoch\":%ld,\"source\":\"%s\","
             "\"last_app_key\":%d,\"monitor\":%s}\n",
             g_home_seq, g_home_epoch, g_home_source, g_home_last_key,
             g_home_monitor ? "true" : "false");
    write_file_atomic(HOME_STATE, buf, 0644);
}

static void home_monitor_line(const char *line)
{
    if (strstr(line, "member=custom_key_value")) {
        snprintf(g_home_member, sizeof(g_home_member), "custom_key_value");
        return;
    }
    if (strstr(line, "member=NotifyRequestHomeScreen")) {
        g_home_seq++;
        snprintf(g_home_source, sizeof(g_home_source), "home");
        g_home_member[0] = 0;
        home_state_write();
        return;
    }
    if (g_home_member[0]) {
        int v;
        /* dbus-monitor prints the argument on its own line, indented by a
         * space.  Do not rely on the exact indentation: trim, then match. */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (sscanf(p, "int32 %d", &v) == 1) {
            g_home_last_key = v;
            g_home_seq++;
            snprintf(g_home_source, sizeof(g_home_source), "star");
            home_state_write();
        }
        g_home_member[0] = 0;
    }
}

static void home_monitor_start(void)
{
    if (g_home_fd >= 0) return;

    char addr[1024];
    if (!session_bus_address(addr, sizeof(addr)))
        return;   /* Dbuslaunch has not written the address yet; retry. */

    /* A stale guid in the address file would make libdbus refuse the
     * connection after a bus restart; the path alone identifies the bus. */
    char *guid = strstr(addr, ",guid=");
    if (guid) *guid = 0;

    int fds[2];
    if (pipe(fds) < 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        execl(DBUS_MONITOR, "dbus-monitor", "--address", addr,
              "type='signal',interface='com.lge.avn.appmanager.info',"
              "member='custom_key_value'",
              "type='method_call',interface='com.lge.hkmc.avn.appfunc.appInterface',"
              "member='NotifyRequestHomeScreen'",
              (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    g_home_fd = fds[0];
    g_home_pid = pid;
    g_home_monitor = true;
    g_home_line_len = 0;
    g_home_member[0] = 0;
    home_state_write();
    log_msg("home monitor: watching %s", addr);
}

static void home_monitor_tick(void)
{
    if (g_home_fd < 0) {
        int64_t now = now_ms();
        if (now >= g_home_retry_ms) {
            g_home_retry_ms = now + 5000;
            home_monitor_start();
        }
        return;
    }

    char buf[1024];
    for (;;) {
        ssize_t n = read(g_home_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (g_home_line_len > 0) {
                        g_home_line[g_home_line_len] = 0;
                        home_monitor_line(g_home_line);
                        g_home_line_len = 0;
                    }
                } else if (g_home_line_len + 1 < sizeof(g_home_line)) {
                    g_home_line[g_home_line_len++] = c;
                }
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        /* EOF or real error: the monitor died (bus shutdown); retry later. */
        close(g_home_fd);
        g_home_fd = -1;
        g_home_monitor = false;
        home_state_write();
        return;
    }
}

static void home_monitor_cleanup(void)
{
    if (g_home_fd >= 0) { close(g_home_fd); g_home_fd = -1; }
    if (g_home_pid > 0) {
        kill(g_home_pid, SIGTERM);
        g_home_pid = -1;
    }
    g_home_monitor = false;
}

static int session_dbus(char *out, size_t n, const char *dest, const char *path,
                        const char *iface, const char *method, const char *arg)
{
    char addr[1024];
    if (!session_bus_address(addr, sizeof(addr))) {
        if (out && n) snprintf(out, n, "no session bus address");
        return -1;
    }
    char addr_q[1200], dest_q[256], path_q[256], iface_q[256];
    shq(addr_q, sizeof(addr_q), addr);
    shq(dest_q, sizeof(dest_q), dest);
    shq(path_q, sizeof(path_q), path);
    shq(iface_q, sizeof(iface_q), iface);

    if (arg && arg[0])
        return sh_out(out, n,
                      "DBUS_SESSION_BUS_ADDRESS=%s dbus-send --session --print-reply "
                      "--dest=%s %s %s.%s %s 2>&1",
                      addr_q, dest_q, path_q, iface_q, method, arg);
    return sh_out(out, n,
                  "DBUS_SESSION_BUS_ADDRESS=%s dbus-send --session --print-reply "
                  "--dest=%s %s %s.%s 2>&1",
                  addr_q, dest_q, path_q, iface_q, method);
}

static const char *BT_DESTS[] = {
    "org.bluez.Adapter.Adapter.v2_0",
    "org.bluez.Adapter.Adapter.v2_0_lge1",
    "org.bluez.Adapter.Adapter",
    "org.bluez",
    NULL
};
static const char *BT_PATHS[] = {
    "/org/bluez/Adapter/Adapter/v2_0",
    "/org/bluez/Adapter/Adapter",
    "/org/bluez/hci0",
    NULL
};
static const char *BT_IFACES[] = {
    "org.bluez.Adapter.Adapter.v2_0",
    "org.bluez.Adapter.Adapter.v2_0_lge1",
    "org.bluez.Adapter.Adapter",
    "org.bluez.Adapter1",
    NULL
};
static char g_bt_dest[128];
static char g_bt_path[128];
static char g_bt_iface[128];

static bool bt_reply_ok(const char *out, int rc)
{
    if (rc != 0) return false;
    if (!out) return true;
    if (strstr(out, "Error") || strstr(out, "error")) return false;
    if (strstr(out, "Unknown") || strstr(out, "does not exist")) return false;
    if (strstr(out, "No such") || strstr(out, "not found")) return false;
    return true;
}

static bool bt_try_combo(const char *dest, const char *path, const char *iface,
                         const char *method, const char *arg,
                         char *out, size_t n)
{
    int rc = session_dbus(out, n, dest, path, iface, method, arg);
    return bt_reply_ok(out, rc);
}

static bool bt_call(const char *method, const char *arg, char *out, size_t n)
{
    if (!g_bt_dest[0] && g_bt_fail_ms &&
        now_ms() - g_bt_fail_ms < 30000) {
        if (out && n) snprintf(out, n, "bluetooth D-Bus unavailable (retry later)");
        return false;
    }
    if (g_bt_dest[0] &&
        bt_try_combo(g_bt_dest, g_bt_path, g_bt_iface, method, arg, out, n))
        return true;
    g_bt_dest[0] = 0;

    for (int d = 0; BT_DESTS[d]; d++) {
        for (int p = 0; BT_PATHS[p]; p++) {
            for (int i = 0; BT_IFACES[i]; i++) {
                if (bt_try_combo(BT_DESTS[d], BT_PATHS[p], BT_IFACES[i],
                                 method, arg, out, n)) {
                    str_copy(g_bt_dest, sizeof(g_bt_dest), BT_DESTS[d]);
                    str_copy(g_bt_path, sizeof(g_bt_path), BT_PATHS[p]);
                    str_copy(g_bt_iface, sizeof(g_bt_iface), BT_IFACES[i]);
                    log_msg("bluetooth interface: %s %s %s",
                            g_bt_dest, g_bt_path, g_bt_iface);
                    return true;
                }
            }
        }
    }
    g_bt_fail_ms = now_ms();
    return false;
}

static bool bt_props_set(const char *name, bool value)
{
    char arg[512], out[4096];
    snprintf(arg, sizeof(arg),
             "string:org.bluez.Adapter1 string:%s variant:boolean:%s",
             name, value ? "true" : "false");
    int rc = session_dbus(out, sizeof(out), "org.bluez", "/org/bluez/hci0",
                          "org.freedesktop.DBus.Properties", "Set", arg);
    return bt_reply_ok(out, rc);
}

static bool bt_props_get(const char *name, char *out, size_t n)
{
    char arg[256];
    snprintf(arg, sizeof(arg), "string:org.bluez.Adapter1 string:%s", name);
    return session_dbus(out, n, "org.bluez", "/org/bluez/hci0",
                        "org.freedesktop.DBus.Properties", "Get", arg) == 0;
}

static bool bt_set_prop(const char *name, bool value)
{
    char method[64], arg[32], out[4096];
    snprintf(method, sizeof(method), "set%sAttribute", name);
    snprintf(arg, sizeof(arg), "boolean:%s", value ? "true" : "false");
    if (bt_call(method, arg, out, sizeof(out))) return true;
    snprintf(method, sizeof(method), "set%s", name);
    if (bt_call(method, arg, out, sizeof(out))) return true;
    return bt_props_set(name, value);
}

static bool bt_parse_string(const char *buf, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    const char *p = strstr(buf, "string \"");
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t len = (size_t)(e - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return out[0] != 0;
}

static bool bt_get_address(char *out, size_t n)
{
    char buf[4096];
    if (bt_call("getAddressAttribute", NULL, buf, sizeof(buf)) &&
        bt_parse_string(buf, out, n))
        return true;
    if (bt_call("getAddress", NULL, buf, sizeof(buf)) &&
        bt_parse_string(buf, out, n))
        return true;
    if (bt_props_get("Address", buf, sizeof(buf)) &&
        bt_parse_string(buf, out, n))
        return true;
    if (read_file("/sys/class/bluetooth/hci0/address", buf, sizeof(buf)) > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        str_copy(out, n, trim(buf));
        return out[0] != 0;
    }
    return false;
}

static bool bt_fetch_address(void)
{
    if (g_cfg.cp_btaddr[0]) return true;
    char addr[64];
    if (!bt_get_address(addr, sizeof(addr))) return false;
    str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), addr);
    config_save();
    return true;
}

static void bt_state_save(void)
{
    char buf[768];
    snprintf(buf, sizeof(buf),
             "discoverable=%d\npairable=%d\naddress=%s\nmessage=%s\n",
             g_bt_discoverable ? 1 : 0, g_bt_pairable ? 1 : 0,
             g_cfg.cp_btaddr, g_pair_msg);
    write_file_atomic(BT_STATE, buf, 0600);
}

static void bt_state_load(void)
{
    char buf[1024];
    if (read_file(BT_STATE, buf, sizeof(buf)) <= 0) return;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line);
        char *v = trim(eq + 1);
        if (!strcmp(k, "discoverable")) g_bt_discoverable = atoi(v) != 0;
        else if (!strcmp(k, "pairable")) g_bt_pairable = atoi(v) != 0;
        else if (!strcmp(k, "address")) str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), v);
        else if (!strcmp(k, "message")) str_copy(g_pair_msg, sizeof(g_pair_msg), v);
    }
}

static bool bt_set_pairing(bool on)
{
    bool powered = on ? bt_set_prop("Powered", true) : true;
    bool disc = bt_set_prop("Discoverable", on);
    bool pair = bt_set_prop("Pairable", on);
    if (on && (!disc || !pair)) {
        usleep(1000000);
        if (!disc) disc = bt_set_prop("Discoverable", true);
        if (!pair) pair = bt_set_prop("Pairable", true);
    }
    (void)powered;
    g_bt_discoverable = disc;
    g_bt_pairable = pair;

    if (on) {
        bt_fetch_address();
        if (disc || pair)
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Pairing on - %s - discoverable %s - pairable %s",
                     g_cfg.cp_btaddr[0] ? g_cfg.cp_btaddr : "BT address unknown",
                     disc ? "yes" : "no", pair ? "yes" : "no");
        else
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Bluetooth control unavailable; use Settings > Bluetooth on the car");
    } else {
        snprintf(g_pair_msg, sizeof(g_pair_msg), "Pairing stopped");
    }
    set_msg("%s", g_pair_msg);
    bt_state_save();
    return disc || pair;
}

static bool carplay_ie_on(void)
{
    char mfg[160], model[160], name[160], id[160], bt[96];
    if (!g_cfg.cp_btaddr[0])
        bt_fetch_address();
    shq(mfg, sizeof(mfg), g_cfg.cp_manufacturer);
    shq(model, sizeof(model), g_cfg.cp_model);
    shq(name, sizeof(name), g_cfg.cp_name);
    shq(id, sizeof(id), g_cfg.cp_id);
    shq(bt, sizeof(bt), g_cfg.cp_btaddr);

    if (!g_cfg.cp_id[0]) {
        char buf[256];
        if (read_file("/app/data/AppProjection/USBSerial.txt", buf, sizeof(buf)) > 0 ||
            read_file("/rw_data/app_data/AppProjection/USBSerial.txt", buf, sizeof(buf)) > 0) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), trim(buf));
        } else if (json_get_string_value("/app/data/vbt/VBTConfig.json",
                                         "CarPlayDeviceId", buf, sizeof(buf)) ||
                   json_get_string_value("/rw_data/app_data/vbt/VBTConfig.json",
                                         "CarPlayDeviceId", buf, sizeof(buf))) {
            str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), buf);
        }
        if (!g_cfg.cp_id[0])
            str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), "IONIQ5-260128");
        shq(id, sizeof(id), g_cfg.cp_id);
    }
    if (!g_cfg.cp_btaddr[0]) {
        char buf[256];
        if (read_file("/sys/class/bluetooth/hci0/address", buf, sizeof(buf)) > 0) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), trim(buf));
        } else if (json_get_string_value("/app/data/vbt/VBTConfig.json",
                                         "DeviceAddress", buf, sizeof(buf)) ||
                   json_get_string_value("/rw_data/app_data/vbt/VBTConfig.json",
                                         "DeviceAddress", buf, sizeof(buf))) {
            str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), buf);
        } else {
            char out[4096];
            if (sh_out(out, sizeof(out), "hcitool dev 2>/dev/null") == 0) {
                char *p = strstr(out, "hci0");
                if (p) {
                    p += 4;
                    while (*p && isspace((unsigned char)*p)) p++;
                    char *e = p;
                    while (*e && !isspace((unsigned char)*e)) e++;
                    if (e > p) {
                        *e = 0;
                        str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), p);
                    }
                }
            }
        }
        shq(bt, sizeof(bt), g_cfg.cp_btaddr);
    }

    if (!g_cfg.cp_btaddr[0]) {
        /* An empty CarPlayBluetoothAddr makes connmand reject the IE.  The
         * pairing path powers Bluetooth and fills this in; tell the user
         * instead of sending a command that cannot succeed. */
        log_msg("CarPlay IE skipped: Bluetooth address unknown");
        set_msg("CarPlay IE not set: Bluetooth address unknown; use Start CarPlay pairing");
        return false;
    }

    char out[2048];
    int rc = sh_out(out, sizeof(out),
                    "connmanctl tether_carplay_ie wifi on %s %s %s %s %s 2>&1",
                    mfg, model, name, id, bt);
    if (rc != 0) {
        log_msg("CarPlay IE rc=%d out=%s", rc, out);
        set_msg("CarPlay IE command failed (manufacturer/model/id/BT may need fixing)");
    } else {
        set_msg("CarPlay IE enabled");
    }
    return rc == 0;
}

static void carplay_ie_off(void)
{
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
}

static void ensure_carplay_services(void)
{
    if (sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") != 0)
        sh("systemctl start carplay_mdnsd.service 2>/dev/null");
    if (sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") != 0)
        sh("systemctl start carplay_iAP2.service 2>/dev/null");
}

/* Patch /app/data/vbt/VBTConfig.json ENABLE_WIRELESS_CARPLAY. */
static bool json_get_string_value(const char *path, const char *key,
                                  char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char text[65536];
    if (read_file(path, text, sizeof(text)) <= 0) return false;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    char *p = strstr(text, needle);
    if (!p) return false;
    char *colon = strchr(p + strlen(needle), ':');
    if (!colon) return false;
    char *v = colon + 1;
    while (*v && isspace((unsigned char)*v)) v++;
    if (*v != '"') {
        /* accept a bare number/string until comma/brace */
        char *e = v;
        while (*e && *e != ',' && *e != '}' && *e != '\n' && !isspace((unsigned char)*e)) e++;
        char saved = *e;
        *e = 0;
        str_copy(out, n, v);
        *e = saved;
        return out[0] != 0;
    }
    v++;
    char *e = strchr(v, '"');
    if (!e) return false;
    size_t len = (size_t)(e - v);
    if (len >= n) len = n - 1;
    memcpy(out, v, len);
    out[len] = 0;
    return true;
}

static bool json_set_bool_in_text(char *text, const char *key, bool value)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    char *p = strstr(text, needle);
    if (p) {
        char *colon = strchr(p + strlen(needle), ':');
        if (!colon) return false;
        char *v = colon + 1;
        while (*v && isspace((unsigned char)*v)) v++;
        char *end = v;
        while (*end && *end != ',' && *end != '}' && *end != '\n') end++;

        char out[65536];
        snprintf(out, sizeof(out), "%.*s%s%s",
                 (int)(v - text), text, value ? "true" : "false", end);
        size_t len = strlen(out);
        if (len >= 65536) return false;
        memcpy(text, out, len + 1);
        return true;
    }

    /* Insert before the last closing brace. */
    char *last = strrchr(text, '}');
    if (!last) return false;
    size_t pos = (size_t)(last - text);
    char out[65536];
    snprintf(out, sizeof(out), "%.*s,\n  \"%s\": %s\n%s",
             (int)pos, text, key, value ? "true" : "false", last);
    size_t len = strlen(out);
    if (len >= 65536) return false;
    memcpy(text, out, len + 1);
    return true;
}

static bool vbt_set_wireless_carplay(bool value, bool *changed)
{
    if (changed) *changed = false;
    const char *paths[] = {
        "/app/data/vbt/VBTConfig.json",
        "/rw_data/app_data/vbt/VBTConfig.json",
        NULL
    };
    bool found = false;
    bool ok = false;

    /* /app is normally read-only at runtime.  Keep going to the writable
     * /rw_data copy instead of stopping at the first failure. */
    for (int i = 0; paths[i]; i++) {
        if (!file_exists(paths[i])) continue;
        found = true;

        char text[65536];
        if (read_file(paths[i], text, sizeof(text)) <= 0) continue;

        char before[65536];
        str_copy(before, sizeof(before), text);
        if (!json_set_bool_in_text(text, "ENABLE_WIRELESS_CARPLAY", value))
            continue;

        if (!strcmp(before, text)) {
            log_msg("VBTConfig already has ENABLE_WIRELESS_CARPLAY=%s in %s",
                    value ? "true" : "false", paths[i]);
            ok = true;
            continue;
        }

        mkdir(VBT_BAK_DIR, 0755);
        char bak[512];
        snprintf(bak, sizeof(bak), VBT_BAK_DIR "/VBTConfig.json.%ld",
                 (long)time(NULL));
        write_file_atomic(bak, before, 0600);
        if (write_file_atomic(paths[i], text, 0644) != 0) {
            log_msg("cannot write %s (continuing to next VBT path)", paths[i]);
            continue;
        }
        if (changed) *changed = true;
        ok = true;
        log_msg("patched %s ENABLE_WIRELESS_CARPLAY=%s",
                paths[i], value ? "true" : "false");
    }

    if (!found)
        set_msg("VBTConfig.json not found");
    else if (ok)
        set_msg("VBTConfig ENABLE_WIRELESS_CARPLAY=%s", value ? "true" : "false");
    else
        set_msg("VBTConfig.json could not be updated");
    return ok;
}

static void restart_vbtd_if_needed(bool changed)
{
    if (!changed) return;
    sh("systemctl restart vbtd.service 2>/dev/null");
    set_msg("restarted vbtd for wireless CarPlay flag");
}

static bool bt_pair_start(void)
{
    bool changed = false;
    g_bt_fail_ms = 0;
    vbt_set_wireless_carplay(true, &changed);
    restart_vbtd_if_needed(changed);
    /* vbtd re-registers its CommonAPI adapter after a restart. */
    if (changed) sleep(2); else usleep(500000);

    bool ok = bt_set_pairing(true);
    for (int i = 0; i < 6 && !g_cfg.cp_btaddr[0]; i++) {
        usleep(500000);
        bt_fetch_address();
    }
    bt_state_save();
    if (!ok && !g_bt_discoverable) {
        snprintf(g_pair_msg, sizeof(g_pair_msg),
                 "Bluetooth discoverability could not be enabled; use Settings > Bluetooth");
        set_msg("%s", g_pair_msg);
    }
    return ok;
}

static void bt_pair_stop(void)
{
    bt_set_pairing(false);
    set_msg("CarPlay pairing stopped");
}

static void client_disconnect(void)
{
    char out[8192];
    if (sh_out(out, sizeof(out), "connmanctl services 2>/dev/null") == 0) {
        char service[256];
        if (g_cfg.ssid[0] && find_service(out, g_cfg.ssid, service, sizeof(service)))
            sh("connmanctl disconnect %s 2>/dev/null", service);
    }
}

static void write_status(void);

static bool client_wait_ip(int attempts)
{
    for (int i = 0; i < attempts; i++) {
        char ip[64];
        if (wlan_ip(ip, sizeof(ip))) {
            set_msg("client connected to '%s' (%s)", g_cfg.ssid, ip);
            return true;
        }
        if (g_stop) return false;
        /* Keep the UI live during the wait. */
        if ((i % 4) == 3) write_status();
        usleep(500000);
    }
    return false;
}

static bool client_connman_find(char *service, size_t service_n)
{
    char out[32768];
    for (int i = 0; i < 16; i++) {
        if (sh_out(out, sizeof(out), "connmanctl services 2>/dev/null") == 0 &&
            find_service(out, g_cfg.ssid, service, service_n))
            return true;
        usleep(500000);
    }
    return false;
}

static bool client_connect_wpa_cli(void)
{
    char idbuf[64];
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 remove_network all >/dev/null 2>&1");
    if (sh_out(idbuf, sizeof(idbuf),
               "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 add_network 2>/dev/null") != 0)
        return false;
    char *idp = trim(idbuf);
    if (!*idp || !strncmp(idp, "FAIL", 4)) {
        log_msg("wpa_cli add_network failed: %s", idp);
        return false;
    }
    int id = atoi(idp);

    char quoted[512], q[600];
    snprintf(quoted, sizeof(quoted), "\"%s\"", g_cfg.ssid);
    shq(q, sizeof(q), quoted);
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 set_network %d ssid %s >/dev/null 2>&1", id, q);

    if (g_cfg.psk[0]) {
        snprintf(quoted, sizeof(quoted), "\"%s\"", g_cfg.psk);
        shq(q, sizeof(q), quoted);
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 set_network %d psk %s >/dev/null 2>&1", id, q);
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 set_network %d key_mgmt WPA-PSK >/dev/null 2>&1", id);
    } else {
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 set_network %d key_mgmt NONE >/dev/null 2>&1", id);
    }
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 enable_network %d >/dev/null 2>&1", id);
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 select_network %d >/dev/null 2>&1", id);
    log_msg("wpa_cli network %d for '%s'", id, g_cfg.ssid);

    for (int i = 0; i < 30; i++) {
        if (wpa_state_completed()) return true;
        if (g_stop) return false;
        if ((i % 4) == 3) write_status();
        usleep(500000);
    }
    return false;
}

/* This vendor connmanctl has no `config --wifi`: the only config options
 * are nameservers/timeservers/domains/ipv4/ipv6/proxy/autoconnect/remove.
 * The passphrase must be supplied by a registered agent, so run the
 * interactive command loop non-interactively:
 *
 *     agent on
 *     connect <service>
 *     <passphrase>
 *     quit
 *
 * Verified against the car binary: it prints "Passphrase? " and reads the
 * next stdin line when connmand raises RequestInput.  The script lives in
 * a 0600 file (never argv) and is removed immediately. */
static bool client_connman_agent(const char *service)
{
    if (strchr(g_cfg.psk, '\n') || strchr(g_cfg.psk, '\r')) {
        log_msg("client: passphrase contains a newline; refusing agent flow");
        return false;
    }

    /* Feed the interactive loop in real time.  Piping all four lines at
     * once races the RequestInput callback: readline can consume the
     * passphrase as a command before connmand asks for it.  A short paced
     * script lets connmanctl register the agent and raise "Passphrase? "
     * first.  The passphrase is in a 0600 script, never in argv, and the
     * script is removed immediately after the run. */
    char path[128];
    snprintf(path, sizeof(path), APP_DIR "/connman.agent");
    FILE *f = fopen(path, "w");
    if (!f) {
        log_msg("client: cannot create %s: %s", path, strerror(errno));
        return false;
    }

    char psk_q[512];
    shq(psk_q, sizeof(psk_q), g_cfg.psk);

    fputs("#!/bin/sh\n", f);
    fputs("printf 'agent on\\n'\n", f);
    fputs("sleep 1\n", f);
    fprintf(f, "printf 'connect %s\\n'\n", service);
    /* Give connmand time to raise RequestInput and install its
     * "Passphrase? " readline prompt before the key line arrives. */
    fputs("sleep 5\n", f);
    if (g_cfg.psk[0])
        fprintf(f, "printf '%%s\\n' %s\n", psk_q);
    fputs("sleep 6\n", f);
    fputs("printf 'quit\\n'\n", f);
    if (fclose(f) != 0) {
        log_msg("client: cannot write %s: %s", path, strerror(errno));
        unlink(path);
        return false;
    }
    chmod(path, 0600);

    char out[4096] = "";
    int rc = sh_out(out, sizeof(out), "sh %s | connmanctl 2>&1", path);
    unlink(path);
    redact_secret(out, g_cfg.psk);
    log_msg("connmanctl agent rc=%d out=%s", rc, trim(out));
    return rc == 0 && !strstr(out, "Error");
}

static bool client_connect(void)
{
    if (!g_cfg.ssid[0]) {
        set_msg("no client SSID imported (use wifi.txt)");
        return false;
    }
    if (!ensure_wifi_ready()) {
        set_msg("client: WiFi stack not ready");
        return false;
    }

    sh("connmanctl enable wifi 2>/dev/null");
    iface_up();
    sh("connmanctl scan wifi 2>/dev/null");
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan >/dev/null 2>&1");

    char service[256];
    bool found = client_connman_find(service, sizeof(service));
    if (found) {
        if (client_connman_agent(service) && client_wait_ip(40))
            return true;
        log_msg("ConnMan agent connect to '%s' did not yield an IP; trying wpa_cli",
                g_cfg.ssid);
    }

    /* Direct wpa_cli path.  This also covers networks ConnMan did not list
     * (hidden/imported SSID).  ConnMan is then used only for DHCP. */
    if (!client_connect_wpa_cli()) {
        char st[2048];
        sh_out(st, sizeof(st), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null");
        set_msg("client: could not associate with '%s': %s",
                g_cfg.ssid, trim(st));
        return false;
    }

    /* The supplicant is associated; let ConnMan register the service and
     * take over DHCP.  If it needs the key, the agent supplies it. */
    found = client_connman_find(service, sizeof(service));
    if (found)
        client_connman_agent(service);

    if (client_wait_ip(40)) return true;

    char st[2048];
    sh_out(st, sizeof(st), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null");
    set_msg("client connected to '%s' but no IP: %s", g_cfg.ssid, trim(st));
    return false;
}

/* ------------------------------------------------------------------ */
/* SSH                                                                 */
/* ------------------------------------------------------------------ */

static void fw_tool(char *out, size_t n)
{
    if (file_exists("/usr/sbin/iptables")) {
        str_copy(out, n, "/usr/sbin/iptables");
        return;
    }
    if (file_exists("/sbin/iptables")) {
        str_copy(out, n, "/sbin/iptables");
        return;
    }
    const char *multi = "/usr/sbin/xtables-multi";
    if (file_exists(multi)) {
        mkdir(APP_DIR "/bin", 0755);
        unlink(APP_DIR "/bin/iptables");
        if (symlink(multi, APP_DIR "/bin/iptables") == 0) {
            str_copy(out, n, APP_DIR "/bin/iptables");
            return;
        }
        str_copy(out, n, multi);
        return;
    }
    str_copy(out, n, "iptables");
}

static void fw_open(int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (sh("%s -C INPUT -i wlan0 -p tcp --dport %d -j ACCEPT 2>/dev/null",
           tool, port) != 0)
        sh("%s -I INPUT -i wlan0 -p tcp --dport %d -j ACCEPT 2>/dev/null",
           tool, port);
}

static void fw_close(int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    sh("%s -D INPUT -i wlan0 -p tcp --dport %d -j ACCEPT 2>/dev/null",
       tool, port);
}

static void fw6_tool(char *out, size_t n)
{
    if (file_exists("/usr/sbin/ip6tables")) {
        str_copy(out, n, "/usr/sbin/ip6tables");
        return;
    }
    if (file_exists("/sbin/ip6tables")) {
        str_copy(out, n, "/sbin/ip6tables");
        return;
    }
    const char *multi = "/usr/sbin/xtables-multi";
    if (file_exists(multi)) {
        mkdir(APP_DIR "/bin", 0755);
        unlink(APP_DIR "/bin/ip6tables");
        if (symlink(multi, APP_DIR "/bin/ip6tables") == 0) {
            str_copy(out, n, APP_DIR "/bin/ip6tables");
            return;
        }
        str_copy(out, n, multi);
        return;
    }
    str_copy(out, n, "ip6tables");
}

static void fw6_port_add(const char *proto, int port)
{
    char tool[256];
    fw6_tool(tool, sizeof(tool));
    if (sh("%s -C INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
           tool, proto, port) != 0)
        sh("%s -I INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
           tool, proto, port);
}

static void fw6_port_del(const char *proto, int port)
{
    char tool[256];
    fw6_tool(tool, sizeof(tool));
    sh("%s -D INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
       tool, proto, port);
}

static void fw_port_add(const char *proto, int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (sh("%s -C INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
           tool, proto, port) != 0)
        sh("%s -I INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
           tool, proto, port);
}

static void fw_port_del(const char *proto, int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    sh("%s -D INPUT -i wlan0 -p %s --dport %d -j ACCEPT 2>/dev/null",
       tool, proto, port);
}

static void carplay_fw_open(void)
{
    /* AirPlay / CarPlay / DHCP / mDNS on the AP interface.  The stock
     * firewall only opens these on ncm0 (USB CarPlay); wireless needs
     * its own wlan0 rules. */
    fw_port_add("udp", 67);
    fw_port_add("udp", 5353);
    fw_port_add("udp", 5020);
    fw_port_add("udp", 6000);
    fw_port_add("udp", 6003);
    fw_port_add("udp", 6011);
    fw_port_add("tcp", 5000);
    fw_port_add("tcp", 5001);
    fw_port_add("tcp", 6030);
    fw_port_add("tcp", 7000);

    /* mDNS and AirPlay also arrive over IPv6 link-local. */
    fw6_port_add("udp", 5353);
    fw6_port_add("tcp", 5000);
    fw6_port_add("tcp", 5001);
    fw6_port_add("tcp", 7000);
    fw6_port_add("udp", 6000);
    fw6_port_add("udp", 6003);
    fw6_port_add("udp", 6011);
}

static void carplay_fw_close(void)
{
    fw_port_del("udp", 67);
    fw_port_del("udp", 5353);
    fw_port_del("udp", 5020);
    fw_port_del("udp", 6000);
    fw_port_del("udp", 6003);
    fw_port_del("udp", 6011);
    fw_port_del("tcp", 5000);
    fw_port_del("tcp", 5001);
    fw_port_del("tcp", 6030);
    fw_port_del("tcp", 7000);

    fw6_port_del("udp", 5353);
    fw6_port_del("tcp", 5000);
    fw6_port_del("tcp", 5001);
    fw6_port_del("tcp", 7000);
    fw6_port_del("udp", 6000);
    fw6_port_del("udp", 6003);
    fw6_port_del("udp", 6011);
}

static bool line_prefix_present(const char *path, const char *prefix)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    size_t n = strlen(prefix);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, prefix, n)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static int append_line(const char *path, const char *line)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        char last = 0;
        if (pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n')
            (void)write(fd, "\n", 1);
    }
    (void)write(fd, line, strlen(line));
    (void)write(fd, "\n", 1);
    close(fd);
    return 0;
}

static bool ensure_line(const char *path, const char *prefix, const char *line)
{
    if (line_prefix_present(path, prefix)) return true;
    return append_line(path, line) == 0;
}

/*
 * /etc/passwd is a symlink to /tmp/passwd on this firmware.  esinit
 * rewrites /tmp/passwd from /etc/passwd.legacy or /etc/passwd.secure on
 * every boot, so the user must live in those templates too.  This guard
 * covers the current boot and any template the installer missed.
 */
static bool ensure_car_user(void)
{
    bool ok = true;

    if (!ensure_line("/etc/passwd", "car:", CAR_PASSWD_LINE))
        ok = false;

    const char *pw_templates[] = {
        "/etc/passwd.legacy",
        "/etc/passwd.secure",
        NULL
    };
    for (int i = 0; pw_templates[i]; i++) {
        if (file_exists(pw_templates[i]))
            (void)ensure_line(pw_templates[i], "car:", CAR_PASSWD_LINE);
    }

    const char *shadow = file_exists("/etc/shadow.Global")
                         ? "/etc/shadow.Global" : "/etc/shadow";
    if (!ensure_line(shadow, "car:", CAR_SHADOW_LINE))
        ok = false;
    if (strcmp(shadow, "/etc/shadow") && file_exists("/etc/shadow"))
        (void)ensure_line("/etc/shadow", "car:", CAR_SHADOW_LINE);

    if (file_exists("/tmp/shells"))
        (void)ensure_line("/tmp/shells", "/bin/sh", "/bin/sh");

    if (!ok)
        log_msg("ensure_car_user: could not persist car user");
    else
        log_msg("ensure_car_user: car user present");
    return ok;
}

static bool port_listening(int port)
{
    const char *files[] = { "/proc/net/tcp", "/proc/net/tcp6", NULL };
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) continue;
        char line[512];
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            continue;
        }
        while (fgets(line, sizeof(line), f)) {
            char local[128], remote[128];
            unsigned st = 0;
            if (sscanf(line, " %*d: %127s %127s %x", local, remote, &st) != 3)
                continue;
            char *colon = strrchr(local, ':');
            if (!colon) continue;
            int p = (int)strtol(colon + 1, NULL, 16);
            if (p == port && st == 0x0A) {
                fclose(f);
                return true;
            }
        }
        fclose(f);
    }
    return false;
}

static int choose_ssh_port(int desired)
{
    if (desired <= 0 || desired > 65535) desired = SSH_DEFAULT_PORT;
    if (!port_listening(desired)) return desired;
    for (int p = SSH_DEFAULT_PORT; p <= SSH_FALLBACK_LAST; p++) {
        if (p == desired) continue;
        if (!port_listening(p)) return p;
    }
    return 0;
}

static bool pid_is_our_dropbear(int pid)
{
    if (pid <= 1) return false;
    char path[64], cmd[2048];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int n = read_file(path, cmd, sizeof(cmd));
    if (n <= 0) return false;
    for (int i = 0; i < n; i++)
        if (cmd[i] == 0) cmd[i] = ' ';
    if (!strstr(cmd, "dropbear")) return false;
    return strstr(cmd, DROPBEAR_BIN) || strstr(cmd, DROPBEAR_MULTI);
}

static bool ssh_running(void)
{
    char buf[64];
    if (read_file(SSH_PID, buf, sizeof(buf)) > 0) {
        int pid = atoi(buf);
        if (pid_is_our_dropbear(pid)) return true;
    }
    if (pid_by_cmdline(DROPBEAR_BIN) > 0) return true;
    if (pid_by_cmdline(DROPBEAR_MULTI) > 0) return true;
    return false;
}

static void ssh_stop(void)
{
    char buf[64];
    if (read_file(SSH_PID, buf, sizeof(buf)) > 0) {
        int pid = atoi(buf);
        if (pid_is_our_dropbear(pid))
            kill_pid(pid);
    }
    int pid = pid_by_cmdline(DROPBEAR_BIN);
    if (pid > 0) kill_pid(pid);
    pid = pid_by_cmdline(DROPBEAR_MULTI);
    if (pid > 0) kill_pid(pid);
    if (g_ssh_active_port) {
        fw_close(g_ssh_active_port);
        g_ssh_active_port = 0;
    } else {
        fw_close(g_cfg.ssh_port);
    }
    g_ssh_port_fallback = false;
    g_ssh_bound_ip[0] = 0;
    unlink(SSH_PID);
    unlink(SSH_BOUND);
}

static bool ssh_start(void)
{
    if (!file_exists(DROPBEAR_BIN) && !file_exists(DROPBEAR_MULTI)) {
        set_msg("dropbear binary missing");
        return false;
    }
    char ip[64];
    if (!wlan_ip(ip, sizeof(ip))) {
        set_msg("SSH deferred: no WLAN IP");
        return false;
    }
    if (ssh_running()) {
        ensure_car_user();
        if (g_ssh_bound_ip[0] && !strcmp(g_ssh_bound_ip, ip))
            return true;
        /* The WLAN address changed, or the daemon restarted; rebind. */
        ssh_stop();
    }
    ensure_car_user();

    int chosen = choose_ssh_port(g_cfg.ssh_port);
    if (!chosen) {
        set_msg("SSH failed: no free port");
        return false;
    }
    if (chosen != g_cfg.ssh_port) {
        set_msg("SSH port %d busy; using %d", g_cfg.ssh_port, chosen);
        g_cfg.ssh_port = chosen;
        g_ssh_port_fallback = true;
        config_save();
    }

    mkdir(SSH_DIR, 0700);
    mkdir(SSH_HOME, 0700);
    mkdir(SSH_HOME "/.ssh", 0700);

    if (file_exists(SSH_KEYS)) {
        char keys[16384];
        if (read_file(SSH_KEYS, keys, sizeof(keys)) > 0) {
            int fd = open(SSH_HOME "/.ssh/authorized_keys",
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (fd >= 0) {
                write(fd, keys, strlen(keys));
                close(fd);
                sh("sort -u " SSH_HOME "/.ssh/authorized_keys -o " SSH_HOME "/.ssh/authorized_keys 2>/dev/null");
            }
        }
    }

    g_ssh_active_port = g_cfg.ssh_port;
    fw_open(g_ssh_active_port);

    const char *bin = file_exists(DROPBEAR_BIN) ? DROPBEAR_BIN : DROPBEAR_MULTI;
    const char *keygen = file_exists(DROPBEAR_KEY) ? DROPBEAR_KEY : NULL;
    char edkey[512], rsakey[512];
    snprintf(edkey, sizeof(edkey), SSH_DIR "/dropbear_ed25519_host_key");
    snprintf(rsakey, sizeof(rsakey), SSH_DIR "/dropbear_rsa_host_key");

    if (keygen) {
        if (!file_exists(edkey))
            sh("%s -t ed25519 -f %s >> " APP_DIR "/ssh.log 2>&1", keygen, edkey);
        if (!file_exists(rsakey))
            sh("%s -t rsa -s 2048 -f %s >> " APP_DIR "/ssh.log 2>&1", keygen, rsakey);
    }

    if (file_exists(edkey) && file_exists(rsakey)) {
        sh("%s -r %s -r %s -p %s:%d -P %s >> " APP_DIR "/ssh.log 2>&1 &",
           bin, edkey, rsakey, ip, g_cfg.ssh_port, SSH_PID);
    } else {
        /* Fallback: let dropbear create its default host keys. */
        sh("%s -R -p %s:%d -P %s >> " APP_DIR "/ssh.log 2>&1 &",
           bin, ip, g_cfg.ssh_port, SSH_PID);
    }
    usleep(400000);
    if (ssh_running()) {
        str_copy(g_ssh_bound_ip, sizeof(g_ssh_bound_ip), ip);
        write_file_atomic(SSH_BOUND, ip, 0600);
        set_msg("SSH started on port %d (user car)", g_cfg.ssh_port);
        return true;
    }
    fw_close(g_ssh_active_port);
    g_ssh_active_port = 0;
    set_msg("SSH failed to start on port %d", g_cfg.ssh_port);
    return false;
}

/* ------------------------------------------------------------------ */
/* mode application                                                    */
/* ------------------------------------------------------------------ */

static void stop_all(void)
{
    ssh_stop();
    ap_stop();
    client_disconnect();
    sh("connmanctl disable wifi 2>/dev/null");
    stop_wpa_supplicant();
    iface_down();
}

static const char *mode_name(void);

/* Runs in the apply child.  All slow network work lives here so the HTTP
 * loop and status polling stay responsive. */
static bool apply_mode_work(bool pair)
{
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        stop_all();
        set_msg("WLAN off");
        config_save();
        return true;
    }

    if (!ensure_wifi_ready())
        return false;

    if (g_cfg.mode == MODE_CLIENT) {
        ap_stop();
        carplay_ie_off();
        bool ok = client_connect();
        /* Capture the real association/DHCP error before ssh_start() can
         * replace g_last_msg with "SSH deferred: no WLAN IP".  That
         * overwrite made every client failure look like an SSH problem in
         * the on-car log. */
        char why[256] = "";
        if (!ok) str_copy(why, sizeof(why), g_last_msg);
        if (g_cfg.ssh_enabled) ssh_start();
        if (ok) set_msg("client mode: %s", g_cfg.ssid);
        else set_msg("client mode failed: %s", why);
        config_save();
        return ok;
    }

    /* MODE_CARPLAY */
    if (pair)
        bt_pair_start();
    client_disconnect();
    if (!ap_start()) {
        set_msg("CarPlay AP failed to start (SSID=%s)", g_cfg.cp_ssid);
        config_save();
        return false;
    }
    carplay_fw_open();
    bool ie_ok = carplay_ie_on();
    ensure_carplay_services();

    bool vbt_changed = false;
    vbt_set_wireless_carplay(true, &vbt_changed);
    restart_vbtd_if_needed(vbt_changed);

    if (g_cfg.ssh_enabled) ssh_start();
    char ap_ssid[80] = {0};
    tethering_on(ap_ssid, sizeof(ap_ssid));
    set_msg("CarPlay AP running (SSID=%s, %d MHz)%s",
            ap_ssid[0] ? ap_ssid : g_cfg.cp_ssid, channel_to_freq(),
            ie_ok ? "" : " - CarPlay IE not set");
    config_save();
    return true;
}

/* Cancel an in-flight apply: the newest user intent wins. */
static void apply_cancel(void)
{
    if (g_apply_pid <= 0) return;
    kill(g_apply_pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        pid_t r = waitpid(g_apply_pid, NULL, WNOHANG);
        if (r == g_apply_pid || (r < 0 && errno == ECHILD)) {
            g_apply_pid = 0;
            return;
        }
        usleep(100000);
    }
    kill(g_apply_pid, SIGKILL);
    waitpid(g_apply_pid, NULL, 0);
    g_apply_pid = 0;
}

/* Reap the apply child and fold its result into the live state. */
static void apply_reap(void)
{
    if (g_apply_pid <= 0) return;
    int st = 0;
    pid_t r = waitpid(g_apply_pid, &st, WNOHANG);
    if (r == 0) {
        /* Client mode can spend ~20 s waiting for the supplicant, ~8 s
         * finding the ConnMan service, ~9 s in the agent flow, then the
         * wpa_cli fallback and DHCP waits.  150 s covers that path; a new
         * mode request cancels the child immediately. */
        if (now_ms() - g_apply_start_ms > 150000) {
            log_msg("apply: timed out; killing");
            kill(g_apply_pid, SIGKILL);
            waitpid(g_apply_pid, NULL, 0);
            g_apply_pid = 0;
            g_apply_failed = true;
            set_msg("apply timed out");
            write_status();
        }
        return;
    }
    if (r < 0) {
        if (errno == ECHILD) g_apply_pid = 0;
        return;
    }
    g_apply_pid = 0;

    char buf[512] = "";
    int n = read_file(APPLY_RESULT, buf, sizeof(buf));
    unlink(APPLY_RESULT);
    bool ok = false;
    char msg[256] = "apply finished";
    if (n > 0) {
        char *nl = strchr(buf, '\n');
        if (nl) {
            ok = atoi(buf) != 0;
            str_copy(msg, sizeof(msg), trim(nl + 1));
        }
    }
    g_apply_failed = !ok;
    set_msg("%s", msg);
    log_msg("apply %s: %s", ok ? "ok" : "failed", msg);
    write_status();
}

static void apply_mode(bool pair)
{
    apply_cancel();
    g_last_apply_ms = now_ms();
    g_apply_failed = false;
    unlink(APPLY_RESULT);
    set_msg("applying %s...", mode_name());
    write_status();

    pid_t pid = fork();
    if (pid == 0) {
        g_in_child = true;
        /* Do not hold the HTTP listen/connection sockets open while the
         * apply runs; otherwise the client connection would stay half-open
         * until the apply finishes. */
        for (int fd = 3; fd < 256; fd++) close(fd);
        bool ok = apply_mode_work(pair);
        char msg[256], buf[512];
        str_copy(msg, sizeof(msg), g_last_msg);
        snprintf(buf, sizeof(buf), "%d\n%s\n", ok ? 1 : 0, msg);
        write_file_atomic(APPLY_RESULT, buf, 0600);
        _exit(0);
    }
    if (pid < 0) {
        g_apply_failed = true;
        set_msg("apply: fork failed");
        return;
    }
    g_apply_pid = pid;
    g_apply_start_ms = now_ms();
}

static bool dump_logs_to_usb(bool verbose);

static void watchdog(void)
{
    int64_t now = now_ms();
    if (now - g_last_watchdog_ms < 10000) return;
    g_last_watchdog_ms = now;

    if (pid_by_comm("supplicantmanager") > 0) {
        log_msg("watchdog: OEM supplicantmanager respawned; stopping it");
        stop_oem_wifi();
    }

    /* Log dumps: one attempt ~15 s after start (captures the install), and
     * a throttled dump while applies keep failing so the failure state
     * lands on the stick without anyone pressing a button. */
    if (!g_boot_dumped && g_start_ms && now - g_start_ms > 15000 &&
        (!g_last_dump_try_ms || now - g_last_dump_try_ms > 60000)) {
        g_last_dump_try_ms = now;
        if (dump_logs_to_usb(false))
            g_boot_dumped = true;
    }
    if (g_apply_failed &&
        (!g_last_autodump_ms || now - g_last_autodump_ms > 600000))
        dump_logs_to_usb(false);

    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) return;

    if (g_apply_pid == 0 && !wpa_ping()) {
        log_msg("watchdog: wpa_supplicant missing; restarting WiFi stack");
        ensure_wifi_ready();
    }

    int64_t retry_ms = g_apply_failed ? 60000 : 15000;
    if (g_apply_pid == 0 &&
        g_cfg.mode == MODE_CARPLAY && !tethering_on(NULL, 0)) {
        if (now - g_last_apply_ms > retry_ms) {
            log_msg("watchdog: CarPlay AP down; re-applying (backoff=%ds)",
                    g_apply_failed ? 60 : 15);
            apply_mode(false);
        }
    } else if (g_apply_pid == 0 && g_cfg.mode == MODE_CLIENT) {
        char ip[64];
        if (!wlan_ip(ip, sizeof(ip)) && now - g_last_apply_ms > retry_ms + 5000) {
            log_msg("watchdog: client has no IP; re-applying (backoff=%ds)",
                    g_apply_failed ? 60 : 15);
            apply_mode(false);
        }
    }
    if (g_cfg.ssh_enabled) {
        char ip[64];
        if (wlan_ip(ip, sizeof(ip))) {
            if (!ssh_running()) {
                log_msg("watchdog: SSH down; restarting");
                ssh_start();
            } else {
                char bound[64] = "";
                if (read_file(SSH_BOUND, bound, sizeof(bound)) > 0) {
                    char *b = trim(bound);
                    if (*b && strcmp(b, ip)) {
                        /* dropbear is bound to the old address; rebind. */
                        log_msg("watchdog: wlan IP changed %s -> %s; rebinding SSH",
                                b, ip);
                        ssh_stop();
                        ssh_start();
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

static const char *mode_name(void)
{
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) return "off";
    return g_cfg.mode == MODE_CARPLAY ? "carplay" : "client";
}

static void build_status(char *out, size_t n)
{
    if (!g_in_child)
        bt_state_load();
    char ip[64] = "";
    char ssid[80] = "";
    int signal = 0;
    char ap_ssid[80] = "";
    bool ap_on = tethering_on(ap_ssid, sizeof(ap_ssid));
    int clients = ap_on ? ap_clients() : 0;
    bool connected = wlan_ip(ip, sizeof(ip));
    bool carplay_svc = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0;
    bool mdns_svc = sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") == 0;
    bool ssh_ours = ssh_running();
    bool vendor_ssh_22 = port_listening(22) && !ssh_ours;
    bool debug_console = file_exists("/tmp/secure_console_disable");
    int secure_console = -1;
    char scbuf[32];
    if (read_file("/sys/module/micom/parameters/secure_console", scbuf, sizeof(scbuf)) > 0)
        secure_console = atoi(trim(scbuf));
    int active_ssh_port = g_ssh_active_port ? g_ssh_active_port : g_cfg.ssh_port;

    if (connected) wlan_link(ssid, sizeof(ssid), &signal);

    char ssid_e[256], ip_e[128], ap_e[256], msg_e[512];
    char imp_e[512], cps_e[256], cpsk_e[256], bt_e[128], id_e[128];
    char client_e[256], pair_e[512];
    json_escape(ssid_e, sizeof(ssid_e), ssid);
    json_escape(ip_e, sizeof(ip_e), ip);
    json_escape(ap_e, sizeof(ap_e), ap_ssid);
    json_escape(msg_e, sizeof(msg_e), g_last_msg);
    json_escape(imp_e, sizeof(imp_e), g_cfg.imported_from);
    json_escape(cps_e, sizeof(cps_e), g_cfg.cp_ssid);
    json_escape(cpsk_e, sizeof(cpsk_e), g_cfg.cp_psk);
    json_escape(bt_e, sizeof(bt_e), g_cfg.cp_btaddr);
    json_escape(id_e, sizeof(id_e), g_cfg.cp_id);
    json_escape(client_e, sizeof(client_e), g_cfg.ssid);
    json_escape(pair_e, sizeof(pair_e), g_pair_msg);

    snprintf(out, n,
             "{\"ok\":true,"
             "\"mode\":\"%s\","
             "\"applying\":%s,"
             "\"home_seq\":%d,"
             "\"home_epoch\":%ld,"
             "\"home_source\":\"%s\","
             "\"last_app_key\":%d,"
             "\"home_monitor\":%s,"
             "\"wlan_on\":%s,"
             "\"driver\":%s,"
             "\"iface\":\"wlan0\","
             "\"ip\":\"%s\","
             "\"ssid\":\"%s\","
             "\"signal\":%d,"
             "\"ap_active\":%s,"
             "\"ap_ssid\":\"%s\","
             "\"ap_clients\":%d,"
             "\"carplay_ready\":%s,"
             "\"carplay_iap2\":%s,"
             "\"carplay_mdns\":%s,"
             "\"ssh_active\":%s,"
             "\"ssh_enabled\":%s,"
             "\"ssh_port\":%d,"
             "\"ssh_port_fallback\":%s,"
             "\"vendor_ssh_22\":%s,"
             "\"secure_console\":%d,"
             "\"debug_console\":%s,"
             "\"imported\":%s,"
             "\"imported_from\":\"%s\","
             "\"client_ssid\":\"%s\","
             "\"carplay_ssid\":\"%s\","
             "\"carplay_psk\":\"%s\","
             "\"carplay_psk_set\":%s,"
             "\"carplay_band\":%d,"
             "\"carplay_channel\":%d,"
             "\"carplay_btaddr\":\"%s\","
             "\"carplay_id\":\"%s\","
             "\"bt_discoverable\":%s,"
             "\"bt_pairable\":%s,"
             "\"pair_message\":\"%s\","
             "\"message\":\"%s\"}\n",
             mode_name(),
             g_apply_pid > 0 ? "true" : "false",
             g_home_seq, g_home_epoch, g_home_source, g_home_last_key,
             g_home_monitor ? "true" : "false",
             g_cfg.wlan_on ? "true" : "false",
             iface_present() ? "true" : "false",
             ip_e, ssid_e, signal,
             ap_on ? "true" : "false", ap_e, clients,
             (ap_on && (carplay_svc || mdns_svc)) ? "true" : "false",
             carplay_svc ? "true" : "false",
             mdns_svc ? "true" : "false",
             ssh_ours ? "true" : "false",
             g_cfg.ssh_enabled ? "true" : "false",
             active_ssh_port,
             g_ssh_port_fallback ? "true" : "false",
             vendor_ssh_22 ? "true" : "false",
             secure_console,
             debug_console ? "true" : "false",
             g_cfg.imported ? "true" : "false", imp_e,
             client_e, cps_e, cpsk_e, g_cfg.cp_psk[0] ? "true" : "false",
             g_cfg.cp_band, g_cfg.cp_channel, bt_e, id_e,
             g_bt_discoverable ? "true" : "false",
             g_bt_pairable ? "true" : "false",
             pair_e, msg_e);
}

static void write_status(void)
{
    if (g_in_child) return;
    char buf[8192];
    build_status(buf, sizeof(buf));
    write_file_atomic(g_status_path, buf, 0600);
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

static void write_status(void);

static void scan_networks(char *out, size_t n)
{
    apply_cancel();

    /* Scanning must not change the configured mode: the client-mode
     * watchdog would auto-connect without the user asking for it. */
    if (g_cfg.mode == MODE_CARPLAY && tethering_on(NULL, 0)) {
        snprintf(out, n,
                 "{\"ok\":false,\"error\":\"Stop the CarPlay AP before scanning\"}\n");
        return;
    }

    set_msg("Scanning...");
    write_status();
    if (!ensure_wifi_ready()) {
        char msg_e[512];
        json_escape(msg_e, sizeof(msg_e), g_last_msg);
        snprintf(out, n, "{\"ok\":false,\"error\":\"%s\"}\n", msg_e);
        write_status();
        return;
    }
    sh("connmanctl enable wifi 2>/dev/null");
    iface_up();

    char buf[32768];
    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan >/dev/null 2>&1");
        sleep(3);
        rc = sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results 2>/dev/null");
        if (rc == 0 && strchr(buf, '\t'))
            break;
    }
    size_t used = 0;
    int count = 0;
    used += (size_t)snprintf(out + used, n - used, "{\"ok\":true,\"networks\":[");

    if (rc == 0 && buf[0]) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (!strchr(line, '\t')) continue;
            char *fields[6] = {0};
            int nf = 0;
            char *p = line;
            for (; nf < 6; nf++) {
                fields[nf] = p;
                char *tab = strchr(p, '\t');
                if (!tab) break;
                *tab = 0;
                p = tab + 1;
            }
            if (nf < 4) continue;
            if (!strncmp(fields[0], "bssid", 5)) continue;
            const char *ssid = fields[4];
            if (!ssid[0]) continue;
            int sig = atoi(fields[2]);
            const char *flags = fields[3];
            const char *sec = strstr(flags, "WPA2") ? "WPA2" :
                              strstr(flags, "WPA") ? "WPA" :
                              strstr(flags, "WEP") ? "WEP" : "Open";
            char ssid_e[256], sec_e[64];
            json_escape(ssid_e, sizeof(ssid_e), ssid);
            json_escape(sec_e, sizeof(sec_e), sec);
            if (count) used += (size_t)snprintf(out + used, n - used, ",");
            used += (size_t)snprintf(out + used, n - used,
                                     "{\"ssid\":\"%s\",\"signal\":%d,\"security\":\"%s\"}",
                                     ssid_e, sig, sec_e);
            count++;
            if (used > n - 512) break;
        }
    }

    if (!count) {
        /* Fallback: ConnMan services (no signal). */
        sh("connmanctl scan wifi 2>/dev/null");
        sleep(3);
        char svc[16384];
        if (sh_out(svc, sizeof(svc), "connmanctl services 2>/dev/null") == 0) {
            char *save = NULL;
            for (char *line = strtok_r(svc, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                char *service = strstr(line, "wifi_");
                if (!service) continue;

                char *ssid = line;
                while (*ssid && isspace((unsigned char)*ssid)) ssid++;
                if (*ssid == '*') {
                    while (*ssid && !isspace((unsigned char)*ssid)) ssid++;
                    while (*ssid && isspace((unsigned char)*ssid)) ssid++;
                }
                char *end = service;
                while (end > ssid && isspace((unsigned char)end[-1])) end--;
                if (end <= ssid) continue;
                char saved = *end;
                *end = 0;

                char ssid_e[256];
                json_escape(ssid_e, sizeof(ssid_e), ssid);
                if (count) used += (size_t)snprintf(out + used, n - used, ",");
                used += (size_t)snprintf(out + used, n - used,
                                         "{\"ssid\":\"%s\",\"signal\":0,\"security\":\"?\"}",
                                         ssid_e);
                *end = saved;
                count++;
                if (used > n - 512) break;
            }
        }
    }

    snprintf(out + used, n - used, "]}\n");
    if (count)
        set_msg("scan: %d network(s) found", count);
    else
        set_msg("scan: no networks found");
    write_status();
}

/* ------------------------------------------------------------------ */
/* log dump to USB                                                     */
/* ------------------------------------------------------------------ */

/* Find the mounted USB stick via /proc/mounts.  Only real mount points
 * (vfat/exfat/ntfs under /media/usb, /run/media or /mnt) qualify, so we
 * never write into the automounter's own directory.  WIFI_USB_ROOT
 * overrides everything (used by the offline tests). */
static bool is_media_mount(const char *mp)
{
    return !strncmp(mp, "/media/usb", 10) ||
           !strncmp(mp, "/media/", 7) ||
           !strncmp(mp, "/run/media", 10);
}

static bool is_removable_dev(const char *dev)
{
    return !strncmp(dev, "/dev/sd", 7) ||
           !strncmp(dev, "/dev/mmcblk", 11) ||
           !strncmp(dev, "/dev/sr", 7);
}

static bool find_usb_dir(char *out, size_t n)
{
    if (out && n) out[0] = 0;
    const char *env = getenv("WIFI_USB_ROOT");
    if (env && *env) {
        str_copy(out, n, env);
        return true;
    }

    /* No fstype whitelist: the car's exFAT is Tuxera texfat, and FUSE
     * mounts show up as fuseblk/fuse.exfat.  Matching real mount points
     * from /proc/mounts (not globbing directories) already avoids the
     * automounter's own workspace.  Prefer removable block devices. */
    const char *mounts_env = getenv("WIFI_MOUNTS_FILE");
    const char *mounts = (mounts_env && *mounts_env) ? mounts_env : "/proc/mounts";
    bool test_mounts = mounts_env && *mounts_env;
    char buf[16384];
    if (read_file(mounts, buf, sizeof(buf)) <= 0) return false;

    for (int pass = 0; pass < 2; pass++) {
        char copy[16384];
        str_copy(copy, sizeof(copy), buf);
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char dev[256], mp[512];
            if (sscanf(line, "%255s %511s", dev, mp) != 2) continue;
            if (!test_mounts && !is_media_mount(mp)) continue;
            if (pass == 0 && !is_removable_dev(dev)) continue;
            struct stat st;
            if (stat(mp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            str_copy(out, n, mp);
            return true;
        }
    }
    return false;
}

static void copy_if_exists(const char *dst_dir, const char *src)
{
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    if (!file_exists(src)) return;
    sh("cp -f %s %s/%s 2>/dev/null", src, dst_dir, base);
}

/* config.conf with the client and CarPlay keys removed. */
static void copy_config_redacted(const char *dst_dir)
{
    char buf[8192];
    if (read_file(CONFIG_PATH, buf, sizeof(buf)) <= 0) return;

    char out[16384];
    size_t j = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        const char *emit = line;
        if (!strncmp(line, "psk=", 4)) emit = "psk=<redacted>";
        else if (!strncmp(line, "carplay_psk=", 12)) emit = "carplay_psk=<redacted>";
        size_t len = strlen(emit);
        if (j + len + 2 >= sizeof(out)) break;
        memcpy(out + j, emit, len);
        j += len;
        out[j++] = '\n';
    }
    out[j] = 0;
    char dst[512];
    snprintf(dst, sizeof(dst), "%s/config.conf", dst_dir);
    write_file_atomic(dst, out, 0644);
}

static void snap_cmd(FILE *f, const char *cmd)
{
    char out[16384];
    fprintf(f, "\n$ %s\n", cmd);
    sh_out(out, sizeof(out), "%s 2>&1", cmd);
    fputs(out, f);
    fprintf(f, "\n");
}

/* Keep only the newest LOG_DUMP_KEEP bundles; stamps are fixed width so
 * a lexical sort is a chronological sort. */
#define LOG_DUMP_KEEP 5

/* A prune candidate must be exactly a bundle this daemon itself wrote:
 * the "wifi_logs_" prefix plus the fixed-width %Y%m%d%H%M%S stamp and
 * nothing else.  Whatever else a stick carries — however it is named —
 * is data, never a command (rule 9): not a candidate, not deleted. */
static bool is_pruneable_name(const char *name)
{
    if (!name || strncmp(name, "wifi_logs_", 10)) return false;
    const char *stamp = name + 10;
    for (int i = 0; i < 14; i++)
        if (!isdigit((unsigned char)stamp[i])) return false;
    return stamp[14] == '\0';
}

/* rm -rf without a shell: lstat first so a symlink is unlinked, never
 * followed; depth-capped so a pathological tree cannot recurse without
 * bound.  Returns false on any refusal — the caller logs and keeps it. */
#define PRUNE_MAX_DEPTH 12
static bool safe_rmtree(const char *path, int depth)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth >= PRUNE_MAX_DEPTH) return false;
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[768];
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name)
            >= (int)sizeof(child)) { ok = false; continue; }
        if (!safe_rmtree(child, depth + 1)) ok = false;
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

static void prune_old_dumps(const char *usb)
{
    DIR *d = opendir(usb);
    if (!d) return;
    char names[16][40];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) && count < 16) {
        if (!is_pruneable_name(de->d_name)) continue;
        size_t len = strlen(de->d_name);
        if (len >= sizeof(names[0])) continue;
        memcpy(names[count], de->d_name, len + 1);
        count++;
    }
    closedir(d);

    for (int i = 1; i < count; i++) {
        char key[40];
        memcpy(key, names[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            memcpy(names[j + 1], names[j], sizeof(names[0]));
            j--;
        }
        memcpy(names[j + 1], key, sizeof(names[0]));
    }
    for (int i = 0; i + LOG_DUMP_KEEP < count; i++) {
        char path[600];
        if (snprintf(path, sizeof(path), "%s/%s", usb, names[i])
            >= (int)sizeof(path)) {
            log_msg("prune: path too long for %s", names[i]);
            continue;
        }
        if (!safe_rmtree(path, 0))
            log_msg("prune: cannot remove %s", path);
    }
}

static bool dump_logs_to_usb(bool verbose)
{
    char usb[512];
    if (!find_usb_dir(usb, sizeof(usb))) {
        log_msg("log dump: no USB mount found");
        if (verbose) set_msg("log dump: no USB stick found");
        return false;
    }
    sh("mount -o remount,rw %s 2>/dev/null", usb);

    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);

    char dir[768];
    snprintf(dir, sizeof(dir), "%s/wifi_logs_%s", usb, stamp);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        log_msg("log dump: mkdir %s failed: %s", dir, strerror(errno));
        if (verbose) set_msg("log dump failed: %s", strerror(errno));
        return false;
    }

    copy_if_exists(dir, LOG_PATH);
    copy_if_exists(dir, WPA_LOG);
    copy_if_exists(dir, APP_DIR "/ssh.log");
    copy_if_exists(dir, APP_DIR "/install.log");
    copy_if_exists(dir, g_status_path);
    copy_if_exists(dir, HOME_STATE);
    copy_config_redacted(dir);

    char state[800];
    snprintf(state, sizeof(state), "%s/state.txt", dir);
    FILE *f = fopen(state, "w");
    if (f) {
        fprintf(f, "wifi log dump %s\n", stamp);
        snap_cmd(f, "uname -a");
        snap_cmd(f, "cat /proc/mounts");
        snap_cmd(f, "date");
        snap_cmd(f, "ps w | grep -E 'wpa_supplicant|connmand|supplicantmanager|wifi_daemon|dropbear|vbtd' | grep -v grep");
        snap_cmd(f, "connmanctl technologies");
        snap_cmd(f, "connmanctl services");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results");
        snap_cmd(f, "ls -la " WPA_CTRL_DIR);
        snap_cmd(f, "getfattr -n security.SMACK64 /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti");
        snap_cmd(f, "getfattr -n security.SMACK64EXEC /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti");
        snap_cmd(f, "systemctl is-enabled supplicantmanager.path supplicantmanager.service");
        snap_cmd(f, "iw dev wlan0 info");
        snap_cmd(f, "iw dev wlan0 link");
        snap_cmd(f, "ifconfig wlan0");
        snap_cmd(f, "iptables -S");
        snap_cmd(f, "dmesg | tail -n 200");
        snap_cmd(f, "grep -n ENABLE_WIRELESS_CARPLAY /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json");
        fclose(f);
    }

    prune_old_dumps(usb);

    g_last_autodump_ms = now_ms();
    log_msg("log dump written to %s", dir);
    if (verbose)
        set_msg("logs saved to %s", dir);
    return true;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static void http_reply(int fd, int code, const char *body)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, code == 200 ? "OK" : "Error", strlen(body));
    if (n > 0) (void)write(fd, hdr, (size_t)n);
    (void)write(fd, body, strlen(body));
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static bool query_get(const char *query, const char *name, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    if (!query) return false;
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq && (size_t)(eq - p) == nlen && !strncmp(p, name, nlen)) {
            size_t vlen = len - (size_t)(eq - p) - 1;
            char tmp[512];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, eq + 1, vlen);
            tmp[vlen] = 0;
            url_decode(tmp);
            str_copy(out, n, tmp);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static void http_handle(int fd)
{
    char req[4096];
    ssize_t nr = read(fd, req, sizeof(req) - 1);
    if (nr <= 0) { close(fd); return; }
    req[nr] = 0;

    char method[8] = {0}, path[256] = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        http_reply(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}\n");
        close(fd);
        return;
    }
    char *query = strchr(path, '?');
    if (query) *query++ = 0;

    char body[32768];
    int code = 200;
    bool changed = false;

    if (!strcmp(method, "GET") && (!strcmp(path, "/status") || !strcmp(path, "/"))) {
        build_status(body, sizeof(body));
    } else if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/scan")) {
        scan_networks(body, sizeof(body));
    } else if (!strcmp(method, "GET") && !strcmp(path, "/logs")) {
        char src[64] = "";
        const char *log_path = LOG_PATH;
        if (query_get(query, "source", src, sizeof(src)) &&
            (!strcmp(src, "wpa") || !strcmp(src, "wpa_supplicant")))
            log_path = WPA_LOG;
        else if (!strcmp(src, "ssh"))
            log_path = APP_DIR "/ssh.log";
        char log[16384];
        int n = read_tail(log_path, log, sizeof(log));
        if (n < 0) n = 0;
        char esc[32768];
        json_escape(esc, sizeof(esc), log);
        snprintf(body, sizeof(body), "{\"ok\":true,\"log\":\"%s\"}\n", esc);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/import")) {
        bool ok = import_wifi();
        if (ok && g_cfg.wlan_on) { apply_mode(false); changed = true; }
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"ssid\":\"%s\",\"carplay_ssid\":\"%s\",\"from\":\"%s\"}\n",
                 ok ? "true" : "false", g_cfg.ssid, g_cfg.cp_ssid,
                 g_cfg.imported_from);
        if (!ok) code = 404;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/ssh/import")) {
        bool ok = import_authorized_keys();
        if (ok && g_cfg.ssh_enabled) { ssh_stop(); ssh_start(); }
        snprintf(body, sizeof(body), "{\"ok\":%s}\n", ok ? "true" : "false");
        if (!ok) code = 404;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/mode")) {
        char v[64];
        if (!query_get(query, "value", v, sizeof(v))) { code = 400; snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"missing value\"}\n"); }
        else {
            if (!strcmp(v, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(v, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else g_cfg.mode = MODE_OFF;
            g_cfg.wlan_on = g_cfg.mode != MODE_OFF;
            if (g_cfg.mode != MODE_CARPLAY)
                bt_pair_stop();
            apply_mode(false);
            snprintf(body, sizeof(body), "{\"ok\":true,\"mode\":\"%s\"}\n", mode_name());
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/wlan")) {
        char v[64];
        if (!query_get(query, "on", v, sizeof(v))) { code = 400; snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"missing on\"}\n"); }
        else {
            g_cfg.wlan_on = atoi(v) != 0;
            if (g_cfg.wlan_on && g_cfg.mode == MODE_OFF) g_cfg.mode = MODE_CLIENT;
            if (!g_cfg.wlan_on)
                bt_pair_stop();
            apply_mode(false);
            snprintf(body, sizeof(body), "{\"ok\":true,\"wlan_on\":%s}\n",
                     g_cfg.wlan_on ? "true" : "false");
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/client/connect")) {
        g_cfg.mode = MODE_CLIENT;
        g_cfg.wlan_on = true;
        bt_pair_stop();
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/client/disconnect")) {
        g_cfg.mode = MODE_OFF;
        g_cfg.wlan_on = false;
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/start")) {
        g_cfg.mode = MODE_CARPLAY;
        g_cfg.wlan_on = true;
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/stop")) {
        g_cfg.mode = MODE_OFF;
        g_cfg.wlan_on = false;
        bt_pair_stop();
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/restart")) {
        g_cfg.mode = MODE_CARPLAY;
        g_cfg.wlan_on = true;
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/pair")) {
        g_cfg.mode = MODE_CARPLAY;
        g_cfg.wlan_on = true;
        apply_mode(true);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"applying\":true,"
                 "\"message\":\"starting CarPlay pairing\"}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/pair/stop")) {
        bt_pair_stop();
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"discoverable\":false,\"pairable\":false}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/logs/dump")) {
        bool ok = dump_logs_to_usb(true);
        char msg_e[512];
        json_escape(msg_e, sizeof(msg_e), g_last_msg);
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"message\":\"%s\"}\n",
                 ok ? "true" : "false", msg_e);
        if (!ok) code = 500;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/ssh")) {
        char en[64], port[64];
        bool has_en = query_get(query, "enabled", en, sizeof(en));
        bool has_port = query_get(query, "port", port, sizeof(port));
        if (has_port) {
            int p = atoi(port);
            if (p > 0 && p < 65536) g_cfg.ssh_port = p;
        }
        if (has_en) g_cfg.ssh_enabled = atoi(en) != 0;
        ssh_stop();
        if (g_cfg.ssh_enabled && g_cfg.wlan_on && g_cfg.mode != MODE_OFF)
            ssh_start();
        config_save();
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"enabled\":%s,\"port\":%d}\n",
                 g_cfg.ssh_enabled ? "true" : "false", g_cfg.ssh_port);
    } else {
        code = 404;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"not found\"}\n");
    }

    (void)changed;
    http_reply(fd, code, body);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port 8090] [--status-file PATH]\n"
            "       %s --status|--mode off|client|carplay|--import|--ssh on|off\n"
            "       %s --ensure-user|--choose-port PORT\n"
            "       %s --dump-logs\n",
            argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_HTTP_PORT;
    const char *one_shot = NULL;
    const char *one_mode = NULL;
    const char *one_ssh = NULL;
    const char *one_file = NULL;
    const char *one_port = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--status-file") && i + 1 < argc) g_status_path = argv[++i];
        else if (!strcmp(argv[i], "--status")) one_shot = "status";
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) { one_shot = "mode"; one_mode = argv[++i]; }
        else if (!strcmp(argv[i], "--import")) one_shot = "import";
        else if (!strcmp(argv[i], "--import-file") && i + 1 < argc) { one_shot = "import-file"; one_file = argv[++i]; }
        else if (!strcmp(argv[i], "--ssh") && i + 1 < argc) { one_shot = "ssh"; one_ssh = argv[++i]; }
        else if (!strcmp(argv[i], "--ensure-user")) one_shot = "ensure-user";
        else if (!strcmp(argv[i], "--choose-port") && i + 1 < argc) { one_shot = "choose-port"; one_port = argv[++i]; }
        else if (!strcmp(argv[i], "--dump-logs")) one_shot = "dump-logs";
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
    }

    mkdir("/rw_data", 0755);
    mkdir(APP_DIR, 0755);
    mkdir(APP_DIR "/bin", 0755);
    mkdir(SSH_DIR, 0700);
    unlink(BT_STATE);
    config_load();
    if (g_config_migrated)
        config_save();
    ensure_car_user();
    if (!g_cfg.cp_ssid[0])
        str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay");
    if (!g_cfg.cp_psk[0]) {
        random_psk(g_cfg.cp_psk, sizeof(g_cfg.cp_psk));
        config_save();
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (one_shot) {
        if (!strcmp(one_shot, "status")) {
            char buf[8192];
            build_status(buf, sizeof(buf));
            fputs(buf, stdout);
            return 0;
        }
        if (!strcmp(one_shot, "import")) {
            bool ok = import_wifi();
            printf("{\"ok\":%s,\"ssid\":\"%s\"}\n", ok ? "true" : "false", g_cfg.ssid);
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "import-file") && one_file) {
            bool ok = import_wifi_path(one_file);
            printf("{\"ok\":%s,\"ssid\":\"%s\",\"carplay_ssid\":\"%s\"}\n",
                   ok ? "true" : "false", g_cfg.ssid, g_cfg.cp_ssid);
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "mode") && one_mode) {
            if (!strcmp(one_mode, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(one_mode, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else g_cfg.mode = MODE_OFF;
            g_cfg.wlan_on = g_cfg.mode != MODE_OFF;
            apply_cancel();
            apply_mode_work(false);
            char buf[8192];
            build_status(buf, sizeof(buf));
            fputs(buf, stdout);
            return 0;
        }
        if (!strcmp(one_shot, "ssh") && one_ssh) {
            g_cfg.ssh_enabled = !strcmp(one_ssh, "on");
            ssh_stop();
            if (g_cfg.ssh_enabled) ssh_start();
            config_save();
            return 0;
        }
        if (!strcmp(one_shot, "ensure-user")) {
            bool ok = ensure_car_user();
            printf("{\"ok\":%s}\n", ok ? "true" : "false");
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "choose-port") && one_port) {
            int chosen = choose_ssh_port(atoi(one_port));
            printf("{\"ok\":%s,\"port\":%d}\n",
                   chosen ? "true" : "false", chosen);
            return chosen ? 0 : 1;
        }
        if (!strcmp(one_shot, "dump-logs")) {
            bool ok = dump_logs_to_usb(true);
            printf("{\"ok\":%s,\"message\":\"%s\"}\n",
                   ok ? "true" : "false", g_last_msg);
            return ok ? 0 : 1;
        }
        usage(argv[0]);
        return 2;
    }

    /* 127.0.0.1 must exist for the local HTTP API.  Normal boots have lo
     * up long before this daemon starts, but an early start or a minimal
     * environment can race it; without this the daemon died instantly with
     * "Cannot assign requested address" (observed in the firmware VM). */
    sh("ip link set lo up 2>/dev/null");
    sh("ifconfig lo up 2>/dev/null");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    int bound = -1;
    for (int i = 0; i < 60; i++) {
        if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            bound = 0;
            break;
        }
        if (i == 0)
            log_msg("bind(http) failed (%s); bringing up lo and retrying",
                    strerror(errno));
        sh("ip link set lo up 2>/dev/null");
        sh("ifconfig lo up 2>/dev/null");
        usleep(500000);
    }
    if (bound != 0) {
        perror("bind(http)");
        close(srv);
        return 1;
    }
    if (listen(srv, 8) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }
    /* popen() children (connmanctl, wpa_cli, systemctl) must not inherit the
     * listening socket: a hung child would otherwise keep the HTTP port
     * bound and prevent a daemon restart. */
    fcntl(srv, F_SETFD, FD_CLOEXEC);

    g_start_ms = now_ms();
    /* Monotonic instance id: a daemon restart within the same boot always
     * gets a new value, even if the wall clock has not been set yet. */
    g_home_epoch = (long)g_start_ms;
    home_state_write();
    log_msg("wifi_daemon started: mode=%s wlan_on=%d http=127.0.0.1:%d",
            mode_name(), g_cfg.wlan_on ? 1 : 0, port);

    /* Bring up the configured mode at service start. */
    if (g_cfg.wlan_on && g_cfg.mode != MODE_OFF) {
        apply_mode(false);
    } else {
        stop_oem_wifi();
        stop_all();
        set_msg("WLAN off");
        write_status();
    }

    while (!g_stop) {
        struct pollfd pfd = {.fd = srv, .events = POLLIN};
        int pr = poll(&pfd, 1, 500);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                http_handle(c);
            }
        }
        watchdog();
        apply_reap();
        home_monitor_tick();
        int64_t now = now_ms();
        if (now - g_last_status_ms > 2000) {
            write_status();
            g_last_status_ms = now;
        }
    }

    home_monitor_cleanup();
    close(srv);
    log_msg("wifi_daemon stopped");
    return 0;
}
