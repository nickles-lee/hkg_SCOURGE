/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
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
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
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
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <stddef.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include "../../common/radio_owner.h"
#include "carplay/cp_network.h"

#ifndef APP_DIR
#define APP_DIR        "/rw_data/wifi1b"
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
#define USB0_HELPER    APP_DIR "/bin/opi-usb0-up.sh"
#define USB0_OWNER     APP_DIR "/usb0.owner"
#ifndef USB0_RULE_PATH
#define USB0_RULE_PATH "/run/udev/rules.d/99-opi-usb0.rules"
#endif
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

#define DEFAULT_HTTP_PORT 8093
#define SSH_DEFAULT_PORT  2224
#define SSH_FALLBACK_LAST 2234

#define WIFI1B_PASSWD_LINE \
    "wifi1b:x:0:0:WiFi2:/rw_data/wifi1b/home:/bin/sh"
#define WIFI1B_SHADOW_LINE \
    "wifi1b:!:19000:0:99999:7:::"
#define CAR_PASSWD_LINE WIFI1B_PASSWD_LINE
#define CAR_SHADOW_LINE WIFI1B_SHADOW_LINE

typedef enum { MODE_OFF = 0, MODE_CLIENT, MODE_CARPLAY } wifi_mode_t;
typedef enum {
    CLIENT_ROUTE_MODEM_SAFE = 0,   /* no WiFi default while modem has IPv4 */
    CLIENT_ROUTE_FALLBACK          /* high-metric WiFi fallback default */
} client_route_policy_t;

typedef struct {
    wifi_mode_t mode;
    bool wlan_on;

    char ssid[80];
    char psk[160];
    char extra_ssid[3][80];
    char extra_psk[3][160];

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
    client_route_policy_t client_route_policy;

    /* 0 = Wifi1b owns wireless CarPlay, 1 = hand back to stock USB CarPlay. */
    int  carplay_control;

    char imported_from[256];
    bool imported;
} config_t;

static config_t g_cfg;
static volatile sig_atomic_t g_stop;
static bool g_shutdown_cleanup;
static bool g_log_stderr = true;   /* one-shot CLI mode; daemon redirects stderr into the same log file */
static bool g_pure_query;
static int g_apply_gen;            /* increments per apply; stale children cannot publish */
static bool g_carplay_ie;          /* IE fields configured in the AP config (not a wire observation) */
static const char *g_status_path = STATUS_PATH;
static char g_last_msg[256] = "starting";
static int64_t g_last_apply_ms;
static int64_t g_last_watchdog_ms;
static int64_t g_last_status_ms;
static bool g_apply_failed;
static int64_t g_start_ms;
static pid_t g_apply_pid;
static int64_t g_apply_start_ms;
static bool g_apply_dhcp_capable;
static bool g_txn_active;
static int64_t g_apply_deadline_ms;
static bool g_in_child;
static int64_t g_last_autodump_ms;
static int64_t g_last_dump_try_ms;
static bool g_boot_dumped;
static int g_ssh_active_port;
static bool g_ssh_fw_open;
static bool g_ssh_port_fallback;
static char g_ssh_bound_ip[64];
/* DHCP client diagnostics (updated in the apply child, parsed from
 * APPLY_RESULT by the parent). */
static int g_dhcp_tx;
static int g_dhcp_rx_offer;
static int g_dhcp_rx_ack;
static int g_dhcp_attempts;
static char g_dhcp_bssid[32];
static char g_dhcp_error[128];
/* write_file_atomic error throttling: a full filesystem must not turn a
 * 2 s status write into an unbounded log stream. */
static int64_t g_wfa_last_log_ms;
static char g_wfa_last_path[256];
static int g_wfa_suppressed;
static int64_t g_disk_low_log_ms;
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

typedef enum {
    W2_DHCP_ACQUIRE = 0,
    W2_DHCP_RENEW,
    W2_DHCP_REBIND,
    W2_DHCP_RELEASE
} w2_dhcp_step_t;

typedef enum {
    WORK_MODE,
    WORK_SCAN,
    WORK_SSH,
    WORK_PAIR_STOP,
    WORK_DUMP,
    WORK_DHCP,
    WORK_ROUTE_AUDIT
} work_t;
static work_t g_work_kind = WORK_MODE, g_pending_kind;
static bool g_work_pending, g_pending_pair, g_apply_cancelled;
static w2_dhcp_step_t g_dhcp_work_step = W2_DHCP_ACQUIRE;
static int g_dhcp_retry_failures;
static int64_t g_dhcp_next_try_ms;
static int64_t g_last_dhcp_work_ms;
static int64_t g_last_route_audit_ms;
static int64_t g_client_t1_at_ms, g_client_t2_at_ms, g_client_expiry_at_ms;
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
    bool ipv4_ready, ipv6_ready;
    bool ipv6_unavailable;
    bool channel_matches_config;
    int64_t sampled_ms;
    int ifindex;
    int channel, frequency; /* observed only, never from configuration */
    char ssid[80], configured_ssid[80], ip[64];
    char linklocal[80], security[32], ipv6_error[160];
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

/* One total budget per apply transaction.  Every command and DHCP wait is
 * capped by the remaining monotonic time, so cleanup/handoff steps cannot
 * silently extend a nominal 35 s into an unbounded sequence of 8 s commands. */
#define WIFI1B_TXN_BUDGET_MS 60000
#define WIFI1B_TXN_GRACE_MS  3000

#ifdef WIFI_TEST
static int64_t (*test_now_ms)(void);
static int test_txn_budget_ms;
#endif

static int64_t now_ms(void)
{
#ifdef WIFI_TEST
    if (test_now_ms) return test_now_ms();
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void w2_txn_begin(bool dhcp_capable)
{
    g_txn_active = true;
    g_apply_dhcp_capable = dhcp_capable;
    g_apply_start_ms = now_ms();
    int budget = WIFI1B_TXN_BUDGET_MS;
#ifdef WIFI_TEST
    if (test_txn_budget_ms > 0) budget = test_txn_budget_ms;
#endif
    g_apply_deadline_ms = g_apply_start_ms + budget;
}

static void w2_txn_end(void)
{
    g_txn_active = false;
    g_apply_dhcp_capable = false;
}

static int64_t w2_txn_remaining_ms(void)
{
    if (!g_txn_active) return -1;
    int64_t left = g_apply_deadline_ms - now_ms();
    return left > 0 ? left : 0;
}

static bool w2_txn_expired(void)
{
    return g_txn_active && w2_txn_remaining_ms() == 0;
}

static int w2_txn_command_timeout(int requested)
{
    int64_t left = w2_txn_remaining_ms();
    if (left < 0) return requested;
    if (left == 0) return 0;
    return left < requested ? (int)left : requested;
}

static long long disk_free_kb(void)
{
    struct statvfs vfs;
    if (statvfs(APP_DIR, &vfs) != 0) return -1;
    return (long long)vfs.f_bavail * (long long)vfs.f_frsize / 1024LL;
}

/* Keep child-process logs bounded.  The live file is rotated once; if the
 * rename fails (read-only or full filesystem), truncate it in place. */
static void cap_log_file(const char *path, off_t max)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= max) return;
    char old[512];
    snprintf(old, sizeof(old), "%s.1", path);
    unlink(old);
    if (rename(path, old) != 0) {
        int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd >= 0) close(fd);
    }
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

static void redact_secret(char *buf, const char *secret);

#define LOG_MAX_BYTES (1024 * 1024)

/* The systemd unit appends stderr to daemon.log and the wpa_supplicant child
 * appends its stdout/stderr to wpa_supplicant.log.  Both writers keep their
 * descriptor across a rename, so the cap must truncate in place: a rename
 * would leave them writing to an unlinked inode, leaking the space instead of
 * freeing it.  /rw_data is a 1.2 GB partition and this daemon is the largest
 * writer on it, so every append path is capped. */
static void log_cap(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= LOG_MAX_BYTES) return;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd >= 0) close(fd);
}

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    redact_secret(buf, g_cfg.psk);
    for (int i = 0; i < 3; i++) redact_secret(buf, g_cfg.extra_psk[i]);
    redact_secret(buf, g_cfg.cp_psk);

    if (!g_pure_query) {
        log_cap(LOG_PATH);
        log_cap(WPA_LOG);
    }
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
#ifdef WIFI_TEST
static int w2_test_net_stub(char *out, size_t n, const char *cmd)
{
    if (out && n) out[0] = 0;
    if (strstr(cmd, "ip -o -4 addr show dev usb0") != NULL) return 1;
    if (strstr(cmd, "ip -o -4 addr show") != NULL) return 0;
    if (strstr(cmd, "ip -4 route show default") != NULL) return 0;
    if (strstr(cmd, "iptables -S WIFI1B_USB0_SSH") != NULL) return 1;
    if (strstr(cmd, "iptables -S") != NULL) return 0;
    return system(cmd);
}
#endif

