/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
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
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_DIR
#define APP_DIR        "/rw_data/wifi"
#endif
#ifndef WIFI_BUILD_ID
#define WIFI_BUILD_ID "development"
#endif
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
#define SCAN_RESULT    APP_DIR "/scan.json"
#define BT_STATE       APP_DIR "/bt.state"
#define SSH_BOUND      APP_DIR "/ssh.bound"
#define AP_STATION_IP  APP_DIR "/ap.station-ip"
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

#define WIFI_NETS_MAX 4

typedef struct {
    wifi_mode_t mode;
    bool wlan_on;

    char ssid[80];           /* client network 1 (highest priority) */
    char psk[160];

    /* Client networks 2..4 by priority.  net_count is 1..WIFI_NETS_MAX;
     * it is 1 for a legacy single-network config (config_version < 3). */
    char net2_ssid[80];  char net2_psk[160];
    char net3_ssid[80];  char net3_psk[160];
    char net4_ssid[80];  char net4_psk[160];
    int  net_count;

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
static bool g_log_stderr = true;   /* one-shot CLI mode; daemon redirects stderr into the same log file */
static bool g_pure_query;
static int g_apply_gen;            /* increments per apply; stale children cannot publish */
static bool g_carplay_ie;          /* last observed CarPlay IE state */
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

typedef enum { WORK_MODE, WORK_SCAN, WORK_SSH, WORK_PAIR_STOP, WORK_DUMP } work_t;
static work_t g_work_kind = WORK_MODE, g_pending_kind;
static bool g_work_pending, g_pending_pair, g_apply_cancelled;
static bool g_current_pair, g_dump_requested;
static bool g_pairing_stop_requested;
static bool g_pair_stop_confirmed;
static bool g_ap_restart_requested; /* explicit user restart, not watchdog recovery */
static int g_scan_gen = -1;
static bool g_network_failed;
static int64_t g_last_mode_ms;
static int64_t g_cancel_ms;
static pid_t g_probe_pid;
static int g_probe_fd = -1;
static int64_t g_probe_start_ms, g_last_probe_ms;
static bool g_probe_child;
static int64_t g_ap_unready_ms;
static int g_ap_unready_gen = -1;

typedef enum { AP_UNKNOWN, AP_ABSENT, AP_INTERFACE_UP, AP_READY } ap_state_t;
typedef struct {
    ap_state_t state;
    int role, tether; /* -1 unknown, 0 absent/non-AP or false, 1 AP or true */
    bool station, conflict;
    int64_t sampled_ms;
    char ssid[80], configured_ssid[80], ip[64];
} ap_probe_t;

typedef struct {
    int gen;
    bool driver, associated, ap_on, carplay_svc, mdns_svc, ssh_ours;
    ap_probe_t ap;
    bool vendor_ssh_22, debug_console;
    int signal, clients, secure_console;
    char ip[64], ssid[80], ap_ssid[80];
} observation_t;
static observation_t g_observed;
static int64_t g_observed_ms;

static void home_monitor_tick(void);
static void apply_reap(void);
static void probe_tick(void);

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

/* ---- multi-network client config (priority order) --------------------- */
/* Priority-ordered access to the configured client networks.  Index 0 is
 * the primary (legacy g_cfg.ssid/psk). */
static const char *net_ssid(int i)
{
    switch (i) {
    case 0: return g_cfg.ssid;
    case 1: return g_cfg.net2_ssid;
    case 2: return g_cfg.net3_ssid;
    case 3: return g_cfg.net4_ssid;
    }
    return "";
}

static const char *net_psk(int i)
{
    switch (i) {
    case 0: return g_cfg.psk;
    case 1: return g_cfg.net2_psk;
    case 2: return g_cfg.net3_psk;
    case 3: return g_cfg.net4_psk;
    }
    return "";
}

static void set_net_ssid(int i, const char *v)
{
    char *dst = NULL; size_t n = 0;
    switch (i) {
    case 0: dst = g_cfg.ssid;        n = sizeof(g_cfg.ssid);        break;
    case 1: dst = g_cfg.net2_ssid;   n = sizeof(g_cfg.net2_ssid);   break;
    case 2: dst = g_cfg.net3_ssid;   n = sizeof(g_cfg.net3_ssid);   break;
    case 3: dst = g_cfg.net4_ssid;   n = sizeof(g_cfg.net4_ssid);   break;
    }
    if (dst) str_copy(dst, n, v);
}

static void set_net_psk(int i, const char *v)
{
    char *dst = NULL; size_t n = 0;
    switch (i) {
    case 0: dst = g_cfg.psk;         n = sizeof(g_cfg.psk);         break;
    case 1: dst = g_cfg.net2_psk;    n = sizeof(g_cfg.net2_psk);    break;
    case 2: dst = g_cfg.net3_psk;    n = sizeof(g_cfg.net3_psk);    break;
    case 3: dst = g_cfg.net4_psk;    n = sizeof(g_cfg.net4_psk);    break;
    }
    if (dst) str_copy(dst, n, v);
}

/* Number of configured client networks (a net counts when its SSID is set). */
static int net_total(void)
{
    int count = 0;
    for (int i = 0; i < WIFI_NETS_MAX; i++)
        if (net_ssid(i)[0]) count++;
    return count;
}

static void redact_secret(char *buf, const char *secret);

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; i < WIFI_NETS_MAX; i++) redact_secret(buf, net_psk(i));
    redact_secret(buf, g_cfg.cp_psk);

    int logfd = g_pure_query ? -1 : open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logfd >= 0) fchmod(logfd, 0600);
    FILE *f = logfd >= 0 ? fdopen(logfd, "a") : NULL;
    if (!f && logfd >= 0) close(logfd);
    if (f) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
        fclose(f);
    }
    /* In daemon mode the service redirects stderr into LOG_PATH already;
     * writing there too duplicates every line.  One-shot CLI invocations
     * keep stderr output. */
    if (g_log_stderr && !g_in_child)
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

/* Bounded command execution. Workers own a process group: a command deadline
 * kills that disposable group, never a detached, persistent supplicant. No
 * secret-bearing command text is logged. The parent uses cached observations
 * and never runs network commands from its HTTP/event loop. */
#ifdef WIFI_TEST
static int (*test_command)(char *, size_t, const char *);
static bool (*test_work)(work_t, bool);
static int test_iface = -1, test_owned_wpa = -1;
#endif
static int run_command(char *out, size_t outsz, const char *cmd, int timeout_ms)
{
    if (out && outsz) out[0] = 0;
#ifdef WIFI_TEST
    if (test_command) return test_command(out, outsz, cmd);
#endif
    if (g_stop) return -1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (!g_in_child) setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    if (!g_in_child) setpgid(pid, pid);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    size_t used = 0;
    bool eof = false, reaped = false;
    int st = 0;
    int64_t until = now_ms() + timeout_ms;
    while (!eof || !reaped) {
        char buf[1024];
        ssize_t nr;
        while ((nr = read(fds[0], buf, sizeof(buf))) > 0) {
            if (out && used + 1 < outsz) {
                size_t take = (size_t)nr;
                if (take > outsz - used - 1) take = outsz - used - 1;
                memcpy(out + used, buf, take); used += take; out[used] = 0;
            }
        }
        if (nr == 0) eof = true;
        if (!reaped && waitpid(pid, &st, WNOHANG) == pid) reaped = true;
        if (eof && reaped) break;
        if (g_stop || now_ms() >= until) {
            close(fds[0]);
            if (g_in_child && getpgrp() == getpid()) {
                log_msg("worker command deadline/cancellation reached");
                kill(-getpgrp(), SIGKILL);
                _exit(124);
            }
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
            if (!reaped) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            return 124;
        }
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        poll(&pfd, 1, 30);
    }
    close(fds[0]);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    return run_command(NULL, 0, cmd, g_probe_child ? 1500 : 8000);
}

static int sh_out(char *out, size_t outsz, const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return run_command(out, outsz, cmd, g_probe_child ? 1500 : 8000);
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

static bool same_smack64(int fd, const char *label, size_t len)
{
    char current[256];
    ssize_t n = fgetxattr(fd, "security.SMACK64", current, sizeof(current));
    return n > 0 && (size_t)n == len && !memcmp(current, label, len);
}

static int write_file_atomic(const char *path, const char *data, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        log_msg("write_file_atomic %s open failed: %s", path, strerror(errno));
        return -1;
    }
    struct stat original;
    if (stat(path, &original) == 0) {
        struct stat current;
        char label[256];
        ssize_t ln = getxattr(path, "security.SMACK64", label, sizeof(label));
        bool label_error = false;
        if (fstat(fd, &current) != 0 ||
            ((current.st_uid != original.st_uid || current.st_gid != original.st_gid) &&
             fchown(fd, original.st_uid, original.st_gid) != 0)) {
            label_error = true;
        } else if (ln > 0) {
            /* The floor label is the default for new files.  Avoid asking
             * SMACK to set security.SMACK64="_" explicitly: on the car this
             * can return EPERM in an otherwise valid "_" installer/daemon
             * context and made config import fail.  For non-floor labels,
             * avoid the syscall when the temp file already inherited it. */
            bool floor = (ln == 1 && label[0] == '_');
            if (!floor && !same_smack64(fd, label, (size_t)ln) &&
                fsetxattr(fd, "security.SMACK64", label, (size_t)ln, 0) != 0 &&
                !same_smack64(fd, label, (size_t)ln)) {
                label_error = true;
            }
        }
        if (label_error) {
            int saved = errno;
            close(fd); unlink(tmp); errno = saved;
            log_msg("write_file_atomic %s label/owner failed: %s", path, strerror(saved));
            return -1;
        }
    }
    size_t len = strlen(data);
    ssize_t w = write(fd, data, len);
    bool runtime = !strcmp(path, g_status_path) || !strcmp(path, HOME_STATE) ||
                   !strcmp(path, APPLY_RESULT) || !strcmp(path, BT_STATE) ||
                   !strcmp(path, SSH_BOUND) || !strcmp(path, SCAN_RESULT);
    int sync_rc = runtime ? 0 : fsync(fd);
    close(fd);
    if (w != (ssize_t)len || sync_rc != 0) {
        int saved = errno;
        unlink(tmp); errno = saved;
        log_msg("write_file_atomic %s write/fsync failed: %s", path, strerror(saved));
        return -1;
    }
    chmod(tmp, mode);
    if (rename(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp); errno = saved;
        log_msg("write_file_atomic %s rename failed: %s", path, strerror(saved));
        return -1;
    }
    return 0;
}

static bool pid_has_argument(int pid, const char *argument)
{
    char path[64], args[4096];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int n = read_file(path, args, sizeof(args));
    if (n <= 0) return false;
    for (int at = 0; at < n;) {
        size_t len = strnlen(args + at, (size_t)(n - at));
        if (len == strlen(argument) && !memcmp(args + at, argument, len)) return true;
        at += (int)len + 1;
    }
    return false;
}

static int owned_wpa_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char path[80], comm[32];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        if (read_file(path, comm, sizeof(comm)) > 0 && !strcmp(trim(comm), "wpa_supplicant") &&
            pid_has_argument(pid, "-c" WPA_CONF) &&
            pid_has_argument(pid, "-O" WPA_CTRL_DIR)) { found = pid; break; }
    }
    closedir(d);
    return found;
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

/* /proc/comm truncates names to 15 bytes. Match executable basename instead,
 * not a substring of arbitrary arguments or a guessed truncated comm. */
static int pid_by_executable(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[80], exe[1024];
        snprintf(path, sizeof(path), "/proc/%s/exe", de->d_name);
        ssize_t n = readlink(path, exe, sizeof(exe) - 1);
        if (n <= 0) continue;
        exe[n] = 0;
        char *deleted = strstr(exe, " (deleted)");
        if (deleted) *deleted = 0;
        const char *base = strrchr(exe, '/');
        if (!strcmp(base ? base + 1 : exe, name)) { found = atoi(de->d_name); break; }
    }
    closedir(d);
    return found;
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
#ifdef WIFI_TEST
    if (test_iface >= 0) return test_iface != 0;
#endif
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
    return i > 0 && strcmp(out, "127.0.0.1") != 0 && strcmp(out, "0.0.0.0") != 0;
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

/* The configured identifier is diagnostic data, never broadcast evidence. */
static int tethering_state(char *ap_ssid, size_t ap_ssid_n)
{
    if (ap_ssid && ap_ssid_n) ap_ssid[0] = 0;
    char buf[8192];
    if (sh_out(buf, sizeof(buf), "connmanctl technologies 2>/dev/null") != 0)
        return -1;

    bool in_wifi = false;
    int tether = -1;
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
            p = trim(p + 12);
            tether = !strcmp(p, "True") ? 1 : !strcmp(p, "False") ? 0 : -1;
        } else if (!strncmp(p, "TetheringIdentifier = ", 22)) {
            if (ap_ssid) str_copy(ap_ssid, ap_ssid_n, trim(p + 22));
        }
    }
    return tether;
}

/* Find a ConnMan service token for an SSID. */
/* Extract the displayed name from a `connmanctl services` line: the text
 * after the optional state marker ('*') and before the wifi_ token. */
static void service_line_name(const char *line, char *out, size_t n)
{
    if (out && n) out[0] = 0;
    const char *svc = NULL;
    for (const char *p = line; (p = strstr(p, "wifi_")); p += 5)
        if (p == line || isspace((unsigned char)p[-1])) svc = p;
    if (!svc) return;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '*') {
        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    size_t len = (size_t)(svc - p);
    while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
}

/* find_service matches the primary network (legacy single-network call
 * sites); find_service_psk matches a specific network whose psk decides
 * the _psk/_none security suffix in the ConnMan service id. */
static bool find_service_psk(const char *services, const char *ssid,
                            const char *security_want,
                            char *service, size_t service_n)
{
    if (!ssid || !*ssid || strlen(ssid) > 32) return false;
    char *copy = strdup(services ? services : "");
    if (!copy) return false;
    bool found = false;
    /* Vendor service IDs encode SSID bytes as hex, not MD5. Require the
     * exact ID component; never select a similarly named guest network. */
    char encoded[68] = "_";
    size_t at = 1;
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) {
        snprintf(encoded + at, sizeof(encoded) - at, "%02x", *p); at += 2;
    }
    encoded[at++] = '_'; encoded[at] = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char name[256];
        service_line_name(line, name, sizeof(name));
        char *token = NULL;
        for (char *p = line; (p = strstr(p, "wifi_")); p += 5)
            if (p == line || isspace((unsigned char)p[-1])) token = p;
        if (!token) continue;
        size_t len = strcspn(token, " \t\r");
        token[len] = 0;
        if (strcmp(name, ssid) && !strstr(token, encoded)) continue;
        const char *security = strrchr(token, '_');
        if (!security || strcmp(security, security_want)) continue;
        /* Treat the token as data even when interpolated into CLI commands. */
        if (strspn(token, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != len)
            continue;
        str_copy(service, service_n, token);
        found = true;
        break;
    }
    free(copy);
    return found;
}

