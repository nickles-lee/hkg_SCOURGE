/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/*
 * tweaks_daemon - Ioniq 5 Gen5W night-mode fix daemon.
 *
 * Cluster amber is controlled by micom CAN 0x063F, not a head-unit CRTC.
 * Front/projection tint uses wrandr's brightness channel on CRTC 1.
 * Overrides have persistent ownership markers and are released explicitly.
 *
 * The daemon itself is static.  A small dynamically linked helper
 * (tweaks_persist) reads the vendor GENIVI persistence keys.
 *
 * It also serves the Reboot app: /reboot asks systemd for a clean reboot and,
 * if the daemon is still alive after reboot_wdt_ms, syncs and sends the
 * vendor micom SetETCCPUWDTReset hard-reset call.  Both paths run from the
 * same SMACK floor label `_` as the cluster CAN writes.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_RW_DIR "/rw_data/tweaks"
#define DEFAULT_PORT 8091
#define DEFAULT_REBOOT_COMMAND "/bin/systemctl"
#define DEFAULT_REBOOT_WDT_MS 20000
#define REBOOT_WDT_MAX_MS 120000
#define MAX_PATH 512
#define MAX_BUF 65536
#define DEFAULT_OFFLINE_STATUS "/update/offline_status"
#define DEFAULT_SESSION_BUS_FILE "/tmp/session_appmgr"
#define DEFAULT_DBUS_SEND "/usr/bin/dbus-send"
/* Battery's micom CAN helper: a copy of the vendor dbus-send installed under
 * the whitelisted basename CANService. The vendor micom drops CAN sends from
 * any other executable name (see battery docs/MICOM_WHITELIST.md). */
#define DEFAULT_CAN_SERVICE "/rw_data/battery/bin/micom-send"
#define DEFAULT_DST_ZONE "Europe/Amsterdam"
#define DEFAULT_DST_DIR "/rw_data/tweaks/zoneinfo"
#define DEFAULT_XDG_RUNTIME_DIR "/var/run/user/root"
#ifndef TWEAKS_BUILD_ID
#define TWEAKS_BUILD_ID "tweaks-development"
#endif
#define CONFIG_VERSION 2
/*
 * Vendor blue-light cluster signalling, verified in ApplicationManager:
 * MicomInterfaceM::setBLUStatus/setBLULevel send SetCANFixedMicomRecv on
 * the system bus (destination com.lge.car.micom, /Can, interface
 * com.lge.car.micom.can) with CAN
 * ID 0x063F:  status at bit 26, 2 bits (data 2 = on, 1 = off); warm level
 * at bit 12, 4 bits (1-9).  The instrument cluster ECU renders the amber
 * itself; it is not a head-unit CRTC.
 */
#define BLU_CAN_ID_HI 0x06
#define BLU_CAN_ID_LO 0x3F
#define BLU_STATUS_BIT 26
#define BLU_STATUS_SIZE 2
#define BLU_LEVEL_BIT 12
#define BLU_LEVEL_SIZE 4
#define BLU_STATUS_ON 2
#define BLU_STATUS_OFF 1
#define BLU_KEEPALIVE_S 30

static const char *GAMMA_TABLE[10] = {
    "128:128:128",
    "128:120:112",
    "128:119:110",
    "128:118:108",
    "128:117:106",
    "128:116:104",
    "128:115:102",
    "128:114:100",
    "128:113:98",
    "128:112:96",
};

/* Deep-red night mode: front display brightness and cluster CAN level used
 * while deep_red is on.  The cluster level field is 4 bits; the vendor UI
 * only offers 1-9, so 9 is the deepest documented setting. */
#define DEEP_RED_GAMMA "128:32:16"
#define DEEP_RED_LEVEL 9

typedef struct {
    int config_version;
    int cluster_fix;
    int infotainment_fix;
    int cluster_crtc;
    int main_crtc;
    int warmth_override;       /* 0 = follow vehicle */
    int force_filter_on;
    int deep_red;              /* 1 = deep-red night mode (front + cluster L9) */
    int projection_always;
    int projection_app_id;     /* 0 = learn */
    int poll_ms;
    int disclaimer_auto;
    int disclaimer_timeout_ms;
    int auto_dst;
    char dst_zone[MAX_PATH];
    int dst_check_ms;
    int ota_block;
    char rw_dir[MAX_PATH];
    char wrandr_path[MAX_PATH];
    char persist_helper[MAX_PATH];
    char tzdata_dir[MAX_PATH];
    char zones_file[MAX_PATH];
    char tzdata_version_file[MAX_PATH];
    char offline_status_path[MAX_PATH];
    char session_bus_file[MAX_PATH];
    char dbus_send_path[MAX_PATH];
    char can_service[MAX_PATH];
    char xdg_runtime_dir[MAX_PATH];
    char reboot_command[MAX_PATH];
    int reboot_wdt_ms;
} config_t;

typedef struct {
    int filter_on;
    int filter_known;
    int warmth_level;
    char gamma[32];
    int gamma_valid;
    char brightness[32];       /* APP_SETTING_DISP_BRIGHTNESS_R_G_B */
    int brightness_valid;
    char last_gamma[32];
    char last_brightness[32];
    int projection_active;
    char projection_source[32];
    int cluster_applied;
    int main_applied;
    int front_target;
    char last_cluster_gamma[32];
    char last_main_gamma[32];
    char message[256];
    char last_error[256];
    int identify_crtc;
    time_t identify_until;
    int night_forced;
    /* cluster blue-light force via micom CAN 0x063F */
    int blu_forced;
    int blu_status_sent;       /* last status data written: 1=off 2=on, 0=none */
    int blu_level_sent;        /* last level written, 0=none */
    time_t blu_last_send;
    /* auto-DST */
    int dst_active;
    char dst_state[32];
    char dst_error[256];
    int dst_raw;
    int dst_expected;
    time_t dst_checked_at;
    int64_t dst_checked_ms;
    time_t dst_written_at;
    int dst_last_value;
    /* OTA block */
    char ota_state[32];
    char ota_error[256];
    pid_t ota_child;
    int ota_child_mode;
    time_t ota_child_started;
    pid_t ota_client_pid;
    time_t ota_last_attempt;
    time_t ota_last_error_time;
    int ota_explicit_off;
    /* disclaimer */
    char tzdata_version[64];
    /* Reboot app: clean reboot first, micom watchdog reset as a last resort */
    int reboot_state;              /* 0 idle, 1 rebooting, 2 error */
    char reboot_message[128];
    char reboot_error[256];
    pid_t reboot_child;
    int64_t reboot_started_ms;
    int64_t reboot_wdt_at_ms;
    int reboot_wdt_sent;
    int reboot_wdt_ok;             /* -1 unknown, 0 failed, 1 sent */
} runtime_t;

static config_t cfg;
static runtime_t rt;
static char g_rw_override[MAX_PATH];
static volatile sig_atomic_t stop_flag;
static int listen_fd = -1;
/* dbus-monitor state.
 *
 * com.lge.avn.appmanager.info (carplay_play_enable_status,
 * android_auto_play_enable_status, front_drawing_application) is emitted
 * on the SESSION bus - AppNavi subscribes to it via
 * QDBusConnection::sessionBus().  The daemon watches both buses and merges
 * the events; the system-bus monitor is kept as a fallback for other
 * firmware variants. */
typedef struct {
    pid_t pid;
    int fd;
    char line[1024];
    size_t len;
    char member[128];
    int64_t retry_ms;
    int is_session;
    int alive;
} mon_t;

static mon_t g_mon[2] = {
    {.pid = -1, .fd = -1, .is_session = 0},
    {.pid = -1, .fd = -1, .is_session = 1},
};
/* Home/Star key watcher.  The steering-wheel key is a session-bus event,
 * not a QML key event; the Reboot overlay closes on a change of home_seq. */
static pid_t g_home_pid = -1;
static int g_home_fd = -1;
static int g_home_alive;
static int g_home_seq;
static char g_home_source[16] = "none";
static char g_home_member[128];
static char g_home_line[512];
static size_t g_home_len;
static int64_t g_home_retry_ms;
static char g_smack_expected[128];
static int g_monitor_enabled = 1;
static int g_config_migrated;
/* One-shot modes (--once/--status/--set) must not fork dbus-monitors: the
 * process exits right after the tick and would orphan the children. */
static int g_once_mode;

static int mon_any_alive(void)
{
    return g_mon[0].alive || g_mon[1].alive;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static time_t mono_s(void) { return (time_t)(mono_ms() / 1000); }

static int mon_carplay_play;
static int mon_android_play;
static int mon_front_app_id;
static time_t mon_front_app_time;
static int mon_carplay_app_id;   /* learned when carplay_play turns on */
static int mon_android_app_id;   /* learned when android_auto_play turns on */
static int64_t mon_carplay_started_ms, mon_android_started_ms;
/* Signal receipt counters: prove on the car which bus carries them. */
static int mon_sig_carplay;
static int mon_sig_android;
static int mon_sig_front;
static int mon_last_bus;         /* 0 none, 1 system, 2 session */
static time_t g_last_vendor_read;
static time_t g_now_override;

static time_t now_time(void)
{
    return g_now_override ? g_now_override : time(NULL);
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static void write_disclaimer_runtime(void);
static void check_dst(time_t now, int force);
static void check_ota(time_t now, int force);
static int restore_main(void);
static int front_owned(void);
static void write_status(void);
static void reboot_tick(void);

static void log_msg(const char *fmt, ...)
{
    char path[MAX_PATH];
    char stamp[64];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;
    FILE *f;

    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(path, sizeof(path), "%s/daemon.log", cfg.rw_dir);
    f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Opt-in timing trace (TWEAKS_TRACE=1) for the single-threaded loop. */
static void trace_msg(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("TWEAKS_TRACE"))
        return;
    va_start(ap, fmt);
    char text[512];
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_msg("trace: %s", text);
}

/*
 * Failure log.  Every entry lands in /rw_data/tweaks/failures.log, and while
 * a USB stick is mounted it is also appended to <stick>/tweaks_failures.log
 * so the stick can be collected after a test drive.  Entries written while
 * no stick was present are flushed to the stick when one appears.
 */
static int usb_mount_point(char *out, size_t size)
{
    FILE *f;
    struct mntent *entry;

    /* Test hook: force the "stick" location. */
    const char *env = getenv("TWEAKS_USB_DIR");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return 0;
    }

    const char *mounts = getenv("TWEAKS_MOUNTS_FILE");
    int relaxed = mounts && *mounts;
    if (!relaxed)
        mounts = "/proc/mounts";

    f = setmntent(mounts, "r");
    if (!f)
        return -1;
    while ((entry = getmntent(f)) != NULL) {
        const char *fs = entry->mnt_type, *mp = entry->mnt_dir;
        if (strcasecmp(fs, "vfat") && strcasecmp(fs, "exfat") &&
            strcasecmp(fs, "texfat") && strcasecmp(fs, "ntfs") &&
            strcasecmp(fs, "msdos"))
            continue;
        if (relaxed || !strncmp(mp, "/media/", 7) || !strncmp(mp, "/mnt/", 5)) {
            snprintf(out, size, "%s", mp);
            endmntent(f);
            return 0;
        }
    }
    endmntent(f);
    return -1;
}

/* Keep the local failure log bounded. */
static void trim_failure_log(const char *path)
{
    char buf[262144];
    long size;
    FILE *f;

    f = fopen(path, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size <= 200 * 1024) {
        fclose(f);
        return;
    }
    fseek(f, size - 100 * 1024, SEEK_SET);
    {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        char *nl;
        buf[n] = 0;
        fclose(f);
        nl = strchr(buf, '\n');
        f = fopen(path, "w");
        if (f) {
            fputs(nl ? nl + 1 : buf, f);
            fclose(f);
        }
    }
}

static void failure_entry(FILE *f, const char *stamp, const char *fmt, va_list ap)
{
    fprintf(f, "%s ", stamp);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
}

static void log_failure(const char *fmt, ...)
{
    char path[MAX_PATH], usb[512], stamp[64];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;
    FILE *f;

    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);

    snprintf(path, sizeof(path), "%s/failures.log", cfg.rw_dir);
    trim_failure_log(path);
    f = fopen(path, "a");
    if (f) {
        va_start(ap, fmt);
        failure_entry(f, stamp, fmt, ap);
        va_end(ap);
        fclose(f);
    }
    if (usb_mount_point(usb, sizeof(usb)) == 0) {
        trace_msg("failure usb target=%s", usb);
        snprintf(path, sizeof(path), "%s/tweaks_failures.log", usb);
        f = fopen(path, "a");
        if (f) {
            va_start(ap, fmt);
            failure_entry(f, stamp, fmt, ap);
            va_end(ap);
            fclose(f);
        }
    }
}

/*
 * Copy entries that were logged while no stick was present.  Lines up to
 * and including the stick log's current last line are skipped, so
 * remounting the same stick does not duplicate history.
 */
static void strip_eol(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static void flush_failures_to_usb(const char *usb)
{
    char src[MAX_PATH], dst[MAX_PATH], last[512] = "", line[512];
    FILE *sf, *df;

    snprintf(src, sizeof(src), "%s/failures.log", cfg.rw_dir);
    snprintf(dst, sizeof(dst), "%s/tweaks_failures.log", usb);

    sf = fopen(src, "r");
    if (!sf)
        return;
    df = fopen(dst, "r");
    if (df) {
        while (fgets(line, sizeof(line), df)) {
            strip_eol(line);
            if (line[0])
                snprintf(last, sizeof(last), "%s", line);
        }
        fclose(df);
        if (last[0]) {
            int found = 0;
            while (!found && fgets(line, sizeof(line), sf)) {
                strip_eol(line);
                if (!strcmp(line, last))
                    found = 1;
            }
            if (!found)
                rewind(sf);
        }
    }
    df = fopen(dst, "a");
    if (!df) {
        fclose(sf);
        return;
    }
    while (fgets(line, sizeof(line), sf)) {
        strip_eol(line);
        if (line[0])
            fprintf(df, "%s\n", line);
    }
    fclose(df);
    fclose(sf);
}

/* Detect stick (un)mounts and flush pending failure entries. */
static void usb_log_tick(time_t now)
{
    static time_t last_check;
    static char mounted[MAX_PATH];
    char usb[MAX_PATH];

    if (now - last_check < 5)
        return;
    last_check = now;
    if (usb_mount_point(usb, sizeof(usb)) == 0) {
        if (strcmp(mounted, usb) != 0) {
            snprintf(mounted, sizeof(mounted), "%s", usb);
            flush_failures_to_usb(usb);
            log_msg("failure log: USB stick mounted at %s", usb);
        }
    } else {
        mounted[0] = 0;
    }
}

static void set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt.message, sizeof(rt.message), fmt, ap);
    va_end(ap);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt.last_error, sizeof(rt.last_error), fmt, ap);
    va_end(ap);
}