static int (*test_command)(char *, size_t, const char *);
static bool (*test_work)(work_t, bool);
static int test_iface = -1, test_owned_wpa = -1, test_dhcpd_start = -1;
static int test_dhcp_pool_first = -1, test_dhcp_pool_last = -1;
static int test_dhcpd_running = -1;
static int test_owner_write_fail;
static int test_owner_dir_sync_fail;
static int test_lineage_require_lock;
static const char *test_if_inet6_path;
static const char *test_ipv6_conf_dir;
static const char *test_mac_override;
static const char *test_authorized_keys_source;
#endif
static int run_command(char *out, size_t outsz, const char *cmd, int timeout_ms)
{
    if (out && outsz) out[0] = 0;
    if (timeout_ms <= 0) return 124;
#ifdef WIFI_TEST
    if (test_command) {
        int rc = test_command(out, outsz, cmd);
        /* A full capture is not complete evidence, including fixture output.
         * Refuse conservatively even when the child would have fit exactly. */
        if (out && outsz && strnlen(out, outsz) >= outsz - 1) {
            out[0] = 0;
            return 125;
        }
        return rc;
    }
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
    bool eof = false, reaped = false, capture_full = false;
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
            if (out && outsz && used >= outsz - 1) capture_full = true;
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
    if (capture_full) {
        out[0] = 0;
        return 125; /* incomplete readback must never prove absence */
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    int timeout = w2_txn_command_timeout(g_probe_child ? 1500 : 8000);
    if (timeout <= 0) return 124;
    return run_command(NULL, 0, cmd, timeout);
}

static int sh_out(char *out, size_t outsz, const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int timeout = w2_txn_command_timeout(g_probe_child ? 1500 : 8000);
    if (timeout <= 0) return 124;
    return run_command(out, outsz, cmd, timeout);
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

/* A full filesystem makes every 2 s status write fail.  Log the first
 * failure immediately, then at most once per minute per path, and report
 * how many identical errors were suppressed. */
static void wfa_log_error(const char *path, const char *what, int err)
{
    int64_t now = now_ms();
    if (!strcmp(path, g_wfa_last_path) && now - g_wfa_last_log_ms < 60000) {
        g_wfa_suppressed++;
        return;
    }
    if (g_wfa_suppressed > 0)
        log_msg("write_file_atomic %s: %d earlier errors suppressed",
                g_wfa_last_path, g_wfa_suppressed);
    g_wfa_suppressed = 0;
    g_wfa_last_log_ms = now;
    str_copy(g_wfa_last_path, sizeof(g_wfa_last_path), path);
    log_msg("write_file_atomic %s %s failed: %s", path, what, strerror(err));
}

/* True only when every existing ancestor is a real directory.  A missing
 * parent is not created here; callers that need one must create it with the
 * checked helper below. */
static bool w2_real_ancestors(const char *path)
{
    char copy[512];
    if (path == NULL || strlen(path) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    while (slash && slash != copy) {
        *slash = 0;
        struct stat st;
        if (lstat(copy, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
        } else if (errno != ENOENT) {
            return false;
        }
        slash = strrchr(copy, '/');
    }
    return true;
}

static bool w2_single_regular(const char *path, struct stat *st_out)
{
    struct stat st;
    if (path == NULL || !w2_real_ancestors(path) ||
        lstat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (uint32_t)st.st_uid != (uint32_t)geteuid()) return false;
    if (st_out) *st_out = st;
    return true;
}

static bool w2_file_equals_text(const char *path, const char *text)
{
    char buf[4096];
    size_t len;
    if (text == NULL || !w2_single_regular(path, NULL)) return false;
    len = strlen(text);
    int n = read_file(path, buf, sizeof(buf));
    return n == (int)len && len < sizeof(buf) && !memcmp(buf, text, len);
}

static bool w2_atomic_target_ok(const char *path, const char *tmp)
{
    struct stat st;
    if (!w2_real_ancestors(path)) return false;
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            (uint32_t)st.st_uid != (uint32_t)geteuid()) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    if (lstat(tmp, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            (uint32_t)st.st_uid != (uint32_t)geteuid()) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    return true;
}

static bool w2_unlink_if_same(const char *path, const struct stat *expected)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (expected && (st.st_dev != expected->st_dev || st.st_ino != expected->st_ino ||
                     st.st_nlink != 1 || !S_ISREG(st.st_mode))) return false;
    return unlink(path) == 0 || errno == ENOENT;
}

/* Create every missing component as a real directory, refusing any existing
 * symlink/non-directory.  Used for the generated helper/rule parents. */
static bool w2_mkdir_mode_checked(const char *path, mode_t mode)
{
    char copy[512];
    if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = 0;
        struct stat st;
        if (lstat(copy, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
            /* Private credential leaves must retain private owner/mode;
             * shared ancestors keep their established directory modes. */
            if (saved == '\0' && mode == 0700 &&
                (st.st_uid != geteuid() || (st.st_mode & 07777) != 0700))
                return false;
        } else if (errno == ENOENT) {
            if (mkdir(copy, mode) != 0) return false;
        } else {
            return false;
        }
        *p = saved;
        if (saved == '\0') break;
    }
    return true;
}

static bool w2_mkdir_checked(const char *path)
{
    return w2_mkdir_mode_checked(path, 0755);
}

static int write_file_atomic(const char *path, const char *data, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    if (!w2_atomic_target_ok(path, tmp)) {
        wfa_log_error(path, "unsafe destination", EINVAL);
        return -1;
    }
    struct stat old_tmp;
    /* Refuse any pre-existing temp path.  It was not created by this call and
     * may be owner data; never unlink it to make O_EXCL succeed. */
    if (lstat(tmp, &old_tmp) == 0 || errno != ENOENT) {
        wfa_log_error(path, "preexisting temp", EEXIST);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        wfa_log_error(path, "open", errno);
        return -1;
    }
    struct stat tst;
    if (fstat(fd, &tst) != 0 || !S_ISREG(tst.st_mode) || tst.st_nlink != 1 ||
        (uint32_t)tst.st_uid != (uint32_t)geteuid()) {
        int saved = errno ? errno : EINVAL;
        close(fd);
        wfa_log_error(path, "temp identity", saved);
        return -1;
    }
    struct stat original;
    if (lstat(path, &original) == 0) {
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
            close(fd); w2_unlink_if_same(tmp, &tst); errno = saved;
            wfa_log_error(path, "label/owner", saved);
            return -1;
        }
    }
    size_t len = strlen(data);
    ssize_t w = write(fd, data, len);
    bool runtime = !strcmp(path, g_status_path) || !strcmp(path, HOME_STATE) ||
                   !strcmp(path, APPLY_RESULT) || !strcmp(path, BT_STATE) ||
                   !strcmp(path, SSH_BOUND) || !strcmp(path, SCAN_RESULT);
    int sync_rc = runtime ? 0 : fsync(fd);
    if (fchmod(fd, mode) != 0) {
        int saved = errno;
        close(fd); w2_unlink_if_same(tmp, &tst); errno = saved;
        wfa_log_error(path, "mode", saved);
        return -1;
    }
    struct stat after;
    if (fstat(fd, &after) != 0 || after.st_dev != tst.st_dev ||
        after.st_ino != tst.st_ino || after.st_nlink != 1) {
        int saved = errno ? errno : EINVAL;
        close(fd); w2_unlink_if_same(tmp, &tst); errno = saved;
        wfa_log_error(path, "fstat identity", saved);
        return -1;
    }
    close(fd);
    if (w != (ssize_t)len || sync_rc != 0) {
        int saved = errno;
        w2_unlink_if_same(tmp, &tst); errno = saved;
        wfa_log_error(path, "write/fsync", saved);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int saved = errno;
        w2_unlink_if_same(tmp, &tst); errno = saved;
        wfa_log_error(path, "rename", saved);
        return -1;
    }
    if (!w2_single_regular(path, &after) || after.st_dev != tst.st_dev ||
        after.st_ino != tst.st_ino) {
        wfa_log_error(path, "published identity", EINVAL);
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
    if (sh("ip link set wlan0 up 2>/dev/null") != 0)
        sh("ifconfig wlan0 up 2>/dev/null");
}

static void iface_down(void)
{
    if (sh("ip link set wlan0 down 2>/dev/null") != 0)
        sh("ifconfig wlan0 down 2>/dev/null");
}

/* Get IPv4 address of wlan0 into out. */
static bool wlan_ip(char *out, size_t n)
{
    if (out && n) out[0] = 0;
    char buf[4096];
    if (sh_out(buf, sizeof(buf), "ifconfig wlan0 2>/dev/null") == 0) {
        const char *p = strstr(buf, "inet addr:");
        if (p) p += 10;
        else { p = strstr(buf, "inet "); if (p) p += 5; }
        if (p) {
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\n' && *p != '\t' && i + 1 < n)
                out[i++] = *p++;
            out[i] = 0;
            if (i && strcmp(out, "0.0.0.0") && strcmp(out, "127.0.0.1")) return true;
        }
    }
    if (sh_out(buf, sizeof(buf), "ip -o -4 addr show dev wlan0 2>/dev/null") == 0) {
        char *p = strstr(buf, " inet ");
        if (p) {
            p += 6;
            size_t i = 0;
            while (*p && *p != '/' && *p != ' ' && *p != '\n' && i + 1 < n)
                out[i++] = *p++;
            out[i] = 0;
            if (i && strcmp(out, "0.0.0.0")) return true;
        }
    }
    return false;
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

static bool find_service(const char *services, const char *ssid,
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
        if (!security || strcmp(security, g_cfg.psk[0] ? "_psk" : "_none")) continue;
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
    str_copy(g_cfg.cp_ssid, sizeof(g_cfg.cp_ssid), "IONIQ5-CarPlay-B");
    g_cfg.ssh_enabled = false;
    g_cfg.ssh_port = SSH_DEFAULT_PORT;
    g_cfg.client_route_policy = CLIENT_ROUTE_MODEM_SAFE;
    g_cfg.carplay_control = 0;
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
        } else if (strlen(k) == 5 && !strncmp(k, "ssid", 4) && k[4] >= '2' && k[4] <= '4') {
            str_copy(g_cfg.extra_ssid[k[4]-'2'], sizeof(g_cfg.extra_ssid[0]), v);
        } else if (strlen(k) == 4 && !strncmp(k, "psk", 3) && k[3] >= '2' && k[3] <= '4') {
            str_copy(g_cfg.extra_psk[k[3]-'2'], sizeof(g_cfg.extra_psk[0]), v);
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
        } else if (!strcmp(k, "carplay_control")) {
            g_cfg.carplay_control = (!strcmp(v, "stock") || !strcmp(v, "usb")) ? 1 : 0;
        } else if (!strcmp(k, "ssh_port")) {
            has_ssh_port = true;
            g_cfg.ssh_port = atoi(v);
            if (g_cfg.ssh_port < 1 || g_cfg.ssh_port > 65535)
                g_cfg.ssh_port = SSH_DEFAULT_PORT;
        } else if (!strcmp(k, "client_route_policy")) {
            g_cfg.client_route_policy = !strcmp(v, "fallback")
                ? CLIENT_ROUTE_FALLBACK : CLIENT_ROUTE_MODEM_SAFE;
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

static bool config_save(void)
{
    /* Only the parent daemon persists config.  An apply child reports
     * observed adjustments through APPLY_RESULT instead, so a canceled or
     * stale worker can never save a config it did not apply. */
    if (g_in_child) return true;
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "# wifi1b_daemon config\n"
             "config_version=2\n"
             "mode=%s\n"
             "wlan_on=%d\n"
             "ssid=%s\n"
             "psk=%s\n"
             "ssid2=%s\npsk2=%s\nssid3=%s\npsk3=%s\nssid4=%s\npsk4=%s\n"
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
             "client_route_policy=%s\n"
             "carplay_control=%s\n"
             "imported=%d\n"
             "imported_from=%s\n",
             g_cfg.mode == MODE_CLIENT ? "client" :
             g_cfg.mode == MODE_CARPLAY ? "carplay" : "off",
             g_cfg.wlan_on ? 1 : 0,
             g_cfg.ssid, g_cfg.psk,
             g_cfg.extra_ssid[0], g_cfg.extra_psk[0], g_cfg.extra_ssid[1], g_cfg.extra_psk[1],
             g_cfg.extra_ssid[2], g_cfg.extra_psk[2],
             g_cfg.cp_ssid, g_cfg.cp_psk,
             g_cfg.cp_band == 1 ? "2.4" : g_cfg.cp_band == 2 ? "5" : "auto",
             g_cfg.cp_channel, g_cfg.cp_hidden ? 1 : 0, g_cfg.cp_max_sta,
             g_cfg.cp_manufacturer, g_cfg.cp_model, g_cfg.cp_name,
             g_cfg.cp_id, g_cfg.cp_btaddr,
             g_cfg.ssh_enabled ? 1 : 0, g_cfg.ssh_port,
             g_cfg.client_route_policy == CLIENT_ROUTE_FALLBACK
                 ? "fallback" : "modem_safe",
             g_cfg.carplay_control ? "stock" : "wifi1b",
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
#ifdef WIFI_TEST
    if (test_authorized_keys_source && !strcmp(name, "authorized_keys")) {
        str_copy(out, n, test_authorized_keys_source);
        return true;
    }
#endif
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
    char extra_ssid[3][80] = {{0}}, extra_psk[3][160] = {{0}};
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
        else if (strlen(k) == 5 && !strncmp(k, "ssid", 4) && k[4] >= '2' && k[4] <= '4')
            str_copy(extra_ssid[k[4]-'2'], sizeof(extra_ssid[0]), v);
        else if (strlen(k) == 4 && !strncmp(k, "psk", 3) && k[3] >= '2' && k[3] <= '4')
            str_copy(extra_psk[k[3]-'2'], sizeof(extra_psk[0]), v);
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
    str_copy(g_cfg.ssid, sizeof(g_cfg.ssid), ssid);
    for (int i = 0; i < 3; i++) {
        if (strlen(extra_ssid[i]) > 32) { set_msg("WiFi SSIDs must be 1-32 bytes"); g_cfg = previous; return false; }
    }
    memcpy(g_cfg.extra_ssid, extra_ssid, sizeof(extra_ssid));
    memcpy(g_cfg.extra_psk, extra_psk, sizeof(extra_psk));
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
    (void)ssh_port; /* imported ssh_port is deliberately ignored */

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
    if (!find_usb_file("iphone_wifi.txt", path, sizeof(path)) &&
        !find_usb_file("wifi.txt", path, sizeof(path))) {
        set_msg("iphone_wifi.txt/wifi.txt not found on USB");
        return false;
    }
    return parse_wifi_txt(path);
}

static bool read_optional_regular(const char *path, char *buf, size_t cap,
                                  bool *existed)
{
    struct stat st;
    if (existed != NULL) *existed = false;
    if (path == NULL || !w2_real_ancestors(path)) return false;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (uint32_t)st.st_uid != (uint32_t)geteuid())
        return false;
    int n = read_file(path, buf, cap);
    if (n < 0 || n >= (int)cap - 1) return false;
    if (existed != NULL) *existed = true;
    return true;
}

static bool restore_optional_regular(const char *path, const char *old_text,
                                     bool existed, const char *new_text)
{
    if (existed) {
        if (write_file_atomic(path, old_text, 0600) != 0 ||
            !w2_file_equals_text(path, old_text))
            return false;
        return true;
    }
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!w2_file_equals_text(path, new_text)) return false;
    return unlink(path) == 0 || errno == ENOENT;
}

/* Explicit owner import: write BOTH destinations with the same bytes and
 * verify each.  A failure restores both previous files.  This is the only
 * path allowed to replace a different valid regular key set. */
static bool write_key_pair_exact(const char *text)
{
    char eff[512], old_keys[16384], old_home[16384];
    bool had_keys = false, had_home = false;
    if (text == NULL || text[0] == '\0') return false;
    snprintf(eff, sizeof(eff), SSH_HOME "/.ssh/authorized_keys");
    if (!read_optional_regular(SSH_KEYS, old_keys, sizeof(old_keys),
                               &had_keys) ||
        !read_optional_regular(eff, old_home, sizeof(old_home), &had_home)) {
        set_msg("SSH import refused: unsafe key destination");
        return false;
    }
    if (write_file_atomic(SSH_KEYS, text, 0600) != 0 ||
        !w2_file_equals_text(SSH_KEYS, text)) {
        (void)restore_optional_regular(SSH_KEYS, old_keys, had_keys, text);
        set_msg("SSH import failed: canonical key write/readback");
        return false;
    }
    if (write_file_atomic(eff, text, 0600) != 0 ||
        !w2_file_equals_text(eff, text)) {
        bool keys_ok = restore_optional_regular(SSH_KEYS, old_keys, had_keys,
                                                text);
        bool home_ok = restore_optional_regular(eff, old_home, had_home, text);
        set_msg("SSH import failed: home key write/readback%s",
                keys_ok && home_ok ? "" : " (rollback incomplete)");
        return false;
    }
#ifndef WIFI_TEST
    sh("chsmack -a _ " SSH_KEYS " " SSH_DIR " " SSH_HOME " " SSH_HOME "/.ssh 2>/dev/null");
#endif
    return true;
}

static bool import_authorized_keys(void)
{
    char path[512], buf[16384];
    struct stat st;
    if (!find_usb_file("authorized_keys", path, sizeof(path))) {
        set_msg("authorized_keys not found on USB");
        return false;
    }
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1)
        return false;
    int n = read_file(path, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf) - 1) {
        set_msg("cannot read %s", path);
        return false;
    }
    if (!w2_mkdir_mode_checked(SSH_DIR, 0700) ||
        !w2_mkdir_mode_checked(SSH_HOME, 0700) ||
        !w2_mkdir_mode_checked(SSH_HOME "/.ssh", 0700)) {
        set_msg("SSH import refused: unsafe credential parents");
        return false;
    }
    if (!write_key_pair_exact(buf)) return false;
    set_msg("imported SSH keys from %s (both destinations verified)", path);
    return true;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi / ConnMan                                                     */
/* ------------------------------------------------------------------ */

#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


/* ------------------------------------------------------------------ */
/* WiFi supplicant                                                     */
/* ------------------------------------------------------------------ */

#if 0
/* replaced by wifi1b backend */
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
#endif


/* Process health only: is a supplicant process alive at all?  This is the
 * signal that matters for "do we restart the stack"; a missing per-interface
 * socket on a live process is a transition, not a crash. */
#if 0
/* replaced by wifi1b backend */
static bool wpa_process_alive(void)
{
#ifdef WIFI_TEST
    if (test_owned_wpa >= 0) return test_owned_wpa != 0;
#endif
    return owned_wpa_pid() > 0;
}
#endif


#if 0
/* replaced by wifi1b backend */
static bool proc_alive(int pid)
{
    char path[64], state[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_file(path, state, sizeof(state)) <= 0) return false;
    char *end = strrchr(state, ')');
    return end && end[1] == ' ' && end[2] != 'Z' && end[2] != 'X';
}
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


/* Signal a supplicant and wait for it to exit; escalate to SIGKILL after a
 * bounded timeout.  Only the owner verified by /proc/<pid>/comm is signaled. */
#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
        log_cap(WPA_LOG);
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


/* -1 means unknown/unavailable, not a verified powered-off technology. */
#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
static bool wifi_powered(void) { return wifi_power_state() == 1; }
#endif


/* Load the driver, evict the OEM manager, and leave a supplicant under our
 * control.  Used by scan, client connect and CarPlay AP start. */
#if 0
/* replaced by wifi1b backend */
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
#endif


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
            if (decode_control_ssid(actual, sizeof(actual), line + 5)) {
                same_ssid = !strcmp(actual, g_cfg.ssid);
                for (int i = 0; i < 3; i++)
                    if (g_cfg.extra_ssid[i][0] && !strcmp(actual, g_cfg.extra_ssid[i])) same_ssid = true;
            }
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
#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


/* Call only for an explicit stop or after actual absence, never as a
 * response to Tethering=False on an otherwise working AP. */
#if 0
/* replaced by wifi1b backend */
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
#endif


static bool client_disconnect(void);

#if 0
/* replaced by wifi1b backend */
static bool ap_remember_station(const ap_probe_t *p)
{
    return !p->station || !p->ip[0] || write_file_atomic(AP_STATION_IP, p->ip, 0600) == 0;
}
#endif


typedef enum { AP_WAIT_READY, AP_WAIT_NEVER, AP_WAIT_PENDING, AP_WAIT_LOST, AP_WAIT_UNKNOWN, AP_WAIT_IPV6_BLOCKED } ap_wait_t;

#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


static void carplay_fw_close(void);

#if 0
/* replaced by wifi1b backend */
static bool ap_stop(void)
{
    g_carplay_ie = false;
    carplay_fw_close();
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    return ap_disable(true);
}
#endif


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
    "org.bluez.Adapter.Adapter.v2_0_lge1",
    "org.bluez.Adapter.Adapter.v2_0",
    "org.bluez.Adapter.Adapter",
    "org.bluez",
    NULL
};
static const char *BT_PATHS[] = {
    "/lge1", /* CommonAPI instance lge1, not the interface name as a path */
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

/* Return -1 for unreadable state: neither on nor off is then confirmed. */
static int bt_get_prop_bool(const char *name)
{
    char out[4096] = "";
    char method[96];
    snprintf(method, sizeof(method), "get%sAttribute", name);
    if (bt_call(method, NULL, out, sizeof(out))) {
        if (strstr(out, "boolean true")) return true;
        if (strstr(out, "boolean false")) return false;
    }
    if (bt_props_get(name, out, sizeof(out))) {
        if (strstr(out, "boolean true")) return true;
        if (strstr(out, "boolean false")) return false;
    }
    // Unreadable is not confirmed on, even when the setter succeeded.
    return -1;
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
    /* Record the resulting state, not the setter's success: setting
     * Discoverable=false successfully must not be reported as "on". */
    int discoverable = bt_get_prop_bool("Discoverable");
    int pairable = bt_get_prop_bool("Pairable");
    bool power_confirmed = !on || bt_get_prop_bool("Powered") == 1;
    g_bt_discoverable = discoverable == 1;
    g_bt_pairable = pairable == 1;

    if (on) {
        bt_fetch_address();
        if (powered && power_confirmed && g_bt_discoverable && g_bt_pairable)
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Pairing on - %s - discoverable %s - pairable %s",
                     g_cfg.cp_btaddr[0] ? g_cfg.cp_btaddr : "BT address unknown",
                     g_bt_discoverable ? "yes" : "no",
                     g_bt_pairable ? "yes" : "no");
        else
            snprintf(g_pair_msg, sizeof(g_pair_msg),
                     "Bluetooth control unavailable; use Settings > Bluetooth on the car");
    } else {
        snprintf(g_pair_msg, sizeof(g_pair_msg), "%s",
                 (!disc_set || !pair_set || discoverable != 0 || pairable != 0) ?
                 "Pairing stop not confirmed" : "Pairing stopped");
    }
    set_msg("%s", g_pair_msg);
    bt_state_save();
    if (!on) g_pair_stop_confirmed = disc_set && pair_set && discoverable == 0 && pairable == 0;
    return on ? (powered && power_confirmed && g_bt_discoverable && g_bt_pairable) : g_pair_stop_confirmed;
}

#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
static void carplay_ie_off(void)
{
    sh("connmanctl tether_carplay_ie wifi off 2>/dev/null");
    g_carplay_ie = false;
}
#endif


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
    if (g_cfg.carplay_control) {
        set_msg("CarPlay control is Stock USB; wireless pairing is disabled");
        return false;
    }
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
#include "wifi1b_backend.inc"

/* ------------------------------------------------------------------ */
/* CarPlay control: Wifi1b wireless vs stock USB handoff               */
/* ------------------------------------------------------------------ */

/* Preference only. This is called from the HTTP loop and startup, so it must
 * never run a command or touch wlan0. The apply worker acquires the shared
 * radio reservation first; only that worker may stop our AP or hand wlan0
 * back to the OEM manager. A peer reservation therefore cannot be brought
 * down by a main-thread control request. */
static bool apply_carplay_control(bool persist)
{
    char owner[32] = "";

    if (g_cfg.carplay_control) {
        g_carplay_ie = false;
        if (g_cfg.mode != MODE_OFF || g_cfg.wlan_on) {
            g_cfg.mode = MODE_OFF;
            g_cfg.wlan_on = false;
            if (persist) config_save();
        }
        if (radio_record_name(owner, sizeof(owner)) &&
            strcmp(owner, "wifi1b_daemon"))
            set_msg("CarPlay control: Stock USB requested (wlan0 reserved by %s; no teardown)", owner);
        else
            set_msg("CarPlay control: Stock USB requested (owned teardown deferred to worker)");
    } else {
        if (radio_record_name(owner, sizeof(owner)) &&
            strcmp(owner, "wifi1b_daemon"))
            set_msg("CarPlay control: Wifi1b wireless requested (wlan0 reserved by %s; no eviction)", owner);
        else
            set_msg("CarPlay control: Wifi1b wireless requested");
    }
    return true;
}



#if 0
/* replaced by wifi1b backend */
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
#endif


static void write_status(void);

/* In client mode an address alone is not proof of connection: a leftover AP
 * address (10.42.0.1) or another interface's address must not count.  Require
 * an actual 4-way-handshake-complete association via the supplicant. */
static bool client_associated(void)
{
    return wpa_state_completed();
}

#if 0
/* replaced by wifi1b backend */
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
#endif


#if 0
/* replaced by wifi1b backend */
static bool client_wait_ip(const char *service, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        char ip[64];
        if (wlan_ip(ip, sizeof(ip)) && client_associated() && client_service_ready(service, ip)) {
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
#endif


#if 0
/* replaced by wifi1b backend */
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
#endif


typedef struct { bool registered, secret_sent, connected, failed; } agent_state_t;

/* This is deliberately driven by received prompts, not elapsed sleeps. It
 * also gives the tests the exact credential-interaction state machine. */
#if 0
/* replaced by wifi1b backend */
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
        if (s->secret_sent || !g_cfg.psk[0]) { s->failed = true; return false; }
        s->secret_sent = true;
        snprintf(reply, n, "%s\n", g_cfg.psk);
    } else if (s->registered && (strstr(received, "Connected ") ||
                                strstr(received, "Connection successful"))) {
        s->connected = true;
    }
    return true;
}
#endif


/* Keep ConnMan as the sole association/DHCP owner. Credentials travel only
 * over the child's stdin after RequestInput; never a script, argv or log. */
#if 0
/* replaced by wifi1b backend */
static bool client_connman_agent(const char *service)
{
    if (strchr(g_cfg.psk, '\n') || strchr(g_cfg.psk, '\r')) {
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
#endif


#if 0
/* replaced by wifi1b backend */
static bool client_connect(void)
{
    if (!g_cfg.ssid[0]) {
        set_msg("no client SSID imported (use wifi.txt)");
        return false;
    }
    size_t keylen = strlen(g_cfg.psk);
    if (keylen && (keylen < 8 || keylen > 64 ||
        (keylen == 64 && strspn(g_cfg.psk, "0123456789abcdefABCDEF") != 64))) {
        set_msg("client: WPA key must be 8-63 characters or 64 hexadecimal digits");
        return false;
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
        set_msg("client: exact SSID not found; hidden-network provisioning is not supported");
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
            g_cfg.ssid, agent_ok ? "" : " (agent failed or timed out)");
    return false;
}
#endif


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
 * OUTPUT hook mdnsd can get EPERM, so the phone can never
 * see the car's _carplay._tcp records even with the AP up. */
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

static bool fw_rule_add(const char *tool, const char *chain, const char *rule)
{
    if (sh("%s -C %s %s 2>/dev/null", tool, chain, rule) == 0) return true;
    return sh("%s -I %s %s 2>/dev/null", tool, chain, rule) == 0;
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
    if (!fw_chain_ensure(tool, "WIFI1B_APP_SSH")) return false;
    return sh("%s -C WIFI1B_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null", tool, port) == 0 ||
           sh("%s -I WIFI1B_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null", tool, port) == 0;
}

static void fw_close(int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    sh("%s -D WIFI1B_APP_SSH -p tcp --dport %d -j ACCEPT 2>/dev/null",
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
    if (!fw_chain_ensure(tool, "WIFI1B_APP_CP")) return false;
    return sh("%s -C WIFI1B_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0 ||
           sh("%s -I WIFI1B_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0;
}

static bool fw_port_add(const char *proto, int port)
{
    char tool[256];
    fw_tool(tool, sizeof(tool));
    if (!fw_chain_ensure(tool, "WIFI1B_APP_CP")) return false;
    return sh("%s -C WIFI1B_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0 ||
           sh("%s -I WIFI1B_APP_CP -p %s --dport %d -j ACCEPT 2>/dev/null", tool, proto, port) == 0;
}

static bool carplay_fw_open(void)
{
    /* AirPlay / CarPlay / DHCP / mDNS on the AP interface.  The stock
     * firewall only opens these on ncm0 (USB CarPlay); wireless needs
     * its own wlan0 rules. Rules live only in our owned chains: never
     * widen a stock/global chain or another interface. */
    bool ok = true;
    const int udp[] = {67, 5353, 5020, 6000, 6003, 6011};
    const int tcp[] = {5000, 5001, 6030, 7000};
    for (size_t i = 0; i < sizeof(udp)/sizeof(udp[0]); i++) if (!fw_port_add("udp", udp[i])) ok = false;
    for (size_t i = 0; i < sizeof(tcp)/sizeof(tcp[0]); i++) if (!fw_port_add("tcp", tcp[i])) ok = false;

    /* mDNS and AirPlay also arrive over IPv6 link-local. Parity with the
     * IPv4 list includes the stock UDP 5020 and TCP 6030 ports. DHCPv6
     * (UDP 547) supports link-local autoconfiguration clients. */
    const int udp6[] = {547, 5353, 5020, 6000, 6003, 6011};
    const int tcp6[] = {5000, 5001, 6030, 7000};
    for (size_t i = 0; i < sizeof(udp6)/sizeof(udp6[0]); i++) if (!fw6_port_add("udp", udp6[i])) ok = false;
    for (size_t i = 0; i < sizeof(tcp6)/sizeof(tcp6[0]); i++) if (!fw6_port_add("tcp", tcp6[i])) ok = false;

    /* NDP/MLD/echo and link-local multicast must traverse the owned wlan0
     * chains before any default-DROP policy can reject an IPv6 link-local
     * CarPlay peer. These accepts are bounded to the wlan0 interface. */
    char tool6[256];
    fw6_tool(tool6, sizeof(tool6));
    if (!fw_rule_add(tool6, "WIFI1B_APP_CP", "-p ipv6-icmp -j ACCEPT")) ok = false;
    if (!fw_rule_add(tool6, "WIFI1B_APP_CP", "-d ff00::/8 -j ACCEPT")) ok = false;
    return ok;
}

static void carplay_fw_close(void)
{
    char tool[256];
    fw_tool(tool, sizeof(tool)); fw_chain_close(tool, "WIFI1B_APP_CP");
    fw6_tool(tool, sizeof(tool)); fw_chain_close(tool, "WIFI1B_APP_CP");
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
#if 0
/* replaced by wifi1b backend */
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
#endif


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

static void ssh_stop_common(bool rewrite_rule)
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
    fw_chain_close(tool, "WIFI1B_APP_SSH");
    if (rewrite_rule) {
        /* Normal stop: close the usb0 chain and park the rule at port 0.
         * Cleanup intentionally leaves both in place as evidence for the
         * exact --usb0-cleanup validator. */
        if (!usb0_rule_write(0))
            log_msg("SSH: usb0 port-0 state could not be applied");
    }
    g_ssh_active_port = 0;
    g_ssh_fw_open = false;
    g_ssh_port_fallback = false;
    g_ssh_bound_ip[0] = 0;
    unlink(SSH_PID);
    unlink(SSH_BOUND);
}

static void ssh_stop(void)
{
    ssh_stop_common(true);
}

static void ssh_stop_cleanup(void)
{
    ssh_stop_common(false);
}

/* The vendor firewall can be reloaded, and a daemon restart can leave
 * dropbear running with its chain flushed.  Re-assert the owned rule
 * periodically; fw_open is idempotent (-C first). */
static void ssh_fw_tick(void)
{
    static int64_t last_ms;
    int64_t now = now_ms();
    if (now - last_ms < 30000) return;
    last_ms = now;
    if (g_apply_pid > 0 || g_work_pending) return;
    if (!g_cfg.ssh_enabled || !g_ssh_active_port || !ssh_running()) return;
    if (ssh_wifi_active()) {
        bool ok = fw_open(g_ssh_active_port);
        if (ok != g_ssh_fw_open) {
            g_ssh_fw_open = ok;
            log_msg("SSH: firewall rule %s on port %d",
                    ok ? "restored" : "missing", g_ssh_active_port);
        }
    } else {
        g_ssh_fw_open = false;
    }
    if (usb0_present() && !usb0_rule_write(g_ssh_active_port))
        log_msg("SSH: usb0 exact state could not be reasserted on port %d",
                g_ssh_active_port);
}

/* A full filesystem can leave status.json.tmp.<pid> files behind when the
 * daemon is killed mid-write.  Remove stale ones at startup. */
static void cleanup_stale_tmp(void)
{
    DIR *d = opendir(APP_DIR);
    if (!d) return;
    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strstr(de->d_name, ".tmp.")) continue;
        char path[512];
        snprintf(path, sizeof(path), APP_DIR "/%s", de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && now - st.st_mtime > 3600)
            log_msg("stale atomic temp preserved (manual review): %s", path);
    }
    closedir(d);
}

#if 0
/* replaced by wifi1b backend */
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
                find_service(services, g_cfg.ssid, service, sizeof(service)) && client_service_ready(service, ip);
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
#endif


/* ------------------------------------------------------------------ */
/* mode application                                                    */
/* ------------------------------------------------------------------ */

#if 0
/* replaced by wifi1b backend */
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
#endif


static const char *mode_name(void);

/* Runs in the apply child.  All slow network work lives here so the HTTP
 * loop and status polling stay responsive.  The child never writes the
 * config: it reports observed adjustments (band fallback, chosen SSH port)
 * as "set key=value" lines and the parent alone persists them, so a canceled
 * or stale child cannot save a config it did not apply. */
#if 0
/* replaced by wifi1b backend */
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
#endif


/* Route-audit results merge only route policy/readback into the existing
 * absolute lease.  They are not a new DHCP lease and must never refresh
 * valid/expiry/T1/T2 or turn a missing router into a ready lease. */
static bool accept_route_audit_result(const char *text)
{
    char copy[4096], *save = NULL;
    int gen = -1, ok = -1;
    int audit = 0, route_ok = 0, wifi_default = 0, modem = 0, legacy = 0;
    unsigned int metric = 0, proto = 0;
    if (text == NULL) return false;
    str_copy(copy, sizeof(copy), text);
    char *l1 = strtok_r(copy, "\n", &save);
    char *msg = strtok_r(NULL, "\n", &save);
    (void)msg;
    char extra;
    if (l1 == NULL || sscanf(l1, "%d %d %c", &gen, &ok, &extra) != 2 ||
        ok != 1 || gen != g_apply_gen)
        return false;
    if (!g_dhcp_lease.valid || g_dhcp_lease.ip == 0 ||
        g_dhcp_lease.gateway == 0 ||
        g_dhcp_lease.prefix <= 0 || g_dhcp_lease.prefix > 32 ||
        !w2_lease_router_consistent(g_dhcp_lease.ip, g_dhcp_lease.prefix,
                                    g_dhcp_lease.gateway))
        return false;
    if (g_client_expiry_at_ms > 0 && now_ms() >= g_client_expiry_at_ms)
        return false;
    for (char *line = strtok_r(NULL, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        int v = atoi(eq);
        if (!strcmp(line, "client_route_audit")) audit = v == 1;
        else if (!strcmp(line, "client_lease_route_ok")) route_ok = v == 1;
        else if (!strcmp(line, "client_wifi_default")) wifi_default = v == 1;
        else if (!strcmp(line, "client_modem_ipv4")) modem = v == 1;
        else if (!strcmp(line, "client_modem_unknown")) {
            if (v == 1) return false;
        }
        else if (!strcmp(line, "client_legacy_route_removed")) legacy = v;
        else if (!strcmp(line, "client_route_metric")) {
            unsigned int tmp = 0;
            if (w2_route_token_u32(eq, &tmp)) metric = tmp;
        } else if (!strcmp(line, "client_route_proto")) {
            unsigned int tmp = 0;
            if (w2_route_token_u32(eq, &tmp)) proto = tmp;
        }
    }
    if (audit != 1 || route_ok != 1) return false;
    if (wifi_default) {
        if (metric == 0 || proto == 0) return false;
    } else if (metric != 0 || proto != 0) {
        return false;
    }
    g_dhcp_lease.route_ok = true;
    g_dhcp_lease.wifi_default = wifi_default != 0;
    g_dhcp_lease.route_metric = metric;
    g_dhcp_lease.route_proto = proto;
    g_client_modem_ipv4 = modem != 0;
    g_client_modem_unknown = false;
    g_client_legacy_routes_removed = 0;
    (void)legacy;
    return true;
}

/* Cancel an in-flight apply: the newest user intent wins.  The child runs in
 * its own process group so shell/connmanctl descendants are signaled too,
 * not just the worker PID. */
static void apply_cancel(void)
{
    if (g_apply_pid <= 0) return;
    g_apply_cancelled = true;
    g_cancel_ms = now_ms();
    /* A DHCP transaction may already have changed the exact address/route.
     * Let its bounded worker finish and harvest the result for cleanup before
     * the superseding work runs. Other work kinds keep prompt cancellation. */
    if (g_apply_dhcp_capable) return;
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
    char lease_ip[32] = "", lease_server[32] = "", lease_gateway[32] = "";
    char lease_iface[16] = "";
    int lease_updated = 0, lease_valid = 0, lease_route_ok = 0;
    int lease_cleanup_pending = 0, lease_fs_unconfirmed = 0;
    int lease_prefix = 0, lease_s = 0, lease_t1 = 0, lease_t2 = 0;
    int lease_wifi_default = 0, lease_modem_ipv4 = 0;
    int lease_modem_unknown = 0, lease_legacy_removed = 0;
    bool have_wifi_default = false;
    unsigned int lease_metric = 0, lease_proto = 0;
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
        else if (!strcmp(line, "dhcp_tx")) g_dhcp_tx = v;
        else if (!strcmp(line, "dhcp_rx_offer")) g_dhcp_rx_offer = v;
        else if (!strcmp(line, "dhcp_rx_ack")) g_dhcp_rx_ack = v;
        else if (!strcmp(line, "dhcp_attempts")) g_dhcp_attempts = v;
        else if (!strcmp(line, "dhcp_bssid")) str_copy(g_dhcp_bssid, sizeof(g_dhcp_bssid), eq);
        else if (!strcmp(line, "dhcp_error")) str_copy(g_dhcp_error, sizeof(g_dhcp_error), eq);
        else if (!strcmp(line, "client_lease_updated")) lease_updated = v == 1;
        else if (!strcmp(line, "client_lease_valid")) lease_valid = v == 1;
        else if (!strcmp(line, "client_lease_ip")) str_copy(lease_ip, sizeof(lease_ip), eq);
        else if (!strcmp(line, "client_lease_server")) str_copy(lease_server, sizeof(lease_server), eq);
        else if (!strcmp(line, "client_lease_gateway")) str_copy(lease_gateway, sizeof(lease_gateway), eq);
        else if (!strcmp(line, "client_lease_iface")) str_copy(lease_iface, sizeof(lease_iface), eq);
        else if (!strcmp(line, "client_lease_prefix")) lease_prefix = v;
        else if (!strcmp(line, "client_lease_lease_s")) lease_s = v;
        else if (!strcmp(line, "client_lease_t1_s")) lease_t1 = v;
        else if (!strcmp(line, "client_lease_t2_s")) lease_t2 = v;
        else if (!strcmp(line, "client_lease_route_ok")) lease_route_ok = v == 1;
        else if (!strcmp(line, "client_lease_cleanup_pending")) lease_cleanup_pending = v == 1;
        else if (!strcmp(line, "client_lease_fs_unconfirmed")) lease_fs_unconfirmed = v == 1;
        else if (!strcmp(line, "client_wifi_default")) {
            lease_wifi_default = v == 1;
            have_wifi_default = true;
        }
        else if (!strcmp(line, "client_modem_ipv4")) lease_modem_ipv4 = v == 1;
        else if (!strcmp(line, "client_modem_unknown")) lease_modem_unknown = v == 1;
        else if (!strcmp(line, "client_legacy_route_removed")) lease_legacy_removed = v;
        else if (!strcmp(line, "client_route_metric")) {
            unsigned int tmp;
            if (w2_route_token_u32(eq, &tmp)) lease_metric = tmp;
        } else if (!strcmp(line, "client_route_proto")) {
            unsigned int tmp;
            if (w2_route_token_u32(eq, &tmp)) lease_proto = tmp;
        }
    }
    g_dhcp_owner_fs_unconfirmed = lease_fs_unconfirmed != 0;
    if (lease_cleanup_pending) {
        /* Pending cleanup wins over any "updated" flag. Adopt the exact
         * retained owner so the bounded tick can retry even in Off/carplay
         * modes; a route-only owner has no IP but still needs the interface. */
        struct in_addr ip_addr;
        bool have_ip = inet_pton(AF_INET, lease_ip, &ip_addr) == 1 &&
                       lease_prefix > 0 && lease_prefix <= 32;
        if (lease_iface[0] != '\0' && (have_ip || lease_ip[0] == '\0')) {
            memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
            /* Not clean: the exact owner is retained only for retry. */
            g_dhcp_lease.updated = false;
            g_dhcp_lease.cleanup_pending = true;
            g_dhcp_lease.ip = have_ip ? ntohl(ip_addr.s_addr) : 0;
            g_dhcp_lease.prefix = have_ip ? lease_prefix : 0;
            str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), lease_iface);
            g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
            g_dhcp_next_try_ms = 0;
        } else {
            /* Keep the parent's existing exact owner rather than forgetting
             * it on a malformed pending result. */
            log_msg("dhcp client: malformed cleanup-pending lease readback");
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.wifi_default = false;
            g_dhcp_lease.cleanup_pending = true;
            g_dhcp_lease.updated = false;
            g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
            g_dhcp_next_try_ms = 0;
        }
    } else if (lease_updated) {
        struct in_addr ip_addr = {0}, server_addr = {0}, gateway_addr = {0};
        if (!have_wifi_default) lease_wifi_default = lease_route_ok;
        bool gateway_ok = lease_gateway[0] != '\0' &&
                          inet_pton(AF_INET, lease_gateway, &gateway_addr) == 1;
        bool parsed = inet_pton(AF_INET, lease_ip, &ip_addr) == 1 &&
                      gateway_ok &&
                      (lease_server[0] == '\0' || inet_pton(AF_INET, lease_server, &server_addr) == 1);
        bool router_ok = parsed && ntohl(gateway_addr.s_addr) != 0 &&
                         w2_lease_router_consistent(ntohl(ip_addr.s_addr),
                                                    lease_prefix,
                                                    ntohl(gateway_addr.s_addr));
        bool route_meta = !lease_route_ok ||
                          (lease_wifi_default
                               ? (lease_metric > 0 && lease_proto > 0)
                               : (lease_metric == 0 && lease_proto == 0));
        if (lease_valid && parsed && router_ok && route_meta &&
            !lease_modem_unknown &&
            lease_prefix > 0 && lease_prefix <= 32 &&
            lease_s >= 60 && lease_t1 > 0 && lease_t2 > lease_t1 &&
            lease_t2 < lease_s && lease_iface[0] != '\0') {
            int64_t now = now_ms();
            g_dhcp_lease.valid = true;
            g_dhcp_lease.updated = true;
            g_dhcp_lease.route_ok = lease_route_ok;
            /* A complete validated result owns this exact state; a stale
             * pending flag from the previous lease must not trigger RELEASE. */
            g_dhcp_lease.cleanup_pending = false;
            g_dhcp_lease.ip = ntohl(ip_addr.s_addr);
            g_dhcp_lease.server = lease_server[0] ? ntohl(server_addr.s_addr) : 0;
            g_dhcp_lease.gateway = ntohl(gateway_addr.s_addr);
            g_dhcp_lease.prefix = lease_prefix;
            g_dhcp_lease.lease_s = lease_s;
            g_dhcp_lease.t1_s = lease_t1;
            g_dhcp_lease.t2_s = lease_t2;
            g_dhcp_lease.wifi_default = lease_wifi_default != 0;
            g_dhcp_lease.route_metric = lease_metric;
            g_dhcp_lease.route_proto = lease_proto;
            g_client_modem_ipv4 = lease_modem_ipv4 != 0;
            g_client_modem_unknown = lease_modem_unknown != 0;
            /* No legacy route is ever deleted; a result cannot create proof. */
            g_client_legacy_routes_removed = 0;
            (void)lease_legacy_removed;
            str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), lease_iface);
            g_client_t1_at_ms = now + (int64_t)lease_t1 * 1000;
            g_client_t2_at_ms = now + (int64_t)lease_t2 * 1000;
            g_client_expiry_at_ms = now + (int64_t)lease_s * 1000;
            g_dhcp_retry_failures = 0;
            g_dhcp_next_try_ms = 0;
        } else {
            if (lease_valid) {
                /* A ready-looking lease without the ACK router/gateway is
                 * malformed.  Keep the current absolute lease instead of
                 * clearing it on a bad worker result. */
                log_msg("dhcp client: malformed lease readback ignored; current lease preserved");
                return false;
            }
            memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
            g_dhcp_lease.updated = true;
            g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
        }
    }
    g_apply_failed = !ok;
    if (g_work_kind == WORK_MODE) g_network_failed = !ok;
    set_msg("%s", msg);
    if (g_work_kind != WORK_DHCP && !config_save()) {
        g_apply_failed = true;
        set_msg("State applied, but preferences could not be saved; they may revert at reboot");
    }
    if (g_work_kind != WORK_DHCP) bt_state_save();
    return true;
}

static bool apply_result_generation(const char *text, int *gen)
{
    int ok = 0;
    if (text == NULL || gen == NULL) return false;
    if (sscanf(text, "%d %d", gen, &ok) != 2) return false;
    return ok == 0 || ok == 1;
}

/* A superseded/cancelled DHCP worker may already have changed the exact
 * address or ledged route. Harvest only ownership facts for bounded cleanup;
 * never publish a stale lease as ready and never replays config/messages. */
static void adopt_superseded_dhcp_result(const char *text)
{
    char copy[4096];
    char ip[32] = "", iface[16] = "";
    int updated = 0, valid = 0, pending = 0, prefix = 0;
    int fs_unconfirmed = 0;
    char *save = NULL;

    if (text == NULL) return;
    str_copy(copy, sizeof(copy), text);
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        int v = atoi(eq);
        if (!strcmp(line, "client_lease_updated")) updated = v == 1;
        else if (!strcmp(line, "client_lease_valid")) valid = v == 1;
        else if (!strcmp(line, "client_lease_cleanup_pending")) pending = v == 1;
        else if (!strcmp(line, "client_lease_fs_unconfirmed")) fs_unconfirmed = v == 1;
        else if (!strcmp(line, "client_lease_ip")) str_copy(ip, sizeof(ip), eq);
        else if (!strcmp(line, "client_lease_iface")) str_copy(iface, sizeof(iface), eq);
        else if (!strcmp(line, "client_lease_prefix")) prefix = v;
    }

    g_dhcp_owner_fs_unconfirmed = fs_unconfirmed != 0;
    struct in_addr ip_addr;
    bool have_ip = inet_pton(AF_INET, ip, &ip_addr) == 1 &&
                   prefix > 0 && prefix <= 32;
    if (pending || (updated && valid)) {
        /* A completed-but-superseded lease is an exact owner to remove. */
        if (iface[0] != '\0' && (have_ip || ip[0] == '\0')) {
            memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
            g_dhcp_lease.ip = have_ip ? ntohl(ip_addr.s_addr) : 0;
            g_dhcp_lease.prefix = have_ip ? prefix : 0;
            str_copy(g_dhcp_lease.iface, sizeof(g_dhcp_lease.iface), iface);
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            g_dhcp_lease.cleanup_pending = true;
            g_dhcp_lease.updated = false;
            g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
            g_dhcp_next_try_ms = 0;
            log_msg("dhcp client: superseded result retained for cleanup (%s/%d %s)",
                    have_ip ? ip : "route-only", prefix, iface);
        } else {
            log_msg("dhcp client: superseded cleanup result lacks exact owner");
        }
    } else if (updated && !valid) {
        /* A clean failure/release with no retained owner: forget the old
         * lease so it cannot be advertised. */
        memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
        g_dhcp_lease.updated = true;
        g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
        log_msg("dhcp client: superseded result confirmed clean cleanup");
    }
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
    if (kind == WORK_DHCP) g_last_dhcp_work_ms = g_last_apply_ms;
    if (kind == WORK_ROUTE_AUDIT) g_last_route_audit_ms = g_last_apply_ms;
    if (kind == WORK_DUMP) g_last_dump_try_ms = g_last_apply_ms;
    g_pending_kind = kind;
    g_pending_pair = pair;
    g_work_pending = true;
    if (kind != WORK_DHCP && kind != WORK_ROUTE_AUDIT)
        config_save();  /* persist user intent, including Off, before slow work */
    set_msg("%s", kind == WORK_SCAN ? "Scanning..." :
            kind == WORK_DUMP ? "Saving logs..." :
            kind == WORK_SSH ? "Updating SSH..." :
            kind == WORK_PAIR_STOP ? "Stopping pairing..." :
            kind == WORK_DHCP ?
                (g_dhcp_work_step == W2_DHCP_RELEASE
                    ? "Clearing expired client lease..."
                    : "Renewing client lease...") :
            kind == WORK_ROUTE_AUDIT ? "Auditing owned route..." :
            "Applying mode...");
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
    bool dhcp_capable = g_work_kind == WORK_DHCP ||
        g_work_kind == WORK_ROUTE_AUDIT ||
        (g_work_kind == WORK_MODE &&
         ((g_cfg.mode == MODE_CLIENT && g_cfg.wlan_on) ||
          g_dhcp_lease.cleanup_pending || g_dhcp_lease.ip != 0 ||
          g_dhcp_lease.iface[0] != '\0' ||
          access(WIFI1B_DHCP_OWNER_JOURNAL, F_OK) == 0 ||
          access(WIFI1B_CLIENT_ROUTE_LEDGER, F_OK) == 0));
    w2_txn_begin(dhcp_capable);
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
        bool release_work = (g_work_kind == WORK_DHCP &&
                             g_dhcp_work_step == W2_DHCP_RELEASE) ||
                            g_work_kind == WORK_ROUTE_AUDIT;
#ifdef WIFI_TEST
        if (test_work) ok = test_work(g_work_kind, g_current_pair);
        else
#endif
        /* SSH start/stop is interface-independent: dropbear, the key files and
         * the usb0 firewall rule do not mutate wlan0.  A foreign/OEM radio
         * reservation must not block the out-of-band listener at boot. */
        if (g_work_kind != WORK_MODE && g_work_kind != WORK_DUMP &&
            g_work_kind != WORK_SSH &&
            radio_begin("wifi1b_daemon",
                        release_work || g_work_kind == WORK_SCAN ||
                        (g_cfg.wlan_on && g_cfg.mode != MODE_OFF)) != 1) {
            set_msg("Radio busy/reserved; turn the owning WiFi app Off first");
        }
        else if (g_work_kind == WORK_MODE) ok = apply_mode_work(g_current_pair);
        else if (g_work_kind == WORK_SSH) {
            if (g_pairing_stop_requested) bt_pair_stop();
            ssh_stop();
            ok = !g_cfg.ssh_enabled || ssh_start();
        } else if (g_work_kind == WORK_PAIR_STOP) {
            bt_pair_stop();
            ok = g_pair_stop_confirmed;
        } else if (g_work_kind == WORK_DHCP) {
            ok = w2_dhcp_client_renew(g_dhcp_work_step);
            if (release_work) radio_release();
        } else if (g_work_kind == WORK_ROUTE_AUDIT) {
            ok = w2_client_route_audit();
            radio_release();
        } else if (g_work_kind == WORK_DUMP) ok = dump_logs_to_usb(true);
        else if (g_work_kind == WORK_SCAN) {
            char scan[32768];
            scan_networks(scan, sizeof(scan));
            ok = strstr(scan, "\"ok\":true") != NULL;
            if (!g_stop) write_file_atomic(SCAN_RESULT, scan, 0600);
        }
        char lease_ip[32] = "", lease_server[32] = "", lease_gateway[32] = "";
        bool lease_relevant = g_work_kind == WORK_MODE || g_work_kind == WORK_DHCP;
        if (lease_relevant && (g_cfg.mode != MODE_CLIENT || !g_cfg.wlan_on)) {
            g_dhcp_lease.valid = false;
            g_dhcp_lease.route_ok = false;
            /* Pending cleanup wins over the mode transition's clean flag. */
            g_dhcp_lease.updated = !g_dhcp_lease.cleanup_pending;
        }
        if (g_work_kind == WORK_DHCP && g_dhcp_work_step == W2_DHCP_RELEASE && !ok)
            g_dhcp_lease.updated = false;
        if (!lease_relevant) g_dhcp_lease.updated = false;
        if (g_dhcp_lease.ip) w2_ip4_str(g_dhcp_lease.ip, lease_ip, sizeof(lease_ip));
        if (g_dhcp_lease.server) w2_ip4_str(g_dhcp_lease.server, lease_server, sizeof(lease_server));
        if (g_dhcp_lease.gateway) w2_ip4_str(g_dhcp_lease.gateway, lease_gateway, sizeof(lease_gateway));
        char buf[4096];
        snprintf(buf, sizeof(buf), "%d %d\n%s\ncp_band=%d\ncp_channel=%d\n"
                 "ssh_port=%d\ncarplay_ie=%d\nssh_active_port=%d\nssh_fallback=%d\n"
                 "bt_discoverable=%d\nbt_pairable=%d\npair_stop_ok=%d\nbt_address=%s\ncp_id=%s\npair_message=%s\n"
                 "dhcp_tx=%d\ndhcp_rx_offer=%d\ndhcp_rx_ack=%d\ndhcp_attempts=%d\n"
                 "dhcp_bssid=%s\ndhcp_error=%s\n"
                 "client_route_audit=%d\n"
                 "client_lease_updated=%d\nclient_lease_valid=%d\n"
                 "client_lease_cleanup_pending=%d\n"
                 "client_lease_fs_unconfirmed=%d\n"
                 "client_lease_ip=%s\nclient_lease_server=%s\nclient_lease_gateway=%s\n"
                 "client_lease_prefix=%d\nclient_lease_lease_s=%d\n"
                 "client_lease_t1_s=%d\nclient_lease_t2_s=%d\n"
                 "client_lease_route_ok=%d\nclient_wifi_default=%d\n"
                 "client_modem_ipv4=%d\nclient_modem_unknown=%d\n"
                 "client_legacy_route_removed=%d\n"
                 "client_route_metric=%u\n"
                 "client_route_proto=%u\nclient_lease_iface=%s\n",
                 g_apply_gen, ok ? 1 : 0, g_last_msg,
                 g_cfg.cp_band, g_cfg.cp_channel, g_cfg.ssh_port,
                 g_carplay_ie, g_ssh_active_port, g_ssh_port_fallback,
                 g_bt_discoverable, g_bt_pairable, g_pair_stop_confirmed,
                 g_cfg.cp_btaddr, g_cfg.cp_id, g_pair_msg,
                 g_dhcp_tx, g_dhcp_rx_offer, g_dhcp_rx_ack, g_dhcp_attempts,
                 g_dhcp_bssid, g_dhcp_error,
                 g_work_kind == WORK_ROUTE_AUDIT ? 1 : 0,
                 g_dhcp_lease.updated ? 1 : 0, g_dhcp_lease.valid ? 1 : 0,
                 g_dhcp_lease.cleanup_pending ? 1 : 0,
                 g_dhcp_owner_fs_unconfirmed ? 1 : 0,
                 lease_ip, lease_server, lease_gateway,
                 g_dhcp_lease.prefix, g_dhcp_lease.lease_s,
                 g_dhcp_lease.t1_s, g_dhcp_lease.t2_s,
                 g_dhcp_lease.route_ok ? 1 : 0,
                 g_dhcp_lease.wifi_default ? 1 : 0,
                 g_client_modem_ipv4 ? 1 : 0,
                 g_client_modem_unknown ? 1 : 0,
                 g_client_legacy_routes_removed,
                 g_dhcp_lease.route_metric,
                 g_dhcp_lease.route_proto, g_dhcp_lease.iface);
        if (!g_stop) write_file_atomic(APPLY_RESULT, buf, 0600);
        _exit(0);
    }
    if (pid < 0) {
        g_apply_failed = true;
        if (g_work_kind == WORK_MODE) g_network_failed = true;
        if (g_work_kind == WORK_DHCP) {
            int shift = g_dhcp_retry_failures > 4 ? 4 : g_dhcp_retry_failures;
            int64_t delay = 5000LL << shift;
            if (delay > 60000LL) delay = 60000LL;
            g_dhcp_retry_failures++;
            g_dhcp_next_try_ms = now_ms() + delay;
        }
        set_msg("apply: fork failed");
        w2_txn_end();
        return;
    }
    setpgid(pid, pid);
    g_apply_pid = pid;
    g_apply_start_ms = now_ms();
}