/* Index of the client network an in-flight connect/agent exchange is for.
 * find_service_any sets it; the agent passphrase reply and the connect
 * result messages read it. */
static int g_client_net = 0;

/* Multi-network selection: scan output -> the highest-priority configured
 * network that is visible.  Returns the network index and fills service
 * (and ssid_out when given), or -1 when none of the networks is visible. */
static int find_service_any(const char *services, char *service, size_t service_n,
                            char *ssid_out, size_t ssid_n)
{
    for (int i = 0; i < WIFI_NETS_MAX; i++) {
        const char *ssid = net_ssid(i);
        if (!ssid[0]) continue;
        if (find_service_psk(services, ssid,
                             net_psk(i)[0] ? "_psk" : "_none",
                             service, service_n)) {
            if (ssid_out && ssid_n) str_copy(ssid_out, ssid_n, ssid);
            return i;
        }
    }
    return -1;
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
        } else if (!strcmp(k, "net2_ssid")) {
            str_copy(g_cfg.net2_ssid, sizeof(g_cfg.net2_ssid), v);
        } else if (!strcmp(k, "net2_psk")) {
            str_copy(g_cfg.net2_psk, sizeof(g_cfg.net2_psk), v);
        } else if (!strcmp(k, "net3_ssid")) {
            str_copy(g_cfg.net3_ssid, sizeof(g_cfg.net3_ssid), v);
        } else if (!strcmp(k, "net3_psk")) {
            str_copy(g_cfg.net3_psk, sizeof(g_cfg.net3_psk), v);
        } else if (!strcmp(k, "net4_ssid")) {
            str_copy(g_cfg.net4_ssid, sizeof(g_cfg.net4_ssid), v);
        } else if (!strcmp(k, "net4_psk")) {
            str_copy(g_cfg.net4_psk, sizeof(g_cfg.net4_psk), v);
        } else if (!strcmp(k, "net_count")) {
            g_cfg.net_count = atoi(v);
            if (g_cfg.net_count < 1) g_cfg.net_count = 1;
            if (g_cfg.net_count > WIFI_NETS_MAX) g_cfg.net_count = WIFI_NETS_MAX;
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
    /* A legacy config (version < 3) has only the primary network; make
     * net_count consistent with what is actually configured.  */
    {
        int total = net_total();
        if (g_cfg.net_count < 1) g_cfg.net_count = 1;
        if (total == 0) g_cfg.net_count = 1;          /* empty primary */
        else if (total > g_cfg.net_count) g_cfg.net_count = total;
    }
}

static bool config_save(void)
{
    /* Only the parent daemon persists config.  An apply child reports
     * observed adjustments through APPLY_RESULT instead, so a canceled or
     * stale worker can never save a config it did not apply. */
    if (g_in_child) return true;
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "# wifi_daemon config\n"
             "config_version=3\n"
             "mode=%s\n"
             "wlan_on=%d\n"
             "ssid=%s\n"
             "psk=%s\n"
             "net2_ssid=%s\n"
             "net2_psk=%s\n"
             "net3_ssid=%s\n"
             "net3_psk=%s\n"
             "net4_ssid=%s\n"
             "net4_psk=%s\n"
             "net_count=%d\n"
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
             g_cfg.net2_ssid, g_cfg.net2_psk,
             g_cfg.net3_ssid, g_cfg.net3_psk,
             g_cfg.net4_ssid, g_cfg.net4_psk,
             g_cfg.net_count,
             g_cfg.cp_ssid, g_cfg.cp_psk,
             g_cfg.cp_band == 1 ? "2.4" : g_cfg.cp_band == 2 ? "5" : "auto",
             g_cfg.cp_channel, g_cfg.cp_hidden ? 1 : 0, g_cfg.cp_max_sta,
             g_cfg.cp_manufacturer, g_cfg.cp_model, g_cfg.cp_name,
             g_cfg.cp_id, g_cfg.cp_btaddr,
             g_cfg.ssh_enabled ? 1 : 0, g_cfg.ssh_port,
             g_cfg.imported ? 1 : 0, g_cfg.imported_from);

    mkdir(APP_DIR, 0755);
    return write_file_atomic(CONFIG_PATH, buf, 0600) == 0;
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
    config_t previous = g_cfg;
    char buf[8192];
    if (read_file(path, buf, sizeof(buf)) <= 0) return false;

    char ssid[80] = {0}, psk[160] = {0};
    char net_ssid_v[WIFI_NETS_MAX - 1][80] = {{0}};
    char net_psk_v[WIFI_NETS_MAX - 1][160] = {{0}};
    char cp_ssid[80] = {0}, cp_psk[160] = {0};
    int cp_band = -1, cp_channel = -1, cp_hidden = -1, cp_max = -1;
    int ssh_port = -1;   /* accepted as a stored preference; `ssh=` ignored */
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
        /* Additional client networks in priority order: ssid2/psk2 ..
         * ssid4/psk4 (net2_ssid/net2_psk synonyms).  The plain ssid/psk
         * pair is network 1 (highest priority). */
        else if (!strcmp(k, "ssid2") || !strcmp(k, "net2_ssid")) str_copy(net_ssid_v[0], sizeof(net_ssid_v[0]), v);
        else if (!strcmp(k, "psk2") || !strcmp(k, "net2_psk"))    str_copy(net_psk_v[0], sizeof(net_psk_v[0]), v);
        else if (!strcmp(k, "ssid3") || !strcmp(k, "net3_ssid")) str_copy(net_ssid_v[1], sizeof(net_ssid_v[1]), v);
        else if (!strcmp(k, "psk3") || !strcmp(k, "net3_psk"))    str_copy(net_psk_v[1], sizeof(net_psk_v[1]), v);
        else if (!strcmp(k, "ssid4") || !strcmp(k, "net4_ssid")) str_copy(net_ssid_v[2], sizeof(net_ssid_v[2]), v);
        else if (!strcmp(k, "psk4") || !strcmp(k, "net4_psk"))    str_copy(net_psk_v[2], sizeof(net_psk_v[2]), v);
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
        /* `ssh` and `mode` are deliberately ignored: importing wifi.txt only
         * stores credentials/AP settings.  Activation (WLAN mode, SSH) is
         * always an explicit user action in the UI, never an import side
         * effect.  ssh_port is accepted as a stored preference only. */
        else if (!strcmp(k, "ssh_port")) ssh_port = atoi(v);
    }

    if (!ssid[0] || strlen(ssid) > 32 || strlen(cp_ssid) > 32) {
        set_msg("WiFi SSIDs must be 1-32 bytes");
        return false;
    }
    for (int i = 0; i < WIFI_NETS_MAX - 1; i++) {
        if (strlen(net_ssid_v[i]) > 32) {
            set_msg("WiFi SSIDs must be 1-32 bytes");
            return false;
        }
    }
    str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), ssid);
    str_copy(g_cfg.psk, sizeof(g_cfg.psk), psk);
    set_net_ssid(1, net_ssid_v[0]);
    set_net_psk(1, net_psk_v[0]);
    set_net_ssid(2, net_ssid_v[1]);
    set_net_psk(2, net_psk_v[1]);
    set_net_ssid(3, net_ssid_v[2]);
    set_net_psk(3, net_psk_v[2]);
    g_cfg.net_count = net_total();
    if (g_cfg.net_count < 1) g_cfg.net_count = 1;
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
    if (ssh_port > 0 && ssh_port < 65536) g_cfg.ssh_port = ssh_port;

    if (!g_cfg.cp_psk[0]) random_psk(g_cfg.cp_psk, sizeof(g_cfg.cp_psk));
    if (g_cfg.cp_max_sta < 1) g_cfg.cp_max_sta = 2;
    if (g_cfg.cp_channel <= 0)
        g_cfg.cp_channel = g_cfg.cp_band == 1 ? 6 : 36;

    str_copy(g_cfg.imported_from, sizeof(g_cfg.imported_from), path);
    g_cfg.imported = true;
    if (!config_save()) {
        g_cfg = previous;
        set_msg("WiFi import failed: could not persist credentials");
        return false;
    }

    /* Keep a private copy for the UI / later use. */
    char copy[8192];
    if (read_file(path, copy, sizeof(copy)) > 0)
        write_file_atomic(WIFI_TXT_COPY, copy, 0600);

    set_msg("imported WiFi config from %s (ssid=%s, %d network%s)",
            path, g_cfg.ssid, g_cfg.net_count,
            g_cfg.net_count == 1 ? "" : "s");
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

    int pid = pid_by_executable("supplicantmanager");
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

    /* A running supplicant with a missing socket is expected while ConnMan
     * removes/recreates wlan0 for AP mode: the per-interface socket is
     * unlinked during teardown and recreated with -O.  Log the socket state
     * (at most once a minute) so the reason is in the console and log bundle;
     * distinguish ENOENT from a permission error in the listing. */
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

/* Process health only: is a supplicant process alive at all?  This is the
 * signal that matters for "do we restart the stack"; a missing per-interface
 * socket on a live process is a transition, not a crash. */
static bool wpa_process_alive(void)
{
#ifdef WIFI_TEST
    if (test_owned_wpa >= 0) return test_owned_wpa != 0;
#endif
    return owned_wpa_pid() > 0;
}

static bool proc_alive(int pid)
{
    char path[64], state[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_file(path, state, sizeof(state)) <= 0) return false;
    char *end = strrchr(state, ')');
    return end && end[1] == ' ' && end[2] != 'Z' && end[2] != 'X';
}

static void close_child_fds(int keep)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++) if (fd != keep) close(fd);
        return;
    }
    struct dirent *de;
    int own = dirfd(d);
    while ((de = readdir(d))) {
        int fd = atoi(de->d_name);
        if (fd >= 3 && fd != own && fd != keep) close(fd);
    }
    closedir(d);
}

/* Signal a supplicant and wait for it to exit; escalate to SIGKILL after a
 * bounded timeout.  Only the owner verified by /proc/<pid>/comm is signaled. */
static void stop_wpa_supplicant(void)
{
    int pid = -1;
    char buf[64];
    if (read_file(WPA_PIDFILE, buf, sizeof(buf)) > 0) {
        int p = atoi(trim(buf));
        if (p > 1) {
            char path[64], comm[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", p);
            if (read_file(path, comm, sizeof(comm)) > 0 &&
                !strcmp(trim(comm), "wpa_supplicant") &&
                pid_has_argument(p, "-c" WPA_CONF) && pid_has_argument(p, "-O" WPA_CTRL_DIR))
                pid = p;
        }
    }
    if (pid <= 0) {
        int p = owned_wpa_pid();
        if (p > 0) pid = p;
    }
    if (pid > 0) {
        if (pid_has_argument(pid, "-c" WPA_CONF)) kill(pid, SIGTERM);
        for (int i = 0; i < 20 && proc_alive(pid); i++)
            usleep(100000);
        if (proc_alive(pid)) {
            log_msg("wpa_supplicant pid %d ignored SIGTERM; SIGKILL", pid);
            if (pid_has_argument(pid, "-c" WPA_CONF)) kill(pid, SIGKILL);
            for (int i = 0; i < 10 && proc_alive(pid); i++)
                usleep(100000);
        }
    }
    /* Remove stale endpoints only after the owner is gone, so a replacement
     * instance cannot race a still-shutting-down predecessor. */
    if (pid <= 0 || !proc_alive(pid)) {
        if (pid > 0) waitpid(pid, NULL, WNOHANG);
        sh("rm -f " WPA_CTRL_DIR "/wlan0 2>/dev/null");
        unlink(WPA_PIDFILE);
    }
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
    /* This build has no CONFIG_DEBUG_FILE: -f is rejected.  Do not use -B:
     * daemon(0,0) would redirect output to /dev/null and the log would stay
     * empty, and -P only writes the pid file on the -B daemonize path, so a
     * foreground launch would leave a stale pid file.  Background via the
     * shell and capture $! ourselves.  Do not pass -e /rw_data/wifi/entropy.bin:
     * the OEM supplicant is identified by that cmdline argument in
     * stop_oem_wifi, and our own instance would be killed by mistake.
     *
     * -O is essential: ConnMan removes and recreates wlan0 over D-Bus when
     * switching to AP mode, and the recreated interface gets no config file
     * (conf 'N/A' ctrl_interface 'N/A').  -O overrides ctrl_interface for
     * every new interface, so the control socket survives the transition
     * instead of disappearing with the removed interface. */
    if (g_stop) return false;
    /* setsid, unlike nohup, removes the persistent radio service from the
     * disposable apply group. Its exact config argument is our ownership
     * marker even if cancellation happens before its PID file is written. */
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) _exit(126);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_IGN);
        int log = open(WPA_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
        int nul = open("/dev/null", O_RDONLY);
        if (log < 0 || nul < 0) _exit(126);
        fchmod(log, 0600);
        dup2(nul, STDIN_FILENO); dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO);
        close(log); close(nul);
        char drv[64]; snprintf(drv, sizeof(drv), "-D%s", driver ? driver : "");
        if (driver && *driver)
            execl("/usr/sbin/wpa_supplicant", "wpa_supplicant", "-u", drv,
                  "-iwlan0", "-c" WPA_CONF, "-O" WPA_CTRL_DIR, (char *)NULL);
        else
            execl("/usr/sbin/wpa_supplicant", "wpa_supplicant", "-u",
                  "-iwlan0", "-c" WPA_CONF, "-O" WPA_CTRL_DIR, (char *)NULL);
        _exit(127);
    }
    char pidbuf[64]; snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)pid);
    if (pid <= 0) return false;
    if (pid > 1)
        write_file_atomic(WPA_PIDFILE, pidbuf, 0644);
    log_msg("started wpa_supplicant pid=%d driver=%s -O%s",
            pid, driver ? driver : "default", WPA_CTRL_DIR);

    for (int i = 0; i < 32; i++) {
        if (wpa_ping()) return true;
        if (pid > 1 && !proc_alive(pid)) break;   /* exited already */
        usleep(250000);
    }
    return false;
}

