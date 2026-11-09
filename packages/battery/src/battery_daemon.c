/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/*
 * battery_daemon — battery preconditioning control for Ioniq 5 Gen5W (260128)
 *
 * The head unit's can0 is I-CAN.  The daemon reads the I-CAN frames it can
 * actually see and sends M-CAN commands through the micom D-Bus service.
 *
 * I-CAN (can0):
 *   0x2FC  RX  SoC byte7/2, battery min temp in byte 2 (signed)
 *   0x326  RX  charge connector, (byte3 >> 2) & 3 != 0 or (byte4 & 3) != 0
 *   0x25D  RX  BMS_04: EV_BatteryConditioning in (data[5] >> 6) & 3
 *   0x036  RX  VCU gear (raw frame kept; byte offset differs between factory
 *              binaries — AppHomeScreen reads data[3]&3, TmsRemoteApp data[4]&0xF)
 *
 * M-CAN (via com.lge.car.micom /Can SetCANMicomRecv), sequence fully mimics
 * the WiCAN "i-precondition" firmware (L1Z3/wicant-i-precondition):
 *   0x0C7  TX  BMU control start/stop burst (3+3 frames, 40 ms apart)
 *   0x4E8  TX  cluster distance/flag countdown display (WiCAN layout)
 *   0x4CC  TX  cluster icon request, data[0] = 0x02
 *   0x4A2  TX  charge door open (factory TmsRemoteApp frames: all-FF, then DF)
 *   0x4C5  TX  charge door close (factory uses a distinct ID for close)
 *
 * M-CAN receive frames (0x036 gear, 0x038 READY, 0x152 temperature, 0x2AD
 * precondition state, 0x2AE utility, 0x3AA charge door) are not always
 * visible on can0.  The QML page reads those from AppPHEVModel /
 * AppNModeModel instead.  The daemon still decodes them if they appear
 * (bench tests, other variants).
 *
 * This is research tooling for a rooted head unit.  It does not flash
 * firmware and does not touch the updater or bootloader.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define BATTERY_DAEMON_VERSION "2026-11-10.2"

#define IFACE_DEFAULT "can0"
#define HTTP_PORT 8088
#define STATUS_PATH "/rw_data/battery/status.json"

#define ID_BMU 0x0C7
#define ID_CLUSTER 0x4E8        /* cluster distance/flag display */
#define ID_CLUSTER_ICON 0x4CC   /* cluster icon request, data[0] = 0x02 */
#define ID_BATT_TEMP 0x152
#define ID_BATT_COND 0x25D
#define ID_SOC 0x2FC
#define ID_PRECOND 0x2AD
#define ID_POWER 0x038
#define ID_UTIL 0x2AE
#define ID_GEAR 0x036
#define ID_CHARGE 0x326
#define ID_CHARGEDOOR 0x3AA

/* WiCAN cluster display encoding for 0x4E8:
 *   data[0] = (distance_tenths & 0xF) << 4 | (unit & 0xF)
 *   data[4] = distance_int & 0xFF
 *   data[5] = (distance_int >> 8) & 0xFF
 *   data[6] = flag & 0xF
 * units: 0 m, 1 km, 2 mi, 3 ft, 4 yd; flags: 0 destination, 1..4 blue, F none */
#define DIST_UNIT_M 0x0
#define DIST_UNIT_KM 0x1
#define DIST_UNIT_MI 0x2
#define DIST_UNIT_FT 0x3
#define DIST_UNIT_YD 0x4
#define FLAG_DESTINATION 0x0
#define FLAG_BLUE_1 0x1
#define FLAG_BLUE_4 0x4
#define FLAG_NONE 0xF

#define DOOR_ID_OPEN 0x4A2
#define DOOR_ID_CLOSE 0x4C5
#define DOOR_STATE_UNKNOWN 0
#define DOOR_STATE_OPEN 1
#define DOOR_STATE_CLOSED 2
#define DOOR_SIGNAL_STALE_MS 2000
#define DOOR_CMD_TIMEOUT_MS 2000
#define DOOR_CMD_ATTEMPTS 2
#define DOOR_CMD_COOLDOWN_MS 500
#define DOOR_DBUS_TIMEOUT_MS 1500

#define SOC_CUTOFF_PCT 20
#define TEMP_CUTOFF_C 21
#define START_PHASE1 3
#define START_PHASE2 3
#define STOP_PHASE1 3
#define STOP_PHASE2 3
#define BURST_TICKS (START_PHASE1 + START_PHASE2)
#define BURST_INTERVAL_MS 40          /* WiCAN: one frame per 40 ms tick */
#define START_RETRY_MS 10000
#define STARTED_TIMEOUT_MS 70000      /* WiCAN started timeout (Ioniq 5) */
#define STOP_RETRY_MS 10000
#define MAX_RETRIES 4
#define DISPLAY_INTERVAL_MS 100       /* cluster display injection period */
#define VEHICLE_IDLE_DEBOUNCE_MS 2000
#define TEMP_STALE_MS 15000
#define SOC_STALE_MS 15000

typedef enum {
    ST_IDLE = 0,
    ST_STARTING,
    ST_PREPARING,
    ST_ACTIVE,
    ST_STOPPING,
    ST_TIMEOUT
} state_t;

typedef enum { BURST_NONE = 0, BURST_START, BURST_STOP } burst_t;

static volatile sig_atomic_t g_stop = 0;
static int g_can_fd = -1;
static const char *g_iface = IFACE_DEFAULT;

static state_t g_state = ST_IDLE;
static burst_t g_burst = BURST_NONE;
static int g_burst_tick = 0;
static int64_t g_burst_next_ms = 0;
static int64_t g_attempt_ms = 0;
static int g_retries = 0;
static int64_t g_last_rx_precond_ms = 0;
static int64_t g_last_idle_ms = 0;

static int g_precond_raw = 0;
static bool g_precond_known = false;
static int g_batt_cond = 0;          /* EV_BatteryConditioning, 0x25D */
static bool g_batt_cond_known = false;
static int64_t g_batt_cond_ms = 0;
static uint8_t g_gear_frame[8];
static bool g_gear_frame_valid = false;
static int g_batt_min_c = 0;
static bool g_batt_valid = false;
static int64_t g_batt_ms = 0;
static const char *g_temp_source = "none";
static uint8_t g_soc_frame[8];
static bool g_soc_frame_valid = false;
static int g_soc_raw = 0;
static bool g_soc_valid = false;
static int64_t g_soc_ms = 0;
static bool g_ready = false;
static bool g_utility = false;
static bool g_factory_nav = false;
static int64_t g_factory_nav_ms = 0;

/* Manual "cluster display test" toggle (independent of preheat). */
static bool g_dash_override = false;
static int64_t g_display_next_ms = 0;

/* Cleanup stop: start retries exhausted -> one stop burst, no stop retries
 * (the WiCAN "retries exhausted in once mode" path). */
static bool g_stop_cleanup = false;

/* Cluster display state (WiCAN mimicry). */
static uint8_t g_display_last[8];   /* our last 0x4E8 payload */

/* Test seams for the retry/timeout pacing (bench tests only). */
static int64_t g_started_timeout_ms = STARTED_TIMEOUT_MS;
static int64_t g_start_retry_ms = START_RETRY_MS;
static int64_t g_stop_retry_ms = STOP_RETRY_MS;