static void apply_reap(void)
{
    if (g_apply_pid <= 0) { w2_txn_end(); start_pending_work(); return; }
    int st = 0;
    pid_t pid = g_apply_pid;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0) {
        int64_t deadline = g_txn_active ? g_apply_deadline_ms
                                        : g_apply_start_ms + 150000;
        if (!g_apply_cancelled && now_ms() >= deadline) {
            g_apply_failed = true;
            if (g_work_kind == WORK_MODE) g_network_failed = true;
            set_msg("apply transaction deadline exceeded");
            apply_cancel();
        }
        if (g_apply_cancelled) {
            if (g_apply_dhcp_capable) {
                /* The worker unwinds through remaining-time command/wait
                 * budgets.  Only after that grace may its process group be
                 * killed; the durable owner journal is the reconciliation. */
                if (now_ms() >= deadline + WIFI1B_TXN_GRACE_MS) {
                    kill(-pid, SIGKILL); kill(pid, SIGKILL);
                }
            } else if (now_ms() - g_cancel_ms > 300) {
                kill(-pid, SIGKILL); kill(pid, SIGKILL);
            }
        }
        return;
    }
    if (r < 0 && errno != ECHILD) return;
    /* Always clean the command group, even when its leader exited first. */
    kill(-pid, SIGKILL);
    g_apply_pid = 0;
    char buf[4096] = "";
    bool have_result = read_file(APPLY_RESULT, buf, sizeof(buf)) > 0;
    bool stale_dhcp = false;
    int result_gen = -1;
    bool current_gen = have_result &&
        apply_result_generation(buf, &result_gen) && result_gen == g_apply_gen;
    bool child_failed = !WIFEXITED(st) || WEXITSTATUS(st) != 0;
    bool txn_over = g_txn_active && now_ms() > g_apply_deadline_ms;
    if (g_apply_dhcp_capable && have_result &&
        (g_apply_cancelled || !current_gen || child_failed || txn_over)) {
        /* A WORK_MODE client transaction contains a DHCP acquisition too;
         * harvest it exactly like a WORK_DHCP worker. */
        adopt_superseded_dhcp_result(buf);
        (void)w2_dhcp_reconcile_owned_state();
        stale_dhcp = true;
    } else if (g_apply_dhcp_capable &&
               (!have_result || child_failed || txn_over)) {
        /* Forced exit/no result: adopt only durable exact ownership, never a
         * stale ready lease. */
        g_apply_failed = true;
        if (g_work_kind == WORK_MODE) g_network_failed = true;
        set_msg("worker result missing or failed; exact owner reconciliation pending");
        (void)w2_dhcp_reconcile_owned_state();
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        if (g_dhcp_lease.ip != 0 || g_dhcp_lease.prefix != 0 ||
            g_dhcp_lease.cleanup_pending) {
            g_dhcp_lease.cleanup_pending = true;
            g_dhcp_lease.updated = false;
        } else {
            g_dhcp_lease.updated = true;
        }
        stale_dhcp = true;
    }
    bool is_audit = g_work_kind == WORK_ROUTE_AUDIT;
    bool valid = !stale_dhcp && !g_apply_cancelled &&
                 WIFEXITED(st) && WEXITSTATUS(st) == 0 && have_result &&
                 (is_audit ? accept_route_audit_result(buf)
                           : accept_apply_result(buf));
    unlink(APPLY_RESULT);
    if (g_work_kind == WORK_MODE && (!g_apply_cancelled || !g_work_pending)) g_ap_restart_requested = false;
    if (!valid && !g_apply_cancelled && !stale_dhcp) {
        g_apply_failed = true;
        if (g_work_kind == WORK_MODE) g_network_failed = true;
        set_msg(is_audit ? "route audit refused; keeping current intent" :
                "worker failed; result absent, invalid or stale");
    }
    if (g_work_kind == WORK_MODE && !g_apply_cancelled && (!valid || g_apply_failed)) {
        /* A failed client-mode apply must not leave an old lease advertised. */
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_client_t1_at_ms = g_client_t2_at_ms = g_client_expiry_at_ms = 0;
    }
    if (g_work_kind == WORK_DHCP && !g_apply_cancelled && !stale_dhcp) {
        if (valid && !g_apply_failed) {
            g_dhcp_retry_failures = 0;
            g_dhcp_next_try_ms = 0;
        } else {
            int shift = g_dhcp_retry_failures > 4 ? 4 : g_dhcp_retry_failures;
            int64_t delay = 5000LL << shift;
            if (delay > 60000LL) delay = 60000LL;
            g_dhcp_retry_failures++;
            g_dhcp_next_try_ms = now_ms() + delay;
        }
    }
    if (g_work_kind == WORK_DUMP) {
        g_last_dump_try_ms = now_ms();
        if (valid && !g_apply_failed) { g_boot_dumped = true; g_last_autodump_ms = now_ms(); }
    }
    g_apply_cancelled = false;
    g_last_probe_ms = 0;
    w2_txn_end();
    start_pending_work();
    if (!g_apply_pid && !g_work_pending && g_dump_requested) {
        g_dump_requested = false;
        request_work(WORK_DUMP, false);
    }
    write_status();
}