static void clear_error(void)
{
    rt.last_error[0] = 0;
}

static int mkdir_p(const char *path)
{
    char tmp[MAX_PATH];
    size_t len;
    if (!path || !*path)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int write_atomic(const char *path, const char *data)
{
    char tmp[MAX_PATH];
    struct stat st;
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fchmod(fileno(f), stat(path, &st) == 0 ? (st.st_mode & 0777) : 0600);
    int failed = fputs(data, f) == EOF;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        failed = 1;
    if (fclose(f) != 0)
        failed = 1;
    if (failed) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
    return (int)n;
}

static int parse_bool(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "on") ||
        !strcasecmp(s, "yes") || !strcasecmp(s, "enable") || !strcasecmp(s, "enabled"))
        return 1;
    if (!strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "off") ||
        !strcasecmp(s, "no") || !strcasecmp(s, "disable") || !strcasecmp(s, "disabled"))
        return 0;
    return -1;
}

static int valid_gamma(const char *s)
{
    int r, g, b;
    char tail;
    if (!s || !*s)
        return 0;
    if (sscanf(s, "%d:%d:%d%c", &r, &g, &b, &tail) != 3)
        return 0;
    return r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255;
}

static int is_neutral_gamma(const char *s)
{
    return s && !strcmp(s, "128:128:128");
}

/* SMACK label of this process, e.g. "_" (floor) or "Privileged". The daemon
 * must run on the floor label to reach the micom service over the system bus;
 * exposing it makes that check possible from /health and status.json. */
static void current_smack(char *out, size_t size)
{
    if (read_file("/proc/self/attr/current", out, size) < 0 || !out[0])
        snprintf(out, size, "unknown");
    else if (!strcmp(out, "(null)"))
        snprintf(out, size, "unknown");
}

/* SMACK exec label of a helper the daemon forks (dbus-send).  An absent
 * label means the helper inherits this process's label. */
static void helper_smack(const char *path, char *out, size_t size)
{
    ssize_t n = getxattr(path, "security.SMACK64EXEC", out, size - 1);
    if (n <= 0) {
        snprintf(out, size, "inherit");
        return;
    }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' '))
        out[--n] = 0;
    if (!out[0])
        snprintf(out, size, "inherit");
}

/* The micom-send wrapper must support the fixed-field call. An older Battery
 * install shipped a raw-only wrapper: exec'ing it with "fixed" sends a
 * malformed raw frame, exits 0, and would make the daemon report success.
 * Treat such a helper as unavailable and fall back to dbus-send. */
static int helper_has_fixed(const char *path)
{
    char buf[4096];
    if (!path || !path[0])
        return 0;
    if (read_file(path, buf, sizeof(buf)) < 0)
        return 0;
    /* Require both the method string and a `fixed)` case label, so a
     * raw-only wrapper that merely mentions the method in a comment is not
     * mistaken for a fixed-capable helper. */
    return strstr(buf, "SetCANFixedMicomRecv") != NULL &&
           (strncmp(buf, "fixed)", 6) == 0 ||
            strstr(buf, "\nfixed)") != NULL);
}

static int can_service_ready(void)
{
    return cfg.can_service[0] &&
           access(cfg.can_service, X_OK) == 0 &&
           helper_has_fixed(cfg.can_service);
}

/* `/set` is reachable from any local process without authentication, and the
 * daemon execs can_service as root. Only accept the installer-managed helper
 * locations so a local process cannot point it at an arbitrary executable.
 * This is a lexical check: the two directories must remain root-owned and
 * non-writable (a symlink planted inside them is out of scope). */
static int trusted_can_service(const char *p)
{
    static const char *dirs[] = {
        "/rw_data/battery/bin/",
        "/rw_data/tweaks/bin/",
    };
    const char *base;
    size_t i;

    if (!p || !p[0] || strstr(p, ".."))
        return 0;
    base = strrchr(p, '/');
    if (!base || strcmp(base, "/micom-send") != 0)
        return 0;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        size_t n = strlen(dirs[i]);
        if (!strncmp(p, dirs[i], n) && p[n] && !strchr(p + n, '/'))
            return 1;
    }
    return 0;
}

static void load_expected_smack(void)
{
    char path[MAX_PATH];
    g_smack_expected[0] = 0;
    snprintf(path, sizeof(path), "%s/smack_expected", cfg.rw_dir);
    if (read_file(path, g_smack_expected, sizeof(g_smack_expected)) < 0) {
        g_smack_expected[0] = 0;
    } else if (!strcmp(g_smack_expected, "(null)")) {
        g_smack_expected[0] = 0;
    }
}

static int smack_ok(const char *actual)
{
    return !g_smack_expected[0] || !strcmp(actual, g_smack_expected);
}

/* Update/upgrade processes: a reboot in the middle of a flash update can
 * brick the unit.  Scan /proc on demand only (the static name strings avoid
 * false matches against this daemon's own command line). */
static int process_running(const char *name)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int found = 0;
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        char path[MAX_PATH], comm[64];
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end || pid <= 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        if (read_file(path, comm, sizeof(comm)) > 0 && !strcmp(comm, name)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int update_running(void)
{
    return process_running("AppUpgrade") || process_running("AppScomoDM");
}

/*
 * Effective night mode.
 *
 * When projection (CarPlay / Android Auto) takes over, the vendor disables the
 * blue-light filter globally, so the persistent filter state alone cannot be
 * trusted: following it would make this daemon clear the amber tint itself.
 * While projection is active we therefore keep night mode on regardless of
 * the filter flag.
 */
static int night_mode_on(void)
{
    return rt.filter_on || rt.projection_active;
}

/*
 * The warmth to enforce while night mode is on.  The vendor tints the front
 * display through wrandr's BRIGHTNESS channel (setFrontBrightnessRGBValue
 * sends "wrandr --crtc=1 --brightness=<r:g:b>"), so prefer the vehicle's
 * BRIGHTNESS_R_G_B value, then the gamma key, then the table default.
 */
static void effective_warmth(char *out, size_t size)
{
    if (cfg.deep_red) {
        /* Deep-red night mode overrides the vehicle gamma on the front
         * display; callers only apply it while night mode is effective. */
        snprintf(out, size, "%s", DEEP_RED_GAMMA);
        return;
    }
    if (valid_gamma(rt.brightness) && !is_neutral_gamma(rt.brightness))
        snprintf(out, size, "%s", rt.brightness);
    else if (valid_gamma(rt.gamma) && !is_neutral_gamma(rt.gamma))
        snprintf(out, size, "%s", rt.gamma);
    else if (valid_gamma(rt.last_brightness) && !is_neutral_gamma(rt.last_brightness))
        snprintf(out, size, "%s", rt.last_brightness);
    else if (valid_gamma(rt.last_gamma) && !is_neutral_gamma(rt.last_gamma))
        snprintf(out, size, "%s", rt.last_gamma);
    else
        snprintf(out, size, "%s", GAMMA_TABLE[5]);
}

static void load_config(void)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    int had_version = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cluster_fix = 1;
    cfg.infotainment_fix = 1;
    /* wrandr numbers CRTCs from 1: CRTC 1 is the front display (HDMI1,
     * 1920x720) - the vendor's setFrontBrightnessRGBValue targets it. */
    cfg.cluster_crtc = 2;
    cfg.main_crtc = 1;
    cfg.warmth_override = 0;
    cfg.force_filter_on = 0;
    cfg.deep_red = 0;
    cfg.projection_always = 0;
    cfg.projection_app_id = 0;
    cfg.poll_ms = 1000;
    cfg.disclaimer_auto = 1;
    cfg.disclaimer_timeout_ms = 2000;
    cfg.auto_dst = 1;
    snprintf(cfg.dst_zone, sizeof(cfg.dst_zone), "%s", DEFAULT_DST_ZONE);
    cfg.dst_check_ms = 60000;
    cfg.ota_block = 0;
    snprintf(cfg.rw_dir, sizeof(cfg.rw_dir), "%s",
             g_rw_override[0] ? g_rw_override : DEFAULT_RW_DIR);
    snprintf(cfg.wrandr_path, sizeof(cfg.wrandr_path), "/usr/bin/wrandr.real");
    snprintf(cfg.persist_helper, sizeof(cfg.persist_helper), "%s/tweaks_persist", cfg.rw_dir);
    snprintf(cfg.tzdata_dir, sizeof(cfg.tzdata_dir), "%s/zoneinfo", cfg.rw_dir);
    snprintf(cfg.zones_file, sizeof(cfg.zones_file), "%s/zones.txt", cfg.rw_dir);
    snprintf(cfg.tzdata_version_file, sizeof(cfg.tzdata_version_file),
             "%s/tzdata_version", cfg.rw_dir);
    snprintf(cfg.offline_status_path, sizeof(cfg.offline_status_path), "%s",
             DEFAULT_OFFLINE_STATUS);
    snprintf(cfg.session_bus_file, sizeof(cfg.session_bus_file), "%s",
             DEFAULT_SESSION_BUS_FILE);
    snprintf(cfg.dbus_send_path, sizeof(cfg.dbus_send_path), "%s", DEFAULT_DBUS_SEND);
    snprintf(cfg.can_service, sizeof(cfg.can_service), "%s", DEFAULT_CAN_SERVICE);
    snprintf(cfg.xdg_runtime_dir, sizeof(cfg.xdg_runtime_dir), "%s",
             DEFAULT_XDG_RUNTIME_DIR);
    snprintf(cfg.reboot_command, sizeof(cfg.reboot_command), "%s",
             DEFAULT_REBOOT_COMMAND);
    cfg.reboot_wdt_ms = DEFAULT_REBOOT_WDT_MS;

    snprintf(path, sizeof(path), "%s/config.conf", cfg.rw_dir);
    f = fopen(path, "r");
    if (!f) {
        cfg.config_version = CONFIG_VERSION;
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq, *k, *v;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        k = line;
        v = eq + 1;
        if (!strcmp(k, "config_version")) {
            cfg.config_version = atoi(v);
            had_version = 1;
        }
        else if (!strcmp(k, "cluster_fix"))
            cfg.cluster_fix = parse_bool(v) == 1;
        else if (!strcmp(k, "infotainment_fix"))
            cfg.infotainment_fix = parse_bool(v) == 1;
        else if (!strcmp(k, "cluster_crtc"))
            cfg.cluster_crtc = atoi(v);
        else if (!strcmp(k, "main_crtc"))
            cfg.main_crtc = atoi(v);
        else if (!strcmp(k, "warmth_override"))
            cfg.warmth_override = atoi(v);
        else if (!strcmp(k, "force_filter_on"))
            cfg.force_filter_on = parse_bool(v) == 1;
        else if (!strcmp(k, "deep_red"))
            cfg.deep_red = parse_bool(v) == 1;
        else if (!strcmp(k, "projection_always"))
            cfg.projection_always = parse_bool(v) == 1;
        else if (!strcmp(k, "projection_app_id"))
            cfg.projection_app_id = atoi(v);
        else if (!strcmp(k, "poll_ms"))
            cfg.poll_ms = atoi(v);
        else if (!strcmp(k, "disclaimer_auto"))
            cfg.disclaimer_auto = parse_bool(v) == 1;
        else if (!strcmp(k, "disclaimer_timeout_ms"))
            cfg.disclaimer_timeout_ms = atoi(v);
        else if (!strcmp(k, "auto_dst"))
            cfg.auto_dst = parse_bool(v) == 1;
        else if (!strcmp(k, "dst_zone"))
            snprintf(cfg.dst_zone, sizeof(cfg.dst_zone), "%s", v);
        else if (!strcmp(k, "dst_check_ms"))
            cfg.dst_check_ms = atoi(v);
        else if (!strcmp(k, "ota_block"))
            cfg.ota_block = parse_bool(v) == 1;
        else if (!strcmp(k, "wrandr_path"))
            snprintf(cfg.wrandr_path, sizeof(cfg.wrandr_path), "%s", v);
        else if (!strcmp(k, "persist_helper"))
            snprintf(cfg.persist_helper, sizeof(cfg.persist_helper), "%s", v);
        else if (!strcmp(k, "tzdata_dir"))
            snprintf(cfg.tzdata_dir, sizeof(cfg.tzdata_dir), "%s", v);
        else if (!strcmp(k, "zones_file"))
            snprintf(cfg.zones_file, sizeof(cfg.zones_file), "%s", v);
        else if (!strcmp(k, "tzdata_version_file"))
            snprintf(cfg.tzdata_version_file, sizeof(cfg.tzdata_version_file), "%s", v);
        else if (!strcmp(k, "offline_status_path"))
            snprintf(cfg.offline_status_path, sizeof(cfg.offline_status_path), "%s", v);
        else if (!strcmp(k, "session_bus_file"))
            snprintf(cfg.session_bus_file, sizeof(cfg.session_bus_file), "%s", v);
        else if (!strcmp(k, "dbus_send_path"))
            snprintf(cfg.dbus_send_path, sizeof(cfg.dbus_send_path), "%s", v);
        else if (!strcmp(k, "can_service"))
            snprintf(cfg.can_service, sizeof(cfg.can_service), "%s", v);
        else if (!strcmp(k, "xdg_runtime_dir"))
            snprintf(cfg.xdg_runtime_dir, sizeof(cfg.xdg_runtime_dir), "%s", v);
        else if (!strcmp(k, "reboot_command"))
            snprintf(cfg.reboot_command, sizeof(cfg.reboot_command), "%s", v);
        else if (!strcmp(k, "reboot_wdt_ms"))
            cfg.reboot_wdt_ms = atoi(v);
    }
    fclose(f);
    /* Version 1 shipped these zero-based defaults. Only migrate the known
     * legacy pair; retain valid, deliberately customized output numbers. */
    if (cfg.config_version < CONFIG_VERSION &&
        cfg.main_crtc == 0 && cfg.cluster_crtc == 1) {
        cfg.main_crtc = 1;
        cfg.cluster_crtc = 2;
    }
    if (cfg.main_crtc < 1)
        cfg.main_crtc = 1;
    if (!had_version || cfg.config_version < CONFIG_VERSION)
        g_config_migrated = 1;
    cfg.config_version = CONFIG_VERSION;
    if (cfg.poll_ms < 200)
        cfg.poll_ms = 200;
    if (cfg.poll_ms > 5000)
        cfg.poll_ms = 5000;
    cfg.disclaimer_timeout_ms = clamp_int(cfg.disclaimer_timeout_ms, 1000, 10000);
    if (cfg.dst_check_ms < 10000)
        cfg.dst_check_ms = 10000;
    if (cfg.dst_check_ms > 3600000)
        cfg.dst_check_ms = 3600000;
    if (!cfg.dst_zone[0])
        snprintf(cfg.dst_zone, sizeof(cfg.dst_zone), "%s", DEFAULT_DST_ZONE);
    cfg.reboot_wdt_ms = clamp_int(cfg.reboot_wdt_ms, 0, REBOOT_WDT_MAX_MS);
    if (!cfg.reboot_command[0])
        snprintf(cfg.reboot_command, sizeof(cfg.reboot_command), "%s",
                 DEFAULT_REBOOT_COMMAND);
}