/* micom send health: every dbus-send child is reaped; non-zero exits are
 * logged and counted so the UI can tell a failed send from a sent frame. */
static volatile int g_send_oks = 0;
static volatile int g_send_fails = 0;
static char g_last_send_cmd[20] = "";
static volatile int g_last_send_status = -1;

/* Charge door state (0x036 park, 0x326 cable, 0x3AA door). */
static int g_gear = -1;
static bool g_park = false;
static bool g_park_known = false;
static int64_t g_gear_ms = 0;
static bool g_cable = false;
static bool g_cable_known = false;
static bool g_charging = false;
static int64_t g_cable_ms = 0;
static int g_door_state = DOOR_STATE_UNKNOWN;
static bool g_door_known = false;
static int g_door_warn = 0;
static int g_door_opt = 0;
static int64_t g_door_ms = 0;
static bool g_door_assumed = false;
static int64_t g_door_assumed_ms = 0;
static bool g_door_busy = false;
static int g_door_target = DOOR_STATE_UNKNOWN;
static int g_door_attempts = 0;
static int64_t g_door_deadline_ms = 0;
static int64_t g_door_last_ms = 0;
static char g_door_msg[64] = "unknown";
static const char *g_dbus_send = "/usr/bin/dbus-send";
/* The charge-door feedback tracks the exit status of its OWN dbus-send
 * child.  A single shared status would be clobbered by the burst and dash
 * sends (the dash forks a child every 500 ms), masking a failed door
 * command as a success. */
static pid_t g_door_send_pid = -1;
static volatile int g_door_dbus_status = -1;
static char g_last_tx[64] = "";
static bool g_sniff = false;
static int g_sniff_seconds = 10;

/* ---------------- session-bus Home/Star monitor ----------------
 *
 * The steering-wheel Home/Star key never reaches QML as a key event.  The
 * app manager delivers it over the session bus (address written by
 * Dbuslaunch.sh to /tmp/session_appmgr):
 *
 *   - Home request: method call NotifyRequestHomeScreen on
 *     com.lge.hkmc.avn.appfunc.appInterface, destination AppHomeScreen.
 *   - Star/custom key: signal custom_key_value(int) on
 *     com.lge.avn.appmanager.info.
 *
 * The daemon tails dbus-monitor for those two members and bumps home_seq so
 * the QML overlay can close itself.  If the bus or the monitor is missing it
 * retries every 5 s and reports home_monitor=false. */

static const char *g_session_bus_file = "/tmp/session_appmgr";
static const char *g_dbus_monitor = "/usr/bin/dbus-monitor";
static pid_t g_mon_pid = -1;
static int g_mon_fd = -1;
static int64_t g_mon_retry_ms = 0;
static int g_home_seq = 0;
static char g_home_source[12] = "none";
static int g_last_app_key = -1;
static bool g_home_monitor = false;
static char g_mon_line[512];
static size_t g_mon_line_len = 0;
static char g_mon_member[48] = "";

static char g_message[160] = "idle";
static const char *g_status_path = STATUS_PATH;

/* SMACK diagnostics.  The installer stamps SMACK64EXEC on the daemon and its
 * helpers with its own label (normally the floor label "_"), and writes the
 * expected value next to the status file.  The daemon reports both so the QML
 * and the USB diagnostic can prove the privileged side really runs in the
 * installer context. */
static char g_smack[64] = "unknown";
static char g_smack_expected[64] = "";
static bool g_smack_ok = true;
static char g_smack_send[64] = "unknown";
static char g_smack_monitor[64] = "unknown";

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void trim_label(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = '\0';
}

static void read_label(const char *path, char *out, size_t sz, const char *fallback)
{
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out, sz - 1);
        close(fd);
        if (n > 0) {
            out[n] = '\0';
            trim_label(out);
        }
    }
    if (out[0] == '\0')
        snprintf(out, sz, "%s", fallback ? fallback : "unknown");
}

static void helper_label(const char *path, char *out, size_t sz)
{
    char val[128];
    ssize_t n = getxattr(path, "security.SMACK64EXEC", val, sizeof(val) - 1);
    if (n > 0) {
        val[n] = '\0';
        trim_label(val);
        if (val[0]) {
            size_t len = strlen(val);
            if (len >= sz) len = sz - 1;
            memcpy(out, val, len);
            out[len] = '\0';
            return;
        }
    }
    /* No exec label: the helper inherits the daemon's label. */
    snprintf(out, sz, "inherited");
}

static void init_smack_diag(void)
{
    read_label("/proc/self/attr/current", g_smack, sizeof(g_smack), "unknown");

    char path[256];
    path[0] = '\0';
    const char *slash = strrchr(g_status_path, '/');
    if (slash)
        snprintf(path, sizeof(path), "%.*s/smack_expected",
                 (int)(slash - g_status_path), g_status_path);
    if (path[0])
        read_label(path, g_smack_expected, sizeof(g_smack_expected), "");
    g_smack_ok = (g_smack_expected[0] == '\0' ||
                  strcmp(g_smack, g_smack_expected) == 0);

    helper_label(g_dbus_send, g_smack_send, sizeof(g_smack_send));
    helper_label(g_dbus_monitor, g_smack_monitor, sizeof(g_smack_monitor));

    fprintf(stderr,
            "battery_daemon: smack=%s expected=%s ok=%s "
            "helper_send=%s helper_monitor=%s\n",
            g_smack,
            g_smack_expected[0] ? g_smack_expected : "(none)",
            g_smack_ok ? "true" : "false",
            g_smack_send, g_smack_monitor);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void set_message(const char *m)
{
    snprintf(g_message, sizeof(g_message), "%s", m);
}

/* ---------------- CAN ---------------- */

static int can_open(const char *iface, bool filtered)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl(SIOCGIFINDEX)");
        close(fd);
        return -1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(AF_CAN)");
        close(fd);
        return -1;
    }

    struct can_filter filter[10];
    filter[0].can_id = ID_BATT_TEMP; filter[0].can_mask = CAN_SFF_MASK;
    filter[1].can_id = ID_BATT_COND; filter[1].can_mask = CAN_SFF_MASK;
    filter[2].can_id = ID_SOC;       filter[2].can_mask = CAN_SFF_MASK;
    filter[3].can_id = ID_PRECOND;   filter[3].can_mask = CAN_SFF_MASK;
    filter[4].can_id = ID_POWER;     filter[4].can_mask = CAN_SFF_MASK;
    filter[5].can_id = ID_UTIL;      filter[5].can_mask = CAN_SFF_MASK;
    filter[6].can_id = ID_CLUSTER;   filter[6].can_mask = CAN_SFF_MASK;
    filter[7].can_id = ID_GEAR;      filter[7].can_mask = CAN_SFF_MASK;
    filter[8].can_id = ID_CHARGE;    filter[8].can_mask = CAN_SFF_MASK;
    filter[9].can_id = ID_CHARGEDOOR;filter[9].can_mask = CAN_SFF_MASK;
    if (filtered &&
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filter, sizeof(filter)) < 0) {
        perror("setsockopt(CAN_RAW_FILTER)");
        close(fd);
        return -1;
    }

    /* Non-blocking so the poll loop can drain frames and still serve HTTP. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

/* Send an 8-byte M-CAN frame through the factory micom service.  The micom
 * array is [CAN ID hi, CAN ID lo, DLC, data0..data7].  Returns the child pid
 * (>= 0) so callers that need the exit status can track it. */