static bool ensure_wpa_supplicant(void)
{
    if (wpa_process_alive() && wpa_ping()) return true;
    if (!iface_present()) return false;
    if (!wpa_process_alive() && pid_by_executable("wpa_supplicant") > 0) {
        set_msg("WiFi has a foreign supplicant owner; refusing a second instance");
        return false;
    }

    /* A live process with a missing socket is usually a ConnMan interface
     * transition (AP remove/recreate).  Give the socket a grace period to
     * reappear (the -O override recreates it) before deciding the process is
     * stale; only a dead process or a socket still missing after the grace
     * period justifies a restart. */
    if (wpa_process_alive()) {
        log_msg("wpa_supplicant alive but control socket missing; "
                "waiting for interface recreation");
        for (int i = 0; i < 40; i++) {
            if (wpa_ping()) return true;
            if (!wpa_process_alive()) break;
            usleep(250000);
        }
        if (!wpa_process_alive()) {
            log_msg("wpa_supplicant exited during interface transition");
        } else if (!wpa_ping()) {
            log_msg("control socket still missing after grace; restarting supplicant");
            stop_wpa_supplicant();
            usleep(500000);
        } else {
            return true;
        }
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

/* -1 means unknown/unavailable, not a verified powered-off technology. */
static int wifi_power_state(void)
{
    char buf[8192];
    if (sh_out(buf, sizeof(buf), "connmanctl technologies 2>/dev/null") != 0)
        return -1;
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
        if (!strncmp(p, "Powered = ", 10)) {
            if (!strcmp(trim(p + 10), "True")) return 1;
            if (!strcmp(trim(p + 10), "False")) return 0;
            return -1;
        }
    }
    return -1;
}

static bool wifi_powered(void) { return wifi_power_state() == 1; }

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
    set_msg("WiFi technology did not power on");
    return false;
}

static bool decode_control_ssid(char *out, size_t n, const char *value)
{
    size_t at = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        if (c == '\\') {
            p++;
            if (!*p) return false;
            if (*p == 'x' && p[1] && p[2] && isxdigit(p[1]) && isxdigit(p[2])) {
                char hex[3] = {(char)p[1], (char)p[2], 0};
                c = (unsigned char)strtol(hex, NULL, 16); p += 2;
            } else if (*p == 'n') c = '\n';
            else if (*p == 'r') c = '\r';
            else if (*p == 't') c = '\t';
            else if (*p == '\\' || *p == '"') c = *p;
            else return false;
        }
        if (!c || at + 1 >= n) return false;
        out[at++] = (char)c;
    }
    out[at] = 0;
    return true;
}

static bool wpa_state_completed(void)
{
    char buf[2048];
    if (sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") != 0)
        return false;
    bool completed = false, station = false, same_ssid = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(line, "wpa_state=COMPLETED")) completed = true;
        else if (!strcmp(line, "mode=station")) station = true;
        else if (!strncmp(line, "ssid=", 5)) {
            char actual[80];
            same_ssid = decode_control_ssid(actual, sizeof(actual), line + 5) && !strcmp(actual, g_cfg.ssid);
        }
    }
    return completed && station && same_ssid;
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

/* Read-only, shared by startup, teardown, observation, SSH and scanning.
 * A failed command is unknown. Neither a stored identifier nor an IPv4
 * address on its own establishes that wlan0 is broadcasting our AP. */
static void ap_probe(ap_probe_t *p)
{
    memset(p, 0, sizeof(*p));
    p->sampled_ms = now_ms();
    p->role = -1;
    p->tether = tethering_state(p->configured_ssid, sizeof(p->configured_ssid));
    char buf[4096], iw_ssid[80] = "", wpa_ssid[80] = "";
    int wpa_role = -1;
    bool completed = false;
    if (!iface_present()) p->role = 0;
    else if (sh_out(buf, sizeof(buf), "iw dev wlan0 info 2>/dev/null") == 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (!strncmp(s, "type ", 5)) {
                const char *type = trim(s + 5);
                p->role = !strcmp(type, "AP") ? 1 : !strcmp(type, "managed") ? 0 : -1;
            }
            if (!strncmp(s, "ssid ", 5) && !decode_control_ssid(iw_ssid, sizeof(iw_ssid), s + 5)) iw_ssid[0] = 0;
        }
    }
    if (sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null") == 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!strcmp(line, "mode=AP")) wpa_role = 1;
            else if (!strcmp(line, "mode=station")) wpa_role = 0;
            else if (!strcmp(line, "wpa_state=COMPLETED")) completed = true;
            else if (!strncmp(line, "ssid=", 5) && !decode_control_ssid(wpa_ssid, sizeof(wpa_ssid), line + 5)) wpa_ssid[0] = 0;
        }
    }
    if ((p->role < 0 || (p->role == 1 && !iw_ssid[0] && wpa_role != 1)) &&
        sh_out(buf, sizeof(buf), "iwconfig wlan0 2>/dev/null") == 0) {
        int role = strstr(buf, "Mode:Master") ? 1 : strstr(buf, "Mode:Managed") ? 0 : -1;
        if (p->role < 0) p->role = role;
        else if (role >= 0 && role != p->role) p->conflict = true;
        char *ssid = strstr(buf, "ESSID:\"");
        if (role == 1 && ssid) {
            ssid += 7;
            char *end = ssid;
            for (; *end; end++) {
                if (*end == '\\' && end[1]) { end++; continue; }
                if (*end == '"') break;
            }
            if (*end == '"') {
                *end = 0;
                if (!decode_control_ssid(iw_ssid, sizeof(iw_ssid), ssid)) iw_ssid[0] = 0;
            }
        }
    }
    p->station = wpa_role == 0 && completed;
    if (p->role < 0 && wpa_role == 1 && completed) p->role = 1;
    p->conflict = p->conflict || (p->role == 1 && p->station) || (p->role == 0 && wpa_role == 1 && completed);
    if (p->role == 1) {
        str_copy(p->ssid, sizeof(p->ssid), iw_ssid[0] ? iw_ssid : wpa_role == 1 ? wpa_ssid : "");
        if (iw_ssid[0] && wpa_role == 1 && wpa_ssid[0] && strcmp(iw_ssid, wpa_ssid)) p->conflict = true;
    }
    if (wlan_ip(p->ip, sizeof(p->ip))) {
        struct in_addr address;
        if (inet_pton(AF_INET, p->ip, &address) != 1 ||
            (ntohl(address.s_addr) >> 24) == 0 || (ntohl(address.s_addr) >> 28) >= 14 ||
            (ntohl(address.s_addr) >> 24) == 127 ||
            (ntohl(address.s_addr) & 0xffff0000U) == 0xa9fe0000U) p->ip[0] = 0;
    } else p->ip[0] = 0;
    char station_ip[64] = "";
    read_file(AP_STATION_IP, station_ip, sizeof(station_ip));
    p->state = AP_UNKNOWN;
    if (!p->conflict) {
        if (p->role == 0 && p->tether == 0) p->state = AP_ABSENT;
        else if (p->role == 1) {
            p->state = AP_INTERFACE_UP;
            if (!strcmp(p->ssid, g_cfg.cp_ssid) && p->ip[0] && strcmp(p->ip, trim(station_ip)))
                p->state = AP_READY;
        }
    }
    if (now_ms() - p->sampled_ms > 6000) p->state = AP_UNKNOWN;
}

static void ap_evidence(const char *stage, const ap_probe_t *p)
{
    /* Deliberately do not log raw technologies/status/CLI output (keys). */
    char actual[512], configured[512];
    json_escape(actual, sizeof(actual), p->ssid);
    json_escape(configured, sizeof(configured), p->configured_ssid);
    log_msg("AP stage=%s mono_ms=%lld state=%d role=%d tether=%d actual_ssid=%s configured_ssid=%s ip=%s station=%d conflict=%d",
            stage, (long long)p->sampled_ms, p->state, p->role, p->tether,
            actual, configured, p->ip, p->station, p->conflict);
}

/* Call only for an explicit stop or after actual absence, never as a
 * response to Tethering=False on an otherwise working AP. */
static bool ap_disable(bool explicit_stop)
{
    if (!explicit_stop) {
        ap_probe_t before;
        ap_probe(&before);
        if (before.role != 0 || before.conflict || now_ms() - before.sampled_ms > 6000) {
            ap_evidence("late-AP-before-teardown", &before);
            return false;
        }
    }
    int rc = sh("connmanctl tether wifi off 2>/dev/null");
    int absent = 0;
    int64_t deadline = now_ms() + 6000;
    ap_probe_t p;
    for (int i = 0; i < 14 && !g_stop && now_ms() < deadline; i++) {
        ap_probe(&p);
        if (p.state == AP_ABSENT) {
            if (++absent >= 2) return true;
        } else absent = 0;
        usleep(500000);
    }
    log_msg("AP teardown unconfirmed (command rc=%d)", rc);
    return false;
}

static bool client_disconnect(void);

static bool ap_remember_station(const ap_probe_t *p)
{
    return !p->station || !p->ip[0] || write_file_atomic(AP_STATION_IP, p->ip, 0600) == 0;
}

typedef enum { AP_WAIT_READY, AP_WAIT_NEVER, AP_WAIT_PENDING, AP_WAIT_LOST, AP_WAIT_UNKNOWN } ap_wait_t;

static ap_wait_t ap_wait_ready(bool seen, ap_probe_t *last)
{
    int ready = 0, absent = 0, lost = 0, station_polls = 0;
    bool reconciled = false;
    int64_t deadline = now_ms() + 30000;
    for (int i = 0; i < 62 && !g_stop && now_ms() < deadline; i++) {
        ap_probe(last);
        if (last->role == 1) seen = true;
        if (last->station) {
            if (!reconciled) {
                if (!ap_remember_station(last) || !client_disconnect()) return AP_WAIT_UNKNOWN;
                reconciled = true;
                log_msg("AP startup: unexpected station association reconciled through ConnMan");
            } else if (++station_polls >= 10) return AP_WAIT_UNKNOWN;
        }
        if (last->state == AP_READY) {
            if (++ready >= 3) {
                if (last->tether != 1) ap_evidence("ready-property-disagreement", last);
                return AP_WAIT_READY;
            }
        } else ready = 0;
        if (last->state == AP_ABSENT) absent++; else absent = 0;
        if (seen && last->role == 0 && !last->conflict) {
            if (++lost >= 3) return AP_WAIT_LOST;
        } else lost = 0;
        usleep(500000);
    }
    if (g_stop) return AP_WAIT_UNKNOWN;
    if (seen) return last->role == 1 ? AP_WAIT_PENDING : AP_WAIT_UNKNOWN;
    return absent >= 2 ? AP_WAIT_NEVER : AP_WAIT_UNKNOWN;
}

static bool ap_start(void)
{
    if (!g_cfg.cp_ssid[0] || strlen(g_cfg.cp_ssid) > 32 ||
        strlen(g_cfg.cp_psk) < 8 || strlen(g_cfg.cp_psk) > 63) {
        set_msg("CarPlay AP requires a 1-32 byte SSID and an 8-63 character key");
        return false;
    }
    ap_probe_t p;
    ap_probe(&p);
    if (p.role != 1 && (p.role < 0 || p.conflict || now_ms() - p.sampled_ms > 6000)) {
        ap_evidence("initial-state-unknown", &p);
        set_msg("CarPlay AP initial state unknown; stack left unchanged");
        return false;
    }
    if (!ap_remember_station(&p)) {
        set_msg("CarPlay AP cannot record previous station address");
        return false;
    }
    if (p.role == 1) {
        /* Reuse an existing AP, or let pending DHCP settle. Do not toggle a
         * functioning radio just because ConnMan's property disagrees. */
        ap_wait_t result = ap_wait_ready(true, &p);
        if (result == AP_WAIT_READY) return true;
        ap_evidence(result == AP_WAIT_LOST ? "enabled_then_lost" : "existing-unready", &p);
        set_msg("CarPlay AP existing interface %s; stop AP before retrying/changing SSID",
                result == AP_WAIT_LOST ? "enabled_then_lost" : "not ready/unknown");
        return false;
    }
    char ssid_q[256], psk_q[256];
    shq(ssid_q, sizeof(ssid_q), g_cfg.cp_ssid);
    shq(psk_q, sizeof(psk_q), g_cfg.cp_psk);
    int freq = channel_to_freq();

    if (!ensure_wifi_ready()) {
        set_msg("CarPlay AP: WiFi stack not ready");
        return false;
    }

    /* Recheck after stack/power setup; an in-flight AP may have appeared. */
    ap_probe(&p);
    if (!ap_remember_station(&p)) {
        set_msg("CarPlay AP cannot record station transition address");
        return false;
    }
    if (p.role == 1) {
        if (ap_wait_ready(true, &p) == AP_WAIT_READY) return true;
        set_msg("CarPlay AP pending interface did not become ready");
        return false;
    }
    if (p.role != 0 || p.conflict || (p.tether != 0 && !ap_disable(false))) {
        ap_evidence("pre-start-unknown", &p);
        set_msg("CarPlay AP previous teardown/absence not confirmed");
        return false;
    }
    if (!client_disconnect()) {
        set_msg("CarPlay AP station disconnect could not be confirmed");
        return false;
    }
    int64_t station_deadline = now_ms() + 6000;
    for (int i = 0; i < 12 && !g_stop && now_ms() < station_deadline; i++) {
        ap_probe(&p);
        if (!ap_remember_station(&p)) {
            set_msg("CarPlay AP cannot record station transition address");
            return false;
        }
        if (!p.station && !p.conflict) break;
        usleep(500000);
    }
    if (p.station || p.conflict || p.state != AP_ABSENT || g_stop) {
        ap_evidence("station-transition-unconfirmed", &p);
        set_msg("CarPlay AP station transition/absence not confirmed");
        return false;
    }
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    g_carplay_ie = false;

    /* This vendor connmanctl parses tether options, not positionals.
     * Verified by executing the car binary with its own loader:
     *   tether wifi on <ssid> <psk> --hidden no --frequency 5180 --maxsta 2
     *            --appinfo 1
     * Positional "<hidden> <frequency> <maxsta>" yields
     * "unknown option [?][no]" and "unknown option [?][5180]" - the exact
     * errors in the on-car log.  --appinfo 1 marks the AP as CarPlay. */
    for (int attempt = 0; attempt < 2 && !g_stop; attempt++) {
        ap_probe(&p);
        if (p.role == 1) {
            if (ap_wait_ready(true, &p) == AP_WAIT_READY) return true;
            set_msg("CarPlay AP appeared before enable but is not ready; no new tether request");
            return false;
        }
        if (p.state != AP_ABSENT || p.station) {
            set_msg("CarPlay AP absence not confirmed immediately before enable");
            return false;
        }
        char out[4096] = "";
        int rc = sh_out(out, sizeof(out),
                    "connmanctl tether wifi on %s %s --hidden %s "
                    "--frequency %d --maxsta %d --appinfo 1 2>&1",
                    ssid_q, psk_q, g_cfg.cp_hidden ? "yes" : "no", freq,
                    g_cfg.cp_max_sta);
        bool denied = strstr(out, "Permission denied") != NULL;
        bool rejected = rc != 0 || denied || strstr(out, "Error ") != NULL;
        log_msg("AP tether request frequency=%d rc=%d rejected=%d permission_denied=%d", freq, rc, rejected, denied);
        ap_wait_t result = ap_wait_ready(false, &p);
        if (result == AP_WAIT_READY) {
            if (attempt) {
                g_cfg.cp_band = 1;
                g_cfg.cp_channel = 6;
                log_msg("CarPlay AP fell back to 2.4 GHz channel 6");
            }
            return true;
        }
        ap_evidence(result == AP_WAIT_LOST ? "enabled_then_lost" : result == AP_WAIT_NEVER ? "never_enabled" : "pending_or_unknown", &p);
        if (result != AP_WAIT_NEVER || attempt || freq < 5000 || denied) {
            set_msg("CarPlay AP %s%s (rc=%d)",
                    result == AP_WAIT_LOST ? "enabled_then_lost" :
                    result == AP_WAIT_PENDING ? "interface up; SSID/address not ready" :
                    result == AP_WAIT_NEVER ? "never enabled" : "state unknown; no destructive retry",
                    denied ? "; tether permission denied" : "", rc);
            return false;
        }
        /* Only a never-observed AP with confirmed absence may change band.
         * A second enable while teardown is pending is not a fallback. */
        if (!ap_disable(false)) {
            set_msg("CarPlay AP teardown did not finish; fallback aborted");
            return false;
        }
        if (!ensure_wifi_ready()) return false;
        if (!client_disconnect()) {
            set_msg("CarPlay AP fallback station reconciliation failed");
            return false;
        }
        ap_probe(&p);
        if (p.state != AP_ABSENT || p.station) {
            set_msg("CarPlay AP fallback absence/station transition not confirmed");
            return false;
        }
        freq = 2437;
    }
    set_msg("CarPlay AP canceled");
    return false;
}