typedef enum {
    DHCP_TICK_NONE = 0,
    DHCP_TICK_RENEW,
    DHCP_TICK_REBIND,
    DHCP_TICK_ACQUIRE
} dhcp_tick_t;

/* Pure monotonic lease clock. Timers are set from the ACK lifetime and are
 * never derived from wall-clock/configuration state. */
static dhcp_tick_t dhcp_lease_action(bool valid, int64_t now, int64_t t1,
                                     int64_t t2, int64_t expiry)
{
    if (!valid || expiry <= 0 || now >= expiry) return DHCP_TICK_ACQUIRE;
    if (t2 > 0 && now >= t2) return DHCP_TICK_REBIND;
    if (t1 > 0 && now >= t1) return DHCP_TICK_RENEW;
    return DHCP_TICK_NONE;
}

/* One bounded worker at a time: the common apply queue owns scheduling,
 * cancellation and the radio reservation. Expired leases stop being ready
 * immediately (status is derived from this clock) even before the worker
 * gets to remove the stale address/route. */
static void dhcp_lease_tick(void)
{
    if (g_stop) return;
    if (g_apply_pid > 0 || g_work_pending) return;
    (void)w2_dhcp_reconcile_owned_state();
    int64_t now = now_ms();
    if (g_cfg.mode == MODE_CLIENT && g_cfg.wlan_on &&
        g_dhcp_lease.valid && g_client_expiry_at_ms > 0 &&
        now >= g_client_expiry_at_ms) {
        g_dhcp_lease.valid = false;
        g_dhcp_lease.route_ok = false;
        g_dhcp_lease.cleanup_pending = true;
        g_client_t1_at_ms = g_client_t2_at_ms = 0;
    }
    if (now < g_dhcp_next_try_ms) return;
    /* Exact-owner cleanup is not limited to client mode: a mode-off or
     * CarPlay transition can leave a pending address/route that must still
     * be retried until both readbacks are absent. */
    if (g_dhcp_lease.cleanup_pending) {
        g_dhcp_work_step = W2_DHCP_RELEASE;
        request_work(WORK_DHCP, false);
        return;
    }
    if (g_cfg.mode != MODE_CLIENT || !g_cfg.wlan_on) return;
    /* Remove an expired/stale lease before trying to acquire another one. */
    if (!g_dhcp_lease.valid &&
        (g_dhcp_lease.ip != 0 || g_dhcp_lease.iface[0] != '\0')) {
        g_dhcp_lease.cleanup_pending = true;
        g_dhcp_work_step = W2_DHCP_RELEASE;
        request_work(WORK_DHCP, false);
        return;
    }
    dhcp_tick_t action = dhcp_lease_action(g_dhcp_lease.valid, now,
                                           g_client_t1_at_ms,
                                           g_client_t2_at_ms,
                                           g_client_expiry_at_ms);
    if (action == DHCP_TICK_NONE) return;
    if (action == DHCP_TICK_ACQUIRE) {
        /* A full acquire needs an association. Use the cached probe result
         * so the HTTP/main loop never runs wpa_cli here. */
        bool associated = g_observed_ms > 0 &&
                          now - g_observed_ms < 15000 &&
                          g_observed.gen == g_apply_gen &&
                          g_observed.associated;
        if (!associated) return;
        if (now - g_last_mode_ms < 5000) return;
        g_dhcp_work_step = W2_DHCP_ACQUIRE;
    } else if (action == DHCP_TICK_REBIND) {
        g_dhcp_work_step = W2_DHCP_REBIND;
    } else {
        g_dhcp_work_step = W2_DHCP_RENEW;
    }
    request_work(WORK_DHCP, false);
}