static int send_micom_frame(uint16_t can_id, const uint8_t data[8])
{
    char arg[200];
    int n = snprintf(arg, sizeof(arg),
                     "array:byte:0x%02x,0x%02x,0x08,0x%02x,0x%02x,0x%02x,"
                     "0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
                     (can_id >> 8) & 0xFF, can_id & 0xFF,
                     data[0], data[1], data[2], data[3],
                     data[4], data[5], data[6], data[7]);
    if (n < 0 || (size_t)n >= sizeof(arg)) return -1;

    snprintf(g_last_tx, sizeof(g_last_tx),
             "%03X %02X %02X %02X %02X %02X %02X %02X %02X",
             can_id, data[0], data[1], data[2], data[3],
             data[4], data[5], data[6], data[7]);
    snprintf(g_last_send_cmd, sizeof(g_last_send_cmd), "%03X",
             can_id);
    if (can_id != ID_CLUSTER && can_id != ID_CLUSTER_ICON) {
        fprintf(stderr, "battery_daemon: tx %s\n", g_last_tx);
        fflush(stderr);
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(g_dbus_send, "dbus-send", "--system",
              "--type=method_call", "--dest=com.lge.car.micom",
              "/Can", "com.lge.car.micom.can.SetCANMicomRecv",
              arg, (char *)NULL);
        _exit(127);
    }
    return (int)pid;
}

static void send_bmu(uint8_t b3, uint8_t b4)
{
    uint8_t d[8] = {0, 0, 0, b3, b4, 0, 0, 0};
    if (send_micom_frame(ID_BMU, d) < 0) {
        set_message("micom send failed");
    }
}

/* WiCAN cluster countdown display: one 0x4E8 (distance/flag) plus one
 * 0x4CC (icon request, data[0] = 0x02). */
static void send_cluster_display(uint16_t distance_int, uint8_t tenths,
                                 uint8_t unit, uint8_t flag)
{
    uint8_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    d[0] = (uint8_t)(((tenths & 0x0F) << 4) | (unit & 0x0F));
    d[4] = (uint8_t)(distance_int & 0xFF);
    d[5] = (uint8_t)((distance_int >> 8) & 0xFF);
    d[6] = (uint8_t)(flag & 0x0F);
    memcpy(g_display_last, d, 8);
    send_micom_frame(ID_CLUSTER, d);

    uint8_t c[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};
    send_micom_frame(ID_CLUSTER_ICON, c);
}

/* ---------------- charge door ---------------- */

static bool door_state_fresh(void)
{
    int64_t now = now_ms();
    if (g_door_known && now - g_door_ms <= DOOR_SIGNAL_STALE_MS) return true;
    if (g_door_assumed && now - g_door_assumed_ms <= 300000) return true;
    return false;
}

static const char *door_source(void)
{
    int64_t now = now_ms();
    if (g_door_known && now - g_door_ms <= DOOR_SIGNAL_STALE_MS) return "can";
    if (g_door_assumed && now - g_door_assumed_ms <= 300000) return "command";
    return "unknown";
}

static const char *door_state_name(void)
{
    if (!door_state_fresh()) return "unknown";
    if (g_door_state == DOOR_STATE_OPEN) return "open";
    if (g_door_state == DOOR_STATE_CLOSED) return "closed";
    return "unknown";
}

static const char *door_block_reason(void)
{
    int64_t now = now_ms();
    /* The gear/Park signal (0x036) is not reliable on this car yet, so it
     * does not gate the charge door.  The cable state (0x326) is invisible
     * on some cars, so an UNKNOWN cable is advisory only and does not block
     * the door.  A cable that is actually observed connected still vetoes. */
    bool cable_fresh = g_cable_known && now - g_cable_ms <= DOOR_SIGNAL_STALE_MS;
    if (cable_fresh && g_cable)
        return "cable_plugged";
    return "";
}

/* The factory TmsRemoteApp sends the charge-door command through the micom
 * D-Bus service with these exact frames (decompiled from the 260128
 * TmsRemoteApp, EVRemoteChargeDoorOpen/Close):
 *   open:  04 A2 08 FF FF FF FF FF FF FF FF, then 04 A2 08 FF FF FF DF FF FF FF FF
 *   close: 04 C5 08 00 00 00 00 00 00 00 00
 * Our previous close frame (04 A2 ... EF ...) does not exist in the factory
 * binary.  Returns the pid of the last send. */
static int send_chargedoor_frame(int target)
{
    if (target == DOOR_STATE_CLOSED) {
        uint8_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        return send_micom_frame(DOOR_ID_CLOSE, d);
    }
    uint8_t ff[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_micom_frame(DOOR_ID_OPEN, ff);
    uint8_t df[8] = {0xFF, 0xFF, 0xFF, 0xDF, 0xFF, 0xFF, 0xFF, 0xFF};
    return send_micom_frame(DOOR_ID_OPEN, df);
}

static void reap_children(void)
{
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        int st = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (p == g_mon_pid) {
            /* The session-bus monitor died: forget the pid so shutdown never
             * kills a recycled one, and never treat its exit as a command
             * result. */
            g_mon_pid = -1;
            continue;
        }
        if (p == g_door_send_pid) {
            g_door_dbus_status = st;
            g_door_send_pid = -1;
            if (st == 0) g_send_oks++;
            else g_send_fails++;
            g_last_send_status = st;
            continue;
        }
        /* Other children (burst/cluster-display sends): fire-and-forget,
         * but count failures so the UI can tell a failed dbus-send from a
         * frame that actually went out. */
        g_last_send_status = st;
        if (st == 0) {
            g_send_oks++;
        } else {
            g_send_fails++;
            fprintf(stderr, "battery_daemon: send failed (%s): exit %d\n",
                    g_last_send_cmd, st);
            fflush(stderr);
        }
    }
}

static void monitor_cleanup(void)
{
    if (g_mon_fd >= 0) { close(g_mon_fd); g_mon_fd = -1; }
    if (g_mon_pid > 0) {
        kill(g_mon_pid, SIGTERM);
        g_mon_pid = -1;
    }
    g_home_monitor = false;
}

static void monitor_start(void)
{
    FILE *f = fopen(g_session_bus_file, "r");
    if (!f) return;
    char addr[256] = "";
    if (!fgets(addr, sizeof(addr), f)) { fclose(f); return; }
    fclose(f);

    char *p = addr;
    while (*p == ' ' || *p == '\t') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' ||
                       p[len-1] == ' ' || p[len-1] == '\t'))
        p[--len] = '\0';
    if (len == 0) return;

    /* A stale guid in the address file would make libdbus refuse the
     * connection after a bus restart; the path alone identifies the bus. */
    char *guid = strstr(p, ",guid=");
    if (guid) *guid = '\0';

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
        execl(g_dbus_monitor, "dbus-monitor", "--address", p,
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
    g_mon_fd = fds[0];
    g_mon_pid = pid;
    g_home_monitor = true;
    g_mon_line_len = 0;
    g_mon_member[0] = '\0';
    fprintf(stderr, "battery_daemon: session-bus monitor on %s\n", p);
    fflush(stderr);
}

/* dbus-monitor prints one header line per message ("... member=X") followed
 * by argument lines ("   int32 7").  Remember the member of the header and
 * pick up the argument on the next line. */