static void carplay_fw_close(void);

static bool ap_stop(void)
{
    g_carplay_ie = false;
    carplay_fw_close();
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    return ap_disable(true);
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
    if (g_home_pid > 0) {
        if (waitpid(g_home_pid, NULL, WNOHANG) == 0) { kill(g_home_pid, SIGKILL); return; }
        g_home_pid = -1;
    }

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
        waitpid(g_home_pid, NULL, WNOHANG);
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
                      "DBUS_SESSION_BUS_ADDRESS=%s dbus-send --session --print-reply --reply-timeout=1200 "
                      "--dest=%s %s %s.%s %s 2>&1",
                      addr_q, dest_q, path_q, iface_q, method, arg);
    return sh_out(out, n,
                  "DBUS_SESSION_BUS_ADDRESS=%s dbus-send --session --print-reply --reply-timeout=1200 "
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
    if (g_in_child) return; /* generation-checked result is the only handoff */
    char buf[768];
    snprintf(buf, sizeof(buf),
             "discoverable=%d\npairable=%d\naddress=%s\nmessage=%s\n",
             g_bt_discoverable ? 1 : 0, g_bt_pairable ? 1 : 0,
             g_cfg.cp_btaddr, g_pair_msg);
    write_file_atomic(BT_STATE, buf, 0600);
}

/* Read back a boolean property; falls back to the intended value when the
 * vendor adapter does not expose a getter. */
static bool bt_get_prop_bool(const char *name, bool fallback)
{
    char out[4096] = "";
    if (bt_props_get(name, out, sizeof(out))) {
        if (strstr(out, "true")) return true;
        if (strstr(out, "false")) return false;
    }
    return fallback;
}

static bool bt_set_pairing(bool on)
{
    bool powered = on ? bt_set_prop("Powered", true) : true;
    bool disc_set = bt_set_prop("Discoverable", on);
    bool pair_set = bt_set_prop("Pairable", on);
    if (on && (!disc_set || !pair_set)) {
        usleep(1000000);
        if (!disc_set) disc_set = bt_set_prop("Discoverable", true);
        if (!pair_set) pair_set = bt_set_prop("Pairable", true);
    }
    (void)powered;
    /* Record the resulting state, not the setter's success: setting
     * Discoverable=false successfully must not be reported as "on". */
    g_bt_discoverable = bt_get_prop_bool("Discoverable", disc_set ? on : g_bt_discoverable);
    g_bt_pairable = bt_get_prop_bool("Pairable", pair_set ? on : g_bt_pairable);

    if (on) {
        bt_fetch_address();
        /* The readback races the vendor pairing window: on a successful
         * pairing vbtd closes the window immediately (Discoverable ->
         * false), so an all-false readback right after the SETs were
         * accepted usually means "a device paired", not "control
         * unavailable" (observed on-car 2026-11-10 11:47: the iPhone
         * paired at :25 and the readback at :46 reported unavailable). */
        if (g_bt_discoverable || g_bt_pairable)
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Pairing on - %s - discoverable %s - pairable %s",
                     g_cfg.cp_btaddr[0] ? g_cfg.cp_btaddr : "BT address unknown",
                     g_bt_discoverable ? "yes" : "no",
                     g_bt_pairable ? "yes" : "no");
        else if (disc_set && pair_set)
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Pairing window closed - a device may have paired; check Settings > Bluetooth");
        else
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Bluetooth control unavailable; use Settings > Bluetooth on the car");
    } else {
        snprintf(g_pair_msg, sizeof(g_pair_msg), "%s",
                 (!disc_set || !pair_set || g_bt_discoverable || g_bt_pairable) ?
                 "Pairing stop not confirmed" : "Pairing stopped");
    }
    set_msg("%s", g_pair_msg);
    bt_state_save();
    if (!on) g_pair_stop_confirmed = disc_set && pair_set && !g_bt_discoverable && !g_bt_pairable;
    return on ? (g_bt_discoverable || g_bt_pairable || (disc_set && pair_set)) : g_pair_stop_confirmed;
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

    char out[2048] = "";
    int rc = sh_out(out, sizeof(out),
                    "connmanctl tether_carplay_ie wifi on %s %s %s %s %s 2>&1",
                    mfg, model, name, id, bt);
    /* The vendor connmanctl can print an asynchronous D-Bus error on stderr
     * and still exit 0, so rc alone is not success.  Treat an error string
     * in the captured output as failure too. */
    bool text_err = strstr(out, "Error") || strstr(out, "error") ||
                    strstr(out, "failed");
    if (rc != 0 || text_err) {
        log_msg("CarPlay IE rc=%d out=%s", rc, trim(out));
        set_msg("CarPlay IE command failed: %s",
                trim(out)[0] ? trim(out) : "see daemon log");
        return false;
    }
    set_msg("CarPlay IE enabled");
    return true;
}

static void carplay_ie_off(void)
{
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    g_carplay_ie = false;
}

static void ensure_carplay_services(void)
{
    /* The vendor carplay_mdnsd.service is RemainAfterExit=true: after the
     * process dies the unit still reports "active", so is-active cannot
     * detect a dead mdnsd (observed on-car: BonjourBrowser
     * kDNSServiceErr_ServiceNotRunning).  Check the process itself and
     * restart the unit when it is gone. */
    if (pid_by_executable("mdnsd") <= 0) {
        log_msg("mdnsd not running; restarting carplay_mdnsd.service");
        sh("systemctl restart carplay_mdnsd.service 2>/dev/null");
    }
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
        snprintf(bak, sizeof(bak), VBT_BAK_DIR "/vbt-%s.original", i == 0 ? "app" : "rw");
        if (!file_exists(bak) && write_file_atomic(bak, before, 0600) != 0) {
            log_msg("cannot preserve VBT baseline for %s", paths[i]);
            continue;
        }
        if (write_file_atomic(paths[i], text, 0644) != 0) {
            log_msg("cannot write %s (continuing to next VBT path)", paths[i]);
            continue;
        }
        if (changed) *changed = true;
        char applied[512];
        snprintf(applied, sizeof(applied), VBT_BAK_DIR "/vbt-%s.applied", i == 0 ? "app" : "rw");
        write_file_atomic(applied, text, 0600);
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
    set_msg("%s", g_pair_msg);
}

static bool client_disconnect(void)
{
    char out[16384];
    if (sh_out(out, sizeof(out), "connmanctl services 2>/dev/null") != 0) return false;
    bool ok = true;
    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *token = NULL;
        for (char *s = line; (s = strstr(s, "wifi_")); s += 5)
            if (s == line || isspace((unsigned char)s[-1])) token = s;
        if (!token) {
            if (!strncmp(trim(line), "Error", 5)) return false;
            continue;
        }
        size_t len = strcspn(token, " \t\r");
        token[len] = 0;
        if (!len || strspn(token, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != len)
            continue;
        if (++count > 32) return false;
        char detail[8192];
        if (sh_out(detail, sizeof(detail), "connmanctl services %s 2>/dev/null", token) != 0) {
            ok = false;
            continue;
        }
        bool active = false, known = false;
        char *d_save = NULL;
        for (char *d = strtok_r(detail, "\n", &d_save); d; d = strtok_r(NULL, "\n", &d_save)) {
            d = trim(d);
            if (strncmp(d, "State = ", 8)) continue;
            known = true;
            const char *state = trim(d + 8);
            active = !strcmp(state, "association") || !strcmp(state, "configuration") ||
                     !strcmp(state, "ready") || !strcmp(state, "online");
        }
        if (!known) ok = false;
        if (active) {
            /* No cellular/ethernet changes, saved-profile deletion, or
             * persistent AutoConnect changes. Read back radio state later. */
            log_msg("station reconciliation: disconnecting active WiFi service");
            if (sh("connmanctl disconnect %s 2>/dev/null", token) != 0) ok = false;
        }
    }
    return ok;
}

static void write_status(void);

/* In client mode an address alone is not proof of connection: a leftover AP
 * address (10.42.0.1) or another interface's address must not count.  Require
 * an actual 4-way-handshake-complete association via the supplicant. */
static bool client_associated(void)
{
    return wpa_state_completed();
}

static bool client_service_ready(const char *service, const char *ip)
{
    char out[8192], compact[8192];
    if (sh_out(out, sizeof(out), "connmanctl services %s 2>/dev/null", service) != 0) return false;
    size_t at = 0;
    for (const char *p = out; *p && at + 1 < sizeof(compact); p++)
        if (!isspace((unsigned char)*p)) compact[at++] = *p;
    compact[at] = 0;
    if (!strstr(compact, "State=ready") && !strstr(compact, "State=online")) return false;
    char address[96]; snprintf(address, sizeof(address), "Address=%s", ip);
    const char *hit = strstr(compact, address);
    if (!hit) return false;
    char next = hit[strlen(address)];
    return next == ',' || next == ']' || next == '\0';
}

static bool client_wait_ip(const char *service, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        char ip[64];
        if (wlan_ip(ip, sizeof(ip)) && client_associated() && client_service_ready(service, ip)) {
            set_msg("client connected to '%s' (%s)", net_ssid(g_client_net), ip);
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
        if (sh_out(out, sizeof(out), "connmanctl services 2>/dev/null") == 0) {
            int net = find_service_any(out, service, service_n, NULL, 0);
            if (net >= 0) {
                g_client_net = net;
                return true;
            }
        }
        usleep(500000);
    }
    return false;
}

typedef struct { bool registered, secret_sent, connected, failed; } agent_state_t;

/* This is deliberately driven by received prompts, not elapsed sleeps. It
 * also gives the tests the exact credential-interaction state machine. */
static bool agent_reply(agent_state_t *s, const char *received,
                        const char *service, char *reply, size_t n)
{
    reply[0] = 0;
    if (strstr(received, "Error") || strstr(received, "connect-failed") ||
        strstr(received, "cancelled") || strstr(received, "Retry (yes/no)")) {
        s->failed = true;
        return false;
    }
    if (!s->registered && strstr(received, "Agent registered")) {
        s->registered = true;
        snprintf(reply, n, "connect %s\n", service);
    } else if (s->registered && strstr(received, "Passphrase?")) {
        const char *psk = net_psk(g_client_net);
        if (s->secret_sent || !psk[0]) { s->failed = true; return false; }
        s->secret_sent = true;
        snprintf(reply, n, "%s\n", psk);
    } else if (s->registered && (strstr(received, "Connected ") ||
                                strstr(received, "Connection successful"))) {
        s->connected = true;
    }
    return true;
}

/* Keep ConnMan as the sole association/DHCP owner. Credentials travel only
 * over the child's stdin after RequestInput; never a script, argv or log. */
