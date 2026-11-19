/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Experimental first-party CarPlay broker helper.
 *
 * This is the supervised process that owns cp_runtime_broker, the credit
 * checked SPP hook socket, and the private/explicit OEM session bus.  It is
 * NOT linked into wifi1b-daemon and is NOT activated by the installer.
 *
 * Inputs are root-owned files:
 *   /rw_data/wifi1b/carplay-broker.conf   identity + bus + service name
 *   /rw_data/wifi1b/status.json           daemon-attested AP readback
 *   /run/wifi1b/carplay/rx.sock           patched vbtd hook datagrams
 *   /run/scourge-carplay-auth/auth.sock   root MFi helper
 *
 * The helper never starts a bus, never runs ip/route/SSH/AP commands, and
 * never synthesizes authentication, pairing or projection success.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cp_receiver_lookup.h"
#include "cp_receiver_policy.h"
#include "cp_runtime_broker.h"
#include "cp_spp_socket.h"
#include "cp_usb.h"
#include "cp_wire.h"

#define CFG_PATH "/rw_data/wifi1b/carplay-broker.conf"
#define DAEMON_STATUS "/rw_data/wifi1b/status.json"
/* Keep status outside the unit's RuntimeDirectory so a failed/stopped unit
 * does not delete the diagnostic.  The RX socket stays under carplay/. */
#define BROKER_STATUS "/run/wifi1b/broker-status.json"
#define RX_SOCKET "/run/wifi1b/carplay/rx.sock"
#define SESSION_ENV "/tmp/session_appmgr.env"
#define MAX_LANGS 8u
#define MAX_FIELD 128u

typedef struct {
    char bus_address[512];
    char service_name[128];
    char name[MAX_FIELD];
    char model[MAX_FIELD];
    char manufacturer[MAX_FIELD];
    char serial[MAX_FIELD];
    char firmware[MAX_FIELD];
    char hardware[MAX_FIELD];
    char eap[MAX_FIELD];
    char language[MAX_FIELD];
    char languages[MAX_FIELD * MAX_LANGS];
    char transport_name[MAX_FIELD];
    char bt_mac[32];
    char device_id[MAX_FIELD];
    char ap_ssid[64];
    char ap_psk[96];
    unsigned ap_channel;
    char ap_security[32];
    char link_local[64];
    unsigned airplay_port;
    char source_version[MAX_FIELD];
    char receiver_interface[CP_OEM_RECEIVER_MAX_INTERFACE];
    unsigned receiver_timeout_ms;
    unsigned receiver_settle_ms;
    char receiver_listener_owner[64];
    char receiver_pi_sha256[CP_OEM_RECEIVER_SHA256_LEN * 2u + 1u];
    char receiver_backend_root[256];
    char receiver_backend_library[512];
    int receiver_allow_unpinned_backend;
    int consent;
    int usb_active;
    char usb_sysfs[256];
    char config_path[512];
    char daemon_status_path[512];
    char broker_status_path[512];
    char rx_path[256];
} broker_config_t;

typedef struct {
    broker_config_t cfg;
    cp_session_config_t session;
    cp_runtime_broker broker;
    cp_spp_socket rx;
    cp_boot_ap_readback ap;
    cp_receiver_lookup_result receiver;
    cp_receiver_policy_result accepted;
    cp_receiver_listener_result listener;
    cp_receiver_lookup_status receiver_lookup_status;
    cp_receiver_policy_status receiver_policy_status;
    char receiver_reason[32];
    bool receiver_policy_evaluated;
    uint8_t receiver_pi_pin[CP_OEM_RECEIVER_SHA256_LEN];
    bool receiver_have_pi_pin;
    bool receiver_ready;
    char lang_storage[MAX_LANGS][MAX_FIELD];
    const char *langs[MAX_LANGS + 1]; /* + NULL terminator */
    bool usb_blocked;
} broker_ctx_t;

static broker_ctx_t g_ctx;
static volatile sig_atomic_t g_stop;

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static char *trim(char *s)
{
    if (s == NULL) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    return s;
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (src == NULL) src = "";
    snprintf(dst, cap, "%s", src);
}

static int load_kv(const char *path, broker_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);
        if (!strcmp(k, "bus_address")) copy_field(cfg->bus_address, sizeof(cfg->bus_address), v);
        else if (!strcmp(k, "service_name")) copy_field(cfg->service_name, sizeof(cfg->service_name), v);
        else if (!strcmp(k, "name")) copy_field(cfg->name, sizeof(cfg->name), v);
        else if (!strcmp(k, "model")) copy_field(cfg->model, sizeof(cfg->model), v);
        else if (!strcmp(k, "manufacturer")) copy_field(cfg->manufacturer, sizeof(cfg->manufacturer), v);
        else if (!strcmp(k, "serial")) copy_field(cfg->serial, sizeof(cfg->serial), v);
        else if (!strcmp(k, "firmware")) copy_field(cfg->firmware, sizeof(cfg->firmware), v);
        else if (!strcmp(k, "hardware")) copy_field(cfg->hardware, sizeof(cfg->hardware), v);
        else if (!strcmp(k, "eap_name")) copy_field(cfg->eap, sizeof(cfg->eap), v);
        else if (!strcmp(k, "language")) copy_field(cfg->language, sizeof(cfg->language), v);
        else if (!strcmp(k, "languages")) copy_field(cfg->languages, sizeof(cfg->languages), v);
        else if (!strcmp(k, "transport_name")) copy_field(cfg->transport_name, sizeof(cfg->transport_name), v);
        else if (!strcmp(k, "bt_mac")) copy_field(cfg->bt_mac, sizeof(cfg->bt_mac), v);
        else if (!strcmp(k, "device_id")) copy_field(cfg->device_id, sizeof(cfg->device_id), v);
        else if (!strcmp(k, "ap_ssid")) copy_field(cfg->ap_ssid, sizeof(cfg->ap_ssid), v);
        else if (!strcmp(k, "ap_psk")) copy_field(cfg->ap_psk, sizeof(cfg->ap_psk), v);
        else if (!strcmp(k, "ap_channel")) cfg->ap_channel = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "ap_security")) copy_field(cfg->ap_security, sizeof(cfg->ap_security), v);
        else if (!strcmp(k, "link_local")) copy_field(cfg->link_local, sizeof(cfg->link_local), v);
        else if (!strcmp(k, "airplay_port")) cfg->airplay_port = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "source_version")) copy_field(cfg->source_version, sizeof(cfg->source_version), v);
        else if (!strcmp(k, "receiver_interface")) copy_field(cfg->receiver_interface,
                                                            sizeof(cfg->receiver_interface), v);
        else if (!strcmp(k, "receiver_timeout_ms")) cfg->receiver_timeout_ms = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "receiver_settle_ms")) cfg->receiver_settle_ms = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "receiver_listener_owner")) copy_field(cfg->receiver_listener_owner,
                                                                   sizeof(cfg->receiver_listener_owner), v);
        else if (!strcmp(k, "receiver_pi_sha256")) copy_field(cfg->receiver_pi_sha256,
                                                              sizeof(cfg->receiver_pi_sha256), v);
        else if (!strcmp(k, "consent")) cfg->consent = atoi(v) != 0;
        else if (!strcmp(k, "usb_active")) cfg->usb_active = atoi(v) != 0;
        else if (!strcmp(k, "usb_sysfs")) copy_field(cfg->usb_sysfs,
                                                     sizeof(cfg->usb_sysfs), v);
    }
    fclose(f);
    return 0;
}