static void monitor_line(const char *line)
{
    if (strstr(line, "member=custom_key_value")) {
        snprintf(g_mon_member, sizeof(g_mon_member), "custom_key_value");
        return;
    }
    if (strstr(line, "member=NotifyRequestHomeScreen")) {
        g_home_seq++;
        snprintf(g_home_source, sizeof(g_home_source), "home");
        g_mon_member[0] = '\0';
        return;
    }
    if (g_mon_member[0]) {
        int v;
        if (sscanf(line, " int32 %d", &v) == 1) {
            g_last_app_key = v;
            g_home_seq++;
            snprintf(g_home_source, sizeof(g_home_source), "star");
        }
        g_mon_member[0] = '\0';
    }
}

static void monitor_tick(void)
{
    if (g_mon_fd < 0) {
        int64_t now = now_ms();
        if (g_session_bus_file[0] && now >= g_mon_retry_ms) {
            g_mon_retry_ms = now + 5000;
            monitor_start();
        }
        return;
    }
    char buf[1024];
    for (;;) {
        ssize_t n = read(g_mon_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (g_mon_line_len > 0) {
                        g_mon_line[g_mon_line_len] = '\0';
                        monitor_line(g_mon_line);
                        g_mon_line_len = 0;
                    }
                } else if (g_mon_line_len + 1 < sizeof(g_mon_line)) {
                    g_mon_line[g_mon_line_len++] = c;
                }
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        /* EOF or real error: the monitor died (bus shutdown); retry later. */
        close(g_mon_fd);
        g_mon_fd = -1;
        g_home_monitor = false;
        return;
    }
}

static int request_chargedoor(int target)
{
    int64_t now = now_ms();
    const char *why = door_block_reason();
    if (why[0]) {
        snprintf(g_door_msg, sizeof(g_door_msg), "%s", why);
        return -1;
    }
    if (g_door_busy) {
        snprintf(g_door_msg, sizeof(g_door_msg), "busy");
        return -1;
    }

    const char *st = door_state_name();
    if (target == DOOR_STATE_OPEN && !strcmp(st, "open")) {
        snprintf(g_door_msg, sizeof(g_door_msg), "open");
        return 0;
    }
    if (target == DOOR_STATE_CLOSED && !strcmp(st, "closed")) {
        snprintf(g_door_msg, sizeof(g_door_msg), "closed");
        return 0;
    }
    if (g_door_last_ms && now - g_door_last_ms < DOOR_CMD_COOLDOWN_MS) {
        snprintf(g_door_msg, sizeof(g_door_msg), "cooldown");
        return -1;
    }

    g_door_target = target;
    g_door_attempts = 1;
    g_door_busy = true;
    g_door_send_pid = -1;
    g_door_dbus_status = -1;
    g_door_deadline_ms = now + DOOR_CMD_TIMEOUT_MS;
    g_door_last_ms = now;
    snprintf(g_door_msg, sizeof(g_door_msg), "%s",
             target == DOOR_STATE_CLOSED ? "closing" : "opening");
    g_door_send_pid = send_chargedoor_frame(target);
    if (g_door_send_pid < 0) {
        g_door_busy = false;
        g_door_attempts = 0;
        g_door_send_pid = -1;
        snprintf(g_door_msg, sizeof(g_door_msg), "send_failed");
        return -1;
    }
    return 0;
}

static void door_finish(const char *msg, bool ok)
{
    g_door_busy = false;
    snprintf(g_door_msg, sizeof(g_door_msg), "%s", msg);
    if (ok) {
        g_door_state = g_door_target;
        g_door_assumed = true;
        g_door_assumed_ms = now_ms();
    }
}

static void tick_chargedoor(int64_t now)
{
    if (!g_door_busy) return;

    bool can_fresh = g_door_known && now - g_door_ms <= DOOR_SIGNAL_STALE_MS;
    if (can_fresh) {
        if (g_door_state == g_door_target) {
            g_door_assumed = false;
            door_finish(g_door_target == DOOR_STATE_OPEN ? "open" : "closed", true);
            return;
        }
    } else if (g_door_dbus_status == 0 && now - g_door_last_ms >= 1200) {
        /* No door-state frame on this bus.  A successful micom send is the
         * best feedback available; remember the commanded state. */
        door_finish(g_door_target == DOOR_STATE_OPEN ? "open" : "closed", true);
        return;
    } else if (g_door_dbus_status > 0 && now - g_door_last_ms >= 600) {
        if (g_door_attempts < DOOR_CMD_ATTEMPTS) {
            g_door_attempts++;
            g_door_dbus_status = -1;
            g_door_send_pid = -1;
            g_door_last_ms = now;
            g_door_send_pid = send_chargedoor_frame(g_door_target);
            if (g_door_send_pid < 0) door_finish("send_failed", false);
            return;
        }
        door_finish("send_failed", false);
        return;
    }

    if (now < g_door_deadline_ms) return;

    if (g_door_attempts < DOOR_CMD_ATTEMPTS) {
        g_door_attempts++;
        g_door_dbus_status = -1;
        g_door_send_pid = -1;
        g_door_last_ms = now;
        g_door_deadline_ms = now + DOOR_CMD_TIMEOUT_MS;
        g_door_send_pid = send_chargedoor_frame(g_door_target);
        if (g_door_send_pid < 0) door_finish("send_failed", false);
        return;
    }

    door_finish("no_response", false);
}

/* ---------------- state helpers ---------------- */

static bool precond_active(void)
{
    return g_state == ST_PREPARING || g_state == ST_ACTIVE;
}

static const char *blocked_reason(void)
{
    /* The BMS / factory setting is the authority for READY, SoC and utility.
     * The one local rule we keep is the WiCAN temperature cutoff: do not
     * start preheating when the minimum battery temperature is at or above
     * 21 C. */
    if (g_state == ST_STOPPING) return "stopping";
    if (g_batt_valid && now_ms() - g_batt_ms <= TEMP_STALE_MS &&
        g_batt_min_c >= TEMP_CUTOFF_C)
        return "temp_high";
    return "";
}

static void enter_idle(const char *msg)
{
    g_state = ST_IDLE;
    g_burst = BURST_NONE;
    g_retries = 0;
    set_message(msg);
}

static void start_burst(void)
{
    g_burst = BURST_START;
    g_burst_tick = 0;
    g_burst_next_ms = now_ms();
    g_attempt_ms = now_ms();
    g_state = ST_STARTING;
}

static void stop_burst(void)
{
    g_burst = BURST_STOP;
    g_burst_tick = 0;
    g_burst_next_ms = now_ms();
    g_attempt_ms = now_ms();
    g_state = ST_STOPPING;
}

static int request_start(void)
{
    if (precond_active() || g_state == ST_STARTING) {
        return 0;
    }
    if (g_state == ST_STOPPING) {
        set_message("stopping");
        return -1;
    }
    const char *why = blocked_reason();
    if (why[0]) {
        set_message(why);
        return -1;
    }
    g_retries = 0;
    start_burst();
    set_message("starting");
    fprintf(stderr, "battery_daemon: start request\n");
    fflush(stderr);
    return 0;
}

static int request_stop(void)
{
    if (g_state == ST_STOPPING) {
        return 0;
    }
    /* Always send the stop burst.  The local 0x2AD state is M-CAN and may be
     * invisible on this bus; the BMS ignores a stop if it is already idle. */
    g_retries = 0;
    g_stop_cleanup = false;
    stop_burst();
    set_message("stopping");
    fprintf(stderr, "battery_daemon: stop request\n");
    fflush(stderr);
    return 0;
}