static bool client_connman_agent(const char *service)
{
    const char *psk = net_psk(g_client_net);
    if (strchr(psk, '\n') || strchr(psk, '\r')) {
        log_msg("client: passphrase contains a newline; refusing agent flow");
        return false;
    }

    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0) return false;
    if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); return false; }
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO); dup2(out[1], STDERR_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp("connmanctl", "connmanctl", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    if (pid < 0) { close(in[1]); close(out[0]); return false; }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    agent_state_t state = {0};
    char received[8192] = "";
    size_t used = 0;
    bool reaped = false;
    int st = 0;
    (void)write(in[1], "agent on\n", 9);
    int64_t deadline = now_ms() + 45000;
    while (!g_stop && now_ms() < deadline && !state.failed && !state.connected) {
        char chunk[512];
        ssize_t nr = read(out[0], chunk, sizeof(chunk));
        if (nr > 0) {
            if (used + (size_t)nr >= sizeof(received)) { state.failed = true; break; }
            memcpy(received + used, chunk, (size_t)nr); used += (size_t)nr;
            received[used] = 0;
            char reply[512];
            agent_reply(&state, received, service, reply, sizeof(reply));
            if (reply[0]) {
                size_t len = strlen(reply);
                if (write(in[1], reply, len) != (ssize_t)len) state.failed = true;
                memset(reply, 0, sizeof(reply));
                used = 0; received[0] = 0;
            }
        } else if (nr == 0) { break; }
        if (waitpid(pid, &st, WNOHANG) == pid) { reaped = true; break; }
        struct pollfd pfd = {.fd = out[0], .events = POLLIN};
        poll(&pfd, 1, 100);
    }
    /* No raw interactive transcript: it may echo credentials or cached keys. */
    log_msg("ConnMan agent: registered=%d requested_key=%d connected=%d failed=%d",
            state.registered, state.secret_sent, state.connected, state.failed);
    close(in[1]); close(out[0]);
    if (!reaped) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    return !g_stop && state.connected && !state.failed;
}

static bool client_connect(void)
{
    if (net_total() == 0) {
        set_msg("no client SSID imported (use wifi.txt)");
        return false;
    }
    /* Validate every configured network's key up front: the connect target
     * is chosen by scan visibility, so a bad key must fail before the
     * 20-60s stack bring-up, not after it. */
    for (int i = 0; i < WIFI_NETS_MAX; i++) {
        const char *ssid = net_ssid(i);
        if (!ssid[0]) continue;
        size_t keylen = strlen(net_psk(i));
        if (keylen && (keylen < 8 || keylen > 64 ||
            (keylen == 64 && strspn(net_psk(i), "0123456789abcdefABCDEF") != 64))) {
            set_msg("client: WPA key for '%s' must be 8-63 characters or 64 hexadecimal digits", ssid);
            return false;
        }
    }
    if (!ensure_wifi_ready()) {
        set_msg("client: WiFi stack not ready");
        return false;
    }

    sh("connmanctl enable wifi 2>/dev/null");
    iface_up();
    bool station = false;
    for (int i = 0; i < 20 && !g_stop; i++) {
        char info[2048];
        if (sh_out(info, sizeof(info), "iw dev wlan0 info 2>/dev/null") == 0 && strstr(info, "type managed")) {
            station = true; break;
        }
        usleep(250000);
    }
    if (!station) {
        set_msg("client: AP teardown did not yield a managed station interface");
        return false;
    }
    sh("connmanctl scan wifi 2>/dev/null");

    char service[256];
    bool found = client_connman_find(service, sizeof(service));
    if (!found) {
        set_msg("client: none of the %d configured network%s visible; hidden-network provisioning is not supported",
                net_total(), net_total() == 1 ? "" : "s");
        return false;
    }
    if (client_associated() && client_wait_ip(service, 2)) return true;
    bool agent_ok = client_connman_agent(service);
    /* A cached connection can finish between the precheck and connect reply
     * (including an AlreadyConnected error). Actual identity/DHCP readback is
     * authoritative, not the CLI's wording or exit code. */
    if (client_wait_ip(service, 40)) return true;

    char st[2048];
    sh_out(st, sizeof(st), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status 2>/dev/null");
    set_msg("client: expected station association/address not ready for '%s'%s",
            net_ssid(g_client_net), agent_ok ? "" : " (agent failed or timed out)");
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

/* Dedicated chains make cleanup independent of a cancelled child's port
 * handoff, and never remove an identical vendor/owner INPUT accept rule.
 * The chain is hooked into INPUT (inbound CarPlay/mDNS/SSH) and OUTPUT
 * (outbound mDNS announcements): the vendor HARDEN_LOUT chain silently
 * drops all multicast (224.0.0.0/4 and ff00::/8) unless a rule accepts
 * it first, and the stock rules only do that for ncm0 — without the
 * OUTPUT hook the phone can never see the car's _airplay._tcp records
 * even with the AP up. */
static bool fw_chain_ensure(const char *tool, const char *chain)
{
    sh("%s -N %s 2>/dev/null", tool, chain);
    if (sh("%s -C INPUT -i wlan0 -j %s 2>/dev/null", tool, chain) != 0 &&
        sh("%s -I INPUT -i wlan0 -j %s 2>/dev/null", tool, chain) != 0) {
        log_msg("cannot install owned firewall chain %s (INPUT)", chain);
        return false;
    }
    if (sh("%s -C OUTPUT -o wlan0 -j %s 2>/dev/null", tool, chain) != 0 &&
        sh("%s -I OUTPUT -o wlan0 -j %s 2>/dev/null", tool, chain) != 0) {
        log_msg("cannot install owned firewall chain %s (OUTPUT)", chain);
        return false;
    }
    return true;
}

static void fw_chain_close(const char *tool, const char *chain)
{
    for (int i = 0; i < 4 && sh("%s -C INPUT -i wlan0 -j %s 2>/dev/null", tool, chain) == 0; i++)
        sh("%s -D INPUT -i wlan0 -j %s 2>/dev/null", tool, chain);
    for (int i = 0; i < 4 && sh("%s -C OUTPUT -o wlan0 -j %s 2>/dev/null", tool, chain) == 0; i++)
        sh("%s -D OUTPUT -o wlan0 -j %s 2>/dev/null", tool, chain);
    sh("%s -F %s 2>/dev/null", tool, chain);
    sh("%s -X %s 2>/dev/null", tool, chain);
}

static bool fw_open(int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, "WIFI_APP_SSH")) return false;
    return sh("%s -C WIFI_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null", tool, port) == 0 ||
           sh("%s -I WIFI_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null", tool, port) == 0;
}

static void fw_close(int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    sh("%s -D WIFI_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null",
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

static bool fw6_port_add(const char *proto, int port)
{
    char tool[256];
    fw6_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, "WIFI_APP_CP")) return false;
    return sh("%s -C WIFI_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0 ||
           sh("%s -I WIFI_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0;
}

static bool fw_port_add(const char *proto, int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, "WIFI_APP_CP")) return false;
    return sh("%s -C WIFI_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0 ||
           sh("%s -I WIFI_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0;
}

static bool carplay_fw_open(void)
{
    /* AirPlay / CarPlay / DHCP / mDNS on the AP interface.  The stock
     * firewall only opens these on ncm0 (USB CarPlay); wireless needs
     * its own wlan0 rules. */
    bool ok = true;
    const int udp[] = {67, 5353, 5020, 6000, 6003, 6011};
    const int tcp[] = {5000, 5001, 6030, 7000};
    for (size_t i = 0; i < sizeof(udp)/sizeof(udp[0]); i++) if (!fw_port_add("udp", udp[i])) ok = false;
    for (size_t i = 0; i < sizeof(tcp)/sizeof(tcp[0]); i++) if (!fw_port_add("tcp", tcp[i])) ok = false;

    /* mDNS and AirPlay also arrive over IPv6 link-local. */
    const int udp6[] = {5353, 6000, 6003, 6011};
    const int tcp6[] = {5000, 5001, 7000};
    for (size_t i = 0; i < sizeof(udp6)/sizeof(udp6[0]); i++) if (!fw6_port_add("udp", udp6[i])) ok = false;
    for (size_t i = 0; i < sizeof(tcp6)/sizeof(tcp6[0]); i++) if (!fw6_port_add("tcp", tcp6[i])) ok = false;
    return ok;
}

static void carplay_fw_close(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool)); fw_chain_close(tool, "WIFI_APP_CP");
    fw6_tool(tool, sizeof(tool)); fw_chain_close(tool, "WIFI_APP_CP");
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
    /* Open read+write: pread() on an O_WRONLY descriptor fails, which used
     * to concatenate a new account entry onto a file lacking a final \n. */
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
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
    char marker[512] = "";
    if (!strcmp(prefix, "car:")) {
        mkdir(VBT_BAK_DIR, 0700);
        mkdir(VBT_BAK_DIR "/accounts", 0700);
        const char *base = strrchr(path, '/');
        snprintf(marker, sizeof(marker), VBT_BAK_DIR "/accounts/%s", base ? base + 1 : path);
        if (write_file_atomic(marker, line, 0600) != 0) return false;
    }
    bool ok = append_line(path, line) == 0;
    if (!ok && marker[0]) unlink(marker);
    return ok;
}

/*
 * /etc/passwd is a symlink to /tmp/passwd on this firmware.  esinit
 * rewrites /tmp/passwd from /etc/passwd.legacy or /etc/passwd.secure on
 * every boot, so the user must live in those templates too.  This guard
 * covers the current boot and any template the installer missed.
 */
static bool ensure_car_user(void)
{
#ifdef WIFI_TEST
    return true; /* never touch host accounts from the unit-test binary */
#endif
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
    /* argv[0], not arbitrary shell text or dropbearkey arguments. */
    return !strcmp(cmd, DROPBEAR_BIN) || !strcmp(cmd, DROPBEAR_MULTI);
}

static int owned_dropbear_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int pid = -1;
    while ((de = readdir(d))) {
        if (isdigit((unsigned char)de->d_name[0]) && pid_is_our_dropbear(atoi(de->d_name))) {
            pid = atoi(de->d_name); break;
        }
    }
    closedir(d);
    return pid;
}

static bool ssh_running(void)
{
    char buf[64];
    if (read_file(SSH_PID, buf, sizeof(buf)) > 0) {
        int pid = atoi(buf);
        if (pid_is_our_dropbear(pid)) return true;
    }
    return owned_dropbear_pid() > 0;
}

static void ssh_stop(void)
{
    char buf[64];
    if (read_file(SSH_PID, buf, sizeof(buf)) > 0) {
        int pid = atoi(buf);
        if (pid_is_our_dropbear(pid))
            kill_pid(pid);
    }
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            int pid = atoi(de->d_name);
            if (pid_is_our_dropbear(pid)) kill_pid(pid);
        }
        closedir(d);
    }
    for (int i = 0; i < 10; i++) {
        int pid = owned_dropbear_pid();
        if (pid <= 0) break;
        if (i == 9 && pid_is_our_dropbear(pid)) kill(pid, SIGKILL);
        usleep(50000);
    }
    char tool[256]; fw_tool(tool, sizeof(tool));
    fw_chain_close(tool, "WIFI_APP_SSH");
    g_ssh_active_port = 0;
    g_ssh_port_fallback = false;
    g_ssh_bound_ip[0] = 0;
    unlink(SSH_PID);
    unlink(SSH_BOUND);
}

static bool ssh_start(void)
{
    char ip[64] = "";
    bool ready = false;
    if (g_cfg.wlan_on && g_cfg.mode == MODE_CARPLAY) {
        ap_probe_t p;
        ap_probe(&p);
        ready = p.state == AP_READY;
        if (ready) str_copy(ip, sizeof(ip), p.ip);
    } else if (g_cfg.wlan_on && g_cfg.mode == MODE_CLIENT && client_associated() && wlan_ip(ip, sizeof(ip))) {
        char services[16384], service[256];
        ready = sh_out(services, sizeof(services), "connmanctl services 2>/dev/null") == 0 &&
                find_service_any(services, service, sizeof(service), NULL, 0) >= 0 &&
                client_service_ready(service, ip);
    }
    if (!ready) {
        ssh_stop();
        set_msg("SSH deferred: requested WLAN role/address not ready");
        return false;
    }
    if (!file_exists(DROPBEAR_BIN) && !file_exists(DROPBEAR_MULTI)) {
        set_msg("dropbear binary missing");
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

    /* Replace (not append to) the effective authorized_keys: importing a new
     * key set must revoke the previous one.  write_file_atomic renames, so
     * dropbear never sees a half-written file. */
    if (file_exists(SSH_KEYS)) {
        char keys[16384];
        if (read_file(SSH_KEYS, keys, sizeof(keys)) > 0) {
            char eff[512];
            snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
            if (write_file_atomic(eff, keys, 0600) != 0) {
                log_msg("cannot write effective authorized_keys: %s", strerror(errno));
                set_msg("SSH not started: effective key replacement failed");
                return false;
            }
        } else {
            set_msg("SSH not started: imported keys are empty or unreadable");
            return false;
        }
    } else {
        unlink(SSH_HOME "/.ssh/authorized_keys");
    }

    g_ssh_active_port = g_cfg.ssh_port;
    if (!fw_open(g_ssh_active_port)) {
        set_msg("SSH not started: owned firewall rule could not be installed");
        g_ssh_active_port = 0;
        return false;
    }

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
    g_carplay_ie = false;
    ssh_stop();
    ap_stop();
    client_disconnect();
    sh("connmanctl disable wifi 2>/dev/null");
    stop_wpa_supplicant();
    iface_down();
}

static const char *mode_name(void);

/* Runs in the apply child.  All slow network work lives here so the HTTP
 * loop and status polling stay responsive.  The child never writes the
 * config: it reports observed adjustments (band fallback, chosen SSH port)
 * as "set key=value" lines and the parent alone persists them, so a canceled
 * or stale child cannot save a config it did not apply. */
static bool apply_mode_work(bool pair)
{
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        stop_oem_wifi();
        stop_all();
        bt_pair_stop();
        ap_probe_t p;
        ap_probe(&p);
        bool off = !wpa_process_alive() && !ssh_running() && p.state == AP_ABSENT &&
                   (!iface_present() || wifi_power_state() == 0);
        set_msg("%s", off ? "WLAN off" : "WLAN-off cleanup could not be confirmed; recovery will retry");
        return off;
    }

    if (g_cfg.mode == MODE_CLIENT) {
        bt_pair_stop();
        if (!ensure_wifi_ready()) { ssh_stop(); return false; }
        if (!ap_stop()) {
            ssh_stop();
            set_msg("Client mode blocked: AP teardown not confirmed");
            return false;
        }
        carplay_ie_off();
        bool ok = client_connect();
        /* Capture the real association/DHCP error before ssh_start() can
         * replace g_last_msg with "SSH deferred: no WLAN IP".  That
         * overwrite made every client failure look like an SSH problem in
         * the on-car log. */
        char why[256] = "";
        if (!ok) str_copy(why, sizeof(why), g_last_msg);
        if (ok && g_cfg.ssh_enabled) ssh_start();
        else ssh_stop();
        if (ok) set_msg("client mode: %s", g_cfg.ssid);
        else set_msg("client mode failed: %s", why);
        return ok;
    }

    /* MODE_CARPLAY */
    if (g_ap_restart_requested) {
        ssh_stop();
        if (!ap_stop()) {
            set_msg("CarPlay explicit restart blocked: AP teardown not confirmed");
            return false;
        }
    }
    if (pair)
        bt_pair_start();
    else if (g_pairing_stop_requested)
        bt_pair_stop();
    if (!ap_start()) {
        /* ap_start() already recorded the stage-specific failure; do not
         * replace it with a generic message. */
        char why[256] = "";
        str_copy(why, sizeof(why), g_last_msg);
        ssh_stop();
        ap_probe_t failure;
        ap_probe(&failure);
        ap_evidence("apply-failed", &failure);
        char scan[128];
        int rc = sh_out(scan, sizeof(scan), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 get ap_scan 2>/dev/null");
        char *value = trim(scan);
        log_msg("AP failure: get ap_scan rc=%d value=%s (vendor support unverified)", rc,
                !strcmp(value, "0") || !strcmp(value, "1") || !strcmp(value, "2") ? value : "unknown/unsupported");
        set_msg("CarPlay AP failed to start (SSID=%s): %s", g_cfg.cp_ssid, why);
        return false;
    }
    bool fw_ok = carplay_fw_open();
    bool ie_ok = carplay_ie_on();
    g_carplay_ie = ie_ok;
    ensure_carplay_services();

    bool vbt_changed = false;
    bool vbt_ok = vbt_set_wireless_carplay(true, &vbt_changed);
    restart_vbtd_if_needed(vbt_changed);

    if (g_cfg.ssh_enabled) ssh_start();
    else ssh_stop();
    ap_probe_t p;
    ap_probe(&p);
    if (p.state != AP_READY) {
        ssh_stop();
        ap_evidence("lost-during-service-setup", &p);
        set_msg("CarPlay AP lost readiness during service setup");
        return false;
    }
    set_msg("CarPlay AP running (SSID=%s, %d MHz)%s",
            p.ssid, channel_to_freq(),
            ie_ok ? "" : " - CarPlay IE not set");
    bool services_ok = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0 &&
                       sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") == 0;
    if (!services_ok) set_msg("CarPlay AP up, but transport services are not ready");
    if (!vbt_ok) set_msg("CarPlay AP up, but wireless CarPlay configuration could not be confirmed");
    if (!fw_ok) set_msg("CarPlay AP up, but owned firewall rules could not be installed");
    return ie_ok && services_ok && vbt_ok && fw_ok;
}

/* Cancel an in-flight apply: the newest user intent wins.  The child runs in
 * its own process group so shell/connmanctl descendants are signaled too,
 * not just the worker PID. */
static void apply_cancel(void)
{
    if (g_apply_pid <= 0) return;
    g_apply_cancelled = true;
    g_cancel_ms = now_ms();
    kill(-g_apply_pid, SIGTERM);
    kill(g_apply_pid, SIGTERM);
}

static bool dump_logs_to_usb(bool verbose);
static void scan_networks(char *out, size_t n);
static void start_pending_work(void);

/* Entire result must belong to the current generation before any mutation,
 * including messages, success flags, Bluetooth or SSH observations. */
static bool accept_apply_result(char *buf)
{
    char *save = NULL;
    char *l1 = strtok_r(buf, "\n", &save), *msg = strtok_r(NULL, "\n", &save);
    int gen = -1, ok = -1;
    char extra;
    if (!l1 || !msg || sscanf(l1, "%d %d %c", &gen, &ok, &extra) != 2 ||
        (ok != 0 && ok != 1) || gen != g_apply_gen) return false;
    char *line;
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        int v = atoi(eq);
        if (!strcmp(line, "cp_band") && v >= 0 && v <= 2) g_cfg.cp_band = v;
        else if (!strcmp(line, "cp_channel") && v > 0 && v <= 165) g_cfg.cp_channel = v;
        else if (!strcmp(line, "ssh_port") && v > 0 && v <= 65535) g_cfg.ssh_port = v;
        else if (!strcmp(line, "carplay_ie")) g_carplay_ie = v == 1;
        else if (!strcmp(line, "ssh_active_port")) g_ssh_active_port = v;
        else if (!strcmp(line, "ssh_fallback")) g_ssh_port_fallback = v == 1;
        else if (!strcmp(line, "bt_discoverable")) g_bt_discoverable = v == 1;
        else if (!strcmp(line, "bt_pairable")) g_bt_pairable = v == 1;
        else if (!strcmp(line, "pair_stop_ok") && v == 1) g_pairing_stop_requested = false;
        else if (!strcmp(line, "bt_address")) str_copy(g_cfg.cp_btaddr, sizeof(g_cfg.cp_btaddr), eq);
        else if (!strcmp(line, "cp_id")) str_copy(g_cfg.cp_id, sizeof(g_cfg.cp_id), eq);
        else if (!strcmp(line, "pair_message")) str_copy(g_pair_msg, sizeof(g_pair_msg), eq);
    }
    g_apply_failed = !ok;
    if (g_work_kind == WORK_MODE) g_network_failed = !ok;
    set_msg("%s", msg);
    if (!config_save()) {
        g_apply_failed = true;
        set_msg("State applied, but preferences could not be saved; they may revert at reboot");
    }
    bt_state_save();
    return true;
}