static void save_config(void)
{
    char path[MAX_PATH], data[4096];
    snprintf(path, sizeof(path), "%s/config.conf", cfg.rw_dir);
    snprintf(data, sizeof(data),
             "# Tweaks daemon configuration\n"
             "config_version=2\n"
             "cluster_fix=%d\n"
             "infotainment_fix=%d\n"
             "cluster_crtc=%d\n"
             "main_crtc=%d\n"
             "warmth_override=%d\n"
             "force_filter_on=%d\n"
             "deep_red=%d\n"
             "projection_always=%d\n"
             "projection_app_id=%d\n"
             "poll_ms=%d\n"
             "disclaimer_auto=%d\n"
             "disclaimer_timeout_ms=%d\n"
             "auto_dst=%d\n"
             "dst_zone=%s\n"
             "dst_check_ms=%d\n"
             "ota_block=%d\n"
             "wrandr_path=%s\n"
             "persist_helper=%s\n"
             "tzdata_dir=%s\n"
             "zones_file=%s\n"
             "tzdata_version_file=%s\n"
             "offline_status_path=%s\n"
             "session_bus_file=%s\n"
             "dbus_send_path=%s\n"
             "can_service=%s\n"
             "xdg_runtime_dir=%s\n"
             "reboot_command=%s\n"
             "reboot_wdt_ms=%d\n",
             cfg.cluster_fix, cfg.infotainment_fix, cfg.cluster_crtc, cfg.main_crtc,
             cfg.warmth_override, cfg.force_filter_on, cfg.deep_red,
             cfg.projection_always,
             cfg.projection_app_id, cfg.poll_ms, cfg.disclaimer_auto,
             cfg.disclaimer_timeout_ms, cfg.auto_dst, cfg.dst_zone, cfg.dst_check_ms,
             cfg.ota_block, cfg.wrandr_path, cfg.persist_helper, cfg.tzdata_dir,
             cfg.zones_file, cfg.tzdata_version_file, cfg.offline_status_path,
             cfg.session_bus_file, cfg.dbus_send_path, cfg.can_service,
             cfg.xdg_runtime_dir,
             cfg.reboot_command, cfg.reboot_wdt_ms);
    write_atomic(path, data);
}

/* Drain output through EOF, and reap exactly once. Child exit is not EOF:
 * the final stdout block may still be in the pipe after waitpid succeeds. */
static int capture_helper(char *const argv[], char *out, size_t out_size,
                          int timeout_ms)
{
    int pipefd[2];
    pid_t pid;
    size_t used = 0;
    int status = 0, reaped = 0, eof = 0, overflow = 0, timed_out = 0;
    int64_t deadline = mono_ms() + timeout_ms;

    out[0] = 0;
    if (access(cfg.persist_helper, X_OK) < 0)
        return -1;
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(cfg.persist_helper, argv);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    while (!reaped || !eof) {
        char buf[1024];
        ssize_t n;
        while (!eof && (n = read(pipefd[0], buf, sizeof(buf))) >= 0) {
            if (n == 0) {
                eof = 1;
                break;
            }
            size_t room = out_size - used - 1;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(out + used, buf, take);
            used += take;
            if (take < (size_t)n)
                overflow = 1;
        }
        if (!reaped) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid)
                reaped = 1;
            else if (r < 0 && errno != EINTR) {
                status = -1;
                reaped = 1;
            }
        }
        if (reaped && eof)
            break;
        if (mono_ms() >= deadline) {
            timed_out = 1;
            break;
        }
        struct pollfd pfd = {.fd = eof ? -1 : pipefd[0],
                             .events = POLLIN | POLLHUP};
        poll(&pfd, 1, 20);
    }
    close(pipefd[0]);
    if (!reaped) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    out[used] = 0;
    return !timed_out && !overflow && status >= 0 &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0 ? (int)used : -1;
}

static int run_persist_helper(char *out, size_t out_size)
{
    char *args[] = {cfg.persist_helper, "read", NULL};
    trace_msg("persist read start");
    int n = capture_helper(args, out, out_size, 2500);
    trace_msg("persist read done n=%d", n);
    return n > 0 ? n : -1;
}

static int line_value(const char *out, const char *key, char *value, size_t value_size)
{
    size_t klen = strlen(key);
    const char *p = out;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            size_t vlen = len - klen - 1;
            if (vlen >= value_size)
                vlen = value_size - 1;
            memcpy(value, p + klen + 1, vlen);
            value[vlen] = 0;
            return 1;
        }
        p = e ? e + 1 : NULL;
    }
    return 0;
}

static void load_tzdata_version(void)
{
    char path[MAX_PATH], value[64];
    snprintf(path, sizeof(path), "%s", cfg.tzdata_version_file);
    if (read_file(path, value, sizeof(value)) > 0 && value[0]) {
        char *nl = strchr(value, '\n');
        if (nl)
            *nl = 0;
        snprintf(rt.tzdata_version, sizeof(rt.tzdata_version), "%s", value);
    } else {
        snprintf(rt.tzdata_version, sizeof(rt.tzdata_version), "unknown");
    }
}

static void set_ota_owned(int on)
{
    char path[MAX_PATH], value[8];
    snprintf(path, sizeof(path), "%s/ota_block_owned", cfg.rw_dir);
    if (on) {
        if (read_file(path, value, sizeof(value)) > 0 && !strcmp(value, "1"))
            return;
        write_atomic(path, "1");
    } else {
        unlink(path);
    }
}

static int ota_owned(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/ota_block_owned", cfg.rw_dir);
    return access(path, F_OK) == 0;
}

static void write_disclaimer_runtime(void)
{
    char path[MAX_PATH], value[32];
    snprintf(path, sizeof(path), "%s/disclaimer_auto", cfg.rw_dir);
    write_atomic(path, cfg.disclaimer_auto ? "1" : "0");
    snprintf(path, sizeof(path), "%s/disclaimer_timeout_ms", cfg.rw_dir);
    snprintf(value, sizeof(value), "%d", cfg.disclaimer_timeout_ms);
    write_atomic(path, value);
}

static int zone_is_safe(const char *zone)
{
    if (!zone || !*zone || zone[0] == '/' || strstr(zone, ".."))
        return 0;
    for (const char *p = zone; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+'))
            return 0;
    }
    return 1;
}

/*
 * Read the vehicle DST flag.
 *
 * TimeManager treats APP_SETTING_CLOCK_DAYLIGHTSAVETIME as the actual DST
 * flag: "1" = DST on, "0" = DST off.  The stock Setup screen can also leave
 * "2" (Custom mode) there, which TimeManager rejects as invalid.  Missing,
 * empty, or "2" values must not stop auto-DST from repairing the key.
 *
 * Returns 0 for a usable 0/1 value, 1 for a missing/unusable value (which the
 * caller should replace), and -1 when persistence itself is unavailable.
 */
static int read_dst_key(int *state, int *raw)
{
    char out[4096], val[64];

    *state = -1;
    *raw = -1;
    if (run_persist_helper(out, sizeof(out)) <= 0) {
        snprintf(rt.dst_error, sizeof(rt.dst_error), "vehicle persistence unavailable");
        return -1;
    }
    if (!line_value(out, "APP_SETTING_CLOCK_DAYLIGHTSAVETIME", val, sizeof(val)) ||
        !val[0]) {
        log_msg("auto-DST: vehicle DST key is missing, will write the expected value");
        return 1;
    }

    {
        static int logged_app;
        if (!logged_app) {
            const char *a = strstr(out, "pcl read app=");
            log_msg("auto-DST: persistence helper registered as: %s",
                    a ? a : "(unknown)");
            logged_app = 1;
        }
    }

    {
        int v = atoi(val);
        *raw = v;
        if (v == 0 || v == 1) {
            *state = v;
            return 0;
        }
        if (v == 2)
            log_msg("auto-DST: vehicle DST mode is Custom (2), replacing with the expected flag");
        else
            log_msg("auto-DST: invalid vehicle DST value '%s', replacing", val);
    }
    return 1;
}

/*
 * Run the persistence helper in write mode, capturing stdout+stderr so the
 * PCL error code and the app name it registered with end up in daemon.log.
 * The helper retries the write under each candidate app name; on total
 * failure it prints "write_failed rc=<code> app=<name>".
 */
static int run_persist_write(const char *key, const char *value,
                             char *out, size_t out_size)
{
    char *args[] = {cfg.persist_helper, "write", (char *)key,
                    (char *)value, NULL};
    return capture_helper(args, out, out_size, 15000) > 0 ? 0 : -1;
}

static void collapse_newlines(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
}

/* Remember a failed DST write across daemon restarts. */
static int dst_failed_marker(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/dst_write_failed", cfg.rw_dir);
    return access(path, F_OK) == 0;
}

static void set_dst_failed_marker(int on)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/dst_write_failed", cfg.rw_dir);
    if (on)
        write_atomic(path, "1");
    else
        unlink(path);
}

static void check_dst(time_t now, int force)
{
    int expected, current = -1;
    char tzpath[MAX_PATH + 2];
    struct tm tmv;

    trace_msg("check_dst force=%d", force);

    if (!cfg.auto_dst) {
        snprintf(rt.dst_state, sizeof(rt.dst_state), "disabled");
        rt.dst_active = 0;
        rt.dst_error[0] = 0;
        rt.dst_checked_at = now;
        return;
    }
    if (!force && rt.dst_checked_ms != 0 &&
        mono_ms() - rt.dst_checked_ms < cfg.dst_check_ms)
        return;

    rt.dst_checked_at = now;
    rt.dst_checked_ms = mono_ms();
    if (!zone_is_safe(cfg.dst_zone)) {
        snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
        snprintf(rt.dst_error, sizeof(rt.dst_error), "invalid DST zone: %s", cfg.dst_zone);
        return;
    }
    snprintf(tzpath, sizeof(tzpath), "%s/%s", cfg.tzdata_dir, cfg.dst_zone);
    if (access(tzpath, R_OK) < 0) {
        snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
        snprintf(rt.dst_error, sizeof(rt.dst_error), "zone file missing: %s", tzpath);
        return;
    }

    {
        char tzspec[MAX_PATH + 4];
        char old_tz[MAX_PATH + 8];
        const char *saved = getenv("TZ");
        int had_tz = 0, lok;
        if (saved && *saved) {
            snprintf(old_tz, sizeof(old_tz), "%s", saved);
            had_tz = 1;
        }
        snprintf(tzspec, sizeof(tzspec), ":%s", tzpath);
        setenv("TZ", tzspec, 1);
        tzset();
        lok = localtime_r(&now, &tmv) != NULL;
        /* Restore the process timezone so log stamps keep using it. */
        if (had_tz)
            setenv("TZ", old_tz, 1);
        else
            unsetenv("TZ");
        tzset();
        if (!lok) {
            snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
            snprintf(rt.dst_error, sizeof(rt.dst_error), "localtime_r failed for %s", cfg.dst_zone);
            return;
        }
    }
    expected = tmv.tm_isdst > 0;
    rt.dst_expected = expected;
    rt.dst_error[0] = 0;

    /* A read failure is not fatal: the write below may still repair the key. */
    if (read_dst_key(&current, &rt.dst_raw) < 0) {
        snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
        return;
    }

    if (current != expected) {
        char wout[4096];
        if (run_persist_write("APP_SETTING_CLOCK_DAYLIGHTSAVETIME",
                              expected ? "1" : "0", wout, sizeof(wout)) == 0) {
            int verify = -1, verify_raw = -1;
            int verify_rc = 1, verify_ok = 0;
            char sverify[64] = "";
            int have_sverify = line_value(wout, "verify", sverify, sizeof(sverify));

            collapse_newlines(wout);
            log_msg("auto-DST: write ok: %s", wout);

            /*
             * Primary confirmation: the helper read the key back in the
             * same PCL registration right after writing.  A value read
             * through a different registration can be stale, so treat the
             * same-session value as authoritative.
             */
            if (have_sverify && strcmp(sverify, "(unreadable)") != 0) {
                int v = atoi(sverify);
                verify = (v == 0 || v == 1) ? v : -1;
                verify_raw = v;
                rt.dst_raw = v;
                verify_ok = (verify == expected);
                verify_rc = 0;
            }

            if (!verify_ok) {
                /* The same-session read could not confirm: cross-check with
                 * a fresh read, allowing propagation time. */
                for (int attempt = 0; attempt < 3; attempt++) {
                    int rv = read_dst_key(&verify, &verify_raw);
                    rt.dst_raw = verify_raw;
                    if (rv == 0) {
                        verify_rc = 0;
                        if (verify == expected)
                            verify_ok = 1;
                        break;
                    }
                    if (rv < 0)
                        break;
                    usleep(200000);
                }
            } else {
                /* Confirmed in-session.  One fresh read as a cross-check:
                 * if a different registration still sees the old value,
                 * the persistence layer is not coherent - log it. */
                int verify2 = -1, verify_raw2 = -1;
                if (read_dst_key(&verify2, &verify_raw2) == 0 &&
                    verify2 != expected) {
                    log_failure("auto-DST: cache discrepancy: same-session %s "
                                "but fresh read %d (expected %d)",
                                sverify, verify2, expected);
                }
            }
            if (verify_ok) {
                rt.dst_written_at = now;
                rt.dst_last_value = expected;
                rt.dst_active = expected;
                rt.dst_error[0] = 0;
                snprintf(rt.dst_state, sizeof(rt.dst_state),
                         expected ? "active" : "inactive");
                log_msg("auto-DST: wrote APP_SETTING_CLOCK_DAYLIGHTSAVETIME=%d zone=%s raw=%d",
                        expected, cfg.dst_zone, rt.dst_raw);
                if (dst_failed_marker()) {
                    log_failure("auto-DST: write ok after failures: %s", wout);
                    set_dst_failed_marker(0);
                }
            } else if (verify_rc == 0) {
                snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
                snprintf(rt.dst_error, sizeof(rt.dst_error),
                         "DST write did not persist (vehicle value %d, wrote %d)",
                         verify, expected);
                log_failure("auto-DST: write did not persist (vehicle value %d, wrote %d): %s",
                            verify, expected, wout);
                set_dst_failed_marker(1);
            } else if (have_sverify) {
                snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
                snprintf(rt.dst_error, sizeof(rt.dst_error),
                         "DST write did not persist (read-back %s, wrote %d)",
                         sverify, expected);
                log_failure("auto-DST: write did not persist (read-back %s, wrote %d): %s",
                            sverify, expected, wout);
                set_dst_failed_marker(1);
            } else {
                snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
                snprintf(rt.dst_error, sizeof(rt.dst_error),
                         "DST write did not persist (vehicle value %d, wrote %d)",
                         verify_raw, expected);
                log_failure("auto-DST: write did not persist (vehicle value %d, wrote %d): %s",
                            verify_raw, expected, wout);
                set_dst_failed_marker(1);
            }
        } else {
            int rc = 0;
            char *p = strstr(wout, "write_failed rc=");

            if (p)
                rc = atoi(p + 16);
            collapse_newlines(wout);
            log_msg("auto-DST: write FAILED: %s", wout);
            log_failure("auto-DST: write FAILED (expected %d): %s", expected, wout);
            set_dst_failed_marker(1);
            snprintf(rt.dst_state, sizeof(rt.dst_state), "error");
            if (rc)
                snprintf(rt.dst_error, sizeof(rt.dst_error),
                         "PCL write rejected (rc=%d)", rc);
            else
                snprintf(rt.dst_error, sizeof(rt.dst_error), "PCL write failed");
        }
    } else {
        rt.dst_active = current;
        rt.dst_last_value = current;
        snprintf(rt.dst_state, sizeof(rt.dst_state), current ? "active" : "inactive");
    }
}