static void tick_burst(int64_t now)
{
    if (g_burst == BURST_NONE || now < g_burst_next_ms) return;

    if (g_burst == BURST_START) {
        if (g_burst_tick < START_PHASE1) send_bmu(0x40, 0x03);
        else send_bmu(0xE0, 0x07);
    } else {
        if (g_burst_tick < STOP_PHASE1) send_bmu(0x00, 0x00);
        else send_bmu(0xE0, 0x07);
    }

    g_burst_tick++;
    g_burst_next_ms = now + BURST_INTERVAL_MS;
    if (g_burst_tick >= BURST_TICKS) {
        g_burst = BURST_NONE;
        g_attempt_ms = now;
        if (g_state == ST_STARTING) {
            /* wait for 0x2AD */
        } else if (g_state == ST_STOPPING) {
            if (g_precond_raw == 0x01) enter_idle("idle");
        }
    }
}

static void tick_state(int64_t now)
{
    /* Follow the BMS if it reports a newer state than we tracked. This also
     * recovers from a local timeout when the car eventually starts heating. */
    if (g_state != ST_IDLE && g_state != ST_STOPPING) {
        if (g_precond_raw == 0x15 && g_state != ST_ACTIVE) {
            g_state = ST_ACTIVE;
            set_message("active");
        } else if (g_precond_raw == 0x05 &&
                   (g_state == ST_STARTING || g_state == ST_TIMEOUT)) {
            g_state = ST_PREPARING;
            set_message("preparing");
        }
    }

    if (g_state == ST_STARTING) {
        if (!g_precond_known) {
            /* WiCAN: without status frames there is nothing to confirm or
             * retry; after the started timeout, assume it worked. */
            if (g_burst == BURST_NONE &&
                now - g_attempt_ms > g_started_timeout_ms) {
                g_state = ST_ACTIVE;
                set_message("active");
            }
        } else if (g_precond_raw == 0x05) {
            g_state = ST_PREPARING;
            set_message("preparing");
        } else if (g_precond_raw == 0x15) {
            g_state = ST_ACTIVE;
            set_message("active");
        } else if (g_burst == BURST_NONE &&
                   now - g_attempt_ms > g_start_retry_ms) {
            if (g_retries < MAX_RETRIES) {
                g_retries++;
                start_burst();
                set_message("retrying");
            } else {
                /* WiCAN: retries exhausted in once mode -> send one stop
                 * request without retrying it, then go idle. */
                g_retries = MAX_RETRIES;
                g_stop_cleanup = true;
                stop_burst();
                set_message("stopping");
            }
        }
    } else if (g_state == ST_PREPARING) {
        if (g_precond_raw == 0x15) {
            g_state = ST_ACTIVE;
            set_message("active");
        } else if (now - g_attempt_ms > g_started_timeout_ms) {
            if (g_retries < MAX_RETRIES) {
                g_retries++;
                start_burst();
                set_message("retrying");
            } else {
                g_retries = MAX_RETRIES;
                g_stop_cleanup = true;
                stop_burst();
                set_message("stopping");
            }
        }
    } else if (g_state == ST_ACTIVE) {
        if (g_precond_known &&
            (g_precond_raw == 0x00 || g_precond_raw == 0x01)) {
            if (g_last_idle_ms == 0) g_last_idle_ms = now;
            if (now - g_last_idle_ms > VEHICLE_IDLE_DEBOUNCE_MS) {
                enter_idle("stopped_by_vehicle");
            }
        } else {
            g_last_idle_ms = 0;
        }
    } else if (g_state == ST_STOPPING) {
        /* 0x01 is the documented idle status; 0x00 is this car's "off".
         * Either way the stop has taken effect.  Without any BMS state on
         * this bus the completed burst itself is the confirmation.  A
         * cleanup stop (start retries exhausted) never retries. */
        if (g_burst == BURST_NONE &&
            (g_stop_cleanup || !g_precond_known ||
             g_precond_raw == 0x00 || g_precond_raw == 0x01)) {
            bool cleanup = g_stop_cleanup;
            g_stop_cleanup = false;
            enter_idle((g_precond_known && !cleanup) ? "idle" : "stopped");
        } else if (g_burst == BURST_NONE && now - g_attempt_ms > g_stop_retry_ms) {
            if (g_retries < MAX_RETRIES) {
                g_retries++;
                stop_burst();
                set_message("stopping");
            } else {
                enter_idle("stop_failed");
            }
        }
    }

    /* The cluster display follows the state machine; nothing else to do. */
}

static void tick_display(int64_t now)
{
    if (now < g_display_next_ms) return;
    g_display_next_ms = now + DISPLAY_INTERVAL_MS;

    /* If the head unit streams its own 0x4E8 (route display), back off. */
    if (g_factory_nav && now - g_factory_nav_ms < 3000) return;

    if (g_state == ST_STARTING || g_state == ST_PREPARING) {
        /* Start-phase countdown: seconds remaining of the started timeout,
         * retry count in the tenths digit (WiCAN starting_display_fwd). */
        int64_t elapsed = now - g_attempt_ms;
        uint32_t remain = elapsed >= g_started_timeout_ms
                          ? 0
                          : (uint32_t)((g_started_timeout_ms - elapsed) / 1000);
        uint8_t unit, flag;
        if (g_precond_known) {
            unit = (g_retries == 0) ? DIST_UNIT_YD : DIST_UNIT_KM;
            flag = (g_precond_raw == 0x05) ? FLAG_DESTINATION : FLAG_BLUE_1;
        } else {
            unit = DIST_UNIT_M;
            flag = FLAG_BLUE_4;
        }
        send_cluster_display((uint16_t)remain, (uint8_t)(g_retries % 10),
                             unit, flag);
    } else if (g_state == ST_STOPPING && g_precond_known &&
               g_retries < MAX_RETRIES) {
        /* Stop-phase retry countdown (WiCAN stopping_display_fwd). */
        int64_t elapsed = now - g_attempt_ms;
        uint32_t remain = elapsed >= g_stop_retry_ms
                          ? 0
                          : (uint32_t)((g_stop_retry_ms - elapsed) / 1000);
        send_cluster_display((uint16_t)remain, (uint8_t)(g_retries % 10),
                             (g_retries == 0) ? DIST_UNIT_FT : DIST_UNIT_MI,
                             FLAG_NONE);
    } else if (g_dash_override) {
        /* Manual toggle: standalone start-style display, independent of
         * preheat state. */
        uint8_t unit = g_precond_known ? DIST_UNIT_YD : DIST_UNIT_M;
        uint8_t flag = g_precond_known ? FLAG_BLUE_1 : FLAG_BLUE_4;
        send_cluster_display((uint16_t)(g_started_timeout_ms / 1000), 0,
                             unit, flag);
    }
}

/* ---------------- RX ---------------- */