/* Route intent drifts when ConnMan brings the modem data call up/down or
 * another service rewrites the default table.  Reconcile every 30 s without
 * ever touching a foreign route. */
static void route_audit_tick(void)
{
    static int64_t last_ms;
    int64_t now = now_ms();
    if (now - last_ms < 30000) return;
    last_ms = now;
    if (g_apply_pid > 0 || g_work_pending) return;
    if (g_cfg.mode != MODE_CLIENT || !g_cfg.wlan_on || !g_dhcp_lease.valid)
        return;
    /* Never mutate routes in the parent/HTTP loop.  The serialized DHCP-
     * capable worker owns the radio reservation and transaction budget. */
    request_work(WORK_ROUTE_AUDIT, false);
}

static void watchdog(void)
{
    int64_t now = now_ms();
    if (now - g_last_watchdog_ms < 10000) return;
    g_last_watchdog_ms = now;

    /* wpa_supplicant writes its own descriptor; nothing here logs once the
     * stack is healthy, so the cap must also run on this tick. */
    log_cap(WPA_LOG);

    /* Log dumps: one attempt ~15 s after start, then only on repeated
     * network failure. */
    if (!g_boot_dumped && g_start_ms && now - g_start_ms > 15000 &&
        (!g_last_dump_try_ms || now - g_last_dump_try_ms > 60000)) {
        g_last_dump_try_ms = now;
        request_work(WORK_DUMP, false);
    }
    if (g_network_failed && (!g_last_dump_try_ms || now - g_last_dump_try_ms > 600000))
        request_work(WORK_DUMP, false);

    if (g_apply_pid > 0 || g_work_pending) return;

    /* The Pi USB link is the out-of-band management path.  Ensure its SSH
     * listener even while WiFi is unassociated or failed: the WiFi health
     * branches below return early and would otherwise starve it. */
    if (g_cfg.ssh_enabled && usb0_present() && !ssh_running()) {
        log_msg("watchdog: USB SSH down; restarting");
        request_work(WORK_SSH, false);
        return;
    }

    if (!g_cfg.wlan_on || g_cfg.mode == MODE_OFF) {
        /* The Pi USB SSH path is expected to outlive WiFi Off.  Dropbear
         * running only for usb0 is not a stale WiFi reservation. */
        bool usb_ssh = g_cfg.ssh_enabled && usb0_present();
        if ((wpa_process_alive() || (ssh_running() && !usb_ssh) ||
             radio_owner_is("wifi1b_daemon")) &&
            now - g_last_mode_ms > 15000)
            apply_mode(false);
        return;
    }

    /* Re-evict the OEM wifi managers while we own wlan0.  This is the
     * public owner recipe's 10-second watchdog; it never kills our own
     * supplicant because ours is identified by its config path. */
    if (pid_by_executable("supplicantmanager") > 0 ||
        pid_by_cmdline("/rw_data/wifi/entropy.bin") > 0)
        { apply_mode(false); return; }

    int64_t retry_ms = g_network_failed ? 60000 : 15000;
    if (now - g_last_mode_ms < retry_ms) return;

    if (!wpa_process_alive()) {
        log_msg("watchdog: wifi1b supplicant gone; re-applying");
        apply_mode(false);
        return;
    }
    if (g_cfg.mode == MODE_CARPLAY) {
        ap_probe_t p;
        ap_probe(&p);
        if (p.state != AP_READY) {
            ap_evidence("watchdog-not-ready", &p);
            apply_mode(false);
            return;
        }
        if (!dhcpd_running()) {
            log_msg("watchdog: DHCP server down; re-applying");
            apply_mode(false);
            return;
        }
    } else if (g_cfg.mode == MODE_CLIENT) {
        char ip[64];
        /* The built-in DHCP client is a one-shot inside the apply; the
         * health signal is a live association plus a visible address. */
        if (!client_associated() || !wlan_ip(ip, sizeof(ip))) {
            log_msg("watchdog: client association/DHCP down; re-applying");
            apply_mode(false);
            return;
        }
        route_audit_tick();
    }
    if (g_cfg.ssh_enabled && !ssh_running()) {
        log_msg("watchdog: SSH down; restarting");
        request_work(WORK_SSH, false);
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
    if (o->associated) wlan_link(o->ssid, sizeof(o->ssid), &o->signal);
    o->carplay_svc = sh("systemctl is-active --quiet carplay_iAP2.service 2>/dev/null") == 0;
    o->mdns_svc = sh("systemctl is-active --quiet carplay_mdnsd.service 2>/dev/null") == 0;
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
    observation_t unknown;
    memset(&unknown, 0, sizeof(unknown));
    unknown.secure_console = -1;
    unknown.ap.role = -1;
    unknown.ap.ifindex = -1;
    const observation_t *o = fresh ? &g_observed : &unknown;
    int active_ssh_port = g_ssh_active_port ? g_ssh_active_port : g_cfg.ssh_port;
    long long free_kb = disk_free_kb();
    bool disk_low = free_kb >= 0 && free_kb < 1024;

    char ssid_e[256], ip_e[128], ap_e[256], msg_e[512];
    char imp_e[512], cps_e[256], cpsk_e[256], bt_e[128], id_e[128];
    char client_e[256], pair_e[512];
    char dhcp_bssid_e[64], dhcp_error_e[256];
    char ap_sec_e[96], ap_ll_e[160], ap_v6e_e[256];
    char client_gw_e[64], client_lease_ip[32] = "";
    char radio_owner[32];
    int64_t now = now_ms();
    if (g_dhcp_lease.ip) w2_ip4_str(g_dhcp_lease.ip, client_lease_ip, sizeof(client_lease_ip));
    bool lease_timer_valid = g_cfg.mode == MODE_CLIENT && g_cfg.wlan_on &&
                             g_dhcp_lease.valid && g_client_expiry_at_ms > now;
    bool client_route_ok = lease_timer_valid && g_dhcp_lease.route_ok;
    bool client_ready = client_route_ok &&
                        o->associated && client_lease_ip[0] &&
                        !strcmp(o->ip, client_lease_ip);
    const char *effective_ip = o->ip;
    if (g_cfg.mode == MODE_CLIENT)
        effective_ip = client_ready ? client_lease_ip : "";
    int client_lease_remaining = lease_timer_valid
        ? (int)((g_client_expiry_at_ms - now) / 1000) : 0;
    const char *client_lease_state = "none";
    if (g_dhcp_lease.valid) {
        if (g_client_expiry_at_ms <= 0) client_lease_state = "unknown";
        else if (now >= g_client_expiry_at_ms) client_lease_state = "expired";
        else if (g_client_t2_at_ms > 0 && now >= g_client_t2_at_ms) client_lease_state = "rebind";
        else if (g_client_t1_at_ms > 0 && now >= g_client_t1_at_ms) client_lease_state = "renew";
        else client_lease_state = "bound";
    } else if (g_dhcp_lease.ip && g_client_expiry_at_ms > 0) {
        client_lease_state = "expired";
    }
    /* Local service state is not a CarPlay session. Until a discovery,
     * iAP2 and projection handshake is actually observed, "ready" must
     * stay false and the UI reports the components separately. */
    const bool carplay_session_observed = false;
    const bool carplay_components_ok =
        g_cfg.mode == MODE_CARPLAY && g_cfg.wlan_on && !g_network_failed &&
        o->ap_on && g_carplay_ie && o->carplay_svc && o->mdns_svc;
    if (!radio_record_name(radio_owner, sizeof(radio_owner))) radio_owner[0] = 0;
    json_escape(ssid_e, sizeof(ssid_e), o->ssid);
    json_escape(ip_e, sizeof(ip_e), effective_ip);
    json_escape(ap_e, sizeof(ap_e), o->ap_ssid);
    json_escape(msg_e, sizeof(msg_e), g_last_msg);
    json_escape(imp_e, sizeof(imp_e), g_cfg.imported_from);
    json_escape(cps_e, sizeof(cps_e), g_cfg.cp_ssid);
    json_escape(cpsk_e, sizeof(cpsk_e), g_cfg.cp_psk);
    json_escape(bt_e, sizeof(bt_e), g_cfg.cp_btaddr);
    json_escape(id_e, sizeof(id_e), g_cfg.cp_id);
    json_escape(client_e, sizeof(client_e), g_cfg.ssid);
    json_escape(pair_e, sizeof(pair_e), g_pair_msg);
    json_escape(dhcp_bssid_e, sizeof(dhcp_bssid_e), g_dhcp_bssid);
    json_escape(dhcp_error_e, sizeof(dhcp_error_e), g_dhcp_error);
    json_escape(ap_sec_e, sizeof(ap_sec_e), o->ap.security);
    json_escape(ap_ll_e, sizeof(ap_ll_e), o->ap.linklocal);
    json_escape(ap_v6e_e, sizeof(ap_v6e_e), o->ap.ipv6_error);
    char gateway_text[32] = "";
    if (g_dhcp_lease.gateway) w2_ip4_str(g_dhcp_lease.gateway, gateway_text, sizeof(gateway_text));
    json_escape(client_gw_e, sizeof(client_gw_e), gateway_text);

    /* Legacy "carplay_ie" stays an alias for the configured flag because the
     * existing QML consumes it. The split fields below make the difference
     * explicit: no IE is claimed as observed. */
    snprintf(out, n,
             "{\"ok\":true,"
             "\"app\":\"wifi1b\","
             "\"backend\":\"static_supplicant\","
             "\"dhcp_active\":%s,"
             "\"dhcp_leases\":%d,"
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
             "\"radio_owner\":\"%s\","
             "\"ip\":\"%s\","
             "\"client_ready\":%s,"
             "\"client_lease_valid\":%s,"
             "\"client_lease_remaining_s\":%d,"
             "\"client_lease_state\":\"%s\","
             "\"client_route_ok\":%s,"
             "\"client_route_policy\":\"%s\","
             "\"client_wifi_default\":%s,"
             "\"client_modem_ipv4\":%s,"
             "\"client_modem_unknown\":%s,"
             "\"client_legacy_route_removed\":%d,"
             "\"client_route_metric\":%u,"
             "\"client_route_proto\":%u,"
             "\"client_gateway\":\"%s\","
             "\"ssid\":\"%s\","
             "\"signal\":%d,"
             "\"ap_active\":%s,"
             "\"ap_interface\":%s,"
             "\"ap_state\":%d,"
             "\"ap_tether_field\":%d,"
             "\"ap_ssid\":\"%s\","
             "\"ap_clients\":%d,"
             "\"ap_channel\":%d,"
             "\"ap_frequency\":%d,"
             "\"ap_security\":\"%s\","
             "\"ap_linklocal\":\"%s\","
             "\"ap_ifindex\":%d,"
             "\"ap_ipv4_ready\":%s,"
             "\"ap_ipv6_ready\":%s,"
             "\"ap_network_ready\":%s,"
             "\"ap_channel_matches_config\":%s,"
             "\"ap_ipv6_error\":\"%s\","
             "\"carplay_ready\":%s,"
             "\"carplay_components_ok\":%s,"
             "\"carplay_session_observed\":%s,"
             "\"carplay_ap_up\":%s,"
             "\"carplay_ie_configured\":%s,"
             "\"carplay_ie_observed\":false,"
             "\"carplay_ie_observation\":\"unverified\","
             "\"carplay_control\":\"%s\","
             "\"carplay_ie\":%s,"
             "\"carplay_iap2\":%s,"
             "\"carplay_mdns\":%s,"
             "\"ssh_active\":%s,"
             "\"ssh_enabled\":%s,"
             "\"ssh_port\":%d,"
             "\"ssh_port_fallback\":%s,"
             "\"ssh_fw_open\":%s,"
             "\"ssh_usb_fw_open\":%s,"
             "\"disk_free_kb\":%lld,"
             "\"disk_low\":%s,"
             "\"dhcp_tx\":%d,\"dhcp_rx_offer\":%d,\"dhcp_rx_ack\":%d,\"dhcp_attempts\":%d,"
             "\"client_bssid\":\"%s\",\"dhcp_error\":\"%s\","
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
             dhcpd_running() ? "true" : "false", dhcp_leases_count(),
             WIFI_BUILD_ID, (long long)(time(NULL) * 1000LL), fresh ? "true" : "false",
             mode_name(),
             (g_apply_pid > 0 || g_work_pending) ? "true" : "false",
             g_home_seq, g_home_epoch, g_home_source, g_home_last_key,
             g_home_monitor ? "true" : "false",
             g_cfg.wlan_on ? "true" : "false",
             o->driver ? "true" : "false",
             radio_owner[0] ? radio_owner : "",
             ip_e,
             client_ready ? "true" : "false",
             lease_timer_valid ? "true" : "false",
             client_lease_remaining,
             client_lease_state,
             client_route_ok ? "true" : "false",
             g_cfg.client_route_policy == CLIENT_ROUTE_FALLBACK
                 ? "fallback" : "modem_safe",
             g_dhcp_lease.wifi_default ? "true" : "false",
             g_client_modem_ipv4 ? "true" : "false",
             g_client_modem_unknown ? "true" : "false",
             g_client_legacy_routes_removed,
             g_dhcp_lease.route_metric,
             g_dhcp_lease.route_proto,
             client_gw_e,
             ssid_e, o->signal,
             o->ap_on ? "true" : "false", o->ap.role == 1 ? "true" : "false",
             o->ap.state, fresh ? o->ap.tether : -1, ap_e, o->clients,
             o->ap.channel, o->ap.frequency, ap_sec_e, ap_ll_e, o->ap.ifindex,
             o->ap.ipv4_ready ? "true" : "false",
             o->ap.ipv6_ready ? "true" : "false",
             o->ap.state == AP_READY ? "true" : "false",
             o->ap.channel_matches_config ? "true" : "false",
             ap_v6e_e,
             /* "ready" requires an observed CarPlay session. The local
              * AP/IE/service checks are reported separately. */
             (carplay_components_ok && carplay_session_observed) ? "true" : "false",
             carplay_components_ok ? "true" : "false",
             carplay_session_observed ? "true" : "false",
             o->ap_on ? "true" : "false",
             g_carplay_ie ? "true" : "false",
             g_cfg.carplay_control ? "stock" : "wifi1b",
             g_carplay_ie ? "true" : "false",
             o->carplay_svc ? "true" : "false",
             o->mdns_svc ? "true" : "false",
             o->ssh_ours ? "true" : "false",
             g_cfg.ssh_enabled ? "true" : "false",
             active_ssh_port,
             g_ssh_port_fallback ? "true" : "false",
             g_ssh_fw_open ? "true" : "false",
             (g_usb0_fw_port > 0 && usb0_present()) ? "true" : "false",
             free_kb,
             disk_low ? "true" : "false",
             g_dhcp_tx, g_dhcp_rx_offer, g_dhcp_rx_ack, g_dhcp_attempts,
             dhcp_bssid_e, dhcp_error_e,
             o->vendor_ssh_22 ? "true" : "false",
             o->secure_console,
             o->debug_console ? "true" : "false",
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
    long long free_kb = disk_free_kb();
    if (free_kb >= 0 && free_kb < 1024) {
        int64_t now = now_ms();
        if (now - g_disk_low_log_ms > 60000) {
            g_disk_low_log_ms = now;
            log_msg("status write skipped: %s free space %lld KiB", APP_DIR, free_kb);
        }
        return;
    }
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
    if (radio_begin("wifi1b_daemon", true) != 1) {
        snprintf(out, n, "{\"ok\":false,\"error\":\"Radio reserved by the other WiFi app; turn it Off first\"}\n");
        return;
    }
    if (g_cfg.mode == MODE_CARPLAY) {
        snprintf(out, n, "{\"ok\":false,\"error\":\"Stop the CarPlay AP before scanning\"}\n");
        return;
    }
    bool was_off = !g_cfg.wlan_on || g_cfg.mode == MODE_OFF;
    set_msg("Scanning...");
    write_status();
    if (!ensure_wifi_ready()) {
        char msg_e[512];
        json_escape(msg_e, sizeof(msg_e), g_last_msg);
        snprintf(out, n, "{\"ok\":false,\"error\":\"%s\"}\n", msg_e);
        return;
    }
    bool temporary = was_off || !wpa_process_alive();
    if (temporary) {
        stop_wpa_supplicant();
        if (!write_scan_conf() || !start_wpa_supplicant()) {
            snprintf(out, n, "{\"ok\":false,\"error\":\"scan supplicant failed\"}\n");
            return;
        }
    }
    sh("wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan 2>/dev/null");
    sleep(3);
    char buf[32768];
    int rc = sh_out(buf, sizeof(buf),
                    "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results 2>/dev/null");
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
            if (nf < 5 || !strncmp(fields[0], "bssid", 5)) continue;
            const char *ssid = fields[4];
            if (!ssid[0]) continue;
            int sig = atoi(fields[2]);
            const char *flags = fields[3];
            const char *sec = strstr(flags, "SAE") ? "WPA3" :
                              strstr(flags, "WPA2") ? "WPA2" :
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
    snprintf(out + used, n - used, "]}\n");
    if (temporary) {
        stop_wpa_supplicant();
        iface_down();
    }
    if (was_off) {
        g_cfg.wlan_on = false;
        g_cfg.mode = MODE_OFF;
        bool scan_txn = !g_txn_active;
        if (scan_txn) w2_txn_begin(false);
        apply_mode_work(false);
        if (scan_txn) w2_txn_end();
    }
    set_msg("scan: %d network(s) found", count);
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
                     strstr(line, " wifi1b_daemon started:") ||
                     strstr(line, " watchdog:") || strstr(line, " apply ") ||
                     strstr(line, " worker ") || strstr(line, " started wpa_supplicant") ||
                     strstr(line, " control socket ") || strstr(line, " msg:");
        if (!event) continue;
        redact_secret(line, g_cfg.cp_psk); redact_secret(line, g_cfg.psk);
        for (int i = 0; i < 3; i++) redact_secret(line, g_cfg.extra_psk[i]);
        fputs(line, out);
    }
    fclose(in); fclose(out);
}

static void scrub_snapshot(char *out)
{
    redact_secret(out, g_cfg.cp_psk); redact_secret(out, g_cfg.psk);
    for (int i = 0; i < 3; i++) redact_secret(out, g_cfg.extra_psk[i]);
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
    if (!name || strlen(name) != 25 || strncmp(name, "wifi1b_logs_", 11)) return false;
    for (int i = 11; i < 25; i++) if (!isdigit((unsigned char)name[i])) return false;
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
    snprintf(dir, sizeof(dir), "%s/wifi1b_logs_%s", usb, stamp);
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
        fprintf(f, "wifi1b log dump %s build_id=%s mono_ms=%lld\n", stamp, WIFI_BUILD_ID, (long long)now_ms());
        snap_cmd(f, "uname -a");
        snap_cmd(f, "cat /proc/mounts");
        snap_cmd(f, "date");
        snap_cmd(f, "ps w | grep -E 'wpa_supplicant|connmand|supplicantmanager|wifi_daemon|wifi1b_daemon|wifi1b_dhcpd|udhcpc|dropbear|vbtd' | grep -v grep");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 status");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 get ap_scan");
        snap_cmd(f, "wpa_cli -p " WPA_CTRL_DIR " -i wlan0 scan_results");
        snap_cmd(f, "ls -la " WPA_CTRL_DIR);
        snap_cmd(f, "getfattr -n security.SMACK64 /rw_data/wifi/wifi_daemon /rw_data/wifi1b/wifi1b_daemon /rw_data/wifi1b/bin/dropbearmulti /rw_data/wifi1b/bin/wifi1b_qmlpatch");
        snap_cmd(f, "getfattr -n security.SMACK64EXEC /rw_data/wifi/wifi_daemon /rw_data/wifi1b/wifi1b_daemon /rw_data/wifi1b/bin/dropbearmulti /rw_data/wifi1b/bin/wifi1b_qmlpatch");
        snap_cmd(f, "cat /sys/fs/smackfs/onlycap");
        snap_cmd(f, "systemctl is-enabled supplicantmanager.path supplicantmanager.service wifi1b-daemon.service");
        snap_cmd(f, "systemctl status wifi1b-daemon.service --no-pager 2>&1 | head -80");
        snap_cmd(f, "systemctl cat carplay_mdnsd.service 2>&1");
        snap_cmd(f, "systemctl show carplay_mdnsd.service -p Type -p ExecStart -p RemainAfterExit -p Restart -p RestartUSec -p MainPID -p ActiveState -p SubState -p Result -p ExecMainCode -p ExecMainStatus 2>&1");
        snap_cmd(f, "journalctl -b -u carplay_mdnsd.service --no-pager 2>&1 | tail -120");
        /* CarPlay handoff evidence: discovery, SPP/iAP2 transport and
         * projection. Without these, AP/IE/service presence alone is not a
         * session. */
        snap_cmd(f, "journalctl -b -u carplay_iAP2.service --no-pager 2>&1 | tail -200");
        snap_cmd(f, "journalctl -b --no-pager 2>&1 | grep -iE 'iAP2|CarPlay|AirPlay|vbtd|spp|rfcomm' | tail -300");
        snap_cmd(f, "ps w | grep -E 'iAP2|mdnsd|vbtd|CarPlay' | grep -v grep");
        snap_cmd(f, "ls -la /tmp/iAp2Socket /dev/ttyIAP2* 2>&1");
        snap_cmd(f, "ss -lntup 2>&1 || netstat -lntup 2>&1");
        snap_cmd(f, "cat /proc/net/arp");
        snap_cmd(f, "hcitool dev 2>&1; hcitool con 2>&1");
        snap_cmd(f, "bluetoothctl devices 2>&1; bluetoothctl show 2>&1");
        snap_cmd(f, "bluetoothctl info 2>&1");
        snap_cmd(f, "sdptool browse local 2>&1 | head -120");
        snap_cmd(f, "ls -la /sys/class/bluetooth 2>&1");
        snap_cmd(f, "dmesg 2>&1 | grep -iE 'bluetooth|rfcomm|spp|hci' | tail -120");
        snap_cmd(f, "iw dev wlan0 station dump 2>&1");
        snap_cmd(f, "cat /app/data/vbt/VBTConfig.json 2>&1 | head -80");
        snap_cmd(f, "ls -la /rw_data/wifi1b /rw_data/wifi1b/ssh /rw_data/wifi1b/home/.ssh");
        snap_cmd(f, "cat /run/wifi1b/active");
        snap_cmd(f, "ip addr show dev wlan0");
        snap_cmd(f, "ip route");
        snap_cmd(f, "cat /etc/resolv.conf");
        snap_cmd(f, "systemctl is-active connman.service 2>&1");
        snap_cmd(f, "systemctl cat connman.service 2>&1 | tail -40");
        snap_cmd(f, "connmanctl services 2>&1 | head -40");
        snap_cmd(f, "systemctl status TmsFgApp.service --no-pager 2>&1 | head -40");
        snap_cmd(f, "ps w | grep -E 'Tms|rmnet|connmand' | grep -v grep");
        snap_cmd(f, "cat /rw_data/wifi1b/dhcp.leases");
        snap_cmd(f, "tail -n 120 /rw_data/wifi1b/dhcpd.log");
        snap_cmd(f, "grep -nE 'WIFI1B_PAYLOAD|WIFI_PAYLOAD|TWEAKS_PAYLOAD|BATTERY_PAYLOAD|id: wifi1b' /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml");
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
        snap_cmd(f, "iptables -S WIFI1B_APP_SSH 2>&1");
        snap_cmd(f, "ss -lntp 2>/dev/null || netstat -lntp 2>/dev/null | head -80");
        snap_cmd(f, "df -h /rw_data 2>&1");
        snap_cmd(f, "du -xk /rw_data 2>/dev/null | sort -n | tail -30");
        snap_cmd(f, "ls -la /rw_data/wifi1b/*.log* 2>&1");
        snap_cmd(f, "ip6tables -S");
        snap_cmd(f, "dmesg | tail -n 200");
        snap_cmd(f, "dmesg 2>&1 | grep -i 'DHCP\\[wlan0\\]' | tail -40");
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
    } else if (!strcmp(method, "POST") && !strcmp(path, "/carplay/control")) {
        char v[64];
        if (!query_get(query, "value", v, sizeof(v)) ||
            (strcmp(v, "wifi1b") && strcmp(v, "stock") && strcmp(v, "usb"))) {
            code = 400;
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"invalid carplay control\"}\n");
        } else {
            g_cfg.carplay_control = (!strcmp(v, "stock") || !strcmp(v, "usb")) ? 1 : 0;
            config_save();
            apply_carplay_control(true);
            apply_mode(false);
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"carplay_control\":\"%s\"}\n",
                     g_cfg.carplay_control ? "stock" : "wifi1b");
        }
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
            "usage: %s [--port 8093] [--status-file PATH]\n"
            "       %s --status|--mode off|client|carplay|--import|--ssh on|off\n"
            "       %s --ensure-user|--choose-port PORT|--ssh-gate\n"
            "       %s --usb0-rule PORT|--usb0-apply PORT|--usb0-cleanup|--dump-logs\n",
            argv0, argv0, argv0, argv0);
}