/* status.json is a bounded flat object of strings, integers and booleans.
 * Reject other shapes rather than searching for key text inside values. */
static const char *skip_space(const char *p)
{
    while (*p && strchr(" \t\r\n", *p)) p++;
    return p;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool json_string(const char **cursor, char *out, size_t cap)
{
    const char *p = *cursor;
    size_t n = 0;
    if (*p++ != '"') return false;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20) return false;
        if (c == '\\') {
            switch (*p) {
            case '"': case '\\': case '/': c = (unsigned char)*p++; break;
            case 'n': c = '\n'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'u': {
                unsigned v = 0;
                p++;
                for (unsigned i = 0; i < 4; i++) {
                    int digit = hex_digit((unsigned char)*p);
                    if (digit < 0) return false;
                    v = 16 * v + (unsigned)digit;
                    p++;
                }
                /* The daemon escapes ASCII controls this way, not Unicode. */
                if (v == 0 || v > 0x7f) return false;
                c = (unsigned char)v;
                break;
            }
            default: return false;
            }
        }
        if (n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    if (*p != '"') return false;
    out[n] = '\0';
    *cursor = p + 1;
    return true;
}

static bool map_security(const char *text, cp_wire_wifi_security *out)
{
    if (text == NULL || out == NULL) return false;
    if (!strcmp(text, "WPA2") || !strcmp(text, "WPA2-PSK")) {
        *out = CP_WIRE_WIFI_WPA2;
        return true;
    }
    if (!strcmp(text, "WPA2/WPA3") || !strcmp(text, "WPA2-WPA3")) {
        *out = CP_WIRE_WIFI_WPA2_WPA3;
        return true;
    }
    if (!strcmp(text, "WPA3") || !strcmp(text, "SAE") ||
        !strcmp(text, "WPA3-SAE")) {
        *out = CP_WIRE_WIFI_WPA3;
        return true;
    }
    return false;
}

static void strip_zone(char *s)
{
    char *p = strchr(s, '%');
    if (p != NULL) *p = '\0';
}

static bool ap_snapshot_matches(const char *p, const broker_config_t *cfg,
                                time_t now)
{
    const char *keys[] = {
        "app", "mode", "observations_fresh", "ap_active", "ap_network_ready",
        "applying", "ap_ssid", "carplay_ssid", "carplay_psk", "ap_channel",
        "ap_security", "ap_linklocal", "updated_ms",
    };
    unsigned seen = 0;
    const unsigned count = sizeof(keys) / sizeof(keys[0]);
    p = skip_space(p);
    if (*p++ != '{' || now <= 0) return false;
    do {
        char key[128], value[1024];
        long number = 0;
        p = skip_space(p);
        if (!json_string(&p, key, sizeof(key))) return false;
        p = skip_space(p);
        if (*p++ != ':') return false;
        p = skip_space(p);
        char type = *p;
        if (type == '"') {
            if (!json_string(&p, value, sizeof(value))) return false;
        } else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
            p += type == 't' ? 4 : 5;
        } else {
            char *end;
            errno = 0;
            if (type != '-' && (type < '0' || type > '9')) return false;
            number = strtol(p, &end, 10);
            if (errno || end == p) return false;
            p = end;
            type = 'i';
        }
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(key, keys[i])) continue;
            if (seen & (1u << i)) return false;
            seen |= 1u << i;
            cp_wire_wifi_security observed, expected;
            switch (i) {
            case 0: if (type != '"' || strcmp(value, "wifi1b")) return false; break;
            case 1: if (type != '"' || strcmp(value, "carplay")) return false; break;
            case 2: case 3: case 4: if (type != 't') return false; break;
            case 5: if (type != 'f') return false; break;
            case 6: case 7: if (type != '"' || strcmp(value, cfg->ap_ssid)) return false; break;
            case 8: if (type != '"' || strcmp(value, cfg->ap_psk)) return false; break;
            case 9: if (type != 'i' || number != (long)cfg->ap_channel) return false; break;
            case 10:
                if (type != '"' || !map_security(value, &observed) ||
                    !map_security(cfg->ap_security, &expected) ||
                    observed != expected) return false;
                break;
            case 11:
                if (type != '"') return false;
                strip_zone(value);
                if (!cp_wire_link_local_usable(value) ||
                    strcmp(value, cfg->link_local)) return false;
                break;
            case 12:
                if (type != 'i' || number <= 0 || number % 1000 ||
                    number / 1000 > now || now - number / 1000 > 5)
                    return false;
                break;
            }
            break;
        }
        p = skip_space(p);
        if (*p == '}')
            return *skip_space(p + 1) == '\0' && seen == (1u << count) - 1;
    } while (*p++ == ',');
    return false;
}