static void handle_frame(const struct can_frame *f)
{
    int64_t now = now_ms();
    switch (f->can_id & CAN_SFF_MASK) {
    case ID_BATT_TEMP:
        /* M-CAN fallback.  Byte 0 is the min temperature; it is only used
         * when the I-CAN 0x2FC temperature is missing or stale. */
        if (!g_batt_valid || now - g_batt_ms > 5000) {
            g_batt_min_c = (int8_t)f->data[0];
            g_batt_valid = true;
            g_batt_ms = now;
            g_temp_source = "0x152";
        }
        break;
    case ID_SOC:
        g_soc_raw = f->data[7];
        g_soc_valid = true;
        g_soc_ms = now;
        memcpy(g_soc_frame, f->data, 8);
        g_soc_frame_valid = true;
        /* The factory CANService uses only byte 2 as the min battery
         * temperature.  Byte 3 is not a reliable max on all variants. */
        g_batt_min_c = (int8_t)f->data[2];
        g_batt_valid = true;
        g_batt_ms = now;
        g_temp_source = "0x2FC";
        break;
    case ID_PRECOND: {
        int raw = f->data[1] & 0x1F;
        if (!g_precond_known || raw != g_precond_raw) {
            fprintf(stderr, "battery_daemon: bms 0x%02X\n", raw);
            fflush(stderr);
        }
        g_precond_raw = raw;
        g_precond_known = true;
        g_last_rx_precond_ms = now;
        break;
    }
    case ID_POWER:
        g_ready = ((f->data[0] & 0x0F) == 0x04);
        break;
    case ID_UTIL:
        /* Heuristic: byte 4 is 0x01 normally and 0x06 in utility mode. */
        g_utility = (f->data[4] == 0x06);
        break;
    case ID_BATT_COND: {
        /* BMS_04 (0x25D): EV_BatteryConditioning = (data[5] >> 6) & 3,
         * per the factory TmsRemoteApp decode.  This is the real
         * "battery heater active" source (distinct from 0x2AD). */
        int bc = (f->data[5] >> 6) & 0x03;
        if (!g_batt_cond_known || bc != g_batt_cond) {
            fprintf(stderr, "battery_daemon: batt_cond %d\n", bc);
            fflush(stderr);
        }
        g_batt_cond = bc;
        g_batt_cond_known = true;
        g_batt_cond_ms = now;
        break;
    }
    case ID_GEAR:
        /* VCU_GearPosSta.  The two factory binaries disagree on the byte:
         * AppHomeScreen uses data[3]&3, TmsRemoteApp data[4]&0xF.  Keep the
         * raw frame and both decodes so one car run settles it. */
        memcpy(g_gear_frame, f->data, 8);
        g_gear_frame_valid = true;
        g_gear = f->data[4] & 0x0F;
        g_park = (g_gear == 0);
        g_park_known = true;
        g_gear_ms = now;
        break;
    case ID_CHARGE:
        /* CHRG_StndChrgSta: (byte3 >> 2) & 3 is non-zero with a cable
         * connected; CHRG_ChrgSta: byte4 & 3 is non-zero while charging. */
        g_charging = ((f->data[4] & 0x03) != 0);
        g_cable = (((f->data[3] >> 2) & 0x03) != 0) || g_charging;
        g_cable_known = true;
        g_cable_ms = now;
        break;
    case ID_CHARGEDOOR:
        /* CDM_HS_OpnClsState: (byte4 >> 1) & 3, 1 = open, 2 = closed. */
        g_door_state = (f->data[4] >> 1) & 0x03;
        g_door_warn = (f->data[4] >> 4) & 0x03;
        g_door_opt = (f->data[4] >> 6) & 0x03;
        g_door_known = true;
        g_door_ms = now;
        g_door_assumed = false;
        break;
    case ID_CLUSTER: {
        /* A factory 0x4E8 that differs from our last display payload means
         * the head unit is streaming its own cluster data; back off. */
        if (memcmp(f->data, g_display_last, 8) != 0) {
            g_factory_nav = true;
            g_factory_nav_ms = now;
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- HTTP ---------------- */

static void json_escape(char *dst, size_t dstsz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstsz; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c < 0x20) {
            dst[j++] = ' ';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static const char *state_name(void)
{
    switch (g_state) {
    case ST_STARTING: return "starting";
    case ST_PREPARING: return "preparing";
    case ST_ACTIVE: return "active";
    case ST_STOPPING: return "stopping";
    case ST_TIMEOUT: return "timeout";
    default: return "idle";
    }
}

static size_t build_status(char *buf, size_t sz)
{
    char msg[200];
    char dmsg[96];
    json_escape(msg, sizeof(msg), g_message);
    json_escape(dmsg, sizeof(dmsg), g_door_msg);
    int eta = 0;
    if (g_state == ST_PREPARING || g_state == ST_ACTIVE) {
        int64_t elapsed = now_ms() - g_attempt_ms;
        if (elapsed < STARTED_TIMEOUT_MS) eta = (int)((STARTED_TIMEOUT_MS - elapsed) / 1000);
    }
    int blocked = blocked_reason()[0] ? 1 : 0;
    const char *door_reason = door_block_reason();
    bool park_fresh = g_park_known && now_ms() - g_gear_ms <= DOOR_SIGNAL_STALE_MS;
    bool cable_fresh = g_cable_known && now_ms() - g_cable_ms <= DOOR_SIGNAL_STALE_MS;
    bool door_fresh = door_state_fresh();
    int door_state = door_fresh ? g_door_state : DOOR_STATE_UNKNOWN;
    char raw[32] = "";
    char gear_raw[32] = "";
    char sm[64], se[64], ss[64], smon[64];
    json_escape(sm, sizeof(sm), g_smack);
    json_escape(se, sizeof(se), g_smack_expected);
    json_escape(ss, sizeof(ss), g_smack_send);
    json_escape(smon, sizeof(smon), g_smack_monitor);
    if (g_soc_frame_valid) {
        snprintf(raw, sizeof(raw), "%02X %02X %02X %02X %02X %02X %02X %02X",
                 g_soc_frame[0], g_soc_frame[1], g_soc_frame[2], g_soc_frame[3],
                 g_soc_frame[4], g_soc_frame[5], g_soc_frame[6], g_soc_frame[7]);
    }
    if (g_gear_frame_valid) {
        snprintf(gear_raw, sizeof(gear_raw), "%02X %02X %02X %02X %02X %02X %02X %02X",
                 g_gear_frame[0], g_gear_frame[1], g_gear_frame[2], g_gear_frame[3],
                 g_gear_frame[4], g_gear_frame[5], g_gear_frame[6], g_gear_frame[7]);
    }
    return (size_t)snprintf(
        buf, sz,
        "{\"ok\":true,\"state\":\"%s\",\"precond_raw\":%d,"
        "\"batt_min_c\":%d,\"batt_valid\":%s,"
        "\"batt_raw\":\"%s\","
        "\"temp_source\":\"%s\","
        "\"soc_pct\":%.1f,\"soc_valid\":%s,"
        "\"ready\":%s,\"utility\":%s,\"blocked\":%s,\"blocked_reason\":\"%s\","
        "\"dash\":%s,\"factory_nav\":%s,"
        "\"ver\":\"%s\",\"send_ok\":%d,\"send_fail\":%d,"
        "\"last_send\":\"%s:%d\","
        "\"retries\":%d,\"eta_s\":%d,\"message\":\"%s\","
        "\"park\":%s,\"park_known\":%s,\"gear\":%d,"
        "\"cable\":%s,\"cable_known\":%s,\"charging\":%s,"
        "\"door_state\":%d,\"door_state_name\":\"%s\",\"door_known\":%s,"
        "\"door_source\":\"%s\","
        "\"door_warn\":%d,\"door_opt\":%d,\"door_busy\":%s,"
        "\"door_allowed\":%s,\"door_block_reason\":\"%s\","
        "\"door_message\":\"%s\",\"last_tx\":\"%s\","
        "\"batt_cond\":%d,\"batt_cond_known\":%s,"
        "\"gear_raw\":\"%s\",\"gear_b3\":%d,\"gear_b4\":%d,"
        "\"smack\":\"%s\",\"smack_expected\":\"%s\",\"smack_ok\":%s,"
        "\"smack_send\":\"%s\",\"smack_monitor\":\"%s\","
        "\"home_seq\":%d,\"home_source\":\"%s\",\"last_app_key\":%d,"
        "\"home_monitor\":%s}\n",
        state_name(), g_precond_raw,
        g_batt_min_c, g_batt_valid ? "true" : "false",
        raw,
        g_temp_source,
        g_soc_raw / 2.0, g_soc_valid ? "true" : "false",
        g_ready ? "true" : "false", g_utility ? "true" : "false",
        blocked ? "true" : "false", blocked_reason(),
        g_dash_override ? "true" : "false",
        g_factory_nav ? "true" : "false",
        BATTERY_DAEMON_VERSION, g_send_oks, g_send_fails,
        g_last_send_cmd, g_last_send_status,
        g_retries, eta, msg,
        (park_fresh && g_park) ? "true" : "false",
        park_fresh ? "true" : "false",
        park_fresh ? g_gear : -1,
        (cable_fresh && g_cable) ? "true" : "false",
        cable_fresh ? "true" : "false",
        (cable_fresh && g_charging) ? "true" : "false",
        door_state, door_state_name(), door_fresh ? "true" : "false",
        door_source(),
        door_fresh ? g_door_warn : 0, door_fresh ? g_door_opt : 0,
        g_door_busy ? "true" : "false",
        door_reason[0] ? "false" : "true", door_reason, dmsg,
        g_last_tx,
        g_batt_cond, g_batt_cond_known ? "true" : "false",
        gear_raw,
        g_gear_frame_valid ? (int)(g_gear_frame[3] & 0x03) : -1,
        g_gear_frame_valid ? (int)(g_gear_frame[4] & 0x0F) : -1,
        sm, se, g_smack_ok ? "true" : "false", ss, smon,
        g_home_seq, g_home_source, g_last_app_key,
        g_home_monitor ? "true" : "false");
}

static void write_status_file(void)
{
    char buf[2048];
    build_status(buf, sizeof(buf));
    int fd = open(g_status_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)write(fd, buf, strlen(buf));
    close(fd);
}

static void http_reply(int fd, int code, const char *body)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code,
                     code == 200 ? "OK" : (code == 409 ? "Blocked" : "Error"),
                     strlen(body));
    if (n > 0) (void)write(fd, hdr, (size_t)n);
    (void)write(fd, body, strlen(body));
}

static void http_handle(int fd)
{
    char req[2048];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';

    char method[8] = {0};
    char path[128] = {0};
    if (sscanf(req, "%7s %127s", method, path) != 2) {
        http_reply(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}\n");
        close(fd);
        return;
    }

    char *q = strchr(path, '?');
    if (q)
        *q = '\0';

    char body[2048];
    int code = 200;

    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/status")) {
        build_status(body, sizeof(body));
    } else if (!strcmp(method, "POST") && !strcmp(path, "/start")) {
        if (request_start() == 0) {
            snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"%s\"}\n", state_name());
        } else {
            code = 409;
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"blocked\",\"reason\":\"%s\"}\n",
                     blocked_reason());
        }
    } else if (!strcmp(method, "POST") && !strcmp(path, "/stop")) {
        request_stop();
        snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"%s\"}\n", state_name());
    } else if (!strcmp(method, "POST") && !strcmp(path, "/dash/on")) {
        g_dash_override = true;
        g_display_next_ms = 0;
        snprintf(body, sizeof(body), "{\"ok\":true,\"dash\":true}\n");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/dash/off")) {
        g_dash_override = false;
        snprintf(body, sizeof(body), "{\"ok\":true,\"dash\":false}\n");
    } else if (!strcmp(method, "POST") &&
               (!strcmp(path, "/chargedoor/open") ||
                !strcmp(path, "/chargedoor/close") ||
                !strcmp(path, "/chargedoor/toggle"))) {
        int target;
        if (!strcmp(path, "/chargedoor/open")) {
            target = DOOR_STATE_OPEN;
        } else if (!strcmp(path, "/chargedoor/close")) {
            target = DOOR_STATE_CLOSED;
        } else {
            target = (door_state_fresh() && g_door_state == DOOR_STATE_OPEN)
                     ? DOOR_STATE_CLOSED : DOOR_STATE_OPEN;
        }
        const char *why = door_block_reason();
        if (why[0]) {
            snprintf(g_door_msg, sizeof(g_door_msg), "%s", why);
            code = 409;
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"blocked\",\"reason\":\"%s\","
                     "\"door\":\"%s\"}\n", why, door_state_name());
        } else if (request_chargedoor(target) == 0) {
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"door\":\"%s\",\"state\":\"%s\"}\n",
                     g_door_msg, door_state_name());
        } else {
            code = 409;
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"blocked\",\"reason\":\"%s\","
                     "\"door\":\"%s\"}\n", g_door_msg, door_state_name());
        }
    } else {
        code = 404;
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"not found\"}\n");
    }

    http_reply(fd, code, body);
    close(fd);
}