static void request_work(work_t kind, bool pair)
{
    if (!g_cfg.wlan_on || g_cfg.mode != MODE_CARPLAY) g_ap_restart_requested = false;
    if (kind == WORK_DUMP && (g_apply_pid > 0 || g_work_pending)) {
        g_dump_requested = true;
        return;
    }
    /* An SSH/import change during a mode transition must complete that
     * transition using the latest preferences, not leave it half applied. */
    if (kind == WORK_SSH && (g_work_pending || g_apply_pid > 0) &&
        (g_work_kind == WORK_MODE || g_pending_kind == WORK_MODE ||
         g_work_kind == WORK_SCAN || g_pending_kind == WORK_SCAN)) {
        kind = WORK_MODE;
        pair = g_current_pair || g_pending_pair;
    }
    apply_cancel();
    if (kind != WORK_DUMP) g_apply_gen++;
    g_last_apply_ms = now_ms();
    g_apply_failed = false;
    if (kind == WORK_MODE) g_last_mode_ms = g_last_apply_ms;
    if (kind == WORK_DUMP) g_last_dump_try_ms = g_last_apply_ms;
    g_pending_kind = kind;
    g_pending_pair = pair;
    g_work_pending = true;
    config_save();  /* persist user intent, including Off, before slow work */
    set_msg("%s", kind == WORK_SCAN ? "Scanning..." :
            kind == WORK_DUMP ? "Saving logs..." :
            kind == WORK_SSH ? "Updating SSH..." :
            kind == WORK_PAIR_STOP ? "Stopping pairing..." : "Applying mode...");
    start_pending_work();
    write_status();
}

static void apply_mode(bool pair) { request_work(WORK_MODE, pair); }

static void start_pending_work(void)
{
    if (g_apply_pid > 0 || !g_work_pending || g_stop) return;
    g_work_kind = g_pending_kind;
    g_current_pair = g_pending_pair;
    g_work_pending = false;
    g_apply_cancelled = false;
    unlink(APPLY_RESULT);
    pid_t pid = fork();
    if (pid == 0) {
        g_in_child = true;
        setpgid(0, 0);
        /* Do not hold the HTTP listen/connection sockets open while the
         * apply runs; otherwise the client connection would stay half-open
         * until the apply finishes. */
        close_child_fds(-1);
        g_apply_pid = 0;
        bool ok = false;
#ifdef WIFI_TEST
        if (test_work) ok = test_work(g_work_kind, g_current_pair);
        else
#endif
        if (g_work_kind == WORK_MODE) ok = apply_mode_work(g_current_pair);
        else if (g_work_kind == WORK_SSH) {
            if (g_pairing_stop_requested) bt_pair_stop();
            ssh_stop();
            ok = !g_cfg.ssh_enabled || !g_cfg.wlan_on || g_cfg.mode == MODE_OFF || ssh_start();
        } else if (g_work_kind == WORK_PAIR_STOP) {
            bt_pair_stop();
            ok = g_pair_stop_confirmed;
        } else if (g_work_kind == WORK_DUMP) ok = dump_logs_to_usb(true);
        else if (g_work_kind == WORK_SCAN) {
            char scan[32768];
            scan_networks(scan, sizeof(scan));
            ok = strstr(scan, "\"ok\":true") != NULL;
            if (!g_stop) write_file_atomic(SCAN_RESULT, scan, 0600);
        }
        char buf[2048];
        snprintf(buf, sizeof(buf), "%d %d\n%s\ncp_band=%d\ncp_channel=%d\n"
                 "ssh_port=%d\ncarplay_ie=%d\nssh_active_port=%d\nssh_fallback=%d\n"
                 "bt_discoverable=%d\nbt_pairable=%d\npair_stop_ok=%d\nbt_address=%s\ncp_id=%s\npair_message=%s\n",
                 g_apply_gen, ok ? 1 : 0, g_last_msg,
                 g_cfg.cp_band, g_cfg.cp_channel, g_cfg.ssh_port,
                 g_carplay_ie, g_ssh_active_port, g_ssh_port_fallback,
                 g_bt_discoverable, g_bt_pairable, g_pair_stop_confirmed,
                 g_cfg.cp_btaddr, g_cfg.cp_id, g_pair_msg);
        if (!g_stop) write_file_atomic(APPLY_RESULT, buf, 0600);
        _exit(0);
    }
    if (pid < 0) {
        g_apply_failed = true;
        if (g_work_kind == WORK_MODE) g_network_failed = true;
        set_msg("apply: fork failed");
        return;
    }
    setpgid(pid, pid);
    g_apply_pid = pid;
    g_apply_start_ms = now_ms();
}

static void apply_reap(void)
{
    if (g_apply_pid <= 0) { start_pending_work(); return; }
    int st = 0;
    pid_t pid = g_apply_pid;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0) {
        if (g_apply_cancelled && now_ms() - g_cancel_ms > 300) {
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
        } else if (!g_apply_cancelled && now_ms() - g_apply_start_ms > 150000) {
            g_apply_failed = true;
            if (g_work_kind == WORK_MODE) g_network_failed = true;
            set_msg("apply timed out");
            apply_cancel();
        }
        return;
    }
    if (r < 0 && errno != ECHILD) return;
    /* Always clean the command group, even when its leader exited first. */
    kill(-pid, SIGKILL);
    g_apply_pid = 0;
    char buf[2048] = "";
    bool valid = !g_apply_cancelled && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
                 read_file(APPLY_RESULT, buf, sizeof(buf)) > 0 && accept_apply_result(buf);
    unlink(APPLY_RESULT);
    if (g_work_kind == WORK_MODE && (!g_apply_cancelled || !g_work_pending)) g_ap_restart_requested = false;
    if (!valid && !g_apply_cancelled) {
        g_apply_failed = true;
        if (g_work_kind == WORK_MODE) g_network_failed = true;
        set_msg("worker failed; result absent, invalid or stale");
    }
    if (g_work_kind == WORK_DUMP) {
        g_last_dump_try_ms = now_ms();
        if (valid && !g_apply_failed) { g_boot_dumped = true; g_last_autodump_ms = now_ms(); }
    }
    g_apply_cancelled = false;
    g_last_probe_ms = 0;
    start_pending_work();
    if (!g_apply_pid && !g_work_pending && g_dump_requested) {
        g_dump_requested = false;
        request_work(WORK_DUMP, false);
    }
    write_status();
}

static void watchdog(void)
{
    int64_t now = now_ms();
    if (now - g_last_watchdog_ms < 10000) return;
    g_last_watchdog_ms = now;

    bool oem_respawned = pid_by_executable("supplicantmanager") > 0;

    /* Log dumps: one attempt ~15 s after start (captures the install), and
     * a throttled dump while applies keep failing so the failure state
     * lands on the stick without anyone pressing a button. */
    if (!g_boot_dumped && g_start_ms && now - g_start_ms > 15000 &&
        (!g_last_dump_try_ms || now - g_last_dump_try_ms > 60000)) {
        g_last_dump_try_ms = now;
        request_work(WORK_DUMP, false);
    }
    if (g_network_failed &&
        (!g_last_dump_try_ms || now - g_last_dump_try_ms > 600000))
        request_work(WORK_DUMP, false);

    if (g_apply_pid > 0 || g_work_pending) return;
    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        if ((oem_respawned || wpa_process_alive() || g_observed.ap.role == 1 || g_observed.ssh_ours) &&
            now - g_last_mode_ms > (g_network_failed ? 60000 : 15000)) apply_mode(false);
        return;
    }

    /* Recovery goes through the serialized apply worker with backoff; never
     * call blocking ensure_wifi_ready() from the HTTP loop.  A live supplicant
     * with a temporarily missing socket is an interface transition, not a
     * reason to tear the stack down. */
    int64_t retry_ms = g_network_failed ? 60000 : 15000;
    bool stack_dead = !wpa_process_alive();
    if ((stack_dead || oem_respawned) && now - g_last_mode_ms > retry_ms) {
        log_msg("watchdog: supplicant process gone; re-applying (backoff=%ds)",
                g_apply_failed ? 60 : 15);
        apply_mode(false);
        return;
    }

    /* Missing/stale observations are not evidence of a radio failure. */
    if (!g_observed_ms || now - g_observed_ms > 15000 || g_observed.gen != g_apply_gen) {
        g_ap_unready_ms = 0;
        return;
    }
    if (g_ap_unready_gen != g_apply_gen) {
        g_ap_unready_gen = g_apply_gen;
        g_ap_unready_ms = 0;
    }
    if (g_cfg.mode == MODE_CARPLAY && !g_observed.ap_on) {
        if (g_observed.ap.state == AP_UNKNOWN) {
            g_ap_unready_ms = 0;
            if (g_observed.ssh_ours) request_work(WORK_SSH, false);
            return;
        }
        if (!g_ap_unready_ms) g_ap_unready_ms = now;
        /* A pending AP gets its address/SSID grace period. ap_start() will
         * reuse/wait for the interface, not turn it off on property lag. */
        if (now - g_ap_unready_ms < 30000) {
            if (g_observed.ssh_ours) request_work(WORK_SSH, false);
            return;
        }
    } else g_ap_unready_ms = 0;

    if (g_apply_pid == 0 &&
        g_cfg.mode == MODE_CARPLAY &&
        (!g_observed.ap_on || !g_carplay_ie || !g_observed.carplay_svc || !g_observed.mdns_svc)) {
        if (now - g_last_mode_ms > retry_ms) {
            log_msg("watchdog: CarPlay AP down; re-applying (backoff=%ds)",
                    g_apply_failed ? 60 : 15);
            apply_mode(false);
            return;
        }
    } else if (g_apply_pid == 0 && g_cfg.mode == MODE_CLIENT) {
        if ((!g_observed.ip[0] || !g_observed.associated) &&
            now - g_last_mode_ms > retry_ms + 5000) {
            log_msg("watchdog: client not associated; re-applying (backoff=%ds)",
                    g_apply_failed ? 60 : 15);
            apply_mode(false);
            return;
        }
    }
    if (g_cfg.ssh_enabled) {
        bool role_ready = (g_cfg.mode == MODE_CARPLAY && g_observed.ap_on) ||
                          (g_cfg.mode == MODE_CLIENT && g_observed.associated);
        if (g_observed.ip[0] && role_ready) {
            if (!g_observed.ssh_ours) {
                log_msg("watchdog: SSH down; restarting");
                request_work(WORK_SSH, false);
            } else {
                char bound[64] = "";
                if (read_file(SSH_BOUND, bound, sizeof(bound)) > 0) {
                    char *b = trim(bound);
                    if (*b && strcmp(b, g_observed.ip)) {
                        /* dropbear is bound to the old address; rebind. */
                        log_msg("watchdog: wlan IP changed %s -> %s; rebinding SSH",
                                b, g_observed.ip);
                        request_work(WORK_SSH, false);
                    }
                }
            }
        } else if (g_observed.ssh_ours) request_work(WORK_SSH, false);
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

static void collect_observation(observation_t *o)
{
    memset(o, 0, sizeof(*o));
    o->gen = g_apply_gen;
    o->driver = iface_present();
    ap_probe(&o->ap);
    o->ap_on = o->ap.state == AP_READY;
    str_copy(o->ap_ssid, sizeof(o->ap_ssid), o->ap.ssid);
    o->clients = o->ap_on ? ap_clients() : 0;
    o->associated = g_cfg.mode == MODE_CLIENT && client_associated();
    if (g_cfg.wlan_on && g_cfg.mode == MODE_CARPLAY && o->ap_on)
        str_copy(o->ip, sizeof(o->ip), o->ap.ip);
    if (g_cfg.wlan_on && g_cfg.mode == MODE_CLIENT && o->associated)
        wlan_ip(o->ip, sizeof(o->ip));
    if (o->associated && o->ip[0]) {
        char services[16384], service[256];
        if (sh_out(services, sizeof(services), "connmanctl services 2>/dev/null") != 0 ||
            find_service_any(services, service, sizeof(service), NULL, 0) < 0 ||
            !client_service_ready(service, o->ip)) {
            o->associated = false;
            o->ip[0] = 0;
        }
    }
    if (o->associated) wlan_link(o->ssid, sizeof(o->ssid), &o->signal);
    o->carplay_svc = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0;
    /* RemainAfterExit makes is-active lie for mdnsd; report the process. */
    o->mdns_svc = pid_by_executable("mdnsd") > 0;
    o->ssh_ours = ssh_running();
    o->vendor_ssh_22 = port_listening(22) && !(o->ssh_ours && g_ssh_active_port == 22);
    o->debug_console = file_exists("/tmp/secure_console_disable");
    o->secure_console = -1;
    char scbuf[32];
    if (read_file("/sys/module/micom/parameters/secure_console", scbuf, sizeof(scbuf)) > 0)
        o->secure_console = atoi(trim(scbuf));
}

/* Slow status probes are read-only children, never part of the HTTP loop.
 * The pipe result is accepted only for the current user-intent generation. */
static void probe_tick(void)
{
    int64_t now = now_ms();
    if (g_probe_pid > 0) {
        int st = 0;
        pid_t r = waitpid(g_probe_pid, &st, WNOHANG);
        if (r == 0) {
            if (now - g_probe_start_ms > 12000) kill(-g_probe_pid, SIGKILL);
            return;
        }
        if (r < 0 && errno != ECHILD) return;
        observation_t sample;
        ssize_t n = read(g_probe_fd, &sample, sizeof(sample));
        if (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
            n == sizeof(sample) && sample.gen == g_apply_gen) {
            g_observed = sample;
            g_observed_ms = sample.ap.sampled_ms;
        }
        kill(-g_probe_pid, SIGKILL);
        close(g_probe_fd); g_probe_fd = -1; g_probe_pid = 0;
        g_last_probe_ms = now;
    }
    if (g_stop || now - g_last_probe_ms < 2000) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0); g_in_child = true; g_probe_child = true;
        close_child_fds(fds[1]);
        observation_t sample;
        collect_observation(&sample);
        if (write(fds[1], &sample, sizeof(sample)) != sizeof(sample)) _exit(1);
        _exit(0);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return; }
    setpgid(pid, pid);
    g_probe_pid = pid; g_probe_fd = fds[0]; g_probe_start_ms = now;
}