static bool identity_complete(const broker_config_t *c)
{
    return c->name[0] && c->model[0] && c->manufacturer[0] &&
           c->serial[0] && c->firmware[0] && c->hardware[0] &&
           c->eap[0] && c->language[0] && c->transport_name[0] &&
           c->bt_mac[0] && c->ap_ssid[0] &&
           c->ap_psk[0] && c->ap_channel != 0 && c->ap_channel <= 255 &&
           c->ap_security[0] && c->link_local[0] &&
           c->service_name[0] && c->receiver_interface[0];
}

static void parse_languages(broker_ctx_t *ctx)
{
    unsigned n = 0;
    char *save = NULL;
    char list[sizeof(ctx->cfg.languages)];
    copy_field(list, sizeof(list), ctx->cfg.languages);
    for (char *p = strtok_r(list, ",", &save);
         p != NULL && n < MAX_LANGS; p = strtok_r(NULL, ",", &save)) {
        char *v = trim(p);
        if (*v == '\0') continue;
        copy_field(ctx->lang_storage[n], sizeof(ctx->lang_storage[n]), v);
        ctx->langs[n] = ctx->lang_storage[n];
        n++;
    }
    if (n == 0) {
        copy_field(ctx->lang_storage[0], sizeof(ctx->lang_storage[0]),
                   ctx->cfg.language);
        ctx->langs[0] = ctx->lang_storage[0];
        n = 1;
    }
    ctx->langs[n] = NULL;
}

static bool build_session_config(broker_ctx_t *ctx)
{
    if (ctx->cfg.device_id[0] == '\0' || ctx->cfg.airplay_port == 0u ||
        ctx->cfg.airplay_port > 65535u) {
        return false;
    }
    cp_session_config_default(&ctx->session);
    cp_boot_config *b = &ctx->session.boot;
    b->name = ctx->cfg.name;
    b->model_identifier = ctx->cfg.model;
    b->manufacturer = ctx->cfg.manufacturer;
    b->serial_number = ctx->cfg.serial;
    b->firmware_version = ctx->cfg.firmware;
    b->hardware_version = ctx->cfg.hardware;
    b->eap_name = ctx->cfg.eap;
    b->current_language = ctx->cfg.language;
    b->supported_languages = ctx->langs;
    b->supported_language_count = 0;
    while (ctx->langs[b->supported_language_count] != NULL)
        b->supported_language_count++;
    b->bluetooth_transport_name = ctx->cfg.transport_name;
    b->bluetooth_mac = ctx->cfg.bt_mac;
    b->ap_ssid = ctx->cfg.ap_ssid;
    b->ap_passphrase = ctx->cfg.ap_psk;
    b->ap_channel = (uint8_t)ctx->cfg.ap_channel;
    if (!map_security(ctx->cfg.ap_security, &b->ap_security))
        return false;
    b->link_local = ctx->cfg.link_local;
    b->airplay_port = (uint16_t)ctx->cfg.airplay_port;
    b->device_identifier = ctx->cfg.device_id;
    b->source_version = ctx->cfg.source_version[0] ? ctx->cfg.source_version : NULL;
    return true;
}

static cp_boot_consent consent_cb(void *user, uint64_t now_ms)
{
    (void)now_ms;
    broker_ctx_t *ctx = user;
    return ctx->cfg.consent ? CP_BOOT_CONSENT_GRANTED : CP_BOOT_CONSENT_DENIED;
}

static bool usb_active_cb(void *user, const char *bluetooth_id,
                          const char *usb_id)
{
    (void)bluetooth_id;
    (void)usb_id;
    broker_ctx_t *ctx = user;
    ctx->usb_blocked = ctx->cfg.usb_active != 0 ||
                       cp_usb_apple_present(ctx->cfg.usb_sysfs);
    return ctx->usb_blocked;
}

static bool read_status_text(const broker_config_t *cfg, uid_t expected_uid,
                             char *text, size_t cap)
{
    struct stat st;
    int fd = open(cfg->daemon_status_path,
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != expected_uid || (st.st_mode & 077) != 0 ||
        st.st_size <= 0 || (uint64_t)st.st_size >= cap) {
        close(fd);
        return false;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return false;
    }
    size_t n = fread(text, 1, cap, f);
    bool valid = !ferror(f) && n == (size_t)st.st_size && !memchr(text, 0, n);
    fclose(f);
    if (!valid) return false;
    text[n] = '\0';
    return true;
}

static bool read_ap_snapshot(const broker_config_t *cfg, uid_t expected_uid)
{
    char text[16384];
    if (!read_status_text(cfg, expected_uid, text, sizeof(text))) return false;
    return ap_snapshot_matches(text, cfg, time(NULL));
}

typedef enum {
    AP_SNAPSHOT_MISSING = 0,
    AP_SNAPSHOT_STALE,
    AP_SNAPSHOT_FRESH
} ap_snapshot_state;

/*
 * A diagnostic --check-receiver classifies the daemon snapshot but does not
 * require it.  The runtime path still requires FRESH before any bus/RX work.
 */
static ap_snapshot_state classify_ap_snapshot(const broker_config_t *cfg)
{
    char text[16384];
    if (!read_status_text(cfg, geteuid(), text, sizeof(text))) {
        return AP_SNAPSHOT_MISSING;
    }
    return ap_snapshot_matches(text, cfg, time(NULL)) ? AP_SNAPSHOT_FRESH
                                                      : AP_SNAPSHOT_STALE;
}

static const char *ap_snapshot_state_string(ap_snapshot_state state)
{
    switch (state) {
    case AP_SNAPSHOT_FRESH: return "fresh";
    case AP_SNAPSHOT_STALE: return "stale";
    default: return "missing";
    }
}

static int preflight_stop(void *context)
{
    (void)context;
    return g_stop != 0;
}

static int receiver_check_exit_code(const broker_ctx_t *ctx);

static bool wait_ap_for_receiver(const broker_ctx_t *ctx, uint64_t deadline,
                                 char *error, size_t error_size)
{
    while (!g_stop) {
        if (read_ap_snapshot(&ctx->cfg, geteuid())) {
            return true;
        }
        uint64_t now = (uint64_t)monotonic_ms();
        if (now >= deadline) {
            break;
        }
        struct timespec pause = {0, 250000000};
        (void)nanosleep(&pause, NULL);
    }
    snprintf(error, error_size,
             "AP snapshot not ready for receiver preflight");
    return false;
}