/* ---------------- main ---------------- */

typedef struct {
    uint32_t id;
    uint64_t count;
    uint8_t last[8];
} sniff_stat_t;

static void sniff_bus(int seconds)
{
    sniff_stat_t stats[64];
    int n = 0;
    int64_t until = now_ms() + (int64_t)seconds * 1000;
    while (now_ms() < until) {
        struct pollfd pfd = {.fd = g_can_fd, .events = POLLIN};
        poll(&pfd, 1, 100);
        if (!(pfd.revents & POLLIN)) continue;
        struct can_frame f;
        while (read(g_can_fd, &f, sizeof(f)) == (ssize_t)sizeof(f)) {
            uint32_t id = f.can_id & CAN_SFF_MASK;
            int i;
            for (i = 0; i < n; i++) if (stats[i].id == id) break;
            if (i == n) {
                if (n >= (int)(sizeof(stats) / sizeof(stats[0]))) continue;
                stats[n].id = id;
                stats[n].count = 0;
                n++;
            }
            stats[i].count++;
            memcpy(stats[i].last, f.data, 8);
        }
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && stats[j].count > stats[j - 1].count; j--) {
            sniff_stat_t tmp = stats[j];
            stats[j] = stats[j - 1];
            stats[j - 1] = tmp;
        }
    }
    printf("SNIFF %s %d s, %d ids\n", g_iface, seconds, n);
    for (int i = 0; i < n; i++) {
        printf("0x%03X %llu %02X %02X %02X %02X %02X %02X %02X %02X\n",
               stats[i].id, (unsigned long long)stats[i].count,
               stats[i].last[0], stats[i].last[1], stats[i].last[2],
               stats[i].last[3], stats[i].last[4], stats[i].last[5],
               stats[i].last[6], stats[i].last[7]);
    }
    fflush(stdout);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--iface can0] [--port 8088] [--status-file PATH]"
            " [--dbus-send PATH]\n"
            "       %s [--session-bus-file PATH] [--dbus-monitor PATH]"
            " [--no-session-monitor]\n"
            "       %s [--retry-ms N] [--started-timeout-ms N]"
            " [--smack] [--version]\n"
            "       %s --start|--stop|--dash-on|--dash-off|"
            "--door-open|--door-close|--door-toggle|--status|--sniff [seconds]\n",
            argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    int port = HTTP_PORT;
    bool one_shot = false;
    bool smack_info = false;
    enum { OS_NONE, OS_START, OS_STOP, OS_DASH_ON, OS_DASH_OFF,
           OS_DOOR_OPEN, OS_DOOR_CLOSE, OS_DOOR_TOGGLE, OS_STATUS,
           OS_SNIFF } op = OS_NONE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc) g_iface = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--status-file") && i + 1 < argc) g_status_path = argv[++i];
        else if (!strcmp(argv[i], "--dbus-send") && i + 1 < argc) g_dbus_send = argv[++i];
        else if (!strcmp(argv[i], "--session-bus-file") && i + 1 < argc) g_session_bus_file = argv[++i];
        else if (!strcmp(argv[i], "--dbus-monitor") && i + 1 < argc) g_dbus_monitor = argv[++i];
        else if (!strcmp(argv[i], "--no-session-monitor")) g_session_bus_file = "";
        else if (!strcmp(argv[i], "--retry-ms") && i + 1 < argc) {
            g_start_retry_ms = atoll(argv[++i]);
            g_stop_retry_ms = g_start_retry_ms;
        }
        else if (!strcmp(argv[i], "--started-timeout-ms") && i + 1 < argc) g_started_timeout_ms = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--smack")) smack_info = true;
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("battery_daemon %s\n", BATTERY_DAEMON_VERSION);
            return 0;
        }
        else if (!strcmp(argv[i], "--start")) { op = OS_START; one_shot = true; }
        else if (!strcmp(argv[i], "--stop")) { op = OS_STOP; one_shot = true; }
        else if (!strcmp(argv[i], "--dash-on")) { op = OS_DASH_ON; one_shot = true; }
        else if (!strcmp(argv[i], "--dash-off")) { op = OS_DASH_OFF; one_shot = true; }
        else if (!strcmp(argv[i], "--door-open")) { op = OS_DOOR_OPEN; one_shot = true; }
        else if (!strcmp(argv[i], "--door-close")) { op = OS_DOOR_CLOSE; one_shot = true; }
        else if (!strcmp(argv[i], "--door-toggle")) { op = OS_DOOR_TOGGLE; one_shot = true; }
        else if (!strcmp(argv[i], "--status")) { op = OS_STATUS; one_shot = true; }
        else if (!strcmp(argv[i], "--sniff")) {
            op = OS_SNIFF; one_shot = true; g_sniff = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') g_sniff_seconds = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
    }

    init_smack_diag();
    if (smack_info) {
        printf("smack=%s\nexpected=%s\nok=%s\nsend=%s\nmonitor=%s\n",
               g_smack, g_smack_expected[0] ? g_smack_expected : "",
               g_smack_ok ? "true" : "false",
               g_smack_send, g_smack_monitor);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_can_fd = can_open(g_iface, !g_sniff);
    if (g_can_fd < 0) {
        fprintf(stderr, "battery_daemon: cannot open %s\n", g_iface);
        return 1;
    }

    if (op == OS_SNIFF) {
        sniff_bus(g_sniff_seconds);
        close(g_can_fd);
        return 0;
    }

    if (one_shot) {
        bool door_op = (op == OS_DOOR_OPEN || op == OS_DOOR_CLOSE || op == OS_DOOR_TOGGLE);
        if (door_op) {
            /* On a live bus the cable frame arrives continuously.  Wait
             * briefly so the CLI sees the latest observed-cable gate. */
            int64_t wait_until = now_ms() + 2000;
            while (now_ms() < wait_until && !g_cable_known) {
                struct pollfd pfd = {.fd = g_can_fd, .events = POLLIN};
                poll(&pfd, 1, 50);
                if (pfd.revents & POLLIN) {
                    struct can_frame f;
                    if (read(g_can_fd, &f, sizeof(f)) == (ssize_t)sizeof(f)) handle_frame(&f);
                }
                reap_children();
            }
        }
        switch (op) {
        case OS_START: request_start(); break;
        case OS_STOP: request_stop(); break;
        case OS_DASH_ON: g_dash_override = true; g_display_next_ms = 0; break;
        case OS_DASH_OFF: g_dash_override = false; break;
        case OS_DOOR_OPEN: request_chargedoor(DOOR_STATE_OPEN); break;
        case OS_DOOR_CLOSE: request_chargedoor(DOOR_STATE_CLOSED); break;
        case OS_DOOR_TOGGLE:
            request_chargedoor((door_state_fresh() && g_door_state == DOOR_STATE_OPEN)
                               ? DOOR_STATE_CLOSED : DOOR_STATE_OPEN);
            break;
        default: break;
        }
        /* Give a burst or a door movement a chance to complete. */
        int64_t until = now_ms() + (door_op ? 5000 :
                                    (op == OS_START || op == OS_STOP ? 1200 : 100));
        while (now_ms() < until) {
            struct pollfd pfd = {.fd = g_can_fd, .events = POLLIN};
            poll(&pfd, 1, 50);
            if (pfd.revents & POLLIN) {
                struct can_frame f;
                if (read(g_can_fd, &f, sizeof(f)) == (ssize_t)sizeof(f)) handle_frame(&f);
            }
            reap_children();
            tick_burst(now_ms());
            tick_state(now_ms());
            tick_chargedoor(now_ms());
        }
        char buf[2048];
        build_status(buf, sizeof(buf));
        fputs(buf, stdout);
        close(g_can_fd);
        return 0;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); close(g_can_fd); return 1; }
    fcntl(srv, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind(http)");
        close(srv); close(g_can_fd);
        return 1;
    }
    if (listen(srv, 8) < 0) {
        perror("listen");
        close(srv); close(g_can_fd);
        return 1;
    }

    mkdir("/rw_data", 0755);
    mkdir("/rw_data/battery", 0755);

    fprintf(stderr, "battery_daemon: v%s, %s, http 127.0.0.1:%d\n",
            BATTERY_DAEMON_VERSION, g_iface, port);
    fflush(stderr);

    int64_t last_status_write = 0;
    while (!g_stop) {
        struct pollfd pfd[3] = {
            {.fd = g_can_fd, .events = POLLIN},
            {.fd = srv, .events = POLLIN},
            {.fd = g_mon_fd, .events = POLLIN},
        };
        int pr = poll(pfd, g_mon_fd >= 0 ? 3 : 2, 50);
        int64_t now = now_ms();

        if (pr > 0) {
            if (pfd[0].revents & POLLIN) {
                struct can_frame f;
                while (read(g_can_fd, &f, sizeof(f)) == (ssize_t)sizeof(f)) {
                    handle_frame(&f);
                }
            }
            if (pfd[1].revents & POLLIN) {
                int c = accept(srv, NULL, NULL);
                if (c >= 0) {
                    fcntl(c, F_SETFD, FD_CLOEXEC);
                    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
                    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    http_handle(c);
                }
            }
        }

        reap_children();
        monitor_tick();
        tick_burst(now);
        tick_state(now);
        tick_display(now);
        tick_chargedoor(now);

        if (now - last_status_write > 1000) {
            write_status_file();
            last_status_write = now;
        }
    }

    monitor_cleanup();
    close(srv);
    close(g_can_fd);
    return 0;
}