static bool shutdown_cleanup_work(void)
{
    g_cfg.wlan_on = false;
    g_cfg.mode = MODE_OFF;
    w2_txn_end();
    w2_txn_begin(false); /* fresh bounded cleanup/handoff budget */
    bool ok = apply_mode_work(false);
    w2_txn_end();
    return ok;
}

static int shutdown_sequence(void)
{
    /* Coordinated shutdown: cancel any in-flight apply and undo the network
     * state we own so stopping the daemon does not leave owned routes,
     * addresses, firewall rules or supplicant state behind. */
    apply_cancel();
    g_work_pending = false; g_dump_requested = false;
    /* Wait only to the transaction's real monotonic deadline (plus a short
     * unwind grace).  The worker itself caps every command/wait by the
     * remaining budget; this is not a nominal 35 s over more 8 s commands. */
    int64_t until = now_ms() + 1500;
    if (g_apply_pid > 0 && g_apply_dhcp_capable) {
        int64_t deadline = g_txn_active ? g_apply_deadline_ms
                                        : now_ms() + WIFI1B_TXN_BUDGET_MS;
        until = deadline + WIFI1B_TXN_GRACE_MS;
        if (until < now_ms() + 1000) until = now_ms() + 1000;
    }
    while (g_apply_pid > 0 && now_ms() < until) {
        apply_reap();
        usleep(20000);
    }
    if (g_apply_pid > 0) {
        set_msg("shutdown: apply transaction deadline reached; exact owner reconciliation required");
        kill(-g_apply_pid, SIGKILL);
        kill(g_apply_pid, SIGKILL);
        while (waitpid(g_apply_pid, NULL, WNOHANG) == 0) {
            if (now_ms() >= until + 2000) break;
            usleep(20000);
        }
        g_apply_pid = 0;
    }
    if (g_apply_dhcp_capable)
        (void)w2_dhcp_reconcile_owned_state();
    if (g_probe_pid > 0) {
        kill(-g_probe_pid, SIGKILL); kill(g_probe_pid, SIGKILL);
        waitpid(g_probe_pid, NULL, WNOHANG);
    }
    /* Cleanup is unconditional: configured Off is not proof an interrupted
     * previous apply removed its firewall/AP/SSH state. Keep it bounded and
     * outside the parent, including on SIGTERM when g_stop is already set. */
    int shutdown_rc = 0;
    pid_t cleanup = fork();
    if (cleanup == 0) {
        setpgid(0, 0); g_in_child = true; g_stop = 0;
        g_shutdown_cleanup = true;
        close_child_fds(-1);
        bool cleanup_ok = shutdown_cleanup_work();
        _exit(cleanup_ok ? 0 : 1);
    }
    if (cleanup > 0) {
        setpgid(cleanup, cleanup);
        until = now_ms() + WIFI1B_TXN_BUDGET_MS + WIFI1B_TXN_GRACE_MS;
        int cst = 0;
        pid_t cr = 0;
        while ((cr = waitpid(cleanup, &cst, WNOHANG)) == 0 && now_ms() < until)
            usleep(20000);
        if (cr == 0) {
            kill(-cleanup, SIGKILL); kill(cleanup, SIGKILL);
            while (waitpid(cleanup, &cst, 0) < 0 && errno == EINTR) {}
        }
        bool cleanup_ok = cr == cleanup && WIFEXITED(cst) &&
                          WEXITSTATUS(cst) == 0;
        if (!cleanup_ok) {
            set_msg("shutdown cleanup not confirmed; exact address/route owner retained");
            log_msg("shutdown: cleanup did not confirm absence; handoff refused");
            write_status();
            shutdown_rc = 1;
        }
    } else {
        shutdown_rc = 1;
    }
    return shutdown_rc;
}

/* The Orange Pi ECM gadget is the out-of-band SSH path.  The helper is
 * deliberately a generated first-party script so udev and the daemon share
 * one implementation: static 10.55.0.2/24, no default route, and only the
 * currently selected SSH port open on usb0. */
/* Read-only firewall tool resolver.  Never creates the APP_DIR symlink that
 * fw_tool() may use as an xtables-multi fallback, so a side-effect-free
 * unknown-ownership cleanup cannot write anything. */
static void fw_tool_ro(char *out, size_t n)
{
    if (file_exists("/usr/sbin/iptables")) {
        str_copy(out, n, "/usr/sbin/iptables");
        return;
    }
    if (file_exists("/sbin/iptables")) {
        str_copy(out, n, "/sbin/iptables");
        return;
    }
    if (file_exists("/usr/sbin/xtables-multi")) {
        snprintf(out, n, "/usr/sbin/xtables-multi iptables");
        return;
    }
    if (file_exists("/sbin/xtables-multi")) {
        snprintf(out, n, "/sbin/xtables-multi iptables");
        return;
    }
    str_copy(out, n, "iptables");
}

/* ------------------------------------------------------------------ */
/* USB0 ownership and live-state contract                              */
/* ------------------------------------------------------------------ */

typedef enum {
    USB0_LIVE_UNKNOWN = -1,
    USB0_LIVE_ABSENT = 0,
    USB0_LIVE_OWNED = 1,
    USB0_LIVE_FOREIGN = 2
} usb0_live_state;

typedef enum {
    USB0_FILE_UNKNOWN = -1,
    USB0_FILE_ABSENT = 0,
    USB0_FILE_OWNED = 1,
    USB0_FILE_FOREIGN = 2
} usb0_file_state;

/* The pinned V22 helper bytes.  This is the old shell implementation; it is
 * accepted only as a legacy fingerprint, never by shape alone.  The current
 * helper delegates all network mutation to the daemon's checked
 * --usb0-apply path so udev cannot run unchecked ifconfig/route/iptables. */
static const char *usb0_helper_legacy_text =
    "#!/bin/sh\n"
    "# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors\n"
    "# SPDX-License-Identifier: AGPL-3.0-only\n"
    "# Generated by wifi1b_daemon.  No default route: this is a management link.\n"
    "IFACE=\"${1:-usb0}\"\n"
    "PORT=\"${2:-0}\"\n"
    "[ \"$IFACE\" = \"usb0\" ] || exit 2\n"
    "case \"$PORT\" in ''|*[!0-9]*) exit 2 ;; esac\n"
    "IFCONFIG=/sbin/ifconfig\n"
    "[ -x \"$IFCONFIG\" ] || IFCONFIG=ifconfig\n"
    "\"$IFCONFIG\" \"$IFACE\" " WIFI1B_USB0_IP " netmask " WIFI1B_USB0_NETMASK \
    " up 2>/dev/null || true\n"
    "IP=/sbin/ip\n"
    "[ -x \"$IP\" ] || IP=ip\n"
    "\"$IP\" -4 route del default dev \"$IFACE\" 2>/dev/null || true\n"
    "IPT=/usr/sbin/iptables\n"
    "[ -x \"$IPT\" ] || IPT=/sbin/iptables\n"
    "[ -x \"$IPT\" ] || IPT=iptables\n"
    "\"$IPT\" -N WIFI1B_USB0_SSH 2>/dev/null || true\n"
    "\"$IPT\" -C INPUT -i \"$IFACE\" -j WIFI1B_USB0_SSH 2>/dev/null || \\\n"
    "    \"$IPT\" -I INPUT -i \"$IFACE\" -j WIFI1B_USB0_SSH 2>/dev/null || true\n"
    "\"$IPT\" -F WIFI1B_USB0_SSH 2>/dev/null || true\n"
    "if [ \"$PORT\" -gt 0 ]; then\n"
    "    \"$IPT\" -A WIFI1B_USB0_SSH -p tcp --dport \"$PORT\" -j ACCEPT 2>/dev/null || true\n"
    "fi\n"
    "exit 0\n";

static bool usb0_file_equals(const char *path, const char *text)
{
    return w2_file_equals_text(path, text);
}

static bool usb0_helper_build(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return false;
    int n = snprintf(out, cap,
        "#!/bin/sh\n"
        "# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors\n"
        "# SPDX-License-Identifier: AGPL-3.0-only\n"
        "# TARGET: Gen5W_L EU 260128\n"
        "# Generated by wifi1b_daemon.  Checked network work is in C.\n"
        "IFACE=\"${1:-usb0}\"\n"
        "PORT=\"${2:-0}\"\n"
        "[ \"$IFACE\" = \"usb0\" ] || exit 2\n"
        "case \"$PORT\" in ''|*[!0-9]*) exit 2 ;; esac\n"
        "DAEMON=\"" APP_DIR "/wifi1b_daemon\"\n"
        "[ -x \"$DAEMON\" ] || exit 3\n"
        "exec \"$DAEMON\" --usb0-apply \"$PORT\"\n");
    return n > 0 && (size_t)n < cap;
}

static bool usb0_rule_build(char *out, size_t cap, int port)
{
    if (out == NULL || cap == 0 || port < 0 || port > 65535) return false;
    int n = snprintf(out, cap,
        "ACTION==\"add\", SUBSYSTEM==\"net\", KERNEL==\"%s\", "
        "RUN+=\"/bin/sh %s %s %d\"\n",
        WIFI1B_USB0_IFACE, USB0_HELPER, WIFI1B_USB0_IFACE, port);
    return n > 0 && (size_t)n < cap;
}

static bool usb0_owner_build(char *out, size_t cap, int port)
{
    if (out == NULL || cap == 0 || port < 0 || port > 65535) return false;
    int n = snprintf(out, cap,
        "v1 scope=wifi1b-usb0 port=%d address=" WIFI1B_USB0_IP "/24\n",
        port);
    return n > 0 && (size_t)n < cap;
}

static usb0_file_state usb0_read_state(const char *path, const char *expected,
                                       struct stat *st_out,
                                       bool *legacy_out)
{
    char buf[4096];
    struct stat st;
    if (st_out != NULL) memset(st_out, 0, sizeof(*st_out));
    if (legacy_out != NULL) *legacy_out = false;
    if (path == NULL) return USB0_FILE_UNKNOWN;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? USB0_FILE_ABSENT : USB0_FILE_UNKNOWN;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (uint32_t)st.st_uid != (uint32_t)geteuid())
        return USB0_FILE_FOREIGN;
    int n = read_file(path, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf) - 1)
        return USB0_FILE_UNKNOWN;
    if (expected != NULL && strcmp(buf, expected) == 0) {
        if (st_out != NULL) *st_out = st;
        return USB0_FILE_OWNED;
    }
    if (legacy_out != NULL && strcmp(buf, usb0_helper_legacy_text) == 0) {
        *legacy_out = true;
        if (st_out != NULL) *st_out = st;
        return USB0_FILE_OWNED;
    }
    return USB0_FILE_FOREIGN;
}

static usb0_file_state usb0_helper_state(struct stat *st_out, bool *legacy)
{
    char expected[4096];
    if (!usb0_helper_build(expected, sizeof(expected)))
        return USB0_FILE_UNKNOWN;
    return usb0_read_state(USB0_HELPER, expected, st_out, legacy);
}

static bool usb0_rule_parse_port(const char *text, int *port_out)
{
    const char *prefix = "ACTION==\"add\", SUBSYSTEM==\"net\", "
                         "KERNEL==\"usb0\", RUN+=\"/bin/sh ";
    const char *helper = USB0_HELPER " usb0 ";
    if (text == NULL || port_out == NULL) return false;
    if (strncmp(text, prefix, strlen(prefix)) != 0) return false;
    const char *p = text + strlen(prefix);
    if (strncmp(p, helper, strlen(helper)) != 0) return false;
    p += strlen(helper);
    errno = 0;
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (errno != 0 || end == p || value < 0 || value > 65535)
        return false;
    if (strcmp(end, "\"\n") != 0) return false;
    *port_out = (int)value;
    return true;
}

static usb0_file_state usb0_rule_state(int *port_out, struct stat *st_out)
{
    char buf[4096], expected[512];
    struct stat st;
    if (port_out != NULL) *port_out = 0;
    if (st_out != NULL) memset(st_out, 0, sizeof(*st_out));
    if (lstat(USB0_RULE_PATH, &st) != 0)
        return errno == ENOENT ? USB0_FILE_ABSENT : USB0_FILE_UNKNOWN;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (uint32_t)st.st_uid != (uint32_t)geteuid())
        return USB0_FILE_FOREIGN;
    int n = read_file(USB0_RULE_PATH, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf) - 1)
        return USB0_FILE_UNKNOWN;
    int port = 0;
    if (!usb0_rule_parse_port(buf, &port) ||
        !usb0_rule_build(expected, sizeof(expected), port) ||
        strcmp(buf, expected) != 0)
        return USB0_FILE_FOREIGN;
    if (port_out != NULL) *port_out = port;
    if (st_out != NULL) *st_out = st;
    return USB0_FILE_OWNED;
}