static void build_status(char *out, size_t n)
{
    bool fresh = g_observed_ms > 0 && now_ms() - g_observed_ms < 15000 &&
                 g_observed.gen == g_apply_gen;
    observation_t unknown = {.secure_console = -1};
    const observation_t *o = fresh ? &g_observed : &unknown;
    int active_ssh_port = g_ssh_active_port ? g_ssh_active_port : g_cfg.ssh_port;

    char ssid_e[256], ip_e[128], ap_e[256], msg_e[512];
    char imp_e[512], cps_e[256], cpsk_e[256], bt_e[128], id_e[128];
    char client_e[256], pair_e[512], nets_e[640];
    char nets_raw[512];
    json_escape(ssid_e, sizeof(ssid_e), o->ssid);
    json_escape(ip_e, sizeof(ip_e), o->ip);
    json_escape(ap_e, sizeof(ap_e), o->ap_ssid);
    json_escape(msg_e, sizeof(msg_e), g_last_msg);
    json_escape(imp_e, sizeof(imp_e), g_cfg.imported_from);
    json_escape(cps_e, sizeof(cps_e), g_cfg.cp_ssid);
    json_escape(cpsk_e, sizeof(cpsk_e), g_cfg.cp_psk);
    json_escape(bt_e, sizeof(bt_e), g_cfg.cp_btaddr);
    json_escape(id_e, sizeof(id_e), g_cfg.cp_id);
    json_escape(client_e, sizeof(client_e), g_cfg.ssid);
    json_escape(pair_e, sizeof(pair_e), g_pair_msg);
    /* Priority-ordered client network list for the UI, e.g.
     * "iPhone|House LANnister" (never the passphrases). */
    {
        size_t at = 0;
        for (int i = 0; i < WIFI_NETS_MAX; i++) {
            const char *ssid = net_ssid(i);
            if (!ssid[0]) continue;
            if (at && at + 1 < sizeof(nets_raw)) nets_raw[at++] = '|';
            for (const char *p = ssid; *p && at + 1 < sizeof(nets_raw); p++)
                nets_raw[at++] = *p;
        }
        nets_raw[at] = 0;
    }
    json_escape(nets_e, sizeof(nets_e), nets_raw);

    snprintf(out, n,
             "{\"ok\":true,"
             "\"build_id\":\"%s\","
             "\"updated_ms\":%lld,"
             "\"observations_fresh\":%s,"
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
             "\"ap_interface\":%s,"
             "\"ap_state\":%d,"
             "\"ap_tether_field\":%d,"
             "\"ap_ssid\":\"%s\","
             "\"ap_clients\":%d,"
             "\"carplay_ready\":%s,"
             "\"carplay_ie\":%s,"
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
             "\"client_networks\":\"%s\","
             "\"client_net_count\":%d,"
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
             WIFI_BUILD_ID, (long long)(time(NULL) * 1000LL), fresh ? "true" : "false",
             mode_name(),
             (g_apply_pid > 0 || g_work_pending) ? "true" : "false",
             g_home_seq, g_home_epoch, g_home_source, g_home_last_key,
             g_home_monitor ? "true" : "false",
             g_cfg.wlan_on ? "true" : "false",
             o->driver ? "true" : "false",
             ip_e, ssid_e, o->signal,
             o->ap_on ? "true" : "false", o->ap.role == 1 ? "true" : "false",
             o->ap.state, fresh ? o->ap.tether : -1, ap_e, o->clients,
             /* "ready" requires every wireless-CarPlay component, not just
              * one of the two services. */
             (g_cfg.mode == MODE_CARPLAY && g_cfg.wlan_on && !g_network_failed && o->ap_on &&
              g_carplay_ie && o->carplay_svc && o->mdns_svc) ? "true" : "false",
             g_carplay_ie ? "true" : "false",
             o->carplay_svc ? "true" : "false",
             o->mdns_svc ? "true" : "false",
             o->ssh_ours ? "true" : "false",
             g_cfg.ssh_enabled ? "true" : "false",
             active_ssh_port,
             g_ssh_port_fallback ? "true" : "false",
             o->vendor_ssh_22 ? "true" : "false",
             o->secure_console,
             o->debug_console ? "true" : "false",
             g_cfg.imported ? "true" : "false", imp_e,
             client_e, nets_e, net_total(), cps_e, cpsk_e, g_cfg.cp_psk[0] ? "true" : "false",
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
    /* Validate before canceling: a scan request while the CarPlay AP is up
     * must not kill the running apply as a side effect.  Scanning must also
     * not change the configured mode; if the stack was off, bring it up for
     * the scan only and put it back down afterwards. */
    if (g_cfg.mode == MODE_CARPLAY) {
        snprintf(out, n,
                 "{\"ok\":false,\"error\":\"Stop the CarPlay AP before scanning\"}\n");
        return;
    }
    ap_probe_t ap;
    ap_probe(&ap);
    if (ap.state != AP_ABSENT) {
        snprintf(out, n, "{\"ok\":false,\"error\":\"AP absence not confirmed; scanning blocked\"}\n");
        return;
    }
    bool was_off = !g_cfg.wlan_on || g_cfg.mode == MODE_OFF;
    if (was_off) {
        /* Scan-only bring-up: do not persist mode/wlan_on. */
        g_cfg.wlan_on = true;
        g_cfg.mode = MODE_CLIENT;
    }
    set_msg("Scanning...");
    write_status();
    if (!ensure_wifi_ready()) {
        if (was_off) {
            stop_all();
            g_cfg.wlan_on = false;
            g_cfg.mode = MODE_OFF;
        }
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
        sh("connmanctl scan wifi 2>/dev/null");
        sleep(3);
        rc = sh_out(buf, sizeof(buf), "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results 2>/dev/null");
        if (rc == 0 && strchr(buf, '\t'))
            break;
    }
    size_t used = 0;
    int count = 0;
    used += (size_t)snprintf(out + used, n - used,
                              "{\"ok\":true,\"gen\":%d,\"scanning\":false,\"networks\":[", g_apply_gen);

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
    if (was_off) {
        /* Restore the configured off state; the scan was temporary. */
        stop_all();
        g_cfg.wlan_on = false;
        g_cfg.mode = MODE_OFF;
    }
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
    FILE *in = fopen(src, "r");
    if (!in) return;
    char path[768]; snprintf(path, sizeof(path), "%s/%s", dst_dir, base);
    FILE *out = fopen(path, "w");
    if (out) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)))
            if (fwrite(buf, 1, n, out) != n) break;
        fclose(out);
    }
    fclose(in);
}

static bool credential_line(const char *s)
{
    return strcasestr(s, "passphrase") || strcasestr(s, "password") ||
           strcasestr(s, "psk") || strstr(s, "set_network") ||
           strstr(s, "connmanctl>") || strstr(s, "Error '") ||
           strcasestr(s, "hexdump") || strcasestr(s, "private key");
}

/* Old logs may contain credentials no longer in g_cfg. Export only recognised
 * event lines, not raw CLI transcripts, arbitrary historical continuation
 * lines or key dumps. Keep the original logs private on the head unit. */
static void copy_event_log(const char *dst_dir, const char *src)
{
    FILE *in = fopen(src, "r");
    if (!in) return;
    const char *base = strrchr(src, '/');
    char path[768]; snprintf(path, sizeof(path), "%s/%s", dst_dir, base ? base + 1 : src);
    FILE *out = fopen(path, "w");
    if (!out) { fclose(in); return; }
    fputs("Sanitized event-only export; raw transcripts and key material omitted.\n", out);
    char line[4096];
    while (fgets(line, sizeof(line), in)) {
        bool whole = strchr(line, '\n') != NULL;
        if (!whole) { int c; while ((c = fgetc(in)) != EOF && c != '\n') {} continue; }
        if (credential_line(line)) continue;
        bool event = strstr(line, "State:") || strstr(line, "CTRL-EVENT-") ||
                     strstr(line, "AP-ENABLED") || strstr(line, "AP-DISABLED") ||
                      strstr(line, "Failed to start AP functionality") ||
                      strstr(line, " AP stage=") || strstr(line, " AP tether request frequency=") ||
                      strstr(line, " AP failure: get ap_scan") || strstr(line, " station reconciliation:") ||
                      strstr(line, " AP startup: unexpected station") ||
                      strstr(line, "[ Disconnect ] wpas_dbus_handler_disconnect") ||
                      strstr(line, "nl80211 driver interface is not designed to be used with ap_scan=2") ||
                     strstr(line, " wifi_daemon started:") ||
                     strstr(line, " watchdog:") || strstr(line, " apply ") ||
                     strstr(line, " worker ") || strstr(line, " started wpa_supplicant") ||
                     strstr(line, " control socket ") || strstr(line, " msg:");
        if (!event) continue;
        redact_secret(line, g_cfg.cp_psk); redact_secret(line, g_cfg.psk);
        fputs(line, out);
    }
    fclose(in); fclose(out);
}

static void scrub_snapshot(char *out)
{
    redact_secret(out, g_cfg.cp_psk); redact_secret(out, g_cfg.psk);
    char *p = out;
    while (*p) {
        char *end = strchr(p, '\n');
        char saved = end ? *end : 0;
        if (end) *end = 0;
        bool sensitive = credential_line(p);
        if (end) *end = saved;
        if (sensitive) {
            if (end) memmove(p, end + 1, strlen(end + 1) + 1);
            else *p = 0;
        } else if (end) p = end + 1;
        else break;
    }
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
        else if (!strncmp(line, "net2_psk=", 9)) emit = "net2_psk=<redacted>";
        else if (!strncmp(line, "net3_psk=", 9)) emit = "net3_psk=<redacted>";
        else if (!strncmp(line, "net4_psk=", 9)) emit = "net4_psk=<redacted>";
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

/* Run a snapshot command and scrub known secret material (the AP passphrase
 * appears both in config.conf and in connmanctl's TetheringPassphrase) before
 * writing it to the USB bundle. */
static void snap_cmd(FILE *f, const char *cmd)
{
    char out[16384] = "";
    fprintf(f, "\n$ %s\n", cmd);
    sh_out(out, sizeof(out), "%s 2>&1", cmd);
    scrub_snapshot(out);
    fputs(out, f);
    fprintf(f, "\n");
}

static bool redact_json_string(char *buf, size_t n, const char *field)
{
    char needle[80]; snprintf(needle, sizeof(needle), "\"%s\"", field);
    char *start = buf, *key;
    while ((key = strstr(start, needle))) {
        char *value = key + strlen(needle);
        while (isspace((unsigned char)*value)) value++;
        if (*value != ':') { start = value; continue; }
        value++;
        while (isspace((unsigned char)*value)) value++;
        if (*value != '"') return false;
        char *end = value + 1;
        while (*end && *end != '"') {
            if (*end == '\\' && end[1]) end++;
            end++;
        }
        if (*end != '"') return false;
        end++;
        const char *redacted = "\"<redacted>\"";
        size_t prefix = (size_t)(value - buf), tail = strlen(end);
        if (prefix + strlen(redacted) + tail + 1 >= n) return false;
        memmove(value + strlen(redacted), end, tail + 1);
        memcpy(value, redacted, strlen(redacted));
        start = value + strlen(redacted);
    }
    return true;
}

/* Preserve the recorded observations/timestamps, including in one-shot dumps
 * that have no live observation cache. Scrub fields structurally so historical
 * keys need not equal the current configuration. Omit free-form old messages. */
static void copy_status_redacted(const char *dst_dir)
{
    char buf[8192];
    if (read_file(g_status_path, buf, sizeof(buf)) <= 0) return;
    const char *fields[] = {"carplay_psk", "psk", "password", "message", NULL};
    for (int i = 0; fields[i]; i++)
        if (!redact_json_string(buf, sizeof(buf), fields[i])) return;
    char dst[512];
    snprintf(dst, sizeof(dst), "%s/status.json", dst_dir);
    write_file_atomic(dst, buf, 0644);
}

/* Keep only the newest LOG_DUMP_KEEP bundles; stamps are fixed width so
 * a lexical sort is a chronological sort. */
#define LOG_DUMP_KEEP 5
static bool is_pruneable_name(const char *name)
{
    if (!name || strlen(name) != 24 || strncmp(name, "wifi_logs_", 10)) return false;
    for (int i = 10; i < 24; i++) if (!isdigit((unsigned char)name[i])) return false;
    return true;
}

static bool remove_tree_at(int parent, const char *name, int depth)
{
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlinkat(parent, name, 0) == 0;
    if (depth >= 16) return false;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return false; }
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!remove_tree_at(fd, de->d_name, depth + 1)) ok = false;
    }
    closedir(d);
    return ok && unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