static pid_t find_ota_client(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    pid_t found = 0;
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        char path[MAX_PATH], comm[64];
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end || pid <= 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        if (read_file(path, comm, sizeof(comm)) > 0 && !strcmp(comm, "AppDMClient")) {
            found = (pid_t)pid;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Scanning /proc is expensive; do it at most every 5 seconds. */
static pid_t ota_client_scan(time_t now, int force)
{
    static time_t last_scan;
    static pid_t last_pid;
    if (force || now - last_scan >= 5) {
        last_pid = find_ota_client();
        last_scan = now;
    }
    return last_pid;
}

static int read_ota_status(void)
{
    char value[32];
    if (read_file(cfg.offline_status_path, value, sizeof(value)) <= 0)
        return -1;
    return atoi(value) != 0;
}

static int write_ota_status(int on)
{
    return write_atomic(cfg.offline_status_path, on ? "1" : "0");
}

static int read_session_bus(char *out, size_t size)
{
    char env[MAX_PATH * 2];
    if (read_file(cfg.session_bus_file, out, size) > 0 && out[0])
        return 0;
    if (read_file("/tmp/session_appmgr.env", env, sizeof(env)) > 0) {
        char *p = strstr(env, "DBUS_SESSION_BUS_ADDRESS=");
        if (p) {
            p += strlen("DBUS_SESSION_BUS_ADDRESS=");
            char *nl = strchr(p, '\n');
            if (nl)
                *nl = 0;
            snprintf(out, size, "%s", p);
            return out[0] ? 0 : -1;
        }
    }
    return -1;
}

static int start_ota_call(int mode)
{
    char bus[MAX_PATH], outpath[MAX_PATH], cmd[2048];
    int fd, have_bus;
    pid_t pid;

    have_bus = read_session_bus(bus, sizeof(bus)) == 0;
    snprintf(outpath, sizeof(outpath), "%s/ota_call.out", cfg.rw_dir);
    fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(rt.ota_state, sizeof(rt.ota_state), "error");
        snprintf(rt.ota_error, sizeof(rt.ota_error), "cannot create %s", outpath);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fd);
        snprintf(rt.ota_state, sizeof(rt.ota_state), "error");
        snprintf(rt.ota_error, sizeof(rt.ota_error), "fork failed");
        return -1;
    }
    if (pid == 0) {
        if (have_bus)
            setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
        dup2(fd, STDOUT_FILENO);
        /* Capture dbus-send's errors for ota_call.out diagnostics. */
        dup2(fd, STDERR_FILENO);
        close(fd);
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" --session --print-reply --reply-timeout=8000 "
                 "--dest=com.lge.hkmc.avn.upgrade.fota /fotaInterface "
                 "com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode int32:%d "
                 "|| \"%s\" --system --print-reply --reply-timeout=8000 "
                 "--dest=com.lge.hkmc.avn.upgrade.fota /fotaInterface "
                 "com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode int32:%d",
                 cfg.dbus_send_path, mode, cfg.dbus_send_path, mode);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fd);
    rt.ota_child = pid;
    rt.ota_child_mode = mode;
    rt.ota_child_started = mono_s();
    snprintf(rt.ota_state, sizeof(rt.ota_state), "pending");
    log_msg("OTA: setOFFlineMode(%d) requested pid=%ld", mode, (long)pid);
    return 0;
}

static void finish_ota_call(time_t now)
{
    int status = 0;
    pid_t r;
    if (rt.ota_child <= 0)
        return;
    r = waitpid(rt.ota_child, &status, WNOHANG);
    if (r == 0 && now - rt.ota_child_started > 20) {
        kill(rt.ota_child, SIGKILL);
        waitpid(rt.ota_child, &status, 0);
        r = rt.ota_child;
        snprintf(rt.ota_error, sizeof(rt.ota_error), "dbus-send timed out");
    }
    if (r != rt.ota_child)
        return;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char out[4096], outpath[MAX_PATH];
        int ok = 0;
        snprintf(outpath, sizeof(outpath), "%s/ota_call.out", cfg.rw_dir);
        if (read_file(outpath, out, sizeof(out)) > 0) {
            /* dbus-send prints the returned int32; non-negative is success. */
            char *p = strstr(out, "int32 ");
            if (p) {
                long v = strtol(p + 6, NULL, 10);
                ok = v >= 0;
            }
        }
        if (ok) {
            if (rt.ota_child_mode)
                snprintf(rt.ota_state, sizeof(rt.ota_state), "blocked");
            else {
                snprintf(rt.ota_state, sizeof(rt.ota_state), "online");
                rt.ota_explicit_off = 0;
                set_ota_owned(0);
            }
            rt.ota_error[0] = 0;
            log_msg("OTA: setOFFlineMode(%d) completed", rt.ota_child_mode);
        } else {
            snprintf(rt.ota_state, sizeof(rt.ota_state), "error");
            snprintf(rt.ota_error, sizeof(rt.ota_error),
                     "setOFFlineMode(%d) returned an error", rt.ota_child_mode);
        }
    } else {
        snprintf(rt.ota_state, sizeof(rt.ota_state), "error");
        snprintf(rt.ota_error, sizeof(rt.ota_error), "dbus-send failed");
    }
    rt.ota_child = -1;
    rt.ota_last_attempt = now;
}

static void check_ota(time_t now, int force)
{
    int file_state = read_ota_status();
    pid_t client;

    if (rt.ota_child > 0) {
        finish_ota_call(now);
        return;
    }
    if (!cfg.ota_block && file_state != 1 &&
        (!rt.ota_state[0] || !strcmp(rt.ota_state, "online") ||
         !strcmp(rt.ota_state, "unknown"))) {
        snprintf(rt.ota_state, sizeof(rt.ota_state), "online");
        if (ota_owned())
            set_ota_owned(0);
        rt.ota_explicit_off = 0;
        return;
    }
    /* Never clear a vendor offline flag that Tweaks did not set. */
    if (!cfg.ota_block && !ota_owned() && !rt.ota_explicit_off) {
        snprintf(rt.ota_state, sizeof(rt.ota_state), "online");
        rt.ota_error[0] = 0;
        return;
    }
    client = ota_client_scan(now, force);

    if (cfg.ota_block) {
        set_ota_owned(1);
        if (file_state != 1 && write_ota_status(1) < 0) {
            snprintf(rt.ota_state, sizeof(rt.ota_state), "error");
            snprintf(rt.ota_error, sizeof(rt.ota_error),
                     "cannot write %s", cfg.offline_status_path);
        }
        if (strcmp(rt.ota_state, "blocked") != 0 || client != rt.ota_client_pid) {
            if (force || client != rt.ota_client_pid ||
                now - rt.ota_last_attempt >= 30) {
                rt.ota_client_pid = client;
                start_ota_call(1);
                rt.ota_last_attempt = now;
            }
        }
    } else {
        if (file_state == 1 || (rt.ota_state[0] && strcmp(rt.ota_state, "online") != 0)) {
            if (force || now - rt.ota_last_attempt >= 30) {
                if (write_ota_status(0) < 0)
                    snprintf(rt.ota_error, sizeof(rt.ota_error),
                             "cannot write %s", cfg.offline_status_path);
                start_ota_call(0);
                rt.ota_last_attempt = now;
            }
        } else if (!rt.ota_state[0]) {
            snprintf(rt.ota_state, sizeof(rt.ota_state), "online");
        }
    }
}

static void drain_ota_child(int seconds)
{
    int64_t deadline = mono_ms() + seconds * 1000;
    while (rt.ota_child > 0 && mono_ms() <= deadline) {
        finish_ota_call(mono_s());
        if (rt.ota_child > 0)
            usleep(100000);
    }
}

static void save_last_gamma(void)
{
    char path[MAX_PATH], saved[32];
    if (!rt.gamma_valid || !valid_gamma(rt.last_gamma) ||
        is_neutral_gamma(rt.last_gamma))
        return;
    snprintf(path, sizeof(path), "%s/last_gamma", cfg.rw_dir);
    if (read_file(path, saved, sizeof(saved)) < 0 || strcmp(saved, rt.last_gamma))
        write_atomic(path, rt.last_gamma);
}

static void load_last_gamma(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/last_gamma", cfg.rw_dir);
    if (read_file(path, rt.last_gamma, sizeof(rt.last_gamma)) > 0 && valid_gamma(rt.last_gamma)) {
        if (is_neutral_gamma(rt.last_gamma))
            rt.last_gamma[0] = 0;
    }
}

static void read_vendor_state(void)
{
    char out[4096], status[128], warm[128], gamma[128] = "", bright[128] = "";
    int st = -1, level = -1;
    int have_gamma = 0, have_bright = 0;

    if (run_persist_helper(out, sizeof(out)) > 0) {
        if (line_value(out, "APP_SETTING_DISP_BLUE_LIGHT_STATUS", status, sizeof(status)))
            st = parse_bool(status);
        if (line_value(out, "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL", warm, sizeof(warm)))
            level = atoi(warm);
        if (line_value(out, "APP_SETTING_DISP_GAMMA_R_G_B", gamma, sizeof(gamma)))
            have_gamma = valid_gamma(gamma);
        if (line_value(out, "APP_SETTING_DISP_BRIGHTNESS_R_G_B", bright, sizeof(bright)))
            have_bright = valid_gamma(bright);
        clear_error();
    } else {
        set_error("vehicle persistence unavailable");
    }

    if (cfg.warmth_override >= 1 && cfg.warmth_override <= 9)
        level = cfg.warmth_override;
    if (cfg.force_filter_on)
        st = 1;

    if (st < 0 && (have_gamma || have_bright))
        st = ((have_gamma && !is_neutral_gamma(gamma)) ||
              (have_bright && !is_neutral_gamma(bright))) ? 1 : 0;

    if (st < 0) {
        if (rt.filter_known)
            return; /* A transient PCL failure must not invent a new state. */
        rt.warmth_level = level > 0 ? level : 0;
        snprintf(rt.gamma, sizeof(rt.gamma), "128:128:128");
        rt.gamma_valid = 1;
        snprintf(rt.brightness, sizeof(rt.brightness), "128:128:128");
        rt.brightness_valid = 1;
        if (!rt.last_error[0])
            set_error("vehicle blue-light state unavailable");
        if (level <= 0 && !have_gamma && !have_bright)
            set_error("vehicle blue-light keys empty");
        return;
    }

    rt.filter_on = (st == 1);
    rt.filter_known = 1;
    rt.warmth_level = (level >= 1 && level <= 9) ? level : 0;

    if (!rt.filter_on) {
        /*
         * The filter is off (the vendor flips it when projection takes
         * over), but PCL often still carries the last warmth values.
         * Record them so the night-mode logic can reuse them; neutral
         * values are skipped by effective_warmth anyway.
         */
        if (have_bright && !is_neutral_gamma(bright)) {
            snprintf(rt.brightness, sizeof(rt.brightness), "%s", bright);
            snprintf(rt.last_brightness, sizeof(rt.last_brightness), "%s", bright);
        } else {
            snprintf(rt.brightness, sizeof(rt.brightness), "128:128:128");
        }
        rt.brightness_valid = 1;
        if (have_gamma && !is_neutral_gamma(gamma)) {
            snprintf(rt.gamma, sizeof(rt.gamma), "%s", gamma);
        } else {
            snprintf(rt.gamma, sizeof(rt.gamma), "128:128:128");
        }
        rt.gamma_valid = 1;
        return;
    }

    if (have_bright && !is_neutral_gamma(bright)) {
        snprintf(rt.brightness, sizeof(rt.brightness), "%s", bright);
    } else if (rt.warmth_level >= 1 && rt.warmth_level <= 9) {
        snprintf(rt.brightness, sizeof(rt.brightness), "%s", GAMMA_TABLE[rt.warmth_level]);
    } else if (valid_gamma(rt.last_brightness) && !is_neutral_gamma(rt.last_brightness)) {
        snprintf(rt.brightness, sizeof(rt.brightness), "%s", rt.last_brightness);
    } else {
        snprintf(rt.brightness, sizeof(rt.brightness), "%s", GAMMA_TABLE[5]);
    }
    rt.brightness_valid = 1;
    if (!is_neutral_gamma(rt.brightness))
        snprintf(rt.last_brightness, sizeof(rt.last_brightness), "%s", rt.brightness);

    if (have_gamma && !is_neutral_gamma(gamma)) {
        snprintf(rt.gamma, sizeof(rt.gamma), "%s", gamma);
    } else if (rt.warmth_level >= 1 && rt.warmth_level <= 9) {
        snprintf(rt.gamma, sizeof(rt.gamma), "%s", GAMMA_TABLE[rt.warmth_level]);
    } else if (valid_gamma(rt.last_gamma) && !is_neutral_gamma(rt.last_gamma)) {
        snprintf(rt.gamma, sizeof(rt.gamma), "%s", rt.last_gamma);
    } else {
        snprintf(rt.gamma, sizeof(rt.gamma), "%s", GAMMA_TABLE[5]);
    }
    rt.gamma_valid = 1;
    if (!is_neutral_gamma(rt.gamma))
        snprintf(rt.last_gamma, sizeof(rt.last_gamma), "%s", rt.gamma);
}