static usb0_file_state usb0_owner_state(int *port_out, struct stat *st_out)
{
    char buf[512], expected[256];
    struct stat st;
    if (port_out != NULL) *port_out = 0;
    if (st_out != NULL) memset(st_out, 0, sizeof(*st_out));
    if (lstat(USB0_OWNER, &st) != 0)
        return errno == ENOENT ? USB0_FILE_ABSENT : USB0_FILE_UNKNOWN;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (uint32_t)st.st_uid != (uint32_t)geteuid())
        return USB0_FILE_FOREIGN;
    int n = read_file(USB0_OWNER, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf) - 1)
        return USB0_FILE_UNKNOWN;
    int port = 0;
    char tail = 0;
    if (sscanf(buf, "v1 scope=wifi1b-usb0 port=%d address=" WIFI1B_USB0_IP
               "/24%c", &port, &tail) != 2 ||
        port < 0 || port > 65535 ||
        !usb0_owner_build(expected, sizeof(expected), port) ||
        strcmp(buf, expected) != 0)
        return USB0_FILE_FOREIGN;
    if (port_out != NULL) *port_out = port;
    if (st_out != NULL) *st_out = st;
    return USB0_FILE_OWNED;
}

/* Exact canonical TCP ACCEPT: -p tcp [ -m tcp ] --dport N -j ACCEPT.  No
 * UDP, extra matches, source/destination selectors, negations or junk. */
static bool usb0_canonical_rule(const char *line, int *port_out)
{
    char copy[512], *tokens[32] = {0};
    int count = 0;
    char *save = NULL;
    if (line == NULL || port_out == NULL) return false;
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 32;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    int i = 0;
    if (i >= count || strcmp(tokens[i++], "-A") != 0) return false;
    if (i >= count || strcmp(tokens[i++], "WIFI1B_USB0_SSH") != 0) return false;
    if (i >= count || strcmp(tokens[i++], "-p") != 0) return false;
    if (i >= count || strcmp(tokens[i++], "tcp") != 0) return false;
    if (i < count && strcmp(tokens[i], "-m") == 0) {
        i++;
        if (i >= count || strcmp(tokens[i++], "tcp") != 0) return false;
    }
    if (i >= count || strcmp(tokens[i++], "--dport") != 0) return false;
    if (i >= count) return false;
    errno = 0;
    char *end = NULL;
    long port = strtol(tokens[i++], &end, 10);
    if (errno != 0 || end == tokens[i - 1] || *end != '\0' ||
        port <= 0 || port > 65535)
        return false;
    if (i >= count || strcmp(tokens[i++], "-j") != 0) return false;
    if (i >= count || strcmp(tokens[i++], "ACCEPT") != 0) return false;
    if (i != count) return false;
    *port_out = (int)port;
    return true;
}

static bool usb0_hook_exact(const char *line)
{
    char copy[256], *tokens[16] = {0};
    int count = 0;
    char *save = NULL;
    if (line == NULL) return false;
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok_r(copy, " \t", &save); tok && count < 16;
         tok = strtok_r(NULL, " \t", &save))
        tokens[count++] = tok;
    return count == 6 && strcmp(tokens[0], "-A") == 0 &&
           strcmp(tokens[1], "INPUT") == 0 &&
           strcmp(tokens[2], "-i") == 0 &&
           strcmp(tokens[3], WIFI1B_USB0_IFACE) == 0 &&
           strcmp(tokens[4], "-j") == 0 &&
           strcmp(tokens[5], "WIFI1B_USB0_SSH") == 0;
}

static usb0_live_state usb0_addr_probe(bool *present)
{
    char out[16384];
    bool scoped = true;
    bool ours = false, foreign = false;
    if (present != NULL) *present = false;
    if (sh_out(out, sizeof(out),
               "ip -o -4 addr show dev %s 2>/dev/null",
               WIFI1B_USB0_IFACE) != 0) {
        scoped = false;
        if (sh_out(out, sizeof(out), "ip -o -4 addr show 2>/dev/null") != 0)
            return USB0_LIVE_UNKNOWN;
    }
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!scoped && strstr(line, ": " WIFI1B_USB0_IFACE " ") == NULL)
            continue;
        const char *owned = strstr(line, "inet " WIFI1B_USB0_IP "/24");
        if (owned != NULL && (owned[17] == ' ' || owned[17] == '\0'))
            ours = true;
        else if (strstr(line, "inet ") != NULL)
            foreign = true;
    }
    if (present != NULL) *present = ours && !foreign;
    if (ours && foreign) return USB0_LIVE_FOREIGN;
    if (foreign) return USB0_LIVE_FOREIGN;
    if (ours) return USB0_LIVE_OWNED;
    return USB0_LIVE_ABSENT;
}

static usb0_live_state usb0_default_probe(void)
{
    char out[4096];
    if (sh_out(out, sizeof(out), "ip -4 route show default 2>/dev/null") != 0)
        return USB0_LIVE_UNKNOWN;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char copy[512], *tokens[64] = {0};
        int count = 0;
        char *tsave = NULL;
        char *p = trim(line);
        if (strncmp(p, "default", 7) != 0) continue;
        snprintf(copy, sizeof(copy), "%s", p);
        for (char *tok = strtok_r(copy, " \t", &tsave); tok && count < 64;
             tok = strtok_r(NULL, " \t", &tsave))
            tokens[count++] = tok;
        bool have_dev = false;
        for (int i = 0; i + 1 < count; i++) {
            if (strcmp(tokens[i], "dev") != 0) continue;
            have_dev = true;
            if (strcmp(tokens[i + 1], WIFI1B_USB0_IFACE) == 0)
                return USB0_LIVE_FOREIGN;
            break;
        }
        if (!have_dev) return USB0_LIVE_FOREIGN;
    }
    return USB0_LIVE_ABSENT;
}

#ifdef WIFI_TEST
/* Test/repro compatibility wrapper: exact chain-only parser with no hook
 * context.  It accepts an absent/empty owned chain and exactly one canonical
 * TCP ACCEPT for the expected port; anything else is false. */
static bool usb0_chain_owned(const char *out, int port)
{
    char copy[4096];
    int rules = 0, found = 0;
    bool have_chain = false;
    if (out == NULL) return false;
    snprintf(copy, sizeof(copy), "%s", out);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (p[0] == '\0') continue;
        if (strncmp(p, "-N WIFI1B_USB0_SSH", 18) == 0 && p[18] == '\0') {
            have_chain = true;
            continue;
        }
        if (strncmp(p, "-A WIFI1B_USB0_SSH", 18) == 0) {
            int value = 0;
            have_chain = true;
            if (!usb0_canonical_rule(p, &value) || rules > 0) return false;
            rules++;
            found = value;
            continue;
        }
        if (strstr(p, "WIFI1B_USB0_SSH") != NULL) return false;
    }
    if (rules == 0) return true;
    (void)have_chain;
    return port > 0 && found == port;
}
#endif

static usb0_live_state usb0_chain_probe(int expected_port, bool require_port,
                                        int *found_port, bool *chain_present,
                                        bool *hook_present,
                                        char *rule_spec, size_t rule_spec_cap)
{
    char out[32768], input[16384], tool[256];
    bool have_chain = false;
    int found = 0, rules = 0, hooks = 0;
    if (found_port != NULL) *found_port = 0;
    if (chain_present != NULL) *chain_present = false;
    if (hook_present != NULL) *hook_present = false;
    if (rule_spec != NULL && rule_spec_cap > 0) rule_spec[0] = '\0';
    fw_tool_ro(tool, sizeof(tool));
    if (sh_out(out, sizeof(out), "%s -S WIFI1B_USB0_SSH 2>/dev/null",
               tool) != 0) {
        if (sh_out(out, sizeof(out), "%s -S 2>/dev/null", tool) != 0)
            return USB0_LIVE_UNKNOWN;
    }
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (p[0] == '\0') continue;
        if (strncmp(p, "-N WIFI1B_USB0_SSH", 18) == 0 &&
            p[18] == '\0') {
            have_chain = true;
            continue;
        }
        if (strncmp(p, "-A WIFI1B_USB0_SSH", 18) == 0) {
            int port = 0;
            have_chain = true;
            if (!usb0_canonical_rule(p, &port) || rules > 0)
                return USB0_LIVE_FOREIGN;
            rules++;
            found = port;
            if (rule_spec != NULL && rule_spec_cap > 0) {
                const char *spec = p + 19; /* after "-A WIFI1B_USB0_SSH " */
                if (*spec == '\0' ||
                    snprintf(rule_spec, rule_spec_cap, "%s", spec) >=
                        (int)rule_spec_cap)
                    return USB0_LIVE_FOREIGN;
            }
            continue;
        }
        /* The unfiltered -S output also contains the exact INPUT hook; it is
         * validated separately below and is not chain junk. */
        if (usb0_hook_exact(p)) continue;
        if (strstr(p, "WIFI1B_USB0_SSH") != NULL)
            return USB0_LIVE_FOREIGN;
    }
    if (sh_out(input, sizeof(input), "%s -S INPUT 2>/dev/null", tool) != 0)
        return USB0_LIVE_UNKNOWN;
    save = NULL;
    for (char *line = strtok_r(input, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (strstr(p, "WIFI1B_USB0_SSH") == NULL) continue;
        if (!usb0_hook_exact(p) || hooks > 0)
            return USB0_LIVE_FOREIGN;
        hooks++;
    }
    if (!have_chain) {
        if (hooks != 0) return USB0_LIVE_FOREIGN;
        return USB0_LIVE_ABSENT;
    }
    if (rules == 0) found = 0;
    if (require_port && rules == 1 && found != expected_port)
        return USB0_LIVE_FOREIGN;
    if (chain_present != NULL) *chain_present = true;
    if (hook_present != NULL) *hook_present = hooks == 1;
    if (found_port != NULL) *found_port = found;
    return USB0_LIVE_OWNED;
}

static bool usb0_apply_owned_ex(int port, int allow_port)
{
    char helper[4096], rule[512], owner[256], tool[256], spec[512];
    int owner_port = 0, rule_port = 0, chain_port = 0;
    bool chain_present = false, hook_present = false, addr_present = false;
    if (port < 0 || port > 65535) return false;
    if (!usb0_helper_build(helper, sizeof(helper)) ||
        !usb0_rule_build(rule, sizeof(rule), port) ||
        !usb0_owner_build(owner, sizeof(owner), port))
        return false;
    bool helper_legacy = false;
    if (usb0_helper_state(NULL, &helper_legacy) != USB0_FILE_OWNED ||
        helper_legacy ||
        usb0_rule_state(&rule_port, NULL) != USB0_FILE_OWNED ||
        rule_port != port ||
        usb0_owner_state(&owner_port, NULL) != USB0_FILE_OWNED ||
        owner_port != port) {
        log_msg("usb0 apply: files are not the exact owned set");
        return false;
    }
    usb0_live_state addr = usb0_addr_probe(&addr_present);
    usb0_live_state def = usb0_default_probe();
    usb0_live_state chain = usb0_chain_probe(-1, false, &chain_port,
                                             &chain_present, &hook_present,
                                             spec, sizeof(spec));
    if (addr == USB0_LIVE_UNKNOWN || addr == USB0_LIVE_FOREIGN ||
        def == USB0_LIVE_UNKNOWN || def == USB0_LIVE_FOREIGN ||
        chain == USB0_LIVE_UNKNOWN || chain == USB0_LIVE_FOREIGN ||
        (chain_present && chain_port != 0 && chain_port != port &&
         chain_port != allow_port)) {
        log_msg("usb0 apply: foreign or unknown live state preserved");
        return false;
    }
    if (!usb0_present()) return true;
    fw_tool_ro(tool, sizeof(tool));
    if (!addr_present) {
        if (sh("ip -4 addr add " WIFI1B_USB0_IP "/24 dev %s 2>/dev/null",
               WIFI1B_USB0_IFACE) != 0 ||
            sh("ip link set dev %s up 2>/dev/null", WIFI1B_USB0_IFACE) != 0) {
            log_msg("usb0 apply: address add failed");
            return false;
        }
    }
    if (!chain_present) {
        if (sh("%s -N WIFI1B_USB0_SSH 2>/dev/null", tool) != 0) {
            log_msg("usb0 apply: chain create failed");
            return false;
        }
        chain_present = true;
        chain_port = 0;
    }
    if (!hook_present) {
        if (sh("%s -I INPUT -i %s -j WIFI1B_USB0_SSH 2>/dev/null",
               tool, WIFI1B_USB0_IFACE) != 0) {
            log_msg("usb0 apply: hook add failed");
            return false;
        }
    }
    if (chain_port != 0 && chain_port != port) {
        if (spec[0] == '\0' ||
            sh("%s -D WIFI1B_USB0_SSH %s 2>/dev/null", tool, spec) != 0) {
            log_msg("usb0 apply: old exact chain rule delete failed");
            return false;
        }
        chain_port = 0;
    }
    if (port > 0 && chain_port != port) {
        if (sh("%s -A WIFI1B_USB0_SSH -p tcp -m tcp --dport %d "
               "-j ACCEPT 2>/dev/null", tool, port) != 0) {
            log_msg("usb0 apply: exact chain rule add failed");
            return false;
        }
    }
    addr = usb0_addr_probe(&addr_present);
    def = usb0_default_probe();
    chain = usb0_chain_probe(port, true, &chain_port, &chain_present,
                             &hook_present, spec, sizeof(spec));
    if (addr != USB0_LIVE_OWNED || !addr_present ||
        def != USB0_LIVE_ABSENT ||
        chain != USB0_LIVE_OWNED || !chain_present || !hook_present ||
        (port == 0 ? chain_port != 0 : chain_port != port)) {
        log_msg("usb0 apply: post-apply readback failed");
        return false;
    }
    g_usb0_fw_port = port;
    log_msg("usb0: exact link state applied (ssh_port=%d)", port);
    return true;
}

static bool usb0_apply_owned(int port)
{
    return usb0_apply_owned_ex(port, -1);
}

static void usb0_cleanup_result(const char *status, const char *helper_state,
                                const char *rule_state,
                                const char *address_state,
                                const char *chain_state, const char *error)
{
    printf("WIFI1B_USB0_CLEANUP\nstatus=%s\n", status);
    printf("helper=%s\nrule=%s\naddress=%s\nchain=%s\n",
           helper_state, rule_state, address_state, chain_state);
    if (error && *error) printf("error=%s\n", error);
}

static bool usb0_path_absent(const char *path)
{
    struct stat st;
    if (lstat(path, &st) == 0) return false;
    return errno == ENOENT;
}

static const char *usb0_file_name(usb0_file_state state)
{
    switch (state) {
    case USB0_FILE_ABSENT: return "absent";
    case USB0_FILE_OWNED: return "owned";
    case USB0_FILE_FOREIGN: return "foreign";
    default: return "unknown";
    }
}

static const char *usb0_live_name(usb0_live_state state)
{
    switch (state) {
    case USB0_LIVE_ABSENT: return "absent";
    case USB0_LIVE_OWNED: return "owned";
    case USB0_LIVE_FOREIGN: return "foreign";
    default: return "unknown";
    }
}