static int dump_filter(const struct dirent *de) { return is_pruneable_name(de->d_name); }

static void prune_old_dumps(const char *usb)
{
    int fd = open(usb, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    struct dirent **names = NULL;
    int count = scandir(usb, &names, dump_filter, alphasort);
    int directories = 0;
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (fstatat(fd, names[i]->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)) directories++;
    }
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (directories > LOG_DUMP_KEEP &&
            fstatat(fd, names[i]->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) &&
            remove_tree_at(fd, names[i]->d_name, 0)) directories--;
        free(names[i]);
    }
    free(names); close(fd);
}

static bool dump_logs_to_usb(bool verbose)
{
    char usb[512];
    if (!find_usb_dir(usb, sizeof(usb))) {
        log_msg("log dump: no USB mount found");
        if (verbose) set_msg("log dump: no USB stick found");
        return false;
    }
    char usb_q[2200]; shq(usb_q, sizeof(usb_q), usb);
    sh("mount -o remount,rw %s 2>/dev/null", usb_q);

    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);

    char dir[768];
    snprintf(dir, sizeof(dir), "%s/wifi_logs_%s", usb, stamp);
    /* Never follow pre-existing bundle/file symlinks or overwrite a bundle
     * created by another request in the same second. */
    if (mkdir(dir, 0700) != 0) {
        log_msg("log dump: mkdir %s failed: %s", dir, strerror(errno));
        if (verbose) set_msg("log dump failed: %s", strerror(errno));
        return false;
    }

    copy_event_log(dir, LOG_PATH);
    copy_event_log(dir, WPA_LOG);
    copy_event_log(dir, APP_DIR "/ssh.log");
    copy_event_log(dir, APP_DIR "/install.log");
    copy_status_redacted(dir);
    copy_if_exists(dir, HOME_STATE);
    copy_config_redacted(dir);

    char state[800];
    snprintf(state, sizeof(state), "%s/state.txt", dir);
    FILE *f = fopen(state, "w");
    if (f) {
        fprintf(f, "wifi log dump %s build_id=%s mono_ms=%lld\n", stamp, WIFI_BUILD_ID, (long long)now_ms());
        snap_cmd(f, "uname -a");
        snap_cmd(f, "cat /proc/mounts");
        snap_cmd(f, "date");
        snap_cmd(f, "ps w | grep -E 'wpa_supplicant|connmand|supplicantmanager|wifi_daemon|dropbear|vbtd' | grep -v grep");
        snap_cmd(f, "connmanctl technologies");
        snap_cmd(f, "connmanctl services");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 get ap_scan");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results");
        snap_cmd(f, "ls -la " WPA_CTRL_DIR);
        snap_cmd(f, "getfattr -n security.SMACK64 /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti");
        snap_cmd(f, "getfattr -n security.SMACK64EXEC /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti");
        snap_cmd(f, "systemctl is-enabled supplicantmanager.path supplicantmanager.service");
        snap_cmd(f, "iw dev wlan0 info");
        snap_cmd(f, "iwconfig wlan0");
        snap_cmd(f,
                 "grep -RIn 'wifiPage\\.\\(visible\\|open\\|close\\)' "
                 "/app/share/AppHomeScreen/qml_eco3.0 2>/dev/null | head -80");
        snap_cmd(f,
                 "journalctl -b --no-pager 2>/dev/null | "
                 "grep -iE 'WifiPage|qml' | tail -n 200");
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
    size_t used = 0;
    req[0] = 0;
    int64_t deadline = now_ms() + 200;
    while (used + 1 < sizeof(req) && !strchr(req, '\n') && now_ms() < deadline) {
        ssize_t nr = recv(fd, req + used, sizeof(req) - used - 1, MSG_DONTWAIT);
        if (nr > 0) { used += (size_t)nr; req[used] = 0; continue; }
        if (nr == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        poll(&pfd, 1, 10);
    }
    if (!strchr(req, '\n')) {
        http_reply(fd, 400, "{\"ok\":false,\"error\":\"incomplete request line\"}\n");
        close(fd); return;
    }

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
        char poll_value[8];
        if (query_get(query, "poll", poll_value, sizeof(poll_value))) {
            if (g_scan_gen != g_apply_gen)
                snprintf(body, sizeof(body), "{\"ok\":false,\"scanning\":false,\"error\":\"scan cancelled\"}\n");
            else if (read_file(SCAN_RESULT, body, sizeof(body)) <= 0)
                snprintf(body, sizeof(body), "{\"ok\":true,\"scanning\":%s,\"networks\":[]}\n",
                         (g_apply_pid || g_work_pending) ? "true" : "false");
        } else if (g_cfg.mode == MODE_CARPLAY || !g_observed_ms ||
                   now_ms() - g_observed_ms > 15000 || g_observed.gen != g_apply_gen ||
                   g_observed.ap.state != AP_ABSENT) {
            code = 409;
            snprintf(body, sizeof(body), "{\"ok\":false,\"scanning\":false,\"error\":\"Wait for confirmed AP absence before scanning\"}\n");
        } else {
            unlink(SCAN_RESULT);
            request_work(WORK_SCAN, false);
            g_scan_gen = g_apply_gen;
            code = 202;
            snprintf(body, sizeof(body), "{\"ok\":true,\"scanning\":true,\"networks\":[]}\n");
        }
    } else if (!strcmp(method, "GET") && !strcmp(path, "/logs")) {
        char src[64] = "";
        const char *log_path = LOG_PATH;
        if (query_get(query, "source", src, sizeof(src)) &&
            (!strcmp(src, "wpa") || !strcmp(src, "wpa_supplicant")))
            log_path = WPA_LOG;
        else if (!strcmp(src, "ssh"))
            log_path = APP_DIR "/ssh.log";
        char log[16384] = "";
        int n = read_tail(log_path, log, sizeof(log));
        if (n < 0) {
            log[0] = 0;
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"cannot read log\"}\n");
            code = 500;
        } else {
            char esc[32768];
            json_escape(esc, sizeof(esc), log);
            snprintf(body, sizeof(body), "{\"ok\":true,\"log\":\"%s\"}\n", esc);
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/import")) {
        /* Import only stores credentials; it must never activate a mode or
         * SSH by itself.  Activation stays an explicit user action. */
        bool ok = import_wifi();
        if (ok) {
            if (g_apply_pid || g_work_pending) apply_mode(false);
            else g_apply_gen++; /* invalidate old read-only probes */
        }
        char ssid_e[256], cps_e2[256], from_e[512];
        json_escape(ssid_e, sizeof(ssid_e), g_cfg.ssid);
        json_escape(cps_e2, sizeof(cps_e2), g_cfg.cp_ssid);
        json_escape(from_e, sizeof(from_e), g_cfg.imported_from);
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"ssid\":\"%s\",\"carplay_ssid\":\"%s\",\"from\":\"%s\"}\n",
                 ok ? "true" : "false", ssid_e, cps_e2, from_e);
        if (!ok) code = 404;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/ssh/import")) {
        bool ok = import_authorized_keys();
        if (ok && g_cfg.ssh_enabled) request_work(WORK_SSH, false);
        snprintf(body, sizeof(body), "{\"ok\":%s}\n", ok ? "true" : "false");
        if (!ok) code = 404;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/mode")) {
        char v[64];
        if (!query_get(query, "value", v, sizeof(v)) ||
            (strcmp(v, "off") && strcmp(v, "client") && strcmp(v, "carplay"))) {
            code = 400; snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"invalid mode\"}\n");
        }
        else {
            if (!strcmp(v, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(v, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else g_cfg.mode = MODE_OFF;
            g_cfg.wlan_on = g_cfg.mode != MODE_OFF;
            apply_mode(false);
            snprintf(body, sizeof(body), "{\"ok\":true,\"mode\":\"%s\"}\n", mode_name());
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/wlan")) {
        char v[64];
        if (!query_get(query, "on", v, sizeof(v))) { code = 400; snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"missing on\"}\n"); }
        else {
            g_cfg.wlan_on = atoi(v) != 0;
            if (g_cfg.wlan_on && g_cfg.mode == MODE_OFF) g_cfg.mode = MODE_CLIENT;
            apply_mode(false);
            snprintf(body, sizeof(body), "{\"ok\":true,\"wlan_on\":%s}\n",
                     g_cfg.wlan_on ? "true" : "false");
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/client/connect")) {
        g_cfg.mode = MODE_CLIENT;
        g_cfg.wlan_on = true;
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
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/restart")) {
        g_cfg.mode = MODE_CARPLAY;
        g_cfg.wlan_on = true;
        g_ap_restart_requested = true;
        apply_mode(false);
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/pair")) {
        g_cfg.mode = MODE_CARPLAY;
        g_cfg.wlan_on = true;
        g_pairing_stop_requested = false;
        apply_mode(true);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"applying\":true,"
                 "\"message\":\"starting CarPlay pairing\"}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/pair/stop")) {
        /* A running pair apply could re-enable pairing after this point;
         * cancel it so the stop is authoritative. */
        g_pairing_stop_requested = true;
        g_pair_stop_confirmed = false;
        request_work((g_apply_pid || g_work_pending) ? WORK_MODE : WORK_PAIR_STOP, false);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"discoverable\":%s,\"pairable\":%s}\n",
                 g_bt_discoverable ? "true" : "false",
                 g_bt_pairable ? "true" : "false");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/logs/dump")) {
        request_work(WORK_DUMP, false);
        code = 202;
        snprintf(body, sizeof(body), "{\"ok\":true,\"queued\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/ssh")) {
        char en[64], port[64];
        bool has_en = query_get(query, "enabled", en, sizeof(en));
        bool has_port = query_get(query, "port", port, sizeof(port));
        if (has_port) {
            int p = atoi(port);
            if (p > 0 && p < 65536) g_cfg.ssh_port = p;
        }
        if (has_en) g_cfg.ssh_enabled = atoi(en) != 0;
        request_work(WORK_SSH, false);
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
    umask(0077);
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

    /* One-shot CLI mode: no side effects, no daemon state, stderr logging.
     * Pure queries (--status, --choose-port) must not create directories,
     * delete state, ensure users or save config. */
    g_log_stderr = one_shot != NULL;
    bool pure_query = one_shot && (!strcmp(one_shot, "status") ||
                                   !strcmp(one_shot, "choose-port") ||
                                   !strcmp(one_shot, "dump-logs"));
    g_pure_query = pure_query;
    if (!pure_query) {
        mkdir(APP_DIR, 0755);
        mkdir(APP_DIR "/bin", 0755);
        mkdir(SSH_DIR, 0700);
        unlink(BT_STATE);
    }
    config_load();
    if (g_config_migrated && !pure_query)
        config_save();
    if (!pure_query) {
        if (!one_shot || !strcmp(one_shot, "ensure-user")) ensure_car_user();
        if (!g_cfg.cp_ssid[0])
            str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay");
        if (!g_cfg.cp_psk[0]) {
            random_psk(g_cfg.cp_psk, sizeof(g_cfg.cp_psk));
            config_save();
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (one_shot) {
        if (!strcmp(one_shot, "status")) {
            g_probe_child = true;
            collect_observation(&g_observed);
            g_observed_ms = now_ms();
            char buf[8192];
            build_status(buf, sizeof(buf));
            fputs(buf, stdout);
            return 0;
        }
        if (!strcmp(one_shot, "import")) {
            bool ok = import_wifi();
            char ssid_e[256];
            json_escape(ssid_e, sizeof(ssid_e), g_cfg.ssid);
            printf("{\"ok\":%s,\"ssid\":\"%s\"}\n", ok ? "true" : "false", ssid_e);
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "import-file") && one_file) {
            bool ok = import_wifi_path(one_file);
            char ssid_e[256], cps_e[256];
            json_escape(ssid_e, sizeof(ssid_e), g_cfg.ssid);
            json_escape(cps_e, sizeof(cps_e), g_cfg.cp_ssid);
            printf("{\"ok\":%s,\"ssid\":\"%s\",\"carplay_ssid\":\"%s\"}\n",
                   ok ? "true" : "false", ssid_e, cps_e);
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "mode") && one_mode) {
            if (!strcmp(one_mode, "client")) g_cfg.mode = MODE_CLIENT;
            else if (!strcmp(one_mode, "carplay")) g_cfg.mode = MODE_CARPLAY;
            else g_cfg.mode = MODE_OFF;
            g_cfg.wlan_on = g_cfg.mode != MODE_OFF;
            config_save();
            bool ok = apply_mode_work(false);
            collect_observation(&g_observed);
            g_observed_ms = now_ms();
            char buf[8192];
            build_status(buf, sizeof(buf));
            fputs(buf, stdout);
            return ok ? 0 : 1;
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
    } else apply_mode(false);

    while (!g_stop) {
        struct pollfd pfd = {.fd = srv, .events = POLLIN};
        int pr = poll(&pfd, 1, 500);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                http_handle(c);
            }
        }
        apply_reap();
        home_monitor_tick();
        probe_tick();
        watchdog();
        int64_t now = now_ms();
        if (now - g_last_status_ms > 2000) {
            write_status();
            g_last_status_ms = now;
        }
    }

    /* Coordinated shutdown: cancel any in-flight apply and undo the network
     * state we own (tether, firewall rules, supplicant, SSH) so stopping the
     * daemon does not leave ConnMan tethering or iptables rules behind. */
    apply_cancel();
    g_work_pending = false; g_dump_requested = false;
    int64_t until = now_ms() + 1500;
    while (g_apply_pid > 0 && now_ms() < until) {
        apply_reap();
        usleep(20000);
    }
    if (g_probe_pid > 0) {
        kill(-g_probe_pid, SIGKILL); kill(g_probe_pid, SIGKILL);
        waitpid(g_probe_pid, NULL, WNOHANG);
    }
    /* Cleanup is unconditional: configured Off is not proof an interrupted
     * previous apply removed its firewall/AP/SSH state. Keep it bounded and
     * outside the parent, including on SIGTERM when g_stop is already set. */
    pid_t cleanup = fork();
    if (cleanup == 0) {
        setpgid(0, 0); g_in_child = true; g_stop = 0;
        close_child_fds(-1);
        stop_all(); bt_pair_stop();
        _exit(0);
    }
    if (cleanup > 0) {
        setpgid(cleanup, cleanup);
        until = now_ms() + 18000;
        while (waitpid(cleanup, NULL, WNOHANG) == 0 && now_ms() < until) usleep(20000);
        kill(-cleanup, SIGKILL); kill(cleanup, SIGKILL);
        waitpid(cleanup, NULL, WNOHANG);
    }
    home_monitor_cleanup();
    close(srv);
    log_msg("wifi_daemon stopped");
    return 0;
}