static int run_wrandr(int crtc, const char *brightness)
{
    pid_t pid;
    int status = 0;
    int64_t deadline;
    char arg_crtc[64], arg_brightness[64];

    snprintf(arg_crtc, sizeof(arg_crtc), "--crtc=%d", crtc);
    snprintf(arg_brightness, sizeof(arg_brightness), "--brightness=%s", brightness);

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* wrandr is a Wayland client: it needs the session's runtime dir
         * (the vendor session uses /var/run/user/root). */
        if (cfg.xdg_runtime_dir[0])
            setenv("XDG_RUNTIME_DIR", cfg.xdg_runtime_dir, 1);
        execl(cfg.wrandr_path, cfg.wrandr_path, arg_crtc, arg_brightness,
              (char *)NULL);
        execl("/usr/bin/wrandr", "/usr/bin/wrandr", arg_crtc, arg_brightness,
              (char *)NULL);
        _exit(127);
    }

    deadline = mono_ms() + 2500;
    while (mono_ms() <= deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

/*
 * Apply a gamma to a CRTC.  force=1 skips the "already applied" shortcut:
 * the vendor rewrites the LUT behind our back (e.g. when projection takes
 * over), so while a fix is active we re-apply every tick.
 */
static int apply_gamma(int crtc, const char *gamma, char *last, size_t last_size,
                       int *applied, int force)
{
    if (!gamma || !valid_gamma(gamma)) {
        return -1;
    }
    if (!force && *applied && !strcmp(last, gamma))
        return 0;
    if (run_wrandr(crtc, gamma) == 0) {
        snprintf(last, last_size, "%s", gamma);
        *applied = 1;
        return 0;
    }
    set_error("wrandr failed on CRTC %d", crtc);
    return -1;
}

/*
 * Send a blue-light fixed CAN signal to the instrument cluster through the
 * vendor micom service, exactly like MicomInterfaceM::setBLUStatus /
 * setBLULevel do: system bus, destination com.lge.car.micom (the bus name
 * the micom service owns), path /Can, interface com.lge.car.micom.can,
 * method SetCANFixedMicomRecv with (canID bytes, startBit, size, data) on
 * CAN 0x063F.
 */
static int send_blu_signal(int start_bit, int size, int data)
{
    /* Preferred path: Battery's micom-send helper. The vendor micom only
     * accepts CAN sends from a process whose executable basename is on its
     * whitelist (CANService); a plain dbus-send child is dropped. The helper
     * is a copy of the vendor dbus-send installed under that name and takes
     * "fixed <idhi> <idlo> <start> <size> <data>".
     *
     * Fallback: the direct dbus-send call (correct destination/interface and
     * separate argv words), kept for hosts without the helper; the micom may
     * reject it because of the basename check, which then shows up in
     * blu_call.out and failures.log. */
    char arg_hi[8], arg_lo[8], arg_bit[8], arg_size[8], arg_data[8];
    char arg_id[32], arg_bit_i[16], arg_size_i[16], arg_data_i[16];
    char outpath[MAX_PATH];
    int fd;
    pid_t pid;

    snprintf(arg_hi, sizeof(arg_hi), "0x%02x", BLU_CAN_ID_HI);
    snprintf(arg_lo, sizeof(arg_lo), "0x%02x", BLU_CAN_ID_LO);
    snprintf(arg_bit, sizeof(arg_bit), "%d", start_bit);
    snprintf(arg_size, sizeof(arg_size), "%d", size);
    snprintf(arg_data, sizeof(arg_data), "%d", data);
    snprintf(arg_id, sizeof(arg_id), "array:byte:0x%02x,0x%02x",
             BLU_CAN_ID_HI, BLU_CAN_ID_LO);
    snprintf(arg_bit_i, sizeof(arg_bit_i), "int32:%d", start_bit);
    snprintf(arg_size_i, sizeof(arg_size_i), "int32:%d", size);
    snprintf(arg_data_i, sizeof(arg_data_i), "int32:%d", data);
    snprintf(outpath, sizeof(outpath), "%s/blu_call.out", cfg.rw_dir);
    fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        /* Capture helper errors too: a failed micom send must leave evidence
         * in blu_call.out for the failure log. */
        dup2(fd, STDERR_FILENO);
        close(fd);
        if (cfg.can_service[0] && access(cfg.can_service, X_OK) == 0 &&
            helper_has_fixed(cfg.can_service)) {
            execl(cfg.can_service, "micom-send", "fixed",
                  arg_hi, arg_lo, arg_bit, arg_size, arg_data, (char *)NULL);
            /* Exec failed; fall through to the direct call. */
        }
        execl(cfg.dbus_send_path, "dbus-send", "--system", "--print-reply",
              "--reply-timeout=8000",
              "--dest=com.lge.car.micom", "/Can",
              "com.lge.car.micom.can.SetCANFixedMicomRecv",
              arg_id, arg_bit_i, arg_size_i, arg_data_i, (char *)NULL);
        {
            char err[320];
            int len = snprintf(err, sizeof(err),
                              "exec %s failed: %s\n",
                              cfg.can_service[0] ? cfg.can_service
                                                 : cfg.dbus_send_path,
                              strerror(errno));
            if (len > 0)
                (void)!write(STDOUT_FILENO, err, strnlen(err, sizeof(err)));
        }
        _exit(127);
    }
    close(fd);
    {
        int status = 0;
        int64_t deadline = mono_ms() + 10000;
        while (mono_ms() <= deadline) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                    return 0;
                char detail[512] = "";
                read_file(outpath, detail, sizeof(detail));
                collapse_newlines(detail);
                log_failure("micom BLU failed bit=%d size=%d data=%d: %s",
                            start_bit, size, data, detail);
                return -1;
            }
            usleep(50000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    log_failure("micom BLU timed out bit=%d size=%d data=%d",
                start_bit, size, data);
    return -1;
}

static int send_blu_status(int on)
{
    return send_blu_signal(BLU_STATUS_BIT, BLU_STATUS_SIZE,
                           on ? BLU_STATUS_ON : BLU_STATUS_OFF);
}

static int send_blu_level(int level)
{
    return send_blu_signal(BLU_LEVEL_BIT, BLU_LEVEL_SIZE, level);
}

static int blu_forced_marker(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/blu_forced", cfg.rw_dir);
    return access(path, F_OK) == 0;
}

static int set_blu_forced_marker(int on)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/blu_forced", cfg.rw_dir);
    if (on)
        return write_atomic(path, "1");
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

/*
 * Cluster night mode: while projection is active and the vendor has flipped
 * the blue-light filter off, keep re-asserting the cluster's amber by
 * writing the same fixed CAN signals the vendor uses.  When the force ends
 * and the vendor filter is still off, restore the vendor state once.
 */
/* Cluster CAN level to assert while forcing: vendor max (9) in deep-red
 * night mode, otherwise the vehicle's own warmth level. */
static int effective_level(void)
{
    return cfg.deep_red ? DEEP_RED_LEVEL : rt.warmth_level;
}

static void check_cluster_blu(time_t now)
{
    static int initialized;
    int want_forced = cfg.cluster_fix && night_mode_on() &&
                      (cfg.deep_red || (rt.projection_active && !rt.filter_on));
    int level = effective_level();

    if (!initialized) {
        /* A previous daemon run may have left the cluster forced on. */
        if (blu_forced_marker()) {
            rt.blu_forced = 1;
            /* Marker means we MAY have changed hardware, not an ACK. */
        }
        initialized = 1;
    }

    if (!rt.filter_known) {
        if (want_forced || rt.blu_forced)
            set_error("cluster restore/override waiting for vehicle state");
        return;
    }

    if (want_forced) {
        if (level < 1 || level > 9) {
            set_error("cluster warmth level unavailable");
            return;
        }
        int need = (rt.blu_status_sent != BLU_STATUS_ON) ||
                   (rt.blu_level_sent != level);
        if (!need && rt.blu_last_send &&
            now - rt.blu_last_send >= BLU_KEEPALIVE_S)
            need = 1;   /* keepalive: the vendor may re-assert its own state */
        if (need) {
            /* Persist ownership BEFORE the first side effect. A partial
             * write or power loss must still leave a recovery obligation. */
            if (!rt.blu_forced && set_blu_forced_marker(1) < 0) {
                set_error("cannot record cluster override ownership");
                return;
            }
            rt.blu_forced = 1;
            int status_ok = send_blu_status(1) == 0;
            if (status_ok)
                rt.blu_status_sent = BLU_STATUS_ON;
            int level_ok = send_blu_level(level) == 0;
            if (level_ok)
                rt.blu_level_sent = level;
            if (status_ok && level_ok) {
                rt.blu_last_send = now;
                rt.cluster_applied = 1;
                rt.blu_forced = 1;
                log_msg("cluster BLU: forced on via CAN 0x063F (level %d)",
                        level);
            } else {
                set_error("micom blue-light send failed");
            }
        }
    } else if (rt.blu_forced) {
        /* Restore both fields. In particular, release deep-red level 9
         * even if the vendor filter became active without a level change. */
        int level_ok = rt.warmth_level >= 1 && rt.warmth_level <= 9;
        if (level_ok) {
            level_ok = send_blu_level(rt.warmth_level) == 0;
            if (level_ok)
                rt.blu_level_sent = rt.warmth_level;
        } else if (!rt.filter_on) {
            level_ok = 1; /* off is safe even when no level is available */
        }
        int status_ok = send_blu_status(rt.filter_on) == 0;
        if (status_ok)
            rt.blu_status_sent = rt.filter_on ? BLU_STATUS_ON : BLU_STATUS_OFF;
        if (level_ok && status_ok && set_blu_forced_marker(0) == 0) {
            rt.blu_forced = 0;
            rt.cluster_applied = 0;
            log_msg("cluster BLU: restored vendor state status=%d level=%d",
                    rt.blu_status_sent, rt.blu_level_sent);
        } else {
            set_error("cluster restore pending; will retry");
        }
    } else {
        /* Vendor owns the cluster state while no override is active. */
        rt.cluster_applied = rt.filter_on;
    }
}

static void write_cluster_desired(void)
{
    char path[MAX_PATH], value[64], current[64];
    snprintf(path, sizeof(path), "%s/cluster_gamma", cfg.rw_dir);
    if (cfg.infotainment_fix && night_mode_on()) {
        effective_warmth(value, sizeof(value));
    } else {
        snprintf(value, sizeof(value), "off");
    }
    /* /rw_data lives on flash; only rewrite the file when it changes. */
    if (read_file(path, current, sizeof(current)) >= 0 && !strcmp(current, value))
        return;
    write_atomic(path, value);
}

static void monitor_stop_slot(mon_t *m)
{
    if (m->fd >= 0) {
        close(m->fd);
        m->fd = -1;
    }
    if (m->pid > 0) {
        kill(m->pid, SIGTERM);
        waitpid(m->pid, NULL, 0);
        m->pid = -1;
    }
    m->alive = 0;
}

static void home_monitor_stop(void)
{
    if (g_home_fd >= 0) {
        close(g_home_fd);
        g_home_fd = -1;
    }
    if (g_home_pid > 0) {
        kill(g_home_pid, SIGTERM);
        waitpid(g_home_pid, NULL, 0);
        g_home_pid = -1;
    }
    g_home_alive = 0;
}

static void home_monitor_start(void)
{
    int pipefd[2];
    pid_t pid;
    char addr[MAX_PATH];

    if (!g_monitor_enabled || g_once_mode || g_home_pid > 0)
        return;
    if (read_session_bus(addr, sizeof(addr)) < 0) {
        g_home_retry_ms = mono_ms() + 5000;
        return;
    }
    /* A stale guid makes libdbus refuse the connection after a bus restart;
     * the path alone identifies the bus. */
    {
        char *guid = strstr(addr, ",guid=");
        if (guid)
            *guid = '\0';
    }
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        g_home_retry_ms = mono_ms() + 5000;
        return;
    }
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        g_home_retry_ms = mono_ms() + 5000;
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(pipefd[1], STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("dbus-monitor", "dbus-monitor", "--address", addr,
               "member=NotifyRequestHomeScreen",
               "member=custom_key_value", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    g_home_fd = pipefd[0];
    g_home_pid = pid;
    g_home_len = 0;
    g_home_member[0] = 0;
    g_home_alive = 1;
    g_home_retry_ms = 0;
    log_msg("home/star monitor started pid=%ld", (long)pid);
}

static void process_home_line(const char *line)
{
    const char *memb = strstr(line, "member=");
    if (memb) {
        size_t n = 0;
        memb += 7;
        while (memb[n] && memb[n] != ' ' && memb[n] != '\t' &&
               memb[n] != '\n' && n + 1 < sizeof(g_home_member)) {
            g_home_member[n] = memb[n];
            n++;
        }
        g_home_member[n] = 0;
        if (!strcmp(g_home_member, "NotifyRequestHomeScreen")) {
            g_home_seq++;
            snprintf(g_home_source, sizeof(g_home_source), "home");
        }
    }
    if (strstr(line, "int32 7") &&
        !strcmp(g_home_member, "custom_key_value")) {
        g_home_seq++;
        snprintf(g_home_source, sizeof(g_home_source), "star");
    }
}

static void home_monitor_read(void)
{
    char buf[4096];
    ssize_t n;

    if (g_home_fd < 0)
        return;
    while ((n = read(g_home_fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                g_home_line[g_home_len] = 0;
                process_home_line(g_home_line);
                g_home_len = 0;
            } else if (g_home_len + 1 < sizeof(g_home_line)) {
                g_home_line[g_home_len++] = c;
            }
        }
    }
}

static void home_monitor_tick(void)
{
    int64_t now = mono_ms();
    if (g_home_pid > 0) {
        int st;
        if (waitpid(g_home_pid, &st, WNOHANG) == g_home_pid) {
            if (g_home_fd >= 0)
                close(g_home_fd);
            g_home_fd = -1;
            g_home_pid = -1;
            g_home_alive = 0;
            g_home_retry_ms = now + 5000;
            log_msg("home/star monitor died");
        }
    }
    if (!g_once_mode && g_monitor_enabled && g_home_pid <= 0 &&
        (g_home_retry_ms == 0 || now >= g_home_retry_ms)) {
        home_monitor_start();
        if (g_home_pid <= 0 && g_home_retry_ms == 0)
            g_home_retry_ms = now + 5000;
    }
    home_monitor_read();
}

static void monitor_cleanup(void)
{
    monitor_stop_slot(&g_mon[0]);
    monitor_stop_slot(&g_mon[1]);
    home_monitor_stop();
}

/*
 * Fork one dbus-monitor.  is_session selects the session bus (address from
 * the app manager, guid stripped - a stale guid makes libdbus refuse the
 * connection) instead of the system bus.
 */
static void monitor_start_slot(mon_t *m)
{
    int pipefd[2];
    pid_t pid;
    char addr[MAX_PATH];

    if (!g_monitor_enabled || g_once_mode || m->pid > 0 || cfg.projection_always)
        return;
    if (m->is_session) {
        if (read_session_bus(addr, sizeof(addr)) < 0) {
            m->retry_ms = mono_ms() + 5000;
            return;
        }
        /* A stale guid makes libdbus refuse the connection after a bus
         * restart; the path alone identifies the bus. */
        {
            char *guid = strstr(addr, ",guid=");
            if (guid)
                *guid = '\0';
        }
    }
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(pipefd[1], STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        if (m->is_session) {
            execlp("dbus-monitor", "dbus-monitor", "--address", addr,
                   "type='signal',interface='com.lge.avn.appmanager.info'",
                   (char *)NULL);
        } else {
            execlp("dbus-monitor", "dbus-monitor", "--system",
                   "type='signal',interface='com.lge.avn.appmanager.info'",
                   (char *)NULL);
        }
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    m->fd = pipefd[0];
    m->pid = pid;
    m->len = 0;
    m->member[0] = 0;
    m->alive = 1;
    log_msg("dbus monitor started on %s bus pid=%ld%s",
            m->is_session ? "session" : "system", (long)pid,
            m->is_session ? "" : " (fallback)");
    m->retry_ms = 0;
}

static void start_dbus_monitor(void)
{
    int64_t now = mono_ms();
    for (int i = 0; i < 2; i++) {
        if (g_mon[i].pid <= 0 && (g_mon[i].retry_ms == 0 || now >= g_mon[i].retry_ms)) {
            monitor_start_slot(&g_mon[i]);
            if (g_mon[i].pid <= 0 && g_mon[i].retry_ms == 0) {
                /* fork/exec failed outright: retry in 5 s. */
                g_mon[i].retry_ms = now + 5000;
            }
        }
    }
}

static void process_monitor_line(mon_t *m, const char *line)
{
    const char *memb;
    int bus = m->is_session ? 2 : 1;

    memb = strstr(line, "member=");
    if (memb) {
        size_t n = 0;
        memb += 7;
        while (memb[n] && memb[n] != ' ' && memb[n] != '\t' &&
               memb[n] != '\n' && n + 1 < sizeof(m->member)) {
            m->member[n] = memb[n];
            n++;
        }
        m->member[n] = 0;
    }
    if (strstr(line, "int32 ")) {
        int id = atoi(strstr(line, "int32 ") + 6);
        if (id >= 0 && !strcmp(m->member, "front_drawing_application")) {
            int changed = id != mon_front_app_id;
            mon_front_app_id = id;
            mon_front_app_time = time(NULL);
            if (changed && id > 0 && mon_carplay_play && !mon_carplay_app_id &&
                mono_ms() - mon_carplay_started_ms < 2000)
                mon_carplay_app_id = id;
            if (changed && id > 0 && mon_android_play && !mon_android_app_id &&
                mono_ms() - mon_android_started_ms < 2000)
                mon_android_app_id = id;
            mon_sig_front++;
            mon_last_bus = bus;
        }
    }
    if (strstr(line, "boolean ")) {
        int on = strstr(line, "boolean true") != NULL;
        if (!strcmp(m->member, "carplay_play_enable_status")) {
            /*
             * Learn the foreground app id at the moment playback turns on;
             * that is when the projection screen has just come forward.
             * Never relearn from a later front-app signal, which may be a
             * different app (e.g. Setup opened while CarPlay audio plays).
             */
            /* The last foreground sample can still be Home/Setup. Wait for
             * a NEW front event after play-enable before learning an id. */
            if (on && !mon_carplay_play) {
                mon_carplay_app_id = 0;
                mon_carplay_started_ms = mono_ms();
            }
            if (on != mon_carplay_play) {
                log_msg("projection signal: carplay_play=%d (%s bus)",
                        on, m->is_session ? "session" : "system");
                if (on)
                    mon_last_bus = bus;
            }
            mon_carplay_play = on;
            mon_sig_carplay++;
        } else if (!strcmp(m->member, "android_auto_play_enable_status")) {
            if (on && !mon_android_play) {
                mon_android_app_id = 0;
                mon_android_started_ms = mono_ms();
            }
            if (on != mon_android_play) {
                log_msg("projection signal: android_auto_play=%d (%s bus)",
                        on, m->is_session ? "session" : "system");
                if (on)
                    mon_last_bus = bus;
            }
            mon_android_play = on;
            mon_sig_android++;
        }
    }
}

static void read_monitor_slot(mon_t *m)
{
    char buf[4096];
    ssize_t n;

    if (m->fd < 0)
        return;
    while ((n = read(m->fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                m->line[m->len] = 0;
                process_monitor_line(m, m->line);
                m->len = 0;
            } else if (m->len + 1 < sizeof(m->line)) {
                m->line[m->len++] = c;
            }
        }
    }
}

static void read_monitor(void)
{
    read_monitor_slot(&g_mon[0]);
    read_monitor_slot(&g_mon[1]);
}

static void update_projection(void)
{
    char path[MAX_PATH], val[16];
    snprintf(path, sizeof(path), "%s/projection_state", cfg.rw_dir);
    if (read_file(path, val, sizeof(val)) >= 0 && val[0]) {
        rt.projection_active = (parse_bool(val) == 1);
        snprintf(rt.projection_source, sizeof(rt.projection_source), "manual");
        return;
    }
    if (cfg.projection_always) {
        rt.projection_active = 1;
        snprintf(rt.projection_source, sizeof(rt.projection_source), "always");
        return;
    }
    if (mon_carplay_play || mon_android_play) {
        if (cfg.projection_app_id > 0) {
            rt.projection_active = (mon_front_app_id == cfg.projection_app_id);
            snprintf(rt.projection_source, sizeof(rt.projection_source), "app-id");
        } else if (mon_carplay_play && mon_android_play) {
            /* Both claim playback: prefer the source whose app is front. */
            if (mon_carplay_app_id > 0 && mon_front_app_id == mon_carplay_app_id) {
                rt.projection_active = 1;
                snprintf(rt.projection_source, sizeof(rt.projection_source), "carplay");
            } else if (mon_android_app_id > 0 && mon_front_app_id == mon_android_app_id) {
                rt.projection_active = 1;
                snprintf(rt.projection_source, sizeof(rt.projection_source), "android");
            } else {
                rt.projection_active = 1;
                snprintf(rt.projection_source, sizeof(rt.projection_source), "play");
            }
        } else if (mon_carplay_play) {
            if (mon_carplay_app_id > 0) {
                rt.projection_active = (mon_front_app_id == mon_carplay_app_id);
                snprintf(rt.projection_source, sizeof(rt.projection_source), "carplay");
            } else {
                rt.projection_active = 1;
                snprintf(rt.projection_source, sizeof(rt.projection_source), "play");
            }
        } else {
            if (mon_android_app_id > 0) {
                rt.projection_active = (mon_front_app_id == mon_android_app_id);
                snprintf(rt.projection_source, sizeof(rt.projection_source), "android");
            } else {
                rt.projection_active = 1;
                snprintf(rt.projection_source, sizeof(rt.projection_source), "play");
            }
        }
    } else {
        rt.projection_active = 0;
        snprintf(rt.projection_source, sizeof(rt.projection_source),
                 mon_any_alive() ? "idle" : "unavailable");
    }
}

static void json_escape(const char *in, char *out, size_t size)
{
    size_t j = 0;
    if (!in)
        in = "";
    for (size_t i = 0; in[i] && j + 2 < size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c < 0x20) {
            out[j++] = '?';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

static const char *reboot_state_name(void)
{
    if (rt.reboot_state == 1)
        return "rebooting";
    if (rt.reboot_state == 2)
        return "error";
    return "idle";
}

static void build_status(char *out, size_t size)
{
    char msg[512], err[512], ps[64];
    char zone[256], dststate[64], dsterr[512], otastate[64], otaerr[512];
    char tzver[128], smack[128], smack_json[256];
    char smack_expected[128], smack_expected_json[256];
    char smack_dbus[128], smack_dbus_json[256];
    char can_service_json[MAX_PATH * 2];
    const char *can_source;
    char rmsg[256], rerr[512], rcmd[MAX_PATH];
    char hsource[32];
    int64_t wdt_in = 0;
    json_escape(rt.message, msg, sizeof(msg));
    json_escape(rt.last_error, err, sizeof(err));
    json_escape(rt.projection_source, ps, sizeof(ps));
    json_escape(cfg.dst_zone, zone, sizeof(zone));
    json_escape(rt.dst_state, dststate, sizeof(dststate));
    json_escape(rt.dst_error, dsterr, sizeof(dsterr));
    json_escape(rt.ota_state, otastate, sizeof(otastate));
    json_escape(rt.ota_error, otaerr, sizeof(otaerr));
    json_escape(rt.tzdata_version, tzver, sizeof(tzver));
    current_smack(smack, sizeof(smack));
    json_escape(smack, smack_json, sizeof(smack_json));
    json_escape(g_smack_expected, smack_expected, sizeof(smack_expected));
    json_escape(smack_expected, smack_expected_json, sizeof(smack_expected_json));
    helper_smack(cfg.dbus_send_path, smack_dbus, sizeof(smack_dbus));
    json_escape(smack_dbus, smack_dbus_json, sizeof(smack_dbus_json));
    json_escape(cfg.can_service, can_service_json, sizeof(can_service_json));
    /* The source send_blu_signal will actually use right now. The micom
     * silently drops a plain dbus-send child (basename whitelist), so a
     * `dbus` source means the cluster write is expected to be ignored. */
    can_source = can_service_ready() ? "service" : "dbus";
    json_escape(rt.reboot_message, rmsg, sizeof(rmsg));
    json_escape(rt.reboot_error, rerr, sizeof(rerr));
    json_escape(cfg.reboot_command, rcmd, sizeof(rcmd));
    json_escape(g_home_source, hsource, sizeof(hsource));
    if (rt.reboot_state == 1 && rt.reboot_wdt_at_ms > mono_ms())
        wdt_in = rt.reboot_wdt_at_ms - mono_ms();
    snprintf(out, size,
             "{\"ok\":true,"
             "\"build\":\"%s\","
             "\"config_version\":%d,"
             "\"smack\":\"%s\","
             "\"smack_expected\":\"%s\","
             "\"smack_ok\":%s,"
             "\"smack_dbus\":\"%s\","
             "\"home_seq\":%d,"
             "\"home_source\":\"%s\","
             "\"reboot_state\":\"%s\","
             "\"reboot_message\":\"%s\","
             "\"reboot_error\":\"%s\","
             "\"reboot_wdt_ms\":%d,"
             "\"reboot_wdt_in_ms\":%ld,"
             "\"reboot_wdt_sent\":%d,"
             "\"reboot_wdt_ok\":%d,"
             "\"reboot_command\":\"%s\","
             "\"cluster_fix\":%d,"
             "\"infotainment_fix\":%d,"
             "\"deep_red\":%d,"
             "\"filter_active\":%d,"
             "\"warmth_level\":%d,"
             "\"gamma\":\"%s\","
             "\"projection_active\":%d,"
             "\"projection_source\":\"%s\","
             "\"mon_system\":%d,"
             "\"mon_session\":%d,"
             "\"sig_carplay\":%d,"
             "\"sig_android\":%d,"
             "\"sig_front\":%d,"
             "\"sig_bus\":%d,"
             "\"cluster_crtc\":%d,"
             "\"main_crtc\":%d,"
             "\"cluster_applied\":%d,"
             "\"main_applied\":%d,"
             "\"front_override\":%d,"
             "\"night_forced\":%d,"
             "\"brightness\":\"%s\","
             "\"blu_forced\":%d,"
             "\"blu_status_sent\":%d,"
             "\"blu_level_sent\":%d,"
             "\"can_service\":\"%s\","
             "\"can_source\":\"%s\","
             "\"disclaimer_auto\":%d,"
             "\"disclaimer_timeout_ms\":%d,"
             "\"auto_dst\":%d,"
             "\"dst_zone\":\"%s\","
             "\"dst_active\":%d,"
             "\"dst_raw\":%d,"
             "\"dst_expected\":%d,"
             "\"dst_written_at\":%ld,"
             "\"dst_state\":\"%s\","
             "\"dst_error\":\"%s\","
             "\"tzdata_version\":\"%s\","
             "\"ota_block\":%d,"
             "\"ota_state\":\"%s\","
             "\"ota_error\":\"%s\","
             "\"message\":\"%s\","
             "\"last_error\":\"%s\"}\n",
             TWEAKS_BUILD_ID,
             CONFIG_VERSION,
             smack_json,
             smack_expected_json,
             smack_ok(smack) ? "true" : "false",
             smack_dbus_json,
             g_home_seq,
             hsource,
             reboot_state_name(),
             rmsg,
             rerr,
             cfg.reboot_wdt_ms,
             (long)wdt_in,
             rt.reboot_wdt_sent,
             rt.reboot_wdt_ok,
             rcmd,
             cfg.cluster_fix ? 1 : 0,
             cfg.infotainment_fix ? 1 : 0,
             cfg.deep_red ? 1 : 0,
             rt.filter_on ? 1 : 0,
             rt.warmth_level,
             valid_gamma(rt.gamma) ? rt.gamma : "128:128:128",
             rt.projection_active ? 1 : 0,
             ps,
             g_mon[0].alive ? 1 : 0,
             g_mon[1].alive ? 1 : 0,
             mon_sig_carplay,
             mon_sig_android,
             mon_sig_front,
             mon_last_bus,
             cfg.cluster_crtc,
             cfg.main_crtc,
             rt.cluster_applied ? 1 : 0,
             rt.main_applied ? 1 : 0,
             front_owned() ? 1 : 0,
             rt.night_forced ? 1 : 0,
             valid_gamma(rt.brightness) ? rt.brightness : "128:128:128",
             rt.blu_forced ? 1 : 0,
             rt.blu_status_sent,
             rt.blu_level_sent,
             can_service_json,
             can_source,
             cfg.disclaimer_auto ? 1 : 0,
             cfg.disclaimer_timeout_ms,
             cfg.auto_dst ? 1 : 0,
             zone,
             rt.dst_active ? 1 : 0,
             rt.dst_raw,
             rt.dst_expected,
             (long)rt.dst_written_at,
             dststate,
             dsterr,
             tzver,
             cfg.ota_block ? 1 : 0,
             otastate,
             otaerr,
             msg, err);
}

static void write_status(void)
{
    static char last[8192];
    char path[MAX_PATH], json[8192];
    build_status(json, sizeof(json));
    /* /rw_data lives on flash; only rewrite the file when it changes. */
    if (last[0] && !strcmp(last, json))
        return;
    snprintf(path, sizeof(path), "%s/status.json", cfg.rw_dir);
    if (write_atomic(path, json) == 0)
        snprintf(last, sizeof(last), "%s", json);
}

/* Vendor hard-reset path, copied from /etc/weston-crash.sh: a micom watchdog
 * reset of the ETC CPU.  The daemon runs on the SMACK floor label `_`, which
 * has rwx access to App::micommanager; the same dbus-send path is already
 * used for the cluster CAN writes. */
static int send_wdt_reset(void)
{
    char outpath[MAX_PATH];
    int fd;
    pid_t pid;
    int status = 0;

    snprintf(outpath, sizeof(outpath), "%s/wdt_call.out", cfg.rw_dir);
    fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execl(cfg.dbus_send_path, "dbus-send", "--system",
              "--type=method_call", "--print-reply", "--reply-timeout=8000",
              "--dest=com.lge.car.micom", "/Ihu",
              "com.lge.car.micom.ihu.SetETCCPUWDTReset",
              "array:byte:0x00", (char *)NULL);
        _exit(127);
    }
    close(fd);
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

static void reboot_wdt_now(void)
{
    if (rt.reboot_state != 1 || rt.reboot_wdt_sent)
        return;
    if (update_running()) {
        if (rt.reboot_child > 0) {
            kill(rt.reboot_child, SIGKILL);
            waitpid(rt.reboot_child, NULL, 0);
            rt.reboot_child = -1;
        }
        rt.reboot_state = 2;
        rt.reboot_wdt_ok = 0;
        snprintf(rt.reboot_error, sizeof(rt.reboot_error),
                 "update started; hard reset withheld");
        log_msg("reboot: hard reset withheld (update process running)");
        write_status();
        return;
    }
    if (rt.reboot_child > 0) {
        kill(rt.reboot_child, SIGKILL);
        waitpid(rt.reboot_child, NULL, 0);
        rt.reboot_child = -1;
    }
    rt.reboot_wdt_sent = 1;
    snprintf(rt.reboot_message, sizeof(rt.reboot_message),
             "hard reset fallback");
    log_msg("reboot: clean path did not complete; sending micom WDT reset");
    write_status();
    sync();
    if (send_wdt_reset() < 0) {
        rt.reboot_state = 2;
        rt.reboot_wdt_ok = 0;
        snprintf(rt.reboot_error, sizeof(rt.reboot_error),
                 "hard reset request failed");
        log_msg("reboot: micom WDT reset failed");
    } else {
        rt.reboot_wdt_ok = 1;
        log_msg("reboot: micom WDT reset accepted");
    }
    write_status();
}

static void start_reboot(void)
{
    pid_t pid;

    rt.reboot_state = 1;
    rt.reboot_child = -1;
    rt.reboot_wdt_sent = 0;
    rt.reboot_wdt_ok = -1;
    rt.reboot_error[0] = 0;
    rt.reboot_started_ms = mono_ms();
    rt.reboot_wdt_at_ms = rt.reboot_started_ms + cfg.reboot_wdt_ms;
    snprintf(rt.reboot_message, sizeof(rt.reboot_message),
             "clean reboot requested");
    log_msg("reboot: clean reboot requested (%s)", cfg.reboot_command);

    pid = fork();
    if (pid == 0) {
        execl(cfg.reboot_command, "systemctl", "reboot", "--no-block",
              (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        snprintf(rt.reboot_error, sizeof(rt.reboot_error),
                 "cannot start clean reboot");
        log_msg("reboot: fork failed");
        reboot_wdt_now();
    } else {
        rt.reboot_child = pid;
    }
    write_status();
}

static void reboot_tick(void)
{
    int status = 0;

    if (rt.reboot_state != 1)
        return;
    if (rt.reboot_child > 0) {
        pid_t got = waitpid(rt.reboot_child, &status, WNOHANG);
        if (got == rt.reboot_child) {
            rt.reboot_child = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                snprintf(rt.reboot_error, sizeof(rt.reboot_error),
                         "clean reboot command failed");
                log_msg("reboot: clean reboot command failed (status=%d)",
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                reboot_wdt_now();
                return;
            }
        }
    }
    if (mono_ms() >= rt.reboot_wdt_at_ms)
        reboot_wdt_now();
}

/* Front ownership survives crashes and service restarts. Do not confuse a
 * programmed output with the transient 'last applied' optimization. */
static void vendor_brightness(char *out, size_t size)
{
    snprintf(out, size, "%s", rt.filter_on && valid_gamma(rt.brightness)
             ? rt.brightness : "128:128:128");
}

static int front_owned(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/front_override", cfg.rw_dir);
    return access(path, F_OK) == 0;
}

static void load_front_override(void)
{
    char path[MAX_PATH], value[128], target[32];
    snprintf(path, sizeof(path), "%s/front_override", cfg.rw_dir);
    if (read_file(path, value, sizeof(value)) > 0 &&
        line_value(value, "crtc", target, sizeof(target)) && atoi(target) > 0) {
        rt.front_target = atoi(target);
        rt.main_applied = 1;
    }
}

static int apply_front(const char *brightness)
{
    char path[MAX_PATH], state[128], base[32];
    if (rt.main_applied && rt.front_target != cfg.main_crtc && restore_main() < 0)
        return -1;
    if (!front_owned()) {
        vendor_brightness(base, sizeof(base));
        snprintf(path, sizeof(path), "%s/front_override", cfg.rw_dir);
        snprintf(state, sizeof(state), "crtc=%d\nrestore=%s\n", cfg.main_crtc, base);
        if (write_atomic(path, state) < 0) {
            set_error("cannot record front override ownership");
            return -1;
        }
    }
    rt.front_target = cfg.main_crtc;
    /* Even a failed client invocation may have changed the output. */
    rt.main_applied = 1;
    return apply_gamma(rt.front_target, brightness, rt.last_main_gamma,
                       sizeof(rt.last_main_gamma), &rt.main_applied, 1);
}

static int restore_main(void)
{
    char path[MAX_PATH], base[32], state[128], saved[32];
    if (!rt.main_applied && !front_owned())
        return 0;
    snprintf(path, sizeof(path), "%s/cluster_gamma", cfg.rw_dir);
    if (write_atomic(path, "off") < 0)
        return -1;
    vendor_brightness(base, sizeof(base));
    snprintf(path, sizeof(path), "%s/front_override", cfg.rw_dir);
    if (!rt.filter_known && read_file(path, state, sizeof(state)) > 0 &&
        line_value(state, "restore", saved, sizeof(saved)) && valid_gamma(saved))
        snprintf(base, sizeof(base), "%s", saved);
    if (run_wrandr(rt.front_target > 0 ? rt.front_target : cfg.main_crtc, base) != 0) {
        set_error("front restore pending; will retry");
        return -1;
    }
    rt.main_applied = 0;
    rt.last_main_gamma[0] = 0;
    if (unlink(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

static int release_overrides(void)
{
    rt.identify_crtc = -1;
    int cluster_enabled = cfg.cluster_fix;
    cfg.cluster_fix = 0;
    check_cluster_blu(mono_s());
    cfg.cluster_fix = cluster_enabled;
    int front_ok = restore_main() == 0;
    return front_ok && !rt.blu_forced ? 0 : -1;
}

/*
 * Log filter / projection state transitions.  One CarPlay session in
 * daemon.log then shows exactly what the vendor did to the filter state.
 */
static void log_night_transitions(void)
{
    static int have_filter, prev_filter;
    static int have_proj, prev_proj;

    if (!have_filter || prev_filter != rt.filter_on) {
        have_filter = 1;
        prev_filter = rt.filter_on;
        log_msg("blue-light filter: %d (projection %s)",
                rt.filter_on, rt.projection_active ? "active" : "idle");
    }
    if (!have_proj || prev_proj != rt.projection_active) {
        have_proj = 1;
        prev_proj = rt.projection_active;
        log_msg("projection: %s (source %s)",
                rt.projection_active ? "active" : "idle", rt.projection_source);
    }
}

static void tick(void)
{
    time_t now = mono_s();
    char warmth[32];
    int night_on;

    trace_msg("tick enter");

    for (int i = 0; i < 2; i++) {
        int st;
        if (g_mon[i].pid > 0 && waitpid(g_mon[i].pid, &st, WNOHANG) == g_mon[i].pid) {
            if (g_mon[i].fd >= 0)
                close(g_mon[i].fd);
            g_mon[i].fd = -1;
            g_mon[i].pid = -1;
            g_mon[i].alive = 0;
            /* Backoff so a monitor that exits immediately cannot loop. */
            g_mon[i].retry_ms = mono_ms() + 5000;
            log_msg("dbus monitor on %s bus died",
                    g_mon[i].is_session ? "session" : "system");
            /* Keep the play flags: a monitor hiccup mid-CarPlay must not
             * drop the amber tint; the next signal refreshes the state. */
        }
    }
    if (!g_once_mode && g_monitor_enabled)
        start_dbus_monitor();
    if (g_last_vendor_read == 0 || now - g_last_vendor_read >= 2) {
        read_vendor_state();
        g_last_vendor_read = now;
    }
    read_monitor();
    home_monitor_tick();
    update_projection();
    log_night_transitions();
    usb_log_tick(now);
    check_dst(now_time(), 0);
    check_ota(now, 0);

    night_on = night_mode_on();
    rt.night_forced = night_on && !rt.filter_on;
    effective_warmth(warmth, sizeof(warmth));

    check_cluster_blu(now);

    if (rt.identify_crtc >= 0 && rt.identify_until <= now) {
        /* Keep front ownership intact; restore_main() rewrites the output
         * on this tick because front_override is still on disk. */
        rt.identify_crtc = -1;
    }

    int identifying_main = (rt.identify_crtc == cfg.main_crtc);

    /*
     * Front (infotainment) display: the vendor tints it with
     * "wrandr --crtc=1 --brightness=<r:g:b>" (setFrontBrightnessRGBValue).
     * Re-apply every tick while the fix is on and night mode is effective,
     * so a vendor reset during projection is corrected within a second.
     */
    if (!identifying_main) {
        if (cfg.infotainment_fix && night_on) {
            apply_front(warmth);
        } else {
            restore_main();
        }
    }

    if (rt.identify_crtc >= 0) {
        if (rt.identify_crtc == cfg.main_crtc)
            apply_front("255:0:0");
        else
            rt.identify_crtc = -1;
    }

    write_cluster_desired();
    save_last_gamma();
    reboot_tick();
    if (!rt.last_error[0])
        set_message("Cluster fix %s; infotainment fix %s; projection %s",
                    cfg.cluster_fix ? "on" : "off",
                    cfg.infotainment_fix ? "on" : "off",
                    rt.projection_active ? "active" : "idle");
    write_status();
    trace_msg("tick exit");
}

static void send_all(int fd, const char *data)
{
    size_t left = strlen(data);
    while (left > 0) {
        ssize_t n = send(fd, data, left, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        data += n;
        left -= (size_t)n;
    }
}

static void send_response(int fd, int code, const char *body)
{
    char header[256];
    const char *reason = code == 200 ? "OK" :
                         code == 202 ? "Accepted" :
                         code == 400 ? "Bad Request" :
                         code == 403 ? "Forbidden" :
                         code == 409 ? "Conflict" : "Not Found";
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             code, reason, strlen(body));
    send_all(fd, header);
    send_all(fd, body);
}

static int url_decode(const char *in, char *out, size_t size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < size; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = 0;
    return (int)j;
}

static int query_param(const char *query, const char *key, char *out, size_t size)
{
    char tmp[2048];
    char *p;
    snprintf(tmp, sizeof(tmp), "%s", query);
    p = tmp;
    while (p && *p) {
        char *amp = strchr(p, '&');
        char *eq;
        if (amp)
            *amp = 0;
        eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            if (!strcmp(p, key)) {
                url_decode(eq + 1, out, size);
                return 1;
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* A browser cross-site request carries an http(s) Origin.  The QML app is
 * not a web origin; allow a file/qrc/none origin so the local UI is not
 * rejected by its own network stack. */
static int remote_origin(const char *request)
{
    const char *p = strcasestr(request, "\r\nOrigin:");
    if (!p)
        return 0;
    p += strlen("\r\nOrigin:");
    while (*p == ' ' || *p == '\t')
        p++;
    return !strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8);
}

static int header_value_is(const char *request, const char *name,
                           const char *value)
{
    const char *p = strstr(request, "\r\n");
    size_t n = strlen(name), vn = strlen(value);
    if (!p)
        return 0;
    while (p) {
        const char *end;
        p += 2;
        end = strstr(p, "\r\n");
        if (!end)
            break;
        if ((size_t)(end - p) > n && !strncasecmp(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1;
            size_t have;
            while (v < end && (*v == ' ' || *v == '\t'))
                v++;
            have = (size_t)(end - v);
            while (have > 0 && (v[have - 1] == ' ' || v[have - 1] == '\t'))
                have--;
            return have == vn && !strncasecmp(v, value, vn);
        }
        p = end;
    }
    return 0;
}

static int apply_config_key(const char *k, const char *v)
{
    if (!strcmp(k, "cluster_fix"))
        cfg.cluster_fix = parse_bool(v) == 1;
    else if (!strcmp(k, "infotainment_fix"))
        cfg.infotainment_fix = parse_bool(v) == 1;
    else if (!strcmp(k, "cluster_crtc"))
        cfg.cluster_crtc = atoi(v);
    else if (!strcmp(k, "main_crtc"))
        cfg.main_crtc = atoi(v);
    else if (!strcmp(k, "warmth_override"))
        cfg.warmth_override = atoi(v);
    else if (!strcmp(k, "force_filter_on"))
        cfg.force_filter_on = parse_bool(v) == 1;
    else if (!strcmp(k, "deep_red"))
        cfg.deep_red = parse_bool(v) == 1;
    else if (!strcmp(k, "disclaimer_auto"))
        cfg.disclaimer_auto = parse_bool(v) == 1;
    else if (!strcmp(k, "disclaimer_timeout_ms"))
        cfg.disclaimer_timeout_ms = clamp_int(atoi(v), 1000, 10000);
    else if (!strcmp(k, "auto_dst"))
        cfg.auto_dst = parse_bool(v) == 1;
    else if (!strcmp(k, "dst_zone")) {
        if (v[0] && strlen(v) < sizeof(cfg.dst_zone) && zone_is_safe(v))
            snprintf(cfg.dst_zone, sizeof(cfg.dst_zone), "%s", v);
    } else if (!strcmp(k, "dst_check_ms"))
        cfg.dst_check_ms = clamp_int(atoi(v), 10000, 3600000);
    else if (!strcmp(k, "ota_block")) {
        cfg.ota_block = parse_bool(v) == 1;
        rt.ota_explicit_off = !cfg.ota_block;
    } else if (!strcmp(k, "can_service")) {
        if (!trusted_can_service(v))
            return 0;
        snprintf(cfg.can_service, sizeof(cfg.can_service), "%s", v);
    } else
        return 0;
    return 1;
}

static void handle_http(int fd)
{
    char buf[8192], method[16], path[2048], query[2048], body[8192], val[256];
    char decoded[2048];
    ssize_t n;
    struct timeval tv = {2, 0};
    char *sp1, *sp2, *q, *headers;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    buf[n] = 0;
    /* Keep a pointer to the header block: the request-line parser below
     * inserts NULs, which would hide it from a later strstr(buf, ...). */
    headers = strstr(buf, "\r\n");
    sp1 = strchr(buf, ' ');
    if (!sp1) {
        send_response(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}\n");
        close(fd);
        return;
    }
    *sp1 = 0;
    snprintf(method, sizeof(method), "%s", buf);
    sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) {
        send_response(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}\n");
        close(fd);
        return;
    }
    *sp2 = 0;
    snprintf(path, sizeof(path), "%s", sp1 + 1);
    q = strchr(path, '?');
    if (q) {
        *q = 0;
        snprintf(query, sizeof(query), "%s", q + 1);
    } else {
        query[0] = 0;
    }
    url_decode(path, decoded, sizeof(decoded));
    trace_msg("http %s %s", method, decoded);

    if (!strcmp(decoded, "/health")) {
        char smack[128], smack_json[256];
        char expected[128], expected_json[256];
        char dbus[128], dbus_json[256];
        current_smack(smack, sizeof(smack));
        json_escape(smack, smack_json, sizeof(smack_json));
        json_escape(g_smack_expected, expected, sizeof(expected));
        json_escape(expected, expected_json, sizeof(expected_json));
        helper_smack(cfg.dbus_send_path, dbus, sizeof(dbus));
        json_escape(dbus, dbus_json, sizeof(dbus_json));
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"build\":\"%s\",\"config_version\":%d,"
                 "\"smack\":\"%s\",\"smack_expected\":\"%s\","
                 "\"smack_ok\":%s,\"smack_dbus\":\"%s\","
                 "\"home_seq\":%d,\"reboot_state\":\"%s\"}\n",
                 TWEAKS_BUILD_ID, CONFIG_VERSION, smack_json, expected_json,
                 smack_ok(smack) ? "true" : "false", dbus_json,
                 g_home_seq, reboot_state_name());
        send_response(fd, 200, body);
    } else if (!strcmp(decoded, "/status")) {
        build_status(body, sizeof(body));
        send_response(fd, 200, body);
    } else if (!strcmp(decoded, "/reboot") && !strcmp(method, "POST")) {
        /* Reject browser cross-origin calls.  A custom header cannot be sent
         * by a plain cross-site form/fetch without a preflight, which this
         * server does not answer. */
        if (!headers || remote_origin(headers) ||
            !header_value_is(headers, "X-Reboot-App", "1")) {
            send_response(fd, 403,
                          "{\"ok\":false,\"reason\":\"forbidden\"}\n");
        } else if (rt.reboot_state == 1) {
            send_response(fd, 409,
                          "{\"ok\":false,\"reason\":\"already_rebooting\"}\n");
        } else if (update_running()) {
            send_response(fd, 409,
                          "{\"ok\":false,\"reason\":\"update_in_progress\","
                          "\"error\":\"An update is running\"}\n");
        } else {
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"state\":\"rebooting\","
                     "\"wdt_ms\":%d}\n", cfg.reboot_wdt_ms);
            /* Answer first so the UI can render "Restarting" before the
             * clean shutdown starts. */
            send_response(fd, 202, body);
            start_reboot();
            close(fd);
            return;
        }
    } else if (!strcmp(decoded, "/set") && !strcmp(method, "POST")) {
        static const char *keys[] = {
            "cluster_fix", "infotainment_fix", "cluster_crtc", "main_crtc",
            "warmth_override", "force_filter_on", "deep_red", "disclaimer_auto",
            "disclaimer_timeout_ms", "auto_dst", "dst_zone", "dst_check_ms",
            "ota_block", "can_service", NULL
        };
        int changed = 0;
        for (int i = 0; keys[i]; i++) {
            if (query_param(query, keys[i], val, sizeof(val))) {
                if (apply_config_key(keys[i], val))
                    changed = 1;
            }
        }
        if (changed) {
            save_config();
            write_disclaimer_runtime();
            g_last_vendor_read = 0;
            rt.last_cluster_gamma[0] = 0;
            rt.last_main_gamma[0] = 0;
            rt.dst_checked_at = 0;
            rt.dst_checked_ms = 0;
            rt.ota_last_attempt = 0;
            rt.ota_client_pid = 0;
            tick();
        }
        build_status(body, sizeof(body));
        send_response(fd, 200, body);
    } else if (!strcmp(decoded, "/apply") && !strcmp(method, "POST")) {
        rt.last_cluster_gamma[0] = 0;
        rt.last_main_gamma[0] = 0;
        tick();
        build_status(body, sizeof(body));
        send_response(fd, 200, body);
    } else if (!strcmp(decoded, "/restore") && !strcmp(method, "POST")) {
        cfg.cluster_fix = 0;
        cfg.infotainment_fix = 0;
        cfg.deep_red = 0;
        save_config();
        release_overrides();
        write_cluster_desired();
        write_status();
        build_status(body, sizeof(body));
        send_response(fd, 200, body);
    } else if (!strcmp(decoded, "/identify") && !strcmp(method, "POST")) {
        int crtc, seconds = 3;
        if (query_param(query, "crtc", val, sizeof(val)))
            crtc = atoi(val);
        else
            crtc = cfg.main_crtc;
        if (query_param(query, "seconds", val, sizeof(val)))
            seconds = atoi(val);
        if (seconds < 1)
            seconds = 1;
        if (seconds > 10)
            seconds = 10;
        /* Only the front display is a wrandr output. The cluster is driven
         * by micom CAN; painting a "cluster CRTC" is not a valid diagnostic. */
        if (crtc != cfg.main_crtc) {
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"CRTC %d is not the front display "
                     "(main_crtc=%d); the cluster is CAN-driven\"}\n",
                     crtc, cfg.main_crtc);
            send_response(fd, 400, body);
        } else {
            rt.identify_crtc = crtc;
            rt.identify_until = mono_s() + seconds;
            tick();
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"crtc\":%d,\"seconds\":%d}\n", crtc, seconds);
            send_response(fd, 200, body);
        }
    } else if (!strcmp(decoded, "/logs")) {
        char logpath[MAX_PATH];
        int got;
        snprintf(logpath, sizeof(logpath), "%s/daemon.log", cfg.rw_dir);
        got = read_file(logpath, body, sizeof(body));
        if (got < 0) {
            send_response(fd, 404, "{\"ok\":false,\"error\":\"no log\"}\n");
        } else {
            send_response(fd, 200, body);
        }
    } else {
        send_response(fd, 404, "{\"ok\":false,\"error\":\"not found\"}\n");
    }
    close(fd);
}

static int create_server(int port)
{
    int fd, one = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    listen(fd, 8);
    return fd;
}

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--rw-dir DIR] [--port N] [--status] [--set key=value] "
            "[--dst-at EPOCH] [--no-dbus] [--foreground] "
            "[--reboot-command PATH] [--reboot-wdt-ms N]\n", argv0);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int no_dbus = 0;
    int do_status = 0;
    int once = 0;
    char set_key[128] = "", set_value[256] = "";
    char rw_override[MAX_PATH] = "";
    char reboot_override[MAX_PATH] = "";
    int reboot_wdt_override = -1;
    time_t dst_at = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rw-dir") && i + 1 < argc)
            snprintf(rw_override, sizeof(rw_override), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--status"))
            do_status = 1;
        else if (!strcmp(argv[i], "--once"))
            once = 1;
        else if (!strcmp(argv[i], "--no-dbus"))
            no_dbus = 1;
        else if (!strcmp(argv[i], "--foreground"))
            ;
        else if (!strcmp(argv[i], "--reboot-command") && i + 1 < argc)
            snprintf(reboot_override, sizeof(reboot_override), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--reboot-wdt-ms") && i + 1 < argc)
            reboot_wdt_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dst-at") && i + 1 < argc)
            dst_at = (time_t)strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char *eq;
            snprintf(set_value, sizeof(set_value), "%s", argv[++i]);
            eq = strchr(set_value, '=');
            if (eq) {
                *eq = 0;
                snprintf(set_key, sizeof(set_key), "%s", set_value);
                snprintf(set_value, sizeof(set_value), "%s", eq + 1);
            }
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    memset(&rt, 0, sizeof(rt));
    rt.identify_crtc = -1;
    rt.ota_child = -1;
    rt.dst_last_value = -1;
    rt.dst_raw = -1;
    rt.dst_expected = -1;
    rt.reboot_child = -1;
    rt.reboot_wdt_ok = -1;
    snprintf(rt.dst_state, sizeof(rt.dst_state), "unknown");
    snprintf(rt.ota_state, sizeof(rt.ota_state), "unknown");
    g_now_override = dst_at;
    /* One-shot modes exit after a single tick: no dbus-monitors, or they
     * would be orphaned when this process returns. */
    if (once || do_status || set_key[0])
        g_once_mode = 1;
    if (rw_override[0])
        snprintf(g_rw_override, sizeof(g_rw_override), "%s", rw_override);
    load_config();
    if (reboot_override[0])
        snprintf(cfg.reboot_command, sizeof(cfg.reboot_command), "%s",
                 reboot_override);
    if (reboot_wdt_override >= 0)
        cfg.reboot_wdt_ms = clamp_int(reboot_wdt_override, 0, REBOOT_WDT_MAX_MS);
    mkdir_p(cfg.rw_dir);
    load_expected_smack();
    if (g_config_migrated)
        save_config();
    load_last_gamma();
    load_front_override();
    load_tzdata_version();
    write_disclaimer_runtime();

    if (set_key[0]) {
        if (!apply_config_key(set_key, set_value)) {
            fprintf(stderr, "unknown setting: %s\n", set_key);
            return 2;
        }
        save_config();
        write_disclaimer_runtime();
        read_vendor_state();
        update_projection();
        rt.dst_checked_at = 0;
        rt.dst_checked_ms = 0;
        rt.ota_last_attempt = 0;
        rt.ota_client_pid = 0;
        tick();
        drain_ota_child(12);
        {
            char body[8192];
            build_status(body, sizeof(body));
            fputs(body, stdout);
        }
        return 0;
    }

    read_vendor_state();
    update_projection();
    if (once) {
        tick();
        drain_ota_child(12);
    }
    if (do_status || once) {
        char body[8192];
        build_status(body, sizeof(body));
        fputs(body, stdout);
        return 0;
    }

    if (!no_dbus) {
        g_monitor_enabled = 1;
        start_dbus_monitor();
    } else {
        g_monitor_enabled = 0;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = create_server(port);
    if (listen_fd < 0) {
        log_msg("cannot bind 127.0.0.1:%d: %s", port, strerror(errno));
        return 1;
    }
    log_msg("tweaks daemon started port=%d rw=%s", port, cfg.rw_dir);
    set_message("starting");
    write_status();

    while (!stop_flag) {
        struct pollfd fds[4];
        int nfds = 1, pr;
        int timeout = cfg.poll_ms;
        int monitor_fired = 0, home_fired = 0;
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        for (int i = 0; i < 2; i++) {
            if (g_mon[i].fd >= 0) {
                fds[nfds].fd = g_mon[i].fd;
                fds[nfds].events = POLLIN | POLLHUP;
                nfds++;
            }
        }
        if (g_home_fd >= 0) {
            fds[nfds].fd = g_home_fd;
            fds[nfds].events = POLLIN | POLLHUP;
            nfds++;
        }
        /* A pending reboot needs a tight poll so the 20 s fallback fires
         * even when the normal status cadence is one second. */
        if (rt.reboot_state == 1 && timeout > 100)
            timeout = 100;
        pr = poll(fds, (nfds_t)nfds, timeout);
        if (pr > 0) {
            if (fds[0].revents & POLLIN) {
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd >= 0)
                    handle_http(cfd);
            }
            for (int i = 1; i < nfds; i++) {
                if (fds[i].revents & (POLLIN | POLLHUP)) {
                    if (fds[i].fd == g_home_fd)
                        home_fired = 1;
                    else
                        monitor_fired = 1;
                }
            }
            if (monitor_fired)
                read_monitor();
            if (home_fired)
                home_monitor_read();
        }
        tick();
    }

    log_msg("tweaks daemon stopping");
    release_overrides();
    monitor_cleanup();
    if (rt.ota_child > 0) {
        kill(rt.ota_child, SIGTERM);
        waitpid(rt.ota_child, NULL, 0);
    }
    if (listen_fd >= 0)
        close(listen_fd);
    return 0;
}