static bool usb0_cleanup_owned(void)
{
    char helper[4096], rule[512], owner[256], tool[256], spec[512];
    struct stat hst, rst, ost;
    int owner_port = 0, rule_port = 0, chain_port = 0;
    bool chain_present = false, hook_present = false;
    bool addr_present = false;
    if (!usb0_helper_build(helper, sizeof(helper)) ||
        !usb0_rule_build(rule, sizeof(rule), 0) ||
        !usb0_owner_build(owner, sizeof(owner), 0))
        return false;
    usb0_file_state hs = usb0_helper_state(&hst, NULL);
    usb0_file_state rs = usb0_rule_state(&rule_port, &rst);
    usb0_file_state os = usb0_owner_state(&owner_port, &ost);
    usb0_live_state addr = usb0_addr_probe(&addr_present);
    usb0_live_state def = usb0_default_probe();
    usb0_live_state chain = usb0_chain_probe(-1, false, &chain_port,
                                             &chain_present, &hook_present,
                                             spec, sizeof(spec));
    const char *helper_name = usb0_file_name(hs);
    const char *rule_name = usb0_file_name(rs);
    const char *address_name = usb0_live_name(addr);
    const char *chain_name = usb0_live_name(chain);
    (void)helper;
    (void)rule;
    (void)owner;

    if (os == USB0_FILE_ABSENT) {
        if (hs == USB0_FILE_ABSENT && rs == USB0_FILE_ABSENT &&
            addr == USB0_LIVE_ABSENT && def == USB0_LIVE_ABSENT &&
            chain == USB0_LIVE_ABSENT) {
            usb0_cleanup_result("OK", "absent", "absent", "absent",
                                "absent", "");
            return true;
        }
        usb0_cleanup_result("FAIL", helper_name, rule_name, address_name,
                            chain_name, "unknown_usb0_ownership");
        return false;
    }
    if (os != USB0_FILE_OWNED ||
        hs == USB0_FILE_FOREIGN || hs == USB0_FILE_UNKNOWN ||
        rs == USB0_FILE_FOREIGN || rs == USB0_FILE_UNKNOWN ||
        addr == USB0_LIVE_FOREIGN || addr == USB0_LIVE_UNKNOWN ||
        def != USB0_LIVE_ABSENT ||
        chain == USB0_LIVE_FOREIGN || chain == USB0_LIVE_UNKNOWN ||
        (rs == USB0_FILE_OWNED && rule_port != owner_port) ||
        (chain == USB0_LIVE_OWNED && chain_port != 0 &&
         chain_port != owner_port)) {
        const char *why = "foreign_or_unknown_usb0_state";
        if (os == USB0_FILE_ABSENT || os == USB0_FILE_UNKNOWN)
            why = "owner_record_missing_or_invalid";
        else if (os == USB0_FILE_FOREIGN)
            why = "owner_record_content";
        else if (hs == USB0_FILE_FOREIGN || hs == USB0_FILE_UNKNOWN)
            why = "helper_unknown";
        else if (rs == USB0_FILE_FOREIGN || rs == USB0_FILE_UNKNOWN)
            why = "rule_unknown";
        else if (addr == USB0_LIVE_FOREIGN)
            why = "foreign_usb0_address";
        else if (addr == USB0_LIVE_UNKNOWN)
            why = "address_state_unknown";
        else if (def == USB0_LIVE_FOREIGN)
            why = "foreign_usb0_default";
        else if (def == USB0_LIVE_UNKNOWN)
            why = "default_state_unknown";
        else if (chain == USB0_LIVE_FOREIGN)
            why = "foreign_usb0_chain";
        else if (chain == USB0_LIVE_UNKNOWN)
            why = "chain_state_unknown";
        else if (rs == USB0_FILE_OWNED && rule_port != owner_port)
            why = "rule_port_mismatch";
        usb0_cleanup_result("FAIL", helper_name, rule_name, address_name,
                            chain_name, why);
        return false;
    }
    /* Prevalidation passed.  Only now may SSH be stopped or any live object
     * be mutated; an unknown ownership state above was side-effect free. */
    g_shutdown_cleanup = true;
    ssh_stop_cleanup();
    fw_tool_ro(tool, sizeof(tool));
    if (hook_present &&
        sh("%s -D INPUT -i %s -j WIFI1B_USB0_SSH 2>/dev/null",
           tool, WIFI1B_USB0_IFACE) != 0) {
        usb0_cleanup_result("FAIL", helper_name, rule_name, address_name,
                            chain_name, "chain_hook_delete");
        return false;
    }
    if (chain_present) {
        if (chain_port != 0) {
            if (spec[0] == '\0' ||
                sh("%s -D WIFI1B_USB0_SSH %s 2>/dev/null", tool, spec) != 0) {
                usb0_cleanup_result("FAIL", helper_name, rule_name,
                                    address_name, chain_name,
                                    "chain_rule_delete");
                return false;
            }
        }
        if (sh("%s -X WIFI1B_USB0_SSH 2>/dev/null", tool) != 0) {
            usb0_cleanup_result("FAIL", helper_name, rule_name, address_name,
                                chain_name, "chain_delete");
            return false;
        }
    }
    if (addr_present &&
        sh("ip -4 addr del " WIFI1B_USB0_IP "/24 dev %s 2>/dev/null",
           WIFI1B_USB0_IFACE) != 0) {
        usb0_cleanup_result("FAIL", helper_name, rule_name, address_name,
                            chain_name, "address_delete");
        return false;
    }
    /* Post-delete kernel readback must prove absence before any proof file is
     * unlinked.  A zero command exit by itself is not evidence. */
    bool post_addr_present = false;
    usb0_live_state post_addr = usb0_addr_probe(&post_addr_present);
    usb0_live_state post_def = usb0_default_probe();
    int post_chain_port = 0;
    bool post_chain_present = false, post_hook_present = false;
    usb0_live_state post_chain = usb0_chain_probe(
        -1, false, &post_chain_port, &post_chain_present, &post_hook_present,
        spec, sizeof(spec));
    if (post_addr != USB0_LIVE_ABSENT || post_addr_present ||
        post_def != USB0_LIVE_ABSENT ||
        post_chain != USB0_LIVE_ABSENT || post_chain_present ||
        post_hook_present) {
        usb0_cleanup_result("FAIL", helper_name, rule_name,
                            usb0_live_name(post_addr),
                            usb0_live_name(post_chain),
                            "post_delete_readback");
        return false;
    }
    if (hs == USB0_FILE_OWNED) {
        if (!w2_unlink_if_same(USB0_HELPER, &hst)) {
            usb0_cleanup_result("FAIL", "owned", rule_name, "absent",
                                "absent", "helper_delete");
            return false;
        }
    }
    if (rs == USB0_FILE_OWNED) {
        if (!w2_unlink_if_same(USB0_RULE_PATH, &rst)) {
            usb0_cleanup_result("FAIL", "removed", "owned", "absent",
                                "absent", "rule_delete");
            return false;
        }
    }
#ifndef WIFI_TEST
    if (rule_name != NULL && strcmp(rule_name, "absent") != 0)
        (void)sh("udevadm control --reload-rules 2>/dev/null");
#endif
    if (!w2_unlink_if_same(USB0_OWNER, &ost)) {
        usb0_cleanup_result("FAIL", "removed", "removed", "absent", "absent",
                            "owner_record_delete");
        return false;
    }
    if ((hs == USB0_FILE_OWNED && !usb0_path_absent(USB0_HELPER)) ||
        (rs == USB0_FILE_OWNED && !usb0_path_absent(USB0_RULE_PATH)) ||
        !usb0_path_absent(USB0_OWNER)) {
        usb0_cleanup_result("FAIL", "removed", "removed", "absent", "absent",
                            "post_delete_file_readback");
        return false;
    }
    usb0_cleanup_result("OK",
                        hs == USB0_FILE_OWNED ? "removed" : "absent",
                        rs == USB0_FILE_OWNED ? "removed" : "absent",
                        addr_present ? "removed" : "absent",
                        chain_present ? "removed" : "absent", "");
    return true;
}

static bool usb0_rule_write(int port)
{
    char helper[4096], rule[512], owner[256], spec[512], tool[256];
    struct stat hst, rst, ost;
    int owner_port = 0, rule_port = 0, chain_port = 0;
    bool helper_legacy = false, chain_present = false, hook_present = false;
    bool addr_present = false;
    if (port < 0 || port > 65535) port = 0;
    if (!usb0_helper_build(helper, sizeof(helper)) ||
        !usb0_rule_build(rule, sizeof(rule), port) ||
        !usb0_owner_build(owner, sizeof(owner), port))
        return false;
    usb0_file_state hs = usb0_helper_state(&hst, &helper_legacy);
    usb0_file_state rs = usb0_rule_state(&rule_port, &rst);
    usb0_file_state os = usb0_owner_state(&owner_port, &ost);
    usb0_live_state addr = usb0_addr_probe(&addr_present);
    usb0_live_state def = usb0_default_probe();
    usb0_live_state chain = usb0_chain_probe(-1, false, &chain_port,
                                             &chain_present, &hook_present,
                                             spec, sizeof(spec));
    if (hs == USB0_FILE_UNKNOWN || hs == USB0_FILE_FOREIGN ||
        rs == USB0_FILE_UNKNOWN || rs == USB0_FILE_FOREIGN ||
        os == USB0_FILE_UNKNOWN || os == USB0_FILE_FOREIGN ||
        addr == USB0_LIVE_UNKNOWN || addr == USB0_LIVE_FOREIGN ||
        def != USB0_LIVE_ABSENT ||
        chain == USB0_LIVE_UNKNOWN || chain == USB0_LIVE_FOREIGN) {
        log_msg("usb0: unknown or foreign ownership state preserved; no write");
        return false;
    }
    if (os == USB0_FILE_OWNED) {
        if (rs == USB0_FILE_OWNED && rule_port != owner_port) {
            log_msg("usb0: owner/rule port mismatch preserved");
            return false;
        }
        if (chain_present && chain_port != 0 && chain_port != owner_port &&
            !(rs == USB0_FILE_OWNED && chain_port == rule_port)) {
            log_msg("usb0: chain port does not match owner/rule; preserved");
            return false;
        }
    } else {
        bool fresh = hs == USB0_FILE_ABSENT && rs == USB0_FILE_ABSENT &&
                     addr == USB0_LIVE_ABSENT && !chain_present;
        bool legacy = hs == USB0_FILE_OWNED && rs == USB0_FILE_OWNED &&
                      addr != USB0_LIVE_FOREIGN && def == USB0_LIVE_ABSENT;
        if (!fresh && !legacy) {
            log_msg("usb0: partial/unknown ownership preserved; no write");
            return false;
        }
        if (legacy && chain_present && chain_port != 0 &&
            chain_port != rule_port) {
            log_msg("usb0: legacy chain port mismatch preserved");
            return false;
        }
    }
    /* A port change must not leave the old exact rule behind when the Pi is
     * currently absent.  The chain rule was prevalidated as our exact old
     * owner/legacy rule; delete only that exact rule, never a generic flush. */
    if (chain_present && chain_port != 0 && chain_port != port) {
        fw_tool_ro(tool, sizeof(tool));
        if (spec[0] == '\0' ||
            sh("%s -D WIFI1B_USB0_SSH %s 2>/dev/null", tool, spec) != 0) {
            log_msg("usb0: old exact chain rule delete failed; preserved");
            return false;
        }
        int after_port = 0;
        bool after_present = false, after_hook = false;
        if (usb0_chain_probe(-1, false, &after_port, &after_present,
                             &after_hook, spec, sizeof(spec)) !=
                USB0_LIVE_OWNED ||
            after_port != 0) {
            log_msg("usb0: old chain rule readback failed; preserved");
            return false;
        }
    }
    char rule_dir[256];
    snprintf(rule_dir, sizeof(rule_dir), "%s", USB0_RULE_PATH);
    char *rule_slash = strrchr(rule_dir, '/');
    if (rule_slash && rule_slash != rule_dir) *rule_slash = 0;
    else rule_dir[0] = 0;
    if (!w2_mkdir_checked(APP_DIR) || !w2_mkdir_checked(APP_DIR "/bin") ||
        (rule_dir[0] && !w2_mkdir_checked(rule_dir))) {
        log_msg("usb0: unsafe helper/rule parent directory");
        return false;
    }
    if (hs != USB0_FILE_OWNED || helper_legacy) {
        if (write_file_atomic(USB0_HELPER, helper, 0755) != 0 ||
            !usb0_file_equals(USB0_HELPER, helper)) {
            log_msg("usb0: helper write/readback failed");
            return false;
        }
    }
    if (rs != USB0_FILE_OWNED || rule_port != port) {
        if (write_file_atomic(USB0_RULE_PATH, rule, 0644) != 0 ||
            !usb0_file_equals(USB0_RULE_PATH, rule)) {
            log_msg("usb0: rule write/readback failed");
            return false;
        }
    }
    if (os != USB0_FILE_OWNED || owner_port != port) {
        if (write_file_atomic(USB0_OWNER, owner, 0600) != 0 ||
            !usb0_file_equals(USB0_OWNER, owner)) {
            log_msg("usb0: ownership record write/readback failed");
            return false;
        }
    }
#ifndef WIFI_TEST
    sh("chsmack -a _ %s 2>/dev/null", USB0_HELPER);
    sh("udevadm control --reload-rules 2>/dev/null");
#endif
    if (usb0_present()) {
        int old_port = os == USB0_FILE_OWNED ? owner_port :
                       (rs == USB0_FILE_OWNED ? rule_port : -1);
        if (!usb0_apply_owned_ex(port, old_port)) {
            log_msg("usb0: exact live apply failed; owner/rule retained for retry");
            return false;
        }
    }
    if (!usb0_present()) g_usb0_fw_port = 0;
    log_msg("usb0: Pi ECM link rule updated (ssh_port=%d)", port);
    return true;
}

/* Bring up the USB ECM link to the Pi when it is connected.  The head unit's
 * MICOM gates usbnet behind a "secure console" flag; clear it and preload the
 * drivers.  The runtime udev rule only runs when usb0 appears, so this is a
 * no-op when no Pi is attached. */
static void usbnet_bootstrap(void)
{
#ifndef WIFI_TEST
    int fd = open("/sys/module/micom/parameters/secure_console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "0\n", 2);
        close(fd);
    }
    (void)system("modprobe usbnet 2>/dev/null; modprobe cdc_ether 2>/dev/null");
    (void)usb0_rule_write(g_cfg.ssh_enabled ? g_cfg.ssh_port : 0);
#endif
}

int main(int argc, char **argv)
{
#ifdef WIFI_TEST
    if (test_command == NULL && getenv("WIFI1B_TEST_NET_STUB") != NULL)
        test_command = w2_test_net_stub;
    if (argc > 1 && !strcmp(argv[1], "--test-shutdown-exit"))
        return shutdown_cleanup_work() ? 0 : 1;
    if (argc > 1 && !strcmp(argv[1], "--test-sigterm-mainloop")) {
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
        (void)write_file_atomic(APP_DIR "/sigterm-ready", "ready\n", 0600);
        while (!g_stop) usleep(10000);
        return shutdown_sequence();
    }
#endif
    umask(0077);
    int port = DEFAULT_HTTP_PORT;
    bool dhcpd_mode = false;
    const char *one_shot = NULL;
    const char *one_mode = NULL;
    const char *one_ssh = NULL;
    const char *one_file = NULL;
    const char *one_port = NULL;
    const char *one_usb0_port = NULL;

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
        else if (!strcmp(argv[i], "--ssh-gate")) one_shot = "ssh-gate";
        else if (!strcmp(argv[i], "--usb0-rule") && i + 1 < argc) { one_shot = "usb0-rule"; one_usb0_port = argv[++i]; }
        else if (!strcmp(argv[i], "--usb0-apply") && i + 1 < argc) { one_shot = "usb0-apply"; one_usb0_port = argv[++i]; }
        else if (!strcmp(argv[i], "--usb0-cleanup")) one_shot = "usb0-cleanup";
        else if (!strcmp(argv[i], "--dump-logs")) one_shot = "dump-logs";
        else if (!strcmp(argv[i], "--dhcpd")) dhcpd_mode = true;
        else if (!strcmp(argv[i], "--dhcp-client")) one_shot = "dhcp-client";
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
    }

    if (one_shot && !strcmp(one_shot, "usb0-cleanup")) {
        /* Explicit one-shot cleanup: no config load, user creation, startup
         * bootstrap or SSH activation. */
        g_log_stderr = true;
        g_shutdown_cleanup = true;
        return usb0_cleanup_owned() ? 0 : 1;
    }
    if (one_shot && !strcmp(one_shot, "usb0-apply")) {
        /* udev helper one-shot: parse the port, require the exact owned files,
         * apply only usb0, and never load config or activate SSH. */
        char *end = NULL;
        long parsed = one_usb0_port ? strtol(one_usb0_port, &end, 10) : -1;
        if (one_usb0_port == NULL || end == one_usb0_port || *end != '\0' ||
            parsed < 0 || parsed > 65535)
            return 2;
        g_log_stderr = true;
        g_shutdown_cleanup = true;
        if (!usb0_present()) return 0;
        return usb0_apply_owned((int)parsed) ? 0 : 1;
    }

    if (dhcpd_mode) {
        mkdir(APP_DIR, 0755);
        return w2_dhcpd_run() ? 0 : 1;
    }

    /* One-shot CLI mode: no side effects, no daemon state, stderr logging.
     * Pure queries (--status, --choose-port) must not create directories,
     * delete state, ensure users or save config. */
    g_log_stderr = one_shot != NULL;
    bool pure_query = one_shot && (!strcmp(one_shot, "status") ||
                                   !strcmp(one_shot, "choose-port") ||
                                   !strcmp(one_shot, "ssh-gate") ||
                                   !strcmp(one_shot, "dump-logs"));
    g_pure_query = pure_query;
    /* Queries are read-only: they must not delete even stale .tmp files. */
    if (!pure_query) cleanup_stale_tmp();
    if (!pure_query) {
        mkdir(APP_DIR, 0755);
        mkdir(APP_DIR "/bin", 0755);
        mkdir(SSH_DIR, 0700);
        unlink(BT_STATE);
    }
    config_load();
    /* Shared wlan0 contract: our peer is the WiFi app. Before any worker. */
    radio_configure("wifi_daemon", "/rw_data/wifi/status.json");
    int instance_fd = -1;
    if (!one_shot) {
        instance_fd = radio_instance_lock(APP_DIR "/daemon.lock");
        if (instance_fd < 0) { fprintf(stderr, "Wifi1b daemon already running or lock unavailable\n"); return 1; }
        /* The Pi link rule/apply must run only after this instance owns the
         * daemon lock; a losing second instance must mutate nothing. */
        usbnet_bootstrap();
    }
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
            w2_txn_begin(false);
            bool ok = apply_mode_work(false);
            w2_txn_end();
            collect_observation(&g_observed);
            g_observed_ms = now_ms();
            char buf[8192];
            build_status(buf, sizeof(buf));
            fputs(buf, stdout);
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "ssh") && one_ssh) {
            if (radio_begin("wifi1b_daemon", g_cfg.wlan_on && g_cfg.mode != MODE_OFF) != 1) return 1;
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
        if (!strcmp(one_shot, "ssh-gate")) {
            printf("{\"ok\":true,\"wifi_active\":%s,\"usb0\":%s,"
                   "\"available\":%s}\n",
                   ssh_wifi_active() ? "true" : "false",
                   usb0_present() ? "true" : "false",
                   ssh_transport_available() ? "true" : "false");
            return 0;
        }
        if (!strcmp(one_shot, "usb0-rule") && one_usb0_port) {
            char *end = NULL;
            long parsed = strtol(one_usb0_port, &end, 10);
            if (end == one_usb0_port || *end != '\0' ||
                parsed < 0 || parsed > 65535)
                return 2;
            int p = (int)parsed;
            bool wrote = usb0_rule_write(p);
            bool ok = wrote && file_exists(USB0_HELPER) && file_exists(USB0_RULE_PATH);
            printf("{\"ok\":%s,\"helper\":\"%s\",\"rule\":\"%s\","
                   "\"port\":%d,\"usb0\":%s}\n",
                   ok ? "true" : "false", USB0_HELPER, USB0_RULE_PATH,
                   p, usb0_present() ? "true" : "false");
            return ok ? 0 : 1;
        }
        if (!strcmp(one_shot, "choose-port") && one_port) {
            int chosen = choose_ssh_port(atoi(one_port));
            printf("{\"ok\":%s,\"port\":%d}\n",
                   chosen ? "true" : "false", chosen);
            return chosen ? 0 : 1;
        }
        if (!strcmp(one_shot, "dhcp-client")) {
            char ip[64] = "";
            bool applies = getenv("WIFI1B_DHCP_TEST_NOAPPLY") == NULL;
            if (applies && radio_begin("wifi1b_daemon", true) != 1) {
                printf("{\"ok\":false,\"ip\":\"\"}\n");
                return 1;
            }
            bool ok = w2_dhcp_client("wlan0", ip, sizeof(ip));
            if (applies) radio_release();
            printf("{\"ok\":%s,\"ip\":\"%s\"}\n", ok ? "true" : "false", ip);
            return ok ? 0 : 1;
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
    log_msg("wifi1b_daemon started: mode=%s wlan_on=%d http=127.0.0.1:%d",
            mode_name(), g_cfg.wlan_on ? 1 : 0, port);

    /* Side-by-side safety: only re-apply an active mode when the user
     * selected it during this boot.  /run is cleared on reboot, so both
     * apps start idle and the user must explicitly choose one. */
    if (!file_exists(WIFI1B_INTENT) && (g_cfg.wlan_on || g_cfg.mode != MODE_OFF)) {
        log_msg("wifi1b startup: no this-boot intent; forcing WLAN off");
        g_cfg.wlan_on = false;
        g_cfg.mode = MODE_OFF;
        config_save();
    }
    /* Apply the CarPlay ownership choice before any mode work: stock handoff
     * must release wlan0 and restore the OEM manager; Wifi1b mode evicts it. */
    apply_carplay_control(false);
    apply_mode(false);

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
        dhcp_lease_tick();
        ssh_fw_tick();
        int64_t now = now_ms();
        if (now - g_last_status_ms > 2000) {
            write_status();
            g_last_status_ms = now;
        }
    }

    int shutdown_rc = shutdown_sequence();
    home_monitor_cleanup();
    close(srv);
    log_msg("wifi1b_daemon stopped");
    return shutdown_rc;
}