static const char *listener_scope_string(const cp_receiver_listener_result *r)
{
    if (!r->found) return "absent";
    if (r->wildcard) return "wildcard";
    if (r->loopback_only) return "loopback";
    if (r->address_allowed) return "specific";
    return "foreign";
}

/*
 * Resolve the stock receiver before any bus/RX work.  The advertisement is
 * untrusted input, so policy evaluates every distinct candidate, ties the
 * advertised device ID to the locally configured Bluetooth MAC (+1), checks
 * an optional pi SHA-256 pin, and requires a local LISTEN socket on the
 * advertised port whose owner is allowlisted.  The observed values, not the
 * configured assumptions, are what go into 0x4301.
 *
 * require_fresh_snapshot is true for the runtime path.  --check-receiver
 * passes false so a stale daemon snapshot is reported, not treated as a
 * preflight blocker.
 */
static bool receiver_preflight(broker_ctx_t *ctx, bool require_fresh_snapshot,
                               char *error, size_t error_size)
{
    unsigned timeout = ctx->cfg.receiver_timeout_ms != 0u
                           ? ctx->cfg.receiver_timeout_ms
                           : 15000u;
    uint64_t deadline = (uint64_t)monotonic_ms() + timeout;
    unsigned ifindex;
    cp_receiver_lookup_config lookup;
    cp_receiver_policy_params params;
    cp_receiver_listener_config listener;
    cp_receiver_lookup_status lookup_status;
    cp_receiver_policy_status policy_status;
    cp_receiver_listener_status listener_status;
    size_t candidate_count;
    uint64_t now;
    uint64_t remaining;

    ctx->receiver_ready = false;
    ctx->receiver_policy_evaluated = false;
    ctx->receiver_policy_status = CP_RECEIVER_POLICY_ERR_NO_SERVICE;
    ctx->receiver_lookup_status = CP_RECEIVER_LOOKUP_OK;
    copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
               "preflight");
    memset(&ctx->receiver, 0, sizeof(ctx->receiver));
    memset(&ctx->accepted, 0, sizeof(ctx->accepted));
    memset(&ctx->listener, 0, sizeof(ctx->listener));
    ctx->listener.status = CP_RECEIVER_LISTENER_ABSENT;

    if (require_fresh_snapshot &&
        !wait_ap_for_receiver(ctx, deadline, error, error_size)) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   "ap-snapshot");
        return false;
    }
    ifindex = if_nametoindex(ctx->cfg.receiver_interface);
    if (ifindex == 0u) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   "interface");
        snprintf(error, error_size, "receiver interface %s is not up",
                 ctx->cfg.receiver_interface);
        return false;
    }
    now = (uint64_t)monotonic_ms();
    remaining = deadline > now ? deadline - now : 0u;
    if (remaining == 0u) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   "timeout");
        snprintf(error, error_size,
                 "receiver preflight timed out waiting for %s",
                 ctx->cfg.receiver_interface);
        return false;
    }
    memset(&lookup, 0, sizeof(lookup));
    memcpy(lookup.interface_name, ctx->cfg.receiver_interface,
           strlen(ctx->cfg.receiver_interface) + 1u);
    lookup.interface_index = ifindex;
    lookup.timeout_ms = remaining;
    lookup.settle_ms = ctx->cfg.receiver_settle_ms != 0u
                           ? ctx->cfg.receiver_settle_ms
                           : 500u;
    lookup.max_services = CP_OEM_RECEIVER_MAX_SERVICES;
    lookup.backend_root = ctx->cfg.receiver_backend_root[0] != '\0'
                              ? ctx->cfg.receiver_backend_root
                              : "/";
    lookup.backend_library = ctx->cfg.receiver_backend_library[0] != '\0'
                                 ? ctx->cfg.receiver_backend_library
                                 : NULL;
    lookup.allow_unpinned_backend =
        ctx->cfg.receiver_allow_unpinned_backend != 0;
    lookup_status = cp_receiver_lookup_live(&lookup, preflight_stop, NULL,
                                            &ctx->receiver);
    ctx->receiver_lookup_status = lookup_status;
    if (lookup_status != CP_RECEIVER_LOOKUP_OK) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   cp_receiver_lookup_status_string(lookup_status));
        snprintf(error, error_size, "receiver lookup on %s: %s",
                 ctx->cfg.receiver_interface,
                 cp_receiver_lookup_status_string(lookup_status));
        return false;
    }
    candidate_count = (size_t)ctx->receiver.resolved_count +
                      (size_t)ctx->receiver.rejected_count;
    if (candidate_count > CP_OEM_RECEIVER_MAX_SERVICES) {
        candidate_count = CP_OEM_RECEIVER_MAX_SERVICES;
    }
    memset(&params, 0, sizeof(params));
    params.bt_mac = ctx->cfg.bt_mac;
    params.expected_device_id = ctx->cfg.device_id;
    params.expected_port = ctx->cfg.airplay_port;
    params.expected_source_version = ctx->cfg.source_version;
    params.expected_pi_sha256 = ctx->receiver_have_pi_pin
                                    ? ctx->receiver_pi_pin
                                    : NULL;
    policy_status = cp_receiver_policy_accept(
        ctx->receiver.services, candidate_count, &params, &ctx->accepted);
    ctx->receiver_policy_status = policy_status;
    ctx->receiver_policy_evaluated = true;
    if (policy_status != CP_RECEIVER_POLICY_OK) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   cp_receiver_policy_status_string(policy_status));
        snprintf(error, error_size,
                 "receiver advertisement refused (%s): service_count=%u",
                 cp_receiver_policy_status_string(policy_status),
                 (unsigned)ctx->receiver.service_count);
        return false;
    }
    memset(&listener, 0, sizeof(listener));
    listener.proc_root = "/proc";
    listener.interface_name = ctx->cfg.receiver_interface;
    listener.owner_allowlist = ctx->cfg.receiver_listener_owner;
    listener_status = cp_receiver_listener_check(
        ctx->accepted.port, &listener, &ctx->listener);
    if (listener_status != CP_RECEIVER_LISTENER_OK) {
        copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason),
                   cp_receiver_listener_status_string(listener_status));
        snprintf(error, error_size,
                 "receiver listener refused (%s) on port %u",
                 cp_receiver_listener_status_string(listener_status),
                 (unsigned)ctx->accepted.port);
        return false;
    }
    copy_field(ctx->cfg.device_id, sizeof(ctx->cfg.device_id),
               ctx->accepted.device_id);
    ctx->cfg.airplay_port = ctx->accepted.port;
    copy_field(ctx->cfg.source_version, sizeof(ctx->cfg.source_version),
               ctx->accepted.source_version);
    copy_field(ctx->receiver_reason, sizeof(ctx->receiver_reason), "ok");
    ctx->receiver_ready = true;
    return true;
}

static cp_session_error_t update_ap_readback(broker_ctx_t *ctx, uint64_t now_ms)
{
    bool on_air = read_ap_snapshot(&ctx->cfg, 0);
    if (on_air == ctx->ap.on_air) return CP_SESSION_OK;
    ctx->ap.on_air = on_air;
    return cp_runtime_broker_set_ap_readback(&ctx->broker, &ctx->ap, now_ms);
}

static void prepare_status_dir(const broker_config_t *cfg)
{
    /* Best effort: status is diagnostics, not a transport gate.  Create the
     * directory before the first write so identity/security failures are
     * visible to the operator.  The status file itself stays outside the
     * unit's RuntimeDirectory so it survives a failed stop.  A fixture path
     * outside /run/wifi1b already has its own parent. */
    if (cfg == NULL ||
        strncmp(cfg->broker_status_path, "/run/wifi1b/", 12) != 0)
        return;
    (void)mkdir("/run/wifi1b", 0755);
    (void)mkdir("/run/wifi1b/carplay", 0700);
}

static void hex_encode(const uint8_t *data, size_t len, char *out,
                       size_t out_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (out == NULL || out_size == 0u) return;
    if (len * 2u + 1u > out_size) {
        out[0] = '\0';
        return;
    }
    for (i = 0u; i < len; i++) {
        out[i * 2u] = digits[(data[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = digits[data[i] & 0x0fu];
    }
    out[len * 2u] = '\0';
}

static void write_status(const broker_ctx_t *ctx, const char *state,
                         const char *error)
{
    char tmp[1024];
    char esc_state[64];
    char esc_error[640];
    char esc_peer[64];
    char esc_reason[64];
    char esc_source_version[128];
    char esc_service_name[384];
    char esc_owner[192];
    char pi_hex[CP_OEM_RECEIVER_SHA256_LEN * 2u + 1u];
    bool ok = !strcmp(state, "started") || !strcmp(state, "running") ||
              !strcmp(state, "stopped");
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", ctx->cfg.broker_status_path,
             (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return;
    (void)cp_receiver_json_escape(state, esc_state, sizeof(esc_state));
    (void)cp_receiver_json_escape(error, esc_error, sizeof(esc_error));
    (void)cp_receiver_json_escape(ctx->broker.peer, esc_peer,
                                  sizeof(esc_peer));
    (void)cp_receiver_json_escape(ctx->receiver_reason, esc_reason,
                                  sizeof(esc_reason));
    (void)cp_receiver_json_escape(ctx->cfg.source_version,
                                  esc_source_version,
                                  sizeof(esc_source_version));
    (void)cp_receiver_json_escape(ctx->accepted.service_name,
                                  esc_service_name, sizeof(esc_service_name));
    (void)cp_receiver_json_escape(ctx->listener.owner_name, esc_owner,
                                  sizeof(esc_owner));
    pi_hex[0] = '\0';
    if (ctx->receiver_ready) {
        hex_encode(ctx->accepted.pi_sha256,
                   sizeof(ctx->accepted.pi_sha256), pi_hex, sizeof(pi_hex));
    }
    fprintf(f,
            "{\"ok\":%s,\"state\":\"%s\",\"error\":\"%s\","
            "\"epoch\":%u,\"peer\":\"%s\",\"ap_on_air\":%s,"
            "\"usb_blocked\":%s,"
            "\"receiver_observed\":%s,\"receiver_port\":%u,"
            "\"receiver_service_count\":%u,"
            "\"receiver_resolved_count\":%u,"
            "\"receiver_rejected_count\":%u,"
            "\"receiver_lookup\":\"%s\",\"receiver_policy\":\"%s\","
            "\"receiver_reason\":\"%s\","
            "\"receiver_service_name\":\"%s\","
            "\"receiver_source_version\":\"%s\","
            "\"receiver_pi_sha256\":\"%s\",\"receiver_pi_len\":%u,"
            "\"listener_scope\":\"%s\",\"listener_owner\":\"%s\","
            "\"listener_owner_allowed\":%s,"
            "\"rx_packets\":%u,\"rx_dropped\":%u,\"tx_chunks\":%u,"
            "\"auth_failures\":%u,\"session_failures\":%u,"
            "\"init_timeouts\":%u,\"drain_timeouts\":%u}\n",
            ok ? "true" : "false", esc_state, esc_error,
            ctx->broker.epoch, esc_peer,
            ctx->ap.on_air ? "true" : "false",
            ctx->usb_blocked ? "true" : "false",
            ctx->receiver_ready ? "true" : "false",
            ctx->receiver_ready ? (unsigned)ctx->cfg.airplay_port : 0u,
            ctx->receiver.service_count,
            ctx->receiver.resolved_count,
            ctx->receiver.rejected_count,
            cp_receiver_lookup_status_string(ctx->receiver_lookup_status),
            ctx->receiver_policy_evaluated
                ? cp_receiver_policy_status_string(ctx->receiver_policy_status)
                : "not-evaluated",
            esc_reason,
            ctx->receiver_ready ? esc_service_name : "",
            ctx->receiver_ready ? esc_source_version : "",
            pi_hex,
            ctx->receiver_ready ? (unsigned)ctx->accepted.pi_len : 0u,
            listener_scope_string(&ctx->listener),
            ctx->listener.owner_name[0] != '\0' ? esc_owner : "",
            ctx->listener.owner_allowed ? "true" : "false",
            ctx->broker.stats.rx_packets, ctx->broker.stats.rx_dropped,
            ctx->broker.stats.tx_chunks, ctx->broker.stats.auth_failures,
            ctx->broker.stats.session_failures,
            ctx->broker.stats.init_timeouts, ctx->broker.stats.drain_timeouts);
    fclose(f);
    rename(tmp, ctx->cfg.broker_status_path);
}

static int prepare_rx_socket(broker_ctx_t *ctx, char *error, size_t error_size)
{
    cp_spp_socket_init(&ctx->rx);
    if (mkdir("/run/wifi1b", 0755) != 0 && errno != EEXIST) {
        snprintf(error, error_size, "cannot create rx parent: %s",
                 strerror(errno));
        return -1;
    }
    if (mkdir("/run/wifi1b/carplay", 0700) != 0 && errno != EEXIST) {
        snprintf(error, error_size, "cannot create rx directory: %s",
                 strerror(errno));
        return -1;
    }
    chmod("/run/wifi1b/carplay", 0700);
    if (ctx->cfg.rx_path[0] && strcmp(ctx->cfg.rx_path, RX_SOCKET) != 0) {
        snprintf(error, error_size, "rx socket path must be %s", RX_SOCKET);
        return -1;
    }
    if (cp_spp_socket_open(&ctx->rx) != 0) {
        snprintf(error, error_size, "cannot open credential-checked rx socket");
        return -1;
    }
    return 0;
}

static void load_bus_default(broker_config_t *cfg)
{
    if (cfg->bus_address[0]) return;
    FILE *f = fopen(SESSION_ENV, "r");
    if (f == NULL) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = strstr(line, "DBUS_SESSION_BUS_ADDRESS=");
        if (p == NULL) continue;
        p += strlen("DBUS_SESSION_BUS_ADDRESS=");
        char *v = trim(p);
        char *comma = strchr(v, ',');
        if (comma != NULL) *comma = '\0';
        copy_field(cfg->bus_address, sizeof(cfg->bus_address), v);
        break;
    }
    fclose(f);
}

static int run_broker(broker_ctx_t *ctx)
{
    char error[256] = "";
    int64_t now = monotonic_ms();
    if (ctx->cfg.receiver_allow_unpinned_backend) {
        /* The unpinned backend override is diagnostic-only.  Never let it
         * weaken a runtime receiver attestation, even if the CLI flag was
         * passed to the service binary by mistake. */
        snprintf(error, sizeof(error),
                 "unpinned DNS-SD backend is diagnostic-only; refusing runtime");
        write_status(ctx, "receiver_incomplete", error);
        fprintf(stderr, "%s\n", error);
        return 3;
    }
    /*
     * Receiver preflight runs before the RX socket is created.  Opening a
     * listener for a receiver that policy refused would advertise more than
     * the observation supports.
     */
    if (!receiver_preflight(ctx, true, error, sizeof(error))) {
        write_status(ctx, "receiver_incomplete", error);
        fprintf(stderr, "%s\n", error);
        return receiver_check_exit_code(ctx);
    }
    if (prepare_rx_socket(ctx, error, sizeof(error)) != 0) {
        write_status(ctx, "fatal", error);
        return 2;
    }
    if (!build_session_config(ctx)) {
        snprintf(error, sizeof(error), "session configuration incomplete");
        write_status(ctx, "identity_incomplete", error);
        fprintf(stderr, "%s\n", error);
        cp_spp_socket_close(&ctx->rx);
        return 2;
    }
    cp_runtime_broker_config_t bc;
    memset(&bc, 0, sizeof(bc));
    bc.experimental = 1;
    bc.enabled = 1;
    bc.spp.bus_address = ctx->cfg.bus_address;
    bc.spp.service_name = ctx->cfg.service_name;
    bc.spp.experimental = 1;
    bc.spp.expected_owner_uid_set = 1;
    bc.spp.expected_owner_uid = 0;
    bc.auth.experimental = true;
    bc.session = ctx->session;
    bc.role = CP_LINK_ROLE_INITIATOR;
    bc.consent = consent_cb;
    bc.usb_active = usb_active_cb;
    bc.policy_context = ctx;
    ctx->ap.ssid = ctx->cfg.ap_ssid;
    ctx->ap.channel = (uint8_t)ctx->cfg.ap_channel;
    ctx->ap.security = CP_WIRE_WIFI_WPA2;
    (void)map_security(ctx->cfg.ap_security, &ctx->ap.security);
    ctx->ap.link_local = ctx->cfg.link_local;
    ctx->ap.on_air = false;
    bc.ap_readback = ctx->ap;
    bc.have_ap_readback = 1;
    bc.init_timeout_ms = 30000;
    bc.drain_timeout_ms = 2000;
    bc.rx = &ctx->rx;

    cp_session_error_t rc = cp_runtime_broker_init(&ctx->broker, &bc);
    if (rc != CP_SESSION_OK) {
        snprintf(error, sizeof(error), "broker init: %d", (int)rc);
        write_status(ctx, "fatal", error);
        cp_spp_socket_close(&ctx->rx);
        return 2;
    }
    rc = cp_runtime_broker_start(&ctx->broker, (uint64_t)now);
    if (rc != CP_SESSION_OK) {
        snprintf(error, sizeof(error), "broker start: %d", (int)rc);
        write_status(ctx, "fatal", error);
        cp_runtime_broker_destroy(&ctx->broker, (uint64_t)now);
        cp_spp_socket_close(&ctx->rx);
        return 2;
    }
    write_status(ctx, "started", "");
    int64_t last_ap = 0;
    bool failed = false;
    while (!g_stop) {
        struct pollfd pfds[CP_RUNTIME_BROKER_MAX_POLLFDS];
        size_t nfds = cp_runtime_broker_pollfds(&ctx->broker, pfds,
                                                CP_RUNTIME_BROKER_MAX_POLLFDS);
        now = monotonic_ms();
        int timeout = 500;
        uint64_t next = cp_runtime_broker_next_deadline(&ctx->broker);
        if (next != UINT64_MAX && next > (uint64_t)now) {
            uint64_t delta = next - (uint64_t)now;
            if (delta < (uint64_t)timeout) timeout = (int)delta;
        } else if (next != UINT64_MAX) {
            timeout = 0;
        }
        int pr = poll(pfds, (nfds_t)nfds, timeout);
        if (pr < 0 && errno != EINTR) {
            snprintf(error, sizeof(error), "poll: %s", strerror(errno));
            write_status(ctx, "fatal", error);
            failed = true;
            break;
        }
        now = monotonic_ms();
        rc = CP_SESSION_OK;
        if (!last_ap || now - last_ap >= 500) {
            rc = update_ap_readback(ctx, (uint64_t)now);
            last_ap = now;
        }
        if (rc == CP_SESSION_OK)
            rc = cp_runtime_broker_tick(&ctx->broker, pfds, nfds, (uint64_t)now);
        if (rc != CP_SESSION_OK || ctx->broker.fatal) {
            snprintf(error, sizeof(error), "broker tick: %d", (int)rc);
            write_status(ctx, "fatal", error);
            failed = true;
            break;
        }
        if (ctx->broker.restart_required) {
            rc = cp_runtime_broker_restart(&ctx->broker, (uint64_t)now);
            if (rc != CP_SESSION_OK) {
                snprintf(error, sizeof(error), "broker restart: %d", (int)rc);
                write_status(ctx, "fatal", error);
                failed = true;
                break;
            }
        }
        write_status(ctx, "running", "");
    }
    cp_runtime_broker_destroy(&ctx->broker, (uint64_t)now);
    cp_spp_socket_close(&ctx->rx);
    if (failed) return 2;
    write_status(ctx, "stopped", "");
    return 0;
}

static bool receiver_config_validate(broker_ctx_t *ctx, const char **reason)
{
    *reason = "ok";
    ctx->receiver_have_pi_pin = false;
    if (ctx->cfg.receiver_listener_owner[0] == '\0') {
        *reason = "listener-owner-config";
        return false;
    }
    if (ctx->cfg.receiver_pi_sha256[0] != '\0') {
        if (!cp_receiver_policy_pin_parse(ctx->cfg.receiver_pi_sha256,
                                          ctx->receiver_pi_pin)) {
            *reason = "pi-pin-config";
            return false;
        }
        ctx->receiver_have_pi_pin = true;
    }
    if (ctx->cfg.receiver_backend_library[0] != '\0' &&
        ctx->cfg.receiver_allow_unpinned_backend == 0) {
        *reason = "dns-sd-override-refused";
        return false;
    }
    return true;
}

static void print_receiver_check(const broker_ctx_t *ctx)
{
    char esc_service[384];
    char esc_owner[192];
    char esc_reason[64];
    char esc_source[128];
    char pi_hex[CP_OEM_RECEIVER_SHA256_LEN * 2u + 1u];

    pi_hex[0] = '\0';
    if (ctx->receiver_ready) {
        hex_encode(ctx->accepted.pi_sha256,
                   sizeof(ctx->accepted.pi_sha256), pi_hex, sizeof(pi_hex));
    }
    (void)cp_receiver_json_escape(ctx->accepted.service_name, esc_service,
                                  sizeof(esc_service));
    (void)cp_receiver_json_escape(ctx->listener.owner_name, esc_owner,
                                  sizeof(esc_owner));
    (void)cp_receiver_json_escape(ctx->receiver_reason, esc_reason,
                                  sizeof(esc_reason));
    (void)cp_receiver_json_escape(ctx->accepted.source_version, esc_source,
                                  sizeof(esc_source));
    printf("{\"ok\":%s,\"receiver\":\"%s\","
           "\"receiver_lookup\":\"%s\",\"receiver_policy\":\"%s\","
           "\"receiver_port\":%u,\"receiver_service_count\":%u,"
           "\"receiver_resolved_count\":%u,\"receiver_rejected_count\":%u,"
           "\"receiver_service_name\":\"%s\","
           "\"receiver_source_version\":\"%s\","
           "\"receiver_pi_sha256\":\"%s\",\"receiver_pi_len\":%u,"
           "\"listener\":\"%s\",\"listener_scope\":\"%s\","
           "\"listener_owner\":\"%s\",\"listener_owner_allowed\":%s,"
           "\"snapshot\":\"%s\"}\n",
           ctx->receiver_ready ? "true" : "false", esc_reason,
           cp_receiver_lookup_status_string(ctx->receiver_lookup_status),
           ctx->receiver_policy_evaluated
               ? cp_receiver_policy_status_string(ctx->receiver_policy_status)
               : "not-evaluated",
           ctx->receiver_ready ? (unsigned)ctx->accepted.port : 0u,
           ctx->receiver.service_count,
           ctx->receiver.resolved_count, ctx->receiver.rejected_count,
           ctx->receiver_ready ? esc_service : "",
           ctx->receiver_ready ? esc_source : "",
           pi_hex,
           ctx->receiver_ready ? (unsigned)ctx->accepted.pi_len : 0u,
           cp_receiver_listener_status_string(ctx->listener.status),
           listener_scope_string(&ctx->listener),
           ctx->listener.owner_name[0] != '\0' ? esc_owner : "",
           ctx->listener.owner_allowed ? "true" : "false",
           ap_snapshot_state_string(classify_ap_snapshot(&ctx->cfg)));
}

static int receiver_check_exit_code(const broker_ctx_t *ctx)
{
    if (ctx->receiver_ready) {
        return 0;
    }
    switch (ctx->receiver_lookup_status) {
    case CP_RECEIVER_LOOKUP_NO_BACKEND:
    case CP_RECEIVER_LOOKUP_BACKEND_PIN:
    case CP_RECEIVER_LOOKUP_ARGUMENT:
        return 3;
    default:
        return 2;
    }
}

int main(int argc, char **argv)
{
    bool check_ap = false;
    bool check_receiver = false;
    memset(&g_ctx, 0, sizeof(g_ctx));
    copy_field(g_ctx.cfg.config_path, sizeof(g_ctx.cfg.config_path), CFG_PATH);
    copy_field(g_ctx.cfg.daemon_status_path,
               sizeof(g_ctx.cfg.daemon_status_path), DAEMON_STATUS);
    copy_field(g_ctx.cfg.broker_status_path,
               sizeof(g_ctx.cfg.broker_status_path), BROKER_STATUS);
    copy_field(g_ctx.cfg.rx_path, sizeof(g_ctx.cfg.rx_path), RX_SOCKET);
    copy_field(g_ctx.cfg.usb_sysfs, sizeof(g_ctx.cfg.usb_sysfs),
               "/sys/bus/usb/devices");
    copy_field(g_ctx.cfg.service_name, sizeof(g_ctx.cfg.service_name),
               "SCOURGE-iAP");
    copy_field(g_ctx.cfg.receiver_interface,
               sizeof(g_ctx.cfg.receiver_interface), "wlan0");
    copy_field(g_ctx.cfg.receiver_listener_owner,
               sizeof(g_ctx.cfg.receiver_listener_owner), "AppProjection");
    g_ctx.cfg.receiver_timeout_ms = 15000u;
    g_ctx.cfg.receiver_settle_ms = 500u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check-ap")) {
            check_ap = true;
        } else if (!strcmp(argv[i], "--check-receiver")) {
            check_receiver = true;
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            copy_field(g_ctx.cfg.config_path, sizeof(g_ctx.cfg.config_path),
                       argv[++i]);
        } else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
            copy_field(g_ctx.cfg.daemon_status_path,
                       sizeof(g_ctx.cfg.daemon_status_path), argv[++i]);
        } else if (!strcmp(argv[i], "--broker-status") && i + 1 < argc) {
            copy_field(g_ctx.cfg.broker_status_path,
                       sizeof(g_ctx.cfg.broker_status_path), argv[++i]);
        } else if (!strcmp(argv[i], "--bus") && i + 1 < argc) {
            copy_field(g_ctx.cfg.bus_address, sizeof(g_ctx.cfg.bus_address),
                       argv[++i]);
        } else if (!strcmp(argv[i], "--usb-sysfs") && i + 1 < argc) {
            copy_field(g_ctx.cfg.usb_sysfs, sizeof(g_ctx.cfg.usb_sysfs),
                       argv[++i]);
        } else if (!strcmp(argv[i], "--receiver-interface") && i + 1 < argc) {
            copy_field(g_ctx.cfg.receiver_interface,
                       sizeof(g_ctx.cfg.receiver_interface), argv[++i]);
        } else if (!strcmp(argv[i], "--receiver-timeout-ms") && i + 1 < argc) {
            g_ctx.cfg.receiver_timeout_ms =
                (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--receiver-settle-ms") && i + 1 < argc) {
            g_ctx.cfg.receiver_settle_ms =
                (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--receiver-listener-owner") &&
                   i + 1 < argc) {
            copy_field(g_ctx.cfg.receiver_listener_owner,
                       sizeof(g_ctx.cfg.receiver_listener_owner), argv[++i]);
        } else if (!strcmp(argv[i], "--receiver-backend-root") &&
                   i + 1 < argc) {
            copy_field(g_ctx.cfg.receiver_backend_root,
                       sizeof(g_ctx.cfg.receiver_backend_root), argv[++i]);
        } else if (!strcmp(argv[i], "--dns-sd-library") && i + 1 < argc) {
            copy_field(g_ctx.cfg.receiver_backend_library,
                       sizeof(g_ctx.cfg.receiver_backend_library), argv[++i]);
        } else if (!strcmp(argv[i], "--allow-unpinned-dns-sd")) {
            g_ctx.cfg.receiver_allow_unpinned_backend = 1;
        } else {
            fprintf(stderr, "usage: %s [--config PATH] [--status PATH] "
                    "[--broker-status PATH] [--bus ADDRESS] "
                    "[--usb-sysfs PATH] [--receiver-interface NAME] "
                    "[--receiver-timeout-ms N] [--receiver-settle-ms N] "
                    "[--receiver-listener-owner NAME] "
                    "[--check-ap] [--check-receiver]\n"
                    "diagnostic-only: [--receiver-backend-root PATH] "
                    "[--dns-sd-library PATH] [--allow-unpinned-dns-sd]\n"
                    "exit codes: 0 ok, 2 refused/incomplete, "
                    "3 backend/pin/usage\n", argv[0]);
            return 3;
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (!check_ap) prepare_status_dir(&g_ctx.cfg);

    if (load_kv(g_ctx.cfg.config_path, &g_ctx.cfg) != 0) {
        fprintf(stderr, "cannot read %s\n", g_ctx.cfg.config_path);
        return 2;
    }
    if (check_ap) {
        bool ok = identity_complete(&g_ctx.cfg) &&
                  read_ap_snapshot(&g_ctx.cfg, geteuid());
        puts(ok ? "AP snapshot matches" : "AP snapshot refused");
        return ok ? 0 : 2;
    }
    load_bus_default(&g_ctx.cfg);
    if (!identity_complete(&g_ctx.cfg)) {
        char error[] = "identity or AP configuration incomplete";
        write_status(&g_ctx, "identity_incomplete", error);
        fprintf(stderr, "%s\n", error);
        return 2;
    }
    {
        const char *reason = "ok";
        if (!receiver_config_validate(&g_ctx, &reason)) {
            char error[128];
            copy_field(g_ctx.receiver_reason, sizeof(g_ctx.receiver_reason),
                       reason);
            snprintf(error, sizeof(error), "receiver configuration refused: %s",
                     reason);
            if (check_receiver) {
                print_receiver_check(&g_ctx);
                fprintf(stderr, "%s\n", error);
                return 3;
            }
            write_status(&g_ctx, "receiver_incomplete", error);
            fprintf(stderr, "%s\n", error);
            return 3;
        }
    }
    parse_languages(&g_ctx);
    {
        cp_wire_wifi_security security;
        if (!map_security(g_ctx.cfg.ap_security, &security)) {
            char error[] = "unsupported AP security";
            write_status(&g_ctx, "identity_incomplete", error);
            fprintf(stderr, "%s\n", error);
            return 2;
        }
        (void)security;
    }
    if (check_receiver) {
        char error[256] = "";
        bool ok = receiver_preflight(&g_ctx, false, error, sizeof(error));
        print_receiver_check(&g_ctx);
        if (!ok && error[0] != '\0') {
            fprintf(stderr, "%s\n", error);
        }
        return receiver_check_exit_code(&g_ctx);
    }
    return run_broker(&g_ctx);
}
